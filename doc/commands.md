# Control and Telemetry Interface Specification

This is the canonical wire specification for MQTT and serial commands, responses
and telemetry. Operational explanations and equations are indexed in
[Mathematics and reported uncertainty](mathematics.md).

The repository's [Python command and lab helpers](python_helpers.md) are a
**convenience test tool**, with typed results, collectors and analysis helpers.
They are not the official Python API; that API is maintained outside this
repository. Their core convention mirrors the wire interface: selectors alone
query; supplied values update; `None` omits a value while zero and `False` remain
explicit values. Normal command methods decode the matching firmware response
without issuing an extra readback. The dedicated guide explains the additional
operations performed by acquisition and plotting helpers.

The PCB owns command validation. The Python command wrappers send encodable
arguments, including deliberately invalid values and incomplete updates, to
firmware and preserve its error response. Python checks only requirements of its
own encoding, local calculations, collection and decoding; it does not duplicate
the firmware's allowed values or argument rules.


## MQTT Topics (roles)

- **Device subscribes:** `cmd/<device>/req/#` *(all request endpoints live under this prefix)*
- **Device publishes (responses):** `cmd/<device>/resp/...`
- **Device publishes (telemetry):** `dt/<device>/...`
- **Device publishes (warnings):** `dt/<device>/warning`

`<device>` is selected from the detected board strap:

| Board profile | MQTT device name |
| --- | --- |
| `tib` | `hsfib-tib` |
| `cal_hk` | `hsfib-rcal` |
| `cal_yj` | `hsfib-bcal` |
| `as` | `hsfib-as` |

### Global (applies to all commands)

- Command request topics use `cmd/<device>/req/<command-key>` and response
  topics use `cmd/<device>/resp/<command-key>` unless the endpoint documents a
  different topic shape. For example, request `cmd/<device>/req/status`
  receives its default response on `cmd/<device>/resp/status`.

- Successful requests with no response data return:
  ```json
  {"status":"ok"}
  ```

- Successful requests with response data return only the response data. They do
  not include a top-level transport `status` key.

- Failures are indicated by the presence of an `error` key:
  ```json
  {"error":"<error message>"}
  ```
  Error responses may include extra diagnostic fields such as `rc` or
  `context` when the command documents them.

- Endpoint sections use request-shape wording:
  - **Request with no payload**
  - **Request with payload**
  - **Request with topic suffix**

  Side effects, persistence, and hardware behavior are described in endpoint
  notes rather than encoded into a separate request class.

- Req/resp also use MQTT5 request/response metadata:
  - **On requests (publisher → device):**
    - `response_topic`: where the device should publish the response (this doc assumes it’s under `cmd/<device>/resp/...`)
    - `correlation_data`: opaque bytes that are echoed back exactly in the
      response so the requester can match replies to requests, up to 16 bytes
    - retained request messages are ignored and return an error response; command
      topics must not replay actions on reconnect
  - **On responses (device → publisher):**
    - `correlation_data`: copied from the request when it is 16 bytes or less
    - `qos`: response QoS
    - user property `ack_utc_ms`: board UTC milliseconds captured centrally after
      command completion, before enqueueing, including error replies. It is not
      the host receipt time or a claim that optical settling is complete.
- Commands have serial port duals. Serial commands use a simpler line format
  for interactive bring-up and debugging.
- Board-specific commands are rejected before their domain handler runs when
  the selected board strap does not provide that hardware.
- Command keys are exact-match by default. Only endpoint families documented
  with topic suffixes, such as `atten/<laser>/coeff`, `mems/split/yj`, or
  `laser/bankpower/<mode>`, opt into prefix matching. Unknown top-level payload
  keys are rejected before the domain handler runs.
- Invalid arguments return an informative `error`; where useful, the message
  names the field and refers to the catalog (`help`, `help/options`, and this
  document). Unknown nested laser settings, PID fields, and attenuator
  coefficient fields are also rejected. Unsigned integer fields reject negative,
  fractional, and overflowing values; floating-point fields require finite values.
- When a selector is supplied both in a topic suffix and in the payload, both
  must be valid and agree. A payload selector cannot repair an invalid suffix.

## Serial Command Form

Serial commands are one line, whitespace separated, and intended for a human at
the console.

Top-level implementation path:

1. Main calls `coo_cmd_runtime_serial_poll()` to consume up to 64 buffered
   characters without waiting and assemble complete console lines. Receive
   buffering and network-wait limitations are described in
   [serial polling](architecture.md#serial-polling).
2. `coo_cmd_runtime_handle_serial_line()` splits the line into `<key>` and optional payload.
3. `coo_cmd_normalize_serial_payload()` turns non-JSON serial payloads into the same JSON shape used by MQTT.
4. Serial `help` prints directly during ingress and does not enter the command
   queues. Other normalized requests are enqueued to `inbound_queue`.
5. `coo_cmd_runtime_executor_thread()` handles built-ins (`help`, `reboot`, and
   enabled `serialguard`/`ota`) before dispatching app commands through the static table.
   Builtins and app commands both check table-based OTA permission before execution.
6. `coo_cmd_runtime_drain_outbound()` prints queued serial responses with
   `coo_cmd_print_serial_response_pretty()`. The simpler
   `coo_cmd_print_serial_response()` remains available as a fallback renderer.

No-payload serial request form is just the key:

```text
status
mems/yj_laser_cal
mems/split/yj
```

Requests with payload use the key followed by a payload. There are no `get` or
`set` keywords in the serial command set.

```text
serialguard seconds=60
mems/yj_laser_cal state=A duty_cycle=0.5 cycle_ms=400 off_in_s=30
mems/split channel=yj ratio1=0.25 ratio2=0.25 cycle_ms=800 stop_in_s=300
laser/bankpower/override_on
```

Payload rules:

- A payload beginning with `{` is copied unchanged into `Command.payload`; it is
  not parsed and rebuilt by the serial layer.
- Payloads containing `=` use `serial_payload_from_key_values()`, for example
  `state=A off_in_s=30`.
- Known compact forms use `serial_payload_from_shorthand()`, for example
  `serialguard off`, `serialguard 60`, or `mems/yj_laser_cal A 0.5 30`.
- Handlers parse and validate the normalized JSON exactly as they do for MQTT.
- Overlong command keys and shorthand tokens are rejected rather than truncated.

Serial response format:

```text
cmd/<device>/resp/<key>
        {"same":"payload MQTT would publish"}
```

The first line is the MQTT response topic. The following lines are the response
payload. The pretty renderer emits LF line endings, indents JSON-like
payloads, and wraps long non-JSON payloads at natural break points where
possible.

Any non-empty serial command refreshes serial override when
`CONFIG_COO_CMD_SERIAL_GUARD` is enabled. While active, MQTT effect commands are
rejected before dispatch and receive an error response when MQTT is available.
The override expires after `serialguard_s` seconds without another serial
command; `serialguard off`, `serialguard 0`, or `serialguard seconds=0`
disables the override. JSON payloads are accepted for MQTT parity, but should
not be needed for normal serial operation.

---

(ota-permissions)=
## Commands while OTA is active

The dispatcher checks the matched table entry's `ota_query_allowed` or
`ota_effect_allowed` after dequeue and before invoking the handler or recording
an effect as `lastcommand`. Both permissions default to false. This covers
requests queued before OTA entry as well as new MQTT/serial requests.

| Commands | Queries | Effects |
| --- | --- | --- |
| `ota`, `serialguard` | allowed | allowed |
| `reboot` | unsupported | allowed |
| `help`, `help/options`, `status`, `temps` | allowed | disallowed |
| `ip`, `mqtt`, `time`, `laser/bankpower` | allowed | disallowed |
| All other application commands | disallowed | disallowed |

A blocked operation returns `{"error":"command unavailable during OTA"}`.
Unknown commands, invalid payloads, unsupported operations, and unavailable board
features retain their usual errors. Serial guard can still reject MQTT effects,
including `ota` effects; OTA permission does not override it. Serial `help` remains
an immediate console operation, allowed during OTA.

The restriction lasts through window close if an admitted SMP operation is still
finishing, a candidate is pending, or the running image is an unconfirmed trial.
Bank-mode writes are all blocked, even a repeated `override_off` in payload or
suffix form. After successful confirmation the operator must select `auto`.

## Command Endpoints

- [`help`](#help)
- [`help/options`](#help-options)
- [`status`](#status)
- [`temps`](#temps)
- [`time`](#time)
- [`ip`](#ip)
- [`mqtt`](#mqtt)
- [`serialguard`](#serialguard)
- [`reboot`](#reboot)
- [`ota`](#ota)
- [`mems`](#mems)
- [`mems/<switchname>`](#mems-switchname)
- [`mems/route`](#mems-route)
- [`mems/route/loss`](#route-loss)
- [`mems/split`](#mems-split)
- [`laser`](#laser)
- [`laser/tune`](#laser-tune)
- [`laser/status`](#laser-status)
- [`laser/settings`](#laser-settings)
- [`laser/calibrate`](#lasercalibrate)
- [`laser/bankpower`](#laserbank-power)
- [`laser/bankheater`](#laserbank-heater)
- [`laser/clearfaults`](#laserbank-clearfaults)
- [`atten/<laser>`](#atten)
- [`atten/<laser>/coeff`](#atten-coeff)
- [`atten/calibrate`](#atten-calibrate)
- [`pd`](#pd)
- [`pd/settings/<yj|hk>`](#pd-settings)
- [`measure_throughput`](#measure-throughput)
- Telemetry: `yj_tput`, `hk_tput`
- Warnings: [`dt/<device>/warning`](#warning-publication)
- Boot telemetry: [`dt/<device>/boot`](#boot-telemetry)

---

(warning-publication)=
### Warning Publication
- **Publish topic:** `dt/<device>/warning`
- **App producer interface:** `app_output_emit(&args)`, using the existing
  common `coo_cmd_runtime_emit()` implementation and outbound queue.
- **Queue behavior:** warnings are usually best-effort MQTT, but the runtime
  helper accepts an explicit delivery mode. Required warnings are retried by the
  outbound drain after successful enqueue; enqueue can still fail if the bounded
  outbound queue is full.
- **Warning payload:**
  ```json
  {
    "severity": "warning",
    "code": "<stable_warning_code>",
    "msg": "<short human text>",
    "context": "<short context>",
    "uptime_ms": 0
  }
  ```

`uptime_ms` is the device uptime in integer milliseconds

Warnings do not imply command failure unless the command response also reports
an error. Most warning delivery is intentionally lossy and is not mirrored into
sticky status fields. Current warning emitters include MQTT command rejection
while serial guard is active and attenuator DAC-range clamping.

(boot-telemetry)=
### Boot Telemetry
- **Publish topic:** `dt/<device>/boot`
- **When published:** after a boot where Zephyr hwinfo reports the prior reset
  cause included watchdog expiration.
- **Delivery:** non-best-effort MQTT outbound message; the main loop requeues it
  until MQTT is available or publish succeeds.
- **Payload:**
  ```json
  {
    "event": "boot",
    "reset_cause": "watchdog",
    "watchdog": true,
    "raw_reset_cause": 16
  }
  ```
- **Notes:** `reset_cause` may contain a comma-separated list if hardware reset
  flags report multiple causes for the same boot.

(help)=
### `help`
- **Serial no payload -> full command help.**
  - Prints directly from command dispatch instead of using the inbound or
    outbound queues.
  - Takes no arguments. `help <anything>` is rejected.
  - Enumerates dispatcher built-ins and app command specs with help metadata,
    including optional fields in `[]`, accepted enum values, unsupported
    commands for the current board profile, and commands allowed as MQTT queries
    while serial guard is active.
- **MQTT no payload -> compact endpoint summary:**
  ```json
  {
    "device": "<device_id>",
    "request_prefix": "cmd/<device_id>/req/",
    "response_prefix": "cmd/<device_id>/resp/",
    "commands": ["<command key>", "..."]
  }
  ```
  MQTT help is intentionally compact so it does not consume the payload budget
  with the full serial help text.

(help-options)=
### `help/options`
- **No payload -> static name catalog for the selected board profile:**
  ```json
  {
    "board": "tib",
    "lasers": ["1028y", "1270j", "1430yj", "1430hk", "1510h", "2330k"],
    "route_inputs": ["yj_1430", "yj_cal"],
    "route_outputs": ["yj_ao", "yj_fei"],
    "routes": [["yj_1430", "yj_ao"], ["yj_cal", "yj_fei"]]
  }
  ```
- **Notes:** `route_inputs`, `route_outputs`, and `routes` come from the
  board-selected MEMS route table. `routes` is the authoritative list of valid
  input/output pairs for `mems/route` and route-bearing commands. `lasers` is
  populated on TIB and empty on non-TIB board profiles.

(status)=
### `status`
- **No payload or payload -> firmware status.**

  Optional payload:
  ```json
  {
    "ip": true,
    "lasers": true,
    "attens": true
  }
  ```

  Response:
  ```json
  {
    "fw": "<tag-or-short-git-hash>",
    "boots": 0,
    "board": "tib|cal_yj|cal_hk|as|unknown",
    "board_ok": true,
    "mems_switches": 8,
    "relay_err": 0,
    "ip": "<response of ip command query>",
    "amb_c": 0.0,
    "pd_on_s": 0,
    "laserbank_on_s": 0,
    "lasers": {
      "<lasername>": {
        "power_mw": 0.0,
        "ready": false,
        "tec_on_s": null,
        "off_in_s": null
      }
    },
    "attens": {
      "<attenname>": {
        "value_db": 0.0
      }
    },
    "lastcmd": {
      "name": "<cmdname>",
      "src": "mqtt",
      "t_ms": 0
    }
  }
  ```
- **Notes:** `ip`, `lasers`, and `attens` are omitted unless requested.
  Laser `tec_on_s` and `off_in_s` are integer seconds while active and `null`
  when inactive or unavailable. `ready` has the public-start blocker meaning documented under `laser`.
  `lastcmd` is restored from command-dispatch NVS storage when available.


(temps)=
### `temps`
- **No payload -> temperature status:**
  ```json
  {
    "ambient_c": 0.0,
    "laserbank_c": 0.0,
    "lasers": {
      "<lasername>": 0.0
    }
  }
  ```

- **Notes:** Laser diode TEC temperatures are included when the laser bank is powered and the relevant driver registers
  can be read. Unavailable values are returned as JSON `null`. `laserbank_c` is
  the mean of valid idle probes whose TEC is stopped; actively regulated TEC
  temperatures are excluded. It is `null` when every TEC is active, the bank is
  off, or no valid idle probe exists. Individual readings include active TECs.
  This remains a fresh foreground read, not the heater cache, and does not power
  the bank. For example, an idle probe at 19 C and an active TEC at 25 C give
  `laserbank_c:19`, while both individual readings remain present. On TIB, if the shared Maiman Modbus bus is busy, this command
  returns `{"error":"busy"}` instead of an ambient-only partial response.

(time)=
### `time`
- **No payload -> firmware time:**
  ```json
  {
    "utc": 0,
    "uptime_s": 0
  }
  ```
- **Payload:** set firmware time.
  ```json
  {"unix_ms":1790812800000}
  ```

- **Notes:** UTC is milliseconds since the Unix epoch; uptime uses the LSE-backed
  native LPTIM clock and is unaffected by UTC corrections. Manual time and SNTP
  share one setter, retaining fractional seconds in running UTC. RTC calendar
  writes store whole seconds and occur only when its time is unreadable or more
  than 1.25 s from the requested UTC. This avoids repeated calendar resets for
  subsecond corrections. A valid retained RTC seeds boot UTC; the RTC has no
  backup supply, so power loss can leave it invalid. Boot restoration carries the
  retained RTC's offset until SNTP. No last-known UTC is restored from NVS.
  SNTP includes Zephyr's estimated one-way response delay when applying server
  time. Successful syncs repeat every 60 s and failures retry after 30 s.
  Startup first tries after one second; reconnect wakes the thread immediately. Manual time may subsequently be overwritten by SNTP.
  `pcb.time(offset_tolerance_ms=50)` warns only if the board time lies outside the
  host issue/receipt bracket by more than the tolerance; round-trip latency is
  therefore not mistaken for clock error. `None` disables this host warning.

(ip)=
### `ip`
- **No payload -> IP configuration:**
  ```json
  {
    "src": "<source>",
    "try_dhcp_first": true,
    "prefer_dhcpdns": true,
    "prefer_dhcpntp": true,
    "manual": {
      "ip": "<ip>",
      "subnet": "<subnet>",
      "gateway": "<gateway>",
      "dns": "<ip>",
      "ntp": "<ip>"
    },
    "active": {
      "ready": true,
      "ip": "<ip>"
    },
    "ntp": {
      "src": "<source>",
      "server": "<ip>"
    }
  }
  ```
- **Payload:** update IP configuration.
  ```json
  {
    "ip": "<ip>",
    "ntp": "<ip>",
    "dns": "<ip>",
    "subnet": "<subnet>",
    "gateway": "<gateway>",
    "try_dhcp_first": true,
    "prefer_dhcpntp": true,
    "prefer_dhcpdns": true,
    "persist": true
  }
  ```

- **Notes:**
  - Unsupported features don’t error; supported changes are still applied and
    partial status reports unsupported fields.
  - Supported address fields must contain valid dotted IPv4 addresses before
    networking or saved settings change. `gateway`, `dns`, and `ntp` also accept
    an empty string to clear the value; `ip` and `subnet` do not.
  - IP precedence: runtime settings → compiled static defaults. The compiled
    static defaults are also the last-resort service fallback.
  - If `try_dhcp_first` is true and DHCP is compiled in, DHCP is tried before the
    runtime static profile. Static fallback remains DHCP-overridable so a later
    lease can replace it.
  - Partial responses include keys indicating which settings are not supported.
  - network-affecting changes are applied at runtime; ordinary changes do not
    require reboot.
  - source names are: `unknown`, `compiled`, `static`, `fallback`, `dhcp`.

(mqtt)=
### `mqtt`
- **No payload -> MQTT broker configuration:**
  ```json
  {"broker":"<value>:<port>","dns_supported":true}
  ```
- **Payload:** update MQTT broker configuration.
  ```json
  {
    "broker": "<ipv4-or-hostname>:<port>",
    "persist": true
  }
  ```

- **Notes:**
  - Broker value must be one `<host-or-ip>:<port>` string.
  - If DNS is not compiled in, hostname values are rejected.
  - Hostname values must resolve before settings are updated. Numeric IPv4
    broker values do not require DNS.
  - Successful set updates runtime settings and triggers MQTT reconnect
    behavior. If the new broker cannot connect, firmware restores the prior
    broker and emits a best-effort `mqtt_broker_revert` warning.

(serialguard)=
### `serialguard`
- **No payload -> serial guard configuration and current state:**
  ```json
  {"serialguard_s":30,"active":true,"remaining_s":12}
  ```
- **Payload:** update serial guard configuration.
  ```json
  {
    "seconds": 30
  }
  ```
  Supplying `persist` is rejected; serial guard is runtime-only and is not restored after reboot.

- **Notes:**
  - Any non-empty serial command activates or refreshes the guard.
  - The `serialguard` command itself is allowed while the guard is active so an
    operator can extend, shorten, or disable the current expiry.
  - Serial shorthand: `serialguard seconds=60`, `serialguard 60`, or
    `serialguard off`.
  - While active, MQTT requests that may change hardware or runtime state are
    rejected before dispatch and logged. Safe read-only MQTT requests are
    allowed according to the app command table.
  - The guard is owned by the command-dispatch library and uses one
    dispatcher-owned `k_work_delayable` item.
  - `seconds:0` disables serial override.

(reboot)=
### `reboot`
- **No payload:** schedule a non-cancelable reboot after the response window.
  ```json
  {"status":"ok","rebooting_in_ms":3000}
  ```
- **Payload:** erase persisted app settings except IP settings and boot count
  immediately before reboot.
  ```json
  {"erase_non_ip_settings":true}
  ```
- **Serial form:**
  ```text
  reboot erase_non_ip_settings
  ```
- **Notes:** command dispatch owns the reboot delayable work item. Immediately
  before reboot it calls the app reboot-prepare hook so firmware can put
  hardware into a safer state and, when requested, erase non-IP persisted
  settings. The erase preserves IP settings, boot count, and storage schema
  metadata. Once a reboot is pending, later commands are rejected before app
  handlers run.

(ota)=
### `ota`

Available only in the explicit MCUboot/OTA build (`CONFIG_COO_OTA`). The default
standalone bench build omits this command. See [firmware updates](ota.md).

- **No payload:** query running MCUboot identity and maintenance/trial state.
  ```json
  {"enabled":false,"active":false,"remaining_s":0,"image_hash":"<64 hex digits>","confirmed":true,"pending":false,"trial_remaining_s":0,"max_image_size":966656}
  ```
  `enabled` means the UDP maintenance window is open. `active` additionally
  includes an operation finishing after close, a pending candidate, or an
  unconfirmed/expired trial; it activates the [OTA command permissions](#ota-permissions).
  `remaining_s` and `trial_remaining_s` are rounded-up seconds. `image_hash` is
  MCUboot's SHA256 of the running header/body, not the whole-file SHA256 or the
  Git build string. `max_image_size` is generated from the slot geometry and
  sysbuild reservation for offset swapping and the trailer.
- **Open:** `{"enable":true,"duration_s":600}`. Duration is 1..1800 seconds,
  default 600. Repeating an open request resets that window's deadline.
  Firmware must be confirmed, with no pending candidate or closing operation.
  TIB also requires the operator to have stopped experiments, selected bank
  `override_off`, and let bank power turn off. This command never powers down
  an active bank on the operator's behalf.
- **Close:** `{"enable":false}`. No duration is accepted when closing.
  Closing or expiry stops UDP access and leaves bank mode unchanged. A pending
  image remains pending; close is not a cancellation of a scheduled test boot.
- **Confirm:** `{"confirm":true,"image_hash":"<exact running hash>"}`.
  This writes only the running image's MCUboot confirmation flag. An incorrect
  hash, or confirmation at/after the five-minute trial deadline, is rejected.
  Repeating confirmation for an already confirmed matching image is harmless.
  Confirmation leaves bank mode `override_off`; the operator restores `auto`.
- **Serial:** `ota`, `ota enable=true duration_s=600`, `ota enable=false`,
  `ota confirm=true image_hash=<hash>`. MQTT effects obey serial guard.
- **Transfer:** image state/test and upload use SMP on IPv4 UDP port 1337 while
  the window is open. Upload is unavailable during a trial or with a pending
  image. Permanent secondary confirmation, image erase, and other SMP groups
  are unavailable. Use `reboot` after selecting the candidate for a test boot.
- **Trial:** UDP starts closed. TIB selects bank `override_off` before workers
  or command ingress. The five-minute deadline starts at application boot,
  independent of network availability and the earlier upload window. Without
  confirmation, firmware requests the existing delayed reboot and stops feeding
  IWDG; MCUboot reverts on reset. No powered-laser or Modbus response check is
  part of confirmation. See [OTA operation](ota.md).

(mems)=
### `mems`
- **No payload -> all MEMS switch states:**
  ```json
  {
    "<switchname>": {
      "state": "A|B|?"
    }
  }
  ```
- **Notes:** The all-switch query is intentionally compact so the TIB
  eight-switch response fits the fixed MQTT payload buffer. Static switches
  report only `state`. A switch currently configured with a non-constant duty
  request also includes `duty_cycle`. Use `mems/<switchname>` for
  actual dwell timing and stop-in details.

(mems-switchname)=
### `mems/<switchname>`
- **Topic:** `cmd/<device>/req/mems/<switchname>`
- **No payload or payload -> one MEMS switch state.**

  Static payload:
  ```json
  {"state":"A","force":false}
  ```
  or:
  ```json
  {"state":"B"}
  ```
  Toggle:
  ```json
  {
    "state": "A",
    "duty_cycle": 0.5,
    "cycle_ms": 400,
    "off_in_s": 30
  }
  ```

  Response:
  ```json
  {
    "state": "A|B|?"
  }
  ```
  For example, `mems/yj_laser_cal state=B` returns:
  ```json
  {"state":"B"}
  ```
  and `mems/yj_laser_cal state=A` returns:
  ```json
  {"state":"A"}
  ```
  Response while configured with a non-constant duty request:
  ```json
  {
    "state": "A|B|?",
    "duty_cycle": 0.5,
    "cycle_ms": 400,
    "a_ms": 200,
    "b_ms": 200,
    "stop_in_s": 30
  }
  ```
- **Notes:**
  - Request payloads accept `state:"A"`/`"B"` and lowercase `state:"a"`/`"b"`;
    responses always use uppercase `A`/`B`.
  - `force:true` is valid only for static A/B requests. It queues one actuation
    pulse even when the switch already reports that state. Repeated force
    requests before the pulse fires coalesce to one pending pulse.
  - `duty_cycle` is only valid with `state:"A"`.
  - Static `{"state":"A"}` and `{"state":"B"}` responses report only  `state`.
  - `cycle_ms` is optional for mixed-duty toggling. If omitted, the firmware
    uses the fastest safe A-B-A cycle for the requested duty cycle. If supplied,
    the firmware may quantize duty inside the requested cycle but does not
    stretch the requested cycle beyond MEMS tick granularity.
  - `cycle_ms` replaces `toggle_rate_hz`; `toggle_rate_hz` is rejected.
  - `{"state":"A","duty_cycle":0.0}` is valid and equivalent to static `B`.
  - Request `off_in_s` is integer seconds, with max 4 hours. Response
    `stop_in_s` is remaining toggle time.
  - Static switch requests persist as user intent. With
    `CONFIG_SET_SWITCH_STATE_AT_BOOT=y`, boot initializes each switch target
    from that intent and resends the pulse shortly after startup. This reasserts
    software intent; firmware still does not physically verify switch position
    across reboot, switch replacement, or external actuation.
  - Mixed-duty toggle requests persist as restart metadata. With
    `CONFIG_RESUME_TOGGLE_STATE_AT_BOOT=y`, boot restarts the stored request
    using its original commanded duration. Runtime remaining time is not
    preserved across reboot.
  - `duty_cycle`, `cycle_ms`, `a_ms`, `b_ms`, and `stop_in_s` are omitted for
    constant A or B profiles.
  - `a_ms` and `b_ms` are the actual scheduled dwell times in the hardware A
    and B states. The firmware keeps all A/B actuation pulses at least
    `1 / MEMS_SWITCH_MAX_TOGGLE_HZ` apart, quantizing
    `cycle_ms` if required.
  - Static state changes can be delayed until the same pulse-spacing rule is
    satisfied; status reports the last pulsed state until the delayed pulse
    occurs. A delayed same-state `force:true` pulse is not separately reported
    in status.
  - If the actual cycle differs from requested, the firmware emits
    `mems_timing_quantized` on `dt/<device>/warning`.


(mems-route)=
### `mems/route`
- **No payload -> active routes:**
  ```json
  {
    "active_routes": {
      "<dest.name>": ["<source.name>", "..."]
    }
  }
  ```
- **Payload:** set one route.
  ```json
  {
    "input": "<source>",
    "output": "<dest>",
    "force": false
  }
  ```
- **Notes:** The no-payload response lists every destination present in the active
  board route table. Each value is an array of currently connected sources
  because one destination may receive multiple sources through combining optics.
  A destination with no currently active source reports `["no source"]`. Active
  routes are read from current switch state and are not persisted as named
  route objects. Applying a route persists the resulting per-switch static
  intent. `force:true` queues one static actuation pulse for every switch step
  in the route, including steps that already report the requested state.

(route-loss)=
### `mems/route/loss`
- **Payload:** set one route-loss record.
  ```json
  {
    "route": "yj_sm_to_yj_pd",
    "1430yj": 0.07,
    "persist": true
  }
  ```
  or:
  ```json
  {
    "route": "yj_sm_to_yj_pd",
    "1430yj": "0.32 dB",
    "persist": false
  }
  ```
  or:
  ```json
  {
    "route": "yj_calin_to_yj_split",
    "split": ["0.32 dB", "0.32 dB", 0.07],
    "persist": true
  }
  ```
- **Payload -> route-loss records for one route:**
  ```json
  {
    "route": "yj_sm_to_yj_pd"
  }
  ```
  ```json
  {
    "route": "yj_sm_to_yj_pd",
    "lasers": {
      "1028y": 0.4,
      "1270j": 0.4,
      "1430yj": 0.07,
      "1430hk": 0.0,
      "1510h": 0.0,
      "2330k": 0.0,
      "generic": 0.4
    }
  }
  ```
  For a split route:
  ```json
  {
    "route": "yj_calin_to_yj_split",
    "split": [0.07, 0.07, 0.0]
  }
  ```

Route-loss records are app settings keyed by route name and source name or split.
Each set request accepts exactly one top-level named laser, `generic`, or `split` tuple.
The `lasers` object is a query response only. `persist` requires a loss value.
Numeric values are fractions of light lost: finite `0 <= loss < 1`. Zero means
no loss; `0.5` means half the light is lost. Exactly `1` is rejected because
transmission must remain positive. Missing records use the compiled TIB defaults
in `devices.c`, documented in [hardware.md](hardware.md#tib-route-loss-defaults),
for the named source, then the route's compiled generic default, then unity
transmission. Explicit records replace the complete default, including an explicit zero loss.

`generic` stores a broadband/passive record, for example
`{"route":"yj_cal_to_yj_ao","generic":"1 dB","persist":false}`.
It is accepted and appears in the `lasers` query map only for the eight TIB
CAL-to-AO/FEI and MM/SM-to-PD routes (four per channel). MM 0.98 and SM 0.60
are the compiled generic defaults. Named sources use independent overrides and
never inherit a stored generic override. Passive capture reports an explicitly
selected CAL launch transmission, but unknown emission still prevents throughput.
AS split losses and named-source scalar storage on AS/CAL boards are unchanged;
AS CAL-in-to-CAL has no loss consumer. The table holds 32 overrides per PCB;
AS split records live on their own PCB. Defaults occupy no override slots.
The obsolete loss selector `none` is rejected. [Schema 14](settings.md) clears
all older app NVS settings at first boot, with no migration.

Strings ending in `dB`, `db`, or `DB` accept nonnegative finite loss in dB in the
same field. Firmware stores transmission: `tx = 1 - loss` for numeric inputs,
or `tx = 10^(-loss_db / 10)` for dB strings. The latter must produce a positive
finite transmission. Queries return `1 - tx` with 17 significant digits to
preserve very small transmissions. Python route-loss representations also
retain float precision. Prefer dB input when a fractional
value would lose precision near an endpoint. The split value is a three-tuple
and may mix numeric fractional losses with dB strings.

For throughput, route keys join the applicable input/output pair from
`help/options` with `_to_`, such as `yj_laser_to_yj_ao`; the laser name selects
its wavelength-dependent loss record. The return path uses keys such as
`yj_mm_to_yj_pd`. AS splitting uses `yj_calin_to_yj_split` or
`hk_calin_to_hk_split` and the three output losses. Storage remains transmission;
these values feed the existing throughput and splitting calculations.


(mems-split)=
### `mems/split`
- **Topics:**
  - `cmd/<device>/req/mems/split`
  - `cmd/<device>/req/mems/split/yj` or `cmd/<device>/req/mems/split/hk`
  - Responses use the same key under `cmd/<device>/resp/...`.
  
- **Payload to `mems/split` -> set splitter state:**
  ```json
  {
    "channel": "yj",
    "ratio1": 0.25,
    "ratio2": 0.25,
    "cycle_ms": 800,
    "stop_in_s": 0
  }
  ```
- **No payload to `mems/split/yj` or `mems/split/hk` -> get splitter state.**
- **Availability:** only available when the AS board strap is selected.

  Response:
  ```json
  {
    "channel": "yj",
    "ratio_ask": [0.25, 0.25, 0.50],
    "ratio_actual": [0.25, 0.25, 0.50],
    "ratio_out": [0.25, 0.25, 0.50],
    "split_transmission": [1.0, 1.0, 1.0],
    "cycle_ms": 800,
    "switches": [
      {
        "name": "yj_as1",
        "state": "A",
        "duty_cycle": 0.25,
        "a_ms": 200,
        "b_ms": 600
      },
      {
        "name": "yj_as2",
        "state": "B",
        "duty_cycle": 1.0,
        "a_ms": 0,
        "b_ms": 800
      },
      {
        "name": "yj_as3",
        "state": "A",
        "duty_cycle": 0.50,
        "a_ms": 400,
        "b_ms": 400
      }
    ],
    "stop_in_s": 0
  }
  ```

- **Notes:**
  - This is intentionally not a general route/switch feature. It is the
    system-level achromatic-splitter operation for the AS PCB.
  - The implementation is anchored in `splitting_set()` and `splitting_get()`.
  - The fixed routes are `yj_calin -> yj_split` and `hk_calin -> hk_split`,
    defined in `setup_mems_switches_and_routes()`. `splitting_set()` gets the
    route with `mems_router_get_route()`, then walks the route steps with
    `mems_router_find_switch()` as `memsroute_set()` does.
  - YJ and HK are set independently with `channel:"yj"` or `channel:"hk"`.
  - Split requests persist as restart metadata. With
    `CONFIG_RESUME_TOGGLE_STATE_AT_BOOT=y`, boot restarts the stored split
    request using its original commanded duration. Runtime remaining time is not
    preserved across reboot.
  - The user sets only `ratio1` and `ratio2`, both as floats from `0.0` to
    `1.0`. They must sum to `<= 1.0`; `ratio3` is computed internally as the
    remaining fraction.
  - `ratio1` maps to the direct branch selected by SW1. The remaining light is
    sent through the downstream branch. SW2 is held on the splitter branch.
    SW3's selected-state dwell is `ratio1 + ratio2`, so its output-2
    interval starts after SW1's output-1 deadtime.
  - `cycle_ms` is optional. If omitted, the firmware uses the fastest period
    that keeps every non-static MEMS actuation pulse within
    `MEMS_SWITCH_MAX_TOGGLE_HZ`. If supplied, the firmware keeps the requested
    cycle except for MEMS tick quantization and quantizes the split ratios
    inside that fixed cycle. `toggle_rate_hz` is rejected.
  - `stop_in_s` is integer seconds, with max 4 hours. `0` disables the split
    auto-stop.
  - Split switch timing may take a few MEMS cycles to settle after a new
    request; startup phase is not guaranteed cycle-exact.
  - `ratio_ask`, `ratio_actual`, `ratio_out`, and `split_transmission` are
    arrays ordered as `[ratio1, ratio2, ratio3]`.
  - `ratio_ask` is the requested output split. `ratio_actual` is the MEMS duty
    split after transmission correction and integer tick quantization.
    `ratio_out` is the estimated optical output split after applying
    `split_transmission`.
  - Each switch report gives the selected route state, the selected-state
    duty-cycle fraction, and the raw A/B dwell timing as `a_ms` and `b_ms`.
    For a `state:"B"` split switch, `duty_cycle` is `b_ms / cycle_ms`.
  - If the actual cycle differs from requested, the firmware emits
    `mems_timing_quantized` on `dt/<device>/warning`.
  - If the attained ratio differs from the requested ratio because MEMS timing
    is quantized, the firmware emits `split_ratio_quantized` on
    `dt/<device>/warning`.
  - The route-loss split tuple sets `split_transmission`. Set all three split
    transmissions to the same value, or leave them unset, to disable relative
    split correction.

(laser)=
### `laser`
- **Payload -> laser status:**
  ```json
  {"name":"<lasername>"}
  ```
  ```json
  {
    "name": "<lasername>",
    "powered": true,
    "ready": true,
    "blocked_reason": null,
    "tec_on_s": null,
    "emit_on_s": null,
    "emit_total_s": null,
    "temp_c": 0.0,
    "i_mA": 0.0,
    "value": 0.0,
    "power_mw": 0.0,
    "wave_ref_nm": 0.0,
    "tuned_nm": 0.0,
    "tune_nm": 0.0,
    "tec_ma": 0.0,
    "diode_v": 0.0,
    "tec_v": 0.0,
    "off_in_s": null,
    "oc_fault": false
  }
  ```
- **Payload:** set one laser output level.
  ```json
  {
    "name": "<lasername>",
    "value": 0.0,
    "autooff_s": 0
  }
  ```

- **Notes:** `value` is a fraction from 0 to 1 of modeled optical emission at nominal current.
  Firmware uses the calibrated inverse to select current; reported `value` uses the
  confirmed current and the same forward curve. Setting a positive value powers
  the laser bank as needed, prepares the TEC, applies the stored `laser/tune`
  request when `tune_nm` is nonzero, sets the laser current, and restarts the
  auto-off timer. Setting value 0 writes zero current without STOP, retains driver/TEC
  readiness, and preserves an existing auto-off deadline unless `autooff_s` is supplied.
  It does not power up/start an idle driver or clear the stored `laser/tune` request.
  Use `{"name":"1028y","stop":true}` for explicit shutdown: zero current, STOP,
  and TEC shutdown according to `disable_tec_at_autooff`. With `stop:true`, `value`
  must be omitted or zero and `autooff_s` must be omitted. Laser auto-off and
  measurement-owned expiry use this shutdown path. Zero-current time does not
  count toward emission time. Identity and applied configuration remain valid
  until bank power cycles or an explicit configuration/reset operation changes them. Laser output current is
  never persisted by app settings. The Maiman
  driver may retain its own current register, so firmware writes 0 whenever emission is disabled or the bank is turned off.
  `ready` reports whether a start attempt has a known public-endpoint blocker.
  The endpoint handles automatic bank power and TEC preparation; neither a
  stopped TEC nor a recoverable historical control fault requires a separate
  user action. An unpowered bank in `auto` can therefore be ready, without
  claiming its currently unreadable controller is healthy. Explicit bank
  override-off, identity failures and applicable hard locks block start.
  `blocked_reason` is null when none is known; otherwise it names the blocker
  (`bank_off`, `driver_identity_mismatch`, `ld_overcurrent`, `ld_overheat`,
  `external_ntc_interlock`, `tec_error`, `tec_selfheat`, or `interlock`, in that
  priority order). An unprepared start programs the firmware-owned interlock
  policy; a lock it resolves is not a public-start blocker. Engineering raw
  state/lock fields remain available. Readiness does not guarantee start success.
  Compact `output_state` is `stopped`, `running`, `stopped_fault` or `unknown`.
  Stopped includes acknowledged zero current, STOP or bank power-off; running
  means established nonzero controller operation, not measured optical output.
  Stopped-fault requires an observed unexpected output stop; failed writes are
  uncertain rather than evidence of running. A ready laser may be stopped by
  a past fault. Acknowledged intentional shutdown and explicit successful recovery
  update output state without changing existing safety-latch clearing rules.
  `i_mA` and `power_mw` retain their setpoint/model-estimate meanings. Active time fields are integer seconds while active and `null`
  when inactive. The persisted lifetime
  total remains available through `laser/settings`. `autooff_s` is optional and
  non-persistent; if supplied, it overrides the default configured through
  `laser/settings` for this start. If another laser-bank operation occupies the
  shared Maiman Modbus bus past the command wait budget, laser commands return
  `{"error":"busy"}`.

(laser-tune)=
### `laser/tune`
- **Payload -> stored tuning request:**
  ```json
  {"name":"<lasername>"}
  ```
  ```json
  {
    "name": "<lasername>",
    "tune_nm": 0.0
  }
  ```
- **Payload:** set the stored tuning request.
  ```json
  {
    "name": "<lasername>",
    "tune_nm": 0.0
  }
  ```
- **Notes:** Sets the wavelength tuning request used when running the laser.
  The request is stored by firmware; it does not immediately write TEC
  temperature or laser current. Future positive `laser` value commands apply
  the stored offset relative to `laser/settings.wave_ref_nm`. Reissuing a
  positive `laser` value after changing value does not require retuning because
  firmware reapplies the stored offset. Setting `laser` value 0 zeros current without issuing STOP
  without clearing the stored tune request. Tuning is best-effort: large shifts
  are clamped by the TEC temperature range and allowed current adjustment.


The existing background check observes both LD and TEC state during established
operation. A confirmed unexpected stop latches the existing warning/fault; no
automatic restart is attempted. Component on-time counters stop on positive
stopped readback, including foreground status and temperature observations.
Unavailable reads do not establish shutdown; requested setpoints and shutdown
deadlines remain retained until the existing explicit shutdown/recovery paths.

Laser errors include channel identity, requested operation, return code and available
failure-stage/controller evidence in the existing `error` text. Controller lock
bits are observations, not proof of a particular transient; TEC limits can block
both too-cold and too-hot operation. No additional reads are made for diagnostics.
The standard busy response, error fields and warning codes remain unchanged.
Python `HispecFibPCBError` text includes topic and available `rc`; `.topic` and
`.response` retain the original structured diagnostics.

For an already emitting laser with valid preparation, level changes write only
required current/TEC setpoints. Startup and invalidated preparation retain the
full identity/profile/control sequence. Failed control operations or confirmed
controller faults invalidate operational health without erasing confirmed configuration. Diagnostic read failures retain setpoints;
sustained communication loss faults operation separately from numerical estimates. See [laser operation and timing](laser_operation.md).

(laser-status)=
### `laser/status`
- **Payload -> detailed engineering status:**
  ```json
  {"name":"<lasername>"}
  ```

Detailed engineering status includes raw state, lock, and TEC-state
registers, measured diode/TEC voltage and current, driver limits, PID, hard
device-id verification, configured expected driver serial, `blocking_lock`,
`blocked_reason`, and interlock flags. This command may be slower than
the basic `laser` query because it reads many Modbus registers. A serial mismatch is
reported as `serial_ok:false` and `blocked_reason:"driver_identity_mismatch"`.
The set diode current is `i_mA`; measured TEC current is `tec_ma`, both in mA.
`max_current_ma`, `current_cal_pct`, `tec_curr_lim_a`, `pid` and
`ntc_coeff_pct_per_c` are controller readbacks whenever powered, including before
first start; all are `null` while off. `laser/settings` always reports PCB
configuration. The NTC readback translates controller B to resistance %/°C at
25°C. Coefficient I/O uses internal LD NTC register 0x007F. External NTC 0x008A
is unused.
`tec_temp_set_c` is the live target, not the configured startup default.
With `powered:false`, raw zeros/false are placeholders. Failed reads return errors.


(laser-settings)=
### `laser/settings`
- **Payload -> laser settings:**
  ```json
  {"name":"<lasername>"}
  ```
  ```json
  {
    "name": "<lasername>",
    "settings": {
      "model": "<string>",
      "ld_serial": "<string>",
      "expected_serial": 0,
      "nominal_current_ma": 0.0,
      "max_current_ma": 0.0,
      "current_cal_pct": 0.0,
      "fractional_noise": 0.001,
      "constant_noise_mw": 0.13505925,
      "threshold_current_ma": 0.0,
      "min_autolevel_current_ma": 0.0,
      "efficiency_mw_per_ma": 0.0,
      "efficiency_mw_per_ma2": 0.0,
      "efficiency_ref_ma": 52.0,
      "ref_mw": 10.0, "ref_ma": 52.0, "ref_temp_c": 25.0,
      "wave_ref_nm": 0.0,
      "wave_ref_ma": 0.0,
      "wave_ref_temp_c": 0.0,
      "operating_temp_range_c": [0.0, 0.0],
      "default_operating_temp_c": 0.0,
      "thermistor_kohm": 0.0,
      "isolation_db": 0.0,
      "tec_curr_lim_a": 0.0,
      "tec_pid": {
        "p": 0,
        "i": 0,
        "d": 0
      },
      "disable_tec_at_autooff": true,
      "ntc_coeff_pct_per_c": -4.4,
      "dlambda_dT_nm_per_k": 0.0,
      "dlambda_dA_nm_per_ma": 0.0,
      "autooff_s": 0,
      "tune_nm": 0.0,
      "emit_total_s": 0
    }
  }
  ```
- **Payload:** replace the complete power curve (example for a linear model).
  ```json
  {
    "name": "1270j",
    "settings": {
      "threshold_current_ma": 13.0,
      "efficiency_mw_per_ma": 0.26,
      "efficiency_mw_per_ma2": 0.0,
      "efficiency_ref_ma": 52.0
    },
    "persist": false
  }
  ```
  Sending any curve coefficient requires all four. Other writable settings remain
  independent partial updates, for example:
  ```json
  {"name":"1270j","settings":{"min_autolevel_current_ma":15.0},"persist":false}
  ```
  A complete curve can be installed without a calibration run or retained dataset.

- **Notes:**
  - `ref_mw`, `ref_ma`, `ref_temp_c` are the independent vendor power anchor.
    Power must be positive/finite, reference current must be positive, <=1000 mA
    and on the 0.1 mA grid, and temperature must be 15–40 C. Calibration additionally
    requires the reference inside the configured current/temperature envelope.
    Reference edits stop emission and optionally persist the triplet.
  - The installed curve is `P=x*(efficiency_mw_per_ma + efficiency_mw_per_ma2*(current_ma-efficiency_ref_ma))`,
    with `x=max(0,current_ma-threshold_current_ma)`. Calibration preserves the
    uncorrected linear fit's slope as efficiency and derives its curve reference
    to reproduce the corrected fit. That finite reference coordinate is independent
    of `ref_ma`; it need not lie on the current grid or below the hardware maximum.
    Nonzero correction requires it above threshold. Validate a positive derivative
    from threshold through maximum; compiled defaults use zero correction.
    All four coefficients must accompany a manual curve replacement. Their changes
    stop emission. Datasheet edits do not change any of these coefficients.
  - `min_autolevel_current_ma` is the autolevel-only floor. A finite nonnegative
    request is raised to at least `threshold_current_ma + 0.1 mA`, then rounded
    upward to the 0.1 mA register grid. Raising threshold also raises this floor
    silently. Updates with no representable interval through nominal current
    are rejected. Manual output and calibration keep their existing minimum rules.
  - `fractional_noise` and `constant_noise_mw` are finite, nonnegative
    app-owned optical-power uncertainty settings. The estimator reads its
    existing per-laser cache and computes `hypot(power_mw * fractional_noise,
    constant_noise_mw)` before converting power and uncertainty to photon flux.
    These fields do not program Maiman or stop emission by themselves; the
    settings command still stops throughput monitoring.
  - Defaults are 0.1% fractional plus a constant 0.31% of each diode's **compiled**
    maximum modeled power. Constant defaults in mW: 1028 = 0.13505925;
    1270 = 0.041912; 1430yj = 0.038688; 1430hk = 0.039432;
    1510 = 0.040176; 2330 = 0.00913911.
    The baseline does not change when current output or user property settings
    change. `persist:true` saves both fields with that laser's policy.
  - `wave_ref_nm`, `wave_ref_ma`, `wave_ref_temp_c` describe an independent
    wavelength measurement. Replace all three together. Wavelength is
    `wave_ref_nm + (I-wave_ref_ma)*dlambda_dA_nm_per_ma + (T-wave_ref_temp_c)*dlambda_dT_nm_per_k`.
    The reference current may be 0–1000 mA and temperature 15–40 C, independent
    of the operating envelope. In particular the 2330 report's 40 C reference
    is retained while operation is limited to 17–38 C. These are coordinates,
    not commanded setpoints. `laser/tune` is an offset from `wave_ref_nm`.
  - Positive `laser.value` requests that fraction of `P(nominal_current_ma)`.
    The calibrated inverse chooses current, rounded to the 0.1 mA grid; zero instead
    requests zero current. With zero quadratic coefficient this reduces to
    `threshold_current_ma + value*(nominal_current_ma-threshold_current_ma)`.
    Tuning begins from that emission request and may shift power within its budget.
    Readback reports the modeled emission fraction at the actual setpoint.
    Nominal current sets the level-1/autolevel ceiling; `max_current_ma` is the
    independent do-not-exceed limit. Neither defines the wavelength reference.
    Changing `current_cal_pct` affects the driver independently; it does not silently
    rescale the optical coefficients. Existing captures retain their recorded model.
  - `default_operating_temp_c` is the persisted TEC startup/baseline setpoint
    applied during driver preparation and TEC start. It is not the live tuned
    TEC setpoint. Tuning may write a different live TEC setpoint when a positive
    `laser` value command applies the stored `tune_nm`, but it does not overwrite
    `default_operating_temp_c`.
  - Changing `default_operating_temp_c` changes the initial TEC setpoint used
    by tuning. The wavelength reference and stored `tune_nm` remain fixed; the
    next positive output request solves from the new initial temperature.
  - `operating_temp_range_c` programs the writable TEC minimum/maximum during
    preparation. Firmware expands the old range if needed, moves the default
    target, then narrows it. A range-only change stops emission and invalidates
    preparation; it is programmed at the next start without powering an idle
    bank for the edit. Combining it with a driver-backed change programs both
    immediately. Absolute device limits are never written.
  - Settings `autooff_s` is the default for later manual `laser` commands;
    command `autooff_s` overrides one operation. Throughput uses `off_in_s` for
    its own duration and disables the separate laser auto-off while controlling
    it. Changing the stored default does not rearm an active deadline.
  - Driver identity is checked at first contact after bank power-up. Profile
    preparation synchronizes PCB-owned driver settings at the first explicit
    start after bank power-up; merely reading status does not program them.
  - `persist` is optional and defaults to false. Without `persist:true`,
    accepted changes apply to runtime and driver-backed state but are not saved
    in app NVS for the next controller boot.
  - Preparation writes the PCB-owned profile; it does not compare all controller
    settings or issue a general settings-mismatch warning.
  - `expected_serial` is the operator-confirmed Maiman driver serial for this
    laser/diode association. It must be nonzero. A serial mismatch blocks
    driver-backed writes until the operator confirms the physical association
    and updates `expected_serial`; firmware does not learn or persist changed
    serials by itself. A device-ID mismatch remains an identity fault.
  - `ld_serial` is the read-only manufacturer diode serial from the compiled
    property table. It is independent of the numeric Maiman driver serial and
    is not stored in app NVS. The two 1430 diodes have distinct properties:
    `1430yj` uses `8897ACR`; `1430hk` uses `8895ACR`.
  - If the laser bank is off, firmware powers it, applies driver-backed settings,
    verifies them as practical, and then restores the previous bank power state.
    Driver-backed settings include `max_current_ma`, `current_cal_pct`,
    `default_operating_temp_c`, `tec_curr_lim_a`, `tec_pid`, and
    `ntc_coeff_pct_per_c`. If
    `laser/bankpower` is `override_off`, driver-backed settings changes return
    an error.
  - Send only independent settings being changed. A curve replacement is the
    exception: send all four curve coefficients together. A wavelength-reference
    replacement likewise requires the complete `wave_ref_*` triplet.
  - The overcurrent threshold is the maximum current the driver will allow the laser to run at and requires physically 
    adjusting a potentiometer on the driver. It has a (weak) temperature dependence and is not a fixed value.
  - Actual changes to threshold, nominal current, efficiency, quadratic correction, wavelength,
    wavelength coefficients, autolevel minimum, operating range, or driver-backed
    settings stop emission before acceptance. The TEC follows
    `disable_tec_at_autooff`. Failed shutdown rejects the update and preserves
    any measurement-owned shutdown obligation for retry. No-op updates and
    changes only to noise, default auto-off, or TEC auto-off policy do not stop
    emission. Successful settings updates still relinquish the measurement;
    that behavior for non-stopping edits and tuning remains a deferred ownership
    audit in `human_review_required.md`.
  - Failures before driver programming completes leave settings unchanged
    in app settings and report an error; partially written driver registers are
    reprogrammed at the next preparation. If programming succeeds but
    restoring the previous bank power state fails, the successfully applied
    settings are still retained and persisted when requested; the command still
    reports the restore error because bank power needs operator attention.
  - Updates require a nonempty `settings` object containing writable fields.
    `persist` alone is invalid. `tec_pid` accepts only `p`, `i`, and `d`.
    `current_cal_pct` is the canonical calibration field.
  - Read-only and unknown settings return an error before changing laser or
    throughput state. Read-only fields include `model`, `ld_serial`, `thermistor_kohm`,
    `isolation_db`, and `emit_total_s`. Use `laser/tune`
    to set `tune_nm`; `name` belongs outside the settings object as the selector.
  - `ntc_coeff_pct_per_c` is writable resistance change in %/°C at 25°C.
    It must be finite and negative, mapping to a rounded internal Maiman B in
    1–65535 K. The constant-B conversion is `B = round(-coefficient * 298.15² / 100)`.
    Accepted settings are normalized to `-100 * B / 298.15²`; query to see the
    representable value. Compiled FLC defaults are −4.4 %/°C (rounded B=3911 K);
    all four Aero channels use vendor B=3900 K, equivalent to −4.387276 %/°C.
    These are vendor-based defaults, not unit-specific thermistor measurements.
    Programming and engineering readback both use 0x007F. No raw-B host field
    or old-key alias is exposed.
    Changes follow ordinary stop-before-profile-update and optional app NVS
    persistence. No new EEPROM SAVE or MCU-startup programming is added.
  - Non-Driver settings:
    - `min_autolevel_current_ma`
    - `nominal_current_ma`
    - `autooff_s`
    - `dlambda_dT_nm_per_k`
    - `dlambda_dA_nm_per_ma`
    - `disable_tec_at_autooff`
    - `wave_ref_nm`, `wave_ref_ma`, `wave_ref_temp_c`
    - `threshold_current_ma`
    - `efficiency_mw_per_ma`
    - `efficiency_mw_per_ma2`
    - `efficiency_ref_ma`
  - Settings that are informational only (included for datasheet posterity):
    - `isolation_db`
  - Ranges:
    - Operating temp range: limited to [15,40] strong advice to limit to 17,38
    - `current_cal_pct`: 95 - 105 in steps of .01
    - TEC max current must be greater than zero and no higher than the compiled-in diode datasheet maximum for that laser.


(laser-calibrate)=
### `laser/calibrate`

- **No payload -> current calibration status.** Acquisition runs asynchronously;
  query again or watch `dt/<device>/laser/calibrate` for progress.
- **Prerequisites:** review the FVOA calibration, physically connect the selected
  outbound output to the selected SM/MM PD return, power that **specific PD**, and
  acquire a valid dark with light blocked. Neither channel may have a dark pending.
  `pd/settings/<channel>.powered` reports actual power without enabling it.
  The laser bank must permit operation; the reference current and temperature
  must be inside configured limits. Firmware sets `ref_temp_c` and qualifies
  temperature after start. A stored dark alone does not mean the PD is on.
- **Payload -> start and return status:**
  ```json
  {
    "laser": "1270j",
    "output": "yj_ao",
    "fiber": "S",
    "dwell_ms": 1000,
    "settle_ms": 2000,
    "apply": true,
    "max_fit_residual_pct": 5.0,
    "pd_target_mv": 1500.0,
    "determine_static_loss": false,
    "update_noise": false,
    "other_output": false,
    "persist": false
  }
  ```
  `laser` and same-band `output` are required. Other values shown are defaults.
  `fiber` is `S` or `M`; dwell is 100–2000 ms and settling is 100–10000 ms.
  Each current receives high/low PD conditions (plus a middle level in the upper
  block), with one early and two late windows per condition. Only late windows fit. Residual tolerance is strictly
  between 0 and 100 percent; PD target is strictly between 0 and 1850 mV.
  The target and preferred peak headroom guide ranging; usable probes outside
  those preferences are retained as fallback when ranging exhausts its budget.
  Loss estimation requires `determine_static_loss:true` plus explicit nonnegative
  `loopback_loss_db` for **external** cable/attenuators, excluding the configured
  PD return. `other_output:true` requires loss estimation.
- **Payload -> stop, continue after physical recabling, or restore defaults:**
  ```json
  {"stop":true}
  ```
  ```json
  {"continue":true,"loopback_loss_db":10.0}
  ```
  ```json
  {"restore_defaults":true,"laser":"1270j","output":"yj_ao","persist":false}
  ```
  STOP retains raw data. Continue is valid only at `awaiting_output`; the optional
  loss replaces the external loss for the second output. Restore stops the owned
  operations and restores compiled threshold, efficiency, curve reference, zero quadratic
  correction, maximum and nominal, automatic floor, reference and noise; it removes the named
  outbound override. Identity, current calibration, independent `wave_ref_*`, other TEC
  settings, return loss and the other output's override are preserved.
- **Response:** status separates acquisition (`state`, `acquisition_complete`,
  `error`) from `fit`, `noise` and `routes`. Each result reports acceptance/validity,
  application and persistence separately. Errors are strings or `null`, for example
  `"pd off"`, `"full-range residual exceeds uncertainty allowance"`,
  `"curve disagrees with vendor characterization"`, or
  `"unexpected error (EIO)"`. The last form uses the actual errno name only when
  no more specific explanation is available. Numerical rejection retains data
  and does not turn completed acquisition into acquisition failure.
  If acquisition fails before fitting, `error` explains the failure while
  `fit.valid:false`, `fit.n:0`, and `fit.error:null` mean the fit never ran.
  Restore reports `laser_applied`, `route_applied`, `laser_persisted`,
  `route_persisted` and independent text/null errors. See the
  [status and data layout](laser_calibration.md#status-and-retained-data) for fields.
- **Notes:**
  - `apply:true` automatically installs an accepted threshold, linear-fit
    efficiency, quadratic coefficient, curve reference and automatic floor. Maximum
    and nominal current, wavelength references and the datasheet power triplet
    are preserved. This is the
    field default and requires no notebook. Loss and noise adoption are opt-in.
    `persist:true` also requests NVS; `apply:false,persist:true` is rejected.
  - Commissioning uses `apply:false,persist:false`, saves raw data, reviews the
    normalized curve/residual plot, then installs the complete coefficients through
    `laser/settings` and selected losses through `mems/route/loss`. No retained run
    is needed for manual installation; the saved model supplies its own reference.
    A failed core fit cannot apply a route or noise estimate. A persistence
    failure may leave RAM updated; inspect each result's errors/flags.
  - Start stops throughput/laser outputs and owns the loopback through acquisition
    and recabling. Other mutations are busy; STOP/zero and bank-off remain available.
    With automatic application, accepted first-stage changes survive a later
    second-stage failure. The source is off during the recabling pause.
  - Fitting uses calibrated FVOA transmission and stored RMS as common gain
    uncertainty for each exact DAC pair. Same-current ratios and revisits test
    consistency; they do not set the normalization. Every fitted current must
    have two usable PD levels. Search only the lowest usable floor, with a line
    preferred when both orders pass; the fit must include the power reference
    and reach the configured maximum. PD residuals allow the requested fraction
    plus three measured standard errors. Immutable unit-specific vendor traces
    allow that fraction of vendor reference power plus three fitted-power SEs.
    Cross-attenuation/gain-shift diagnostics have no separate veto. Check repeats
    and darks once after selecting the curve; measurement inconsistency rejects
    the whole calibration without another floor search. See
    [acceptance](laser_calibration.md#curve-meaning-and-acceptance) for equations.
  - MQTT binary queries `laser/calibrate/records` and
    `laser/calibrate/records/<chunk>` retrieve metadata and raw chunks. Starting
    either optical calibration replaces the shared dataset. Save it first.
  - Python: `laser_calibrate(name, ..., apply=True, max_fit_residual_pct=5.0,
    settle_ms=2000, pd_target_mv=1500.0, determine_static_loss=False, loopback_loss_db=None,
    update_noise=False, other_output=False, persist=False)`;
    `laser_calibrate_continue(loopback_loss_db=None)`, `laser_calibrate_stop()`,
    `laser_calibration_data()`, and
    `laser_calibrate_restore_defaults(name, *, output, persist=False)`.

(laserbank-power)=
### `laser/bankpower`
- **No payload -> laser-bank power state:**
  ```json
  {
    "mode": "auto|override_on|override_off",
    "powered": false
  }
  ```
- **Payload or topic suffix -> laser-bank power state after update:**
  ```json
  {"mode":"auto|override_on|override_off"}
  ```
  Suffix requests use
  `cmd/<device>/req/laser/bankpower/auto`,
  `cmd/<device>/req/laser/bankpower/override_on`, or
  `cmd/<device>/req/laser/bankpower/override_off`.

- **Notes:** `auto` is the ordinary confirmed-boot default. An unconfirmed OTA trial
  selects `override_off` before workers start, and confirmation leaves it off.
  During an OTA window, an in-flight close, a pending candidate, or a trial,
  all mode writes (including `override_off`) are rejected before the handler, in
  both payload and suffix forms. Queries remain allowed.
  In `auto`, power to the laser bank is handled by the bank
  heater and commands interacting with laser drivers. `override_on` forces bank power on. `override_off` stops all laser
  emission, writes driver currents to 0 as practical, powers the bank off, and rejects commands that need a live driver
  while the override is active. If the pre-off driver-current shutdown reports a Modbus failure, the command returns an
  error response that still includes the current `mode` and firmware-requested `powered` state. If another laser-bank
  operation occupies the shared Maiman Modbus bus past the command wait budget,
  mode changes return `{"error":"busy"}`.

(laserbank-heater)=
### `laser/bankheater`
- **No payload -> laser-bank heater state:**
  ```json
  {
    "mode": "auto|override_on|override_off",
    "auto_state": "waiting_for_temps|warming_disabled_tec|disabled_tec_warm|tecs_running|holding|override_on|override_off",
    "heater_on": false,
    "bank_power": true,
    "ambient_c": null,
    "idle_tec_temps": 0,
    "idle_tec_avg_c": null,
    "last_error": 0,
    "poll_age_s": 0
  }
  ```
- **Payload or topic suffix -> laser-bank heater state after update:**
  ```json
  {"mode":"auto|override_on|override_off"}
  ```
  Suffix requests use
  `cmd/<device>/req/laser/bankheater/auto`,
  `cmd/<device>/req/laser/bankheater/override_on`, or
  `cmd/<device>/req/laser/bankheater/override_off`.

- **Notes:** `auto` is the default at boot. In `auto`, laser-bank
  temperature-control work powers the bank so the Maiman temperature monitors
  can initialize, polls TEC temperatures at a fixed interval, and drives the
  laser-bank heater through housekeeping relay-power helpers.
  `auto_state` summarizes the internal policy state without exposing the
  control-loop booleans: `waiting_for_temps` means no fresh driver
  temperatures are available; `warming_disabled_tec` means at least one idle TEC
  probe is below 19 C; `disabled_tec_warm` means every fresh idle TEC probe
  is above 21 C, allowing heater turnoff; `tecs_running` means all
  driver TECs are enabled; `holding` means no heater state change was requested
  in the latest loop. `idle_tec_temps` counts fresh driver temperature readings
  whose TEC is not started, and `idle_tec_avg_c` averages only those readings;
  actively controlled TEC temperatures remain laser telemetry and are not used
  for this aggregate. `ambient_c` and `idle_tec_avg_c` are `null` when
  unavailable. `poll_age_s` is an integer age in seconds or `null` before the
  first poll. The heater turns on below 19 C and off only when all fresh idle
  probes exceed 21 C, retaining its previous state between these thresholds.
  This band is independent of ambient temperature. Stale probes do not contribute
  to either threshold decision; partial coverage cannot establish that every diode
  is warm. If all laser temperatures are stale, auto mode turns the
  heater off when ambient is invalid or at least 15 C. When valid ambient is
  below 15 C, auto mode powers the bank so driver
  temperature monitors can initialize and leaves heater state unchanged until
  valid laser temperature data is available. If all TECs remain enabled for at
  least one control interval, the heater is turned off.
  `override_on` and `override_off` force the heater state and suspend the
  automatic warmup policy. The forced relay state is enforced before polling.
  Both overrides continue the existing opportunistic temperature polling and
  cache summaries when the bank is powered, without powering the bank themselves.
  Bank-off readings age out through the normal 20-second freshness rule.
  While a heater override is active, firmware emits
  `laserbank_heater_override` on `dt/<device>/warning` every 20 minutes.
  If the off-board DS2408 relay expander is offline, set requests return an I/O
  error because the heater relay cannot be driven.

(laserbank-clearfaults)=
### `laser/clearfaults`
- **No payload -> clear result:**
  ```json
  {"off_ms":250}
  ```

This command performs an off-on cycle iff the bank is powered and at least one of the drivers reports an overcurrent
fault. It is a convenience command that has no effect when the bank is not powered or is powered and without fault. 
The return indicates if the bank was power cycled. `off_ms` is the time that the bank was turned off (0 if bank was 
off or no faults).
If another laser-bank operation occupies the shared Maiman Modbus bus past the
command wait budget, this command returns `{"error":"busy"}`.


(atten)=
(atten-coeff)=
### `atten`
- **Top-level handlers:** `atten_setting_get()`, `atten_setting_set()`
- **Topics:**
  - `cmd/<device>/req/atten/<laser>`
  - `cmd/<device>/req/atten/<laser>/coeff`
  - Responses use the same key under `cmd/<device>/resp/...`.
- **No payload to `atten/<laser>` -> attenuator setting:**
  ```json
  {
    "db": 12.5,
    "linear": 0.0562,
    "v1_mv": 1234.0,
    "v2_mv": 0.0,
    "db1": 12.5,
    "db2": 0.0,
    "linear1": 0.0562,
    "linear2": 1.0
  }
  ```
- **Payload to `atten/<laser>`:** set total attenuation or one or both physical
  attenuators. `value` is total linear transmission and `value_db` is total
  attenuation in dB. The total fields are mutually exclusive with the physical
  `value1*` and `value2*` fields.
  ```json
  {"value":0.25}
  {"value_db":12.5}
  {"value1":0.25,"value2_db":6.0}
  {"value1_mv":1234.0,"value2":1.0}
  ```
  Each physical attenuator may use a different unit, but a single physical
  attenuator may only use one unit per request. For example, `value1` and
  `value2_db` is valid, while `value1` and `value1_db` in the same request is
  rejected. Millivolt inputs are clamped to the firmware drive span, quantized
  through the board-configured DAC transfer, and responses report the applied
  DAC-side millivolts read back from the DAC.
- **No payload to `coeff` -> model coefficients:**
  ```json
  {
    "dac1": {
      "fvoa_50pct_mv": 2529.45,
      "slope_inv_fvoa_mv": 0.00158137,
      "max_atten_db": 55.0,
      "gain": 1.533,
      "rms_db": 2.0,
      "correction_coeff": [0.0, 0.0, 0.0, 0.0, 0.0, 0.0]
    },
    "dac2": {
      "fvoa_50pct_mv": 2529.45,
      "slope_inv_fvoa_mv": 0.00158137,
      "max_atten_db": 55.0,
      "gain": 1.533,
      "rms_db": 2.0,
      "correction_coeff": [0.0, 0.0, 0.0, 0.0, 0.0, 0.0]
    }
  }
  ```
- **Payload to `coeff`:** set the model coefficients for the two physical
  attenuators that make up the logical attenuator. Both `dac1` and `dac2`
  objects are required.
  ```json
  {
    "dac1": {
      "fvoa_50pct_mv": 3144.95,
      "slope_inv_fvoa_mv": 0.00303104,
      "max_atten_db": 48.36,
      "max_calibrated_db": 45.0,
      "gain": 1.533,
      "rms_db": 2.0,
      "correction_coeff": [0.12, -0.03, 0.01, 0.0, 0.0, 0.0]
    },
    "dac2": {
      "fvoa_50pct_mv": 3456.12,
      "slope_inv_fvoa_mv": 0.00247498,
      "max_atten_db": 61.95,
      "max_calibrated_db": 55.0,
      "gain": 1.533,
      "rms_db": 2.0,
      "correction_coeff": [0.0, 0.0, 0.0, 0.0, 0.0, 0.0]
    },
    "persist": true
  }
  ```
- **Serial form for `coeff`:** send the JSON object after the key. The default
  serial shorthand only builds a `value` payload, so it is not useful for
  coefficient objects. The MQTT payload is the same JSON object without the
  serial key prefix.
  ```text
  atten/1028y/coeff {"dac1":{"fvoa_50pct_mv":3144.95,"slope_inv_fvoa_mv":0.00303104,"max_atten_db":48.36,"max_calibrated_db":45.0,"gain":1.533,"rms_db":2.0,"correction_coeff":[0.12,-0.03,0.01,0.0,0.0,0.0]},"dac2":{"fvoa_50pct_mv":3456.12,"slope_inv_fvoa_mv":0.00247498,"max_atten_db":61.95,"max_calibrated_db":55.0,"gain":1.533,"rms_db":2.0,"correction_coeff":[0.0,0.0,0.0,0.0,0.0,0.0]},"persist":true}
  ```

- **Notes:**
  - On TIB, `<laser>` is one of `1028y`, `1270j`, `1430yj`, `1430hk`, `1510h`,
    or `2330k`. On calibration boards only, the LFC attenuator is addressed as
    `atten/lfc` and `atten/lfc/coeff`.
  - Laser aliases accepted by the laser profile table, such as `1028`, also
    resolve to the matching TIB attenuator channel, but canonical command docs
    use the full logical laser names.
  - Each logical attenuator is a pair of physical FVOAs. Total set commands use
    directional balancing: increases first use the less-attenuated device,
    decreases first use the more-attenuated device, then share changes once
    balanced. Both devices respect their modeled limits; an unchanged total
    retains the existing physical allocation.
  - `value` is a unitless linear transmission fraction in `(0, 1]`.
    The same range applies to `value1` and `value2`; dB values must be nonnegative.
    Both physical values are validated before either device is written. Hardware
    failure during a pair update can still leave an earlier write applied.
  - Compact value requests reject coefficient fields and `persist`;
    coefficient requests accept only `dac1`, `dac2`, and optional `persist`.
  - `v1_mv` and `v2_mv` are DAC-output setpoints in the firmware 0-3300 mV
    drive span. The firmware converts them to DAC codes using the
    board-configured DAC reference transfer, then responses report the applied
    DAC-side millivolts after code quantization and output-rail clipping.
  - Coefficients are loaded from persistent app NVS during
    `setup_attenuators()`. They define the erf coordinate
    `delta = slope_inv_fvoa_mv * (gain * dac_mv - fvoa_50pct_mv)` and
    ideal transmission `ideal_tx = (erf(4) - erf(delta)) / (2 * erf(4))`.
    Runtime dB/linear set commands normalize against the modeled open
    transmission at DAC 0, then apply the physical FVOA leakage floor
    `floor_tx = 10^(-max_atten_db / 10)`. `correction_coeff` is an optional
    six-term Chebyshev residual correction in model dB space. Query responses
    always include it. In set payloads, omitting `correction_coeff` leaves the
    currently active correction unchanged; include `[0.0, 0.0, 0.0, 0.0, 0.0, 0.0]` to
    clear it intentionally.
  - `max_calibrated_db` is required in each replacement coefficient object and
    returned by coefficient queries. It is the corrected-curve operating endpoint,
    separate from the leakage-floor parameter `max_atten_db`. Automatic fitting
    uses the usable measured prefix through the first point above
    `ATTENUATOR_CALIBRATED_MAX_DB` (55 dB). The operating limit is the minimum
    of 55 dB, the last supporting measurement, its model prediction and the floor.
    Above the endpoint, the endpoint residual fades linearly
    in base-model dB to zero at the existing floor; the polynomial is not extrapolated.
  - Manual dB/linear and voltage commands retain full-range access. Values beyond
    the calibrated endpoint are rough estimates. Autolevel uses individual
    calibrated limits and switches to laser adjustment when they are exhausted.
  - Schema 14 clears the app NVS store when the schema marker differs; see
    [settings reset policy](settings.md#schema-reset).

  - `persist` is optional and defaults to false. A non-persistent coefficient
    update changes runtime behavior until reboot or a later coefficient command.
  - Each physical model includes finite, nonnegative `rms_db`, the RMS residual
    in attenuation dB for fitting-support measurements at or below
    `max_calibrated_db`, including negative reference-relative measurements. It defaults
    to `ATTENUATOR_DEFAULT_RMS_DB` (2.0 dB).
    A manual model replacement omitting `rms_db` uses that default, rather than
    inheriting confidence from the previous fit. An explicit zero is allowed.
  - Accepted autocalibration installs the final model's unweighted residual RMS
    with its coefficients and saves both when persistence is requested. Rejected
    fits replace neither. No extra sweep or offline analysis is required.
  - The pair transmission estimate uses
    `sigma_db = hypot(hypot(dac1.rms_db, dac2.rms_db), hypot(electrical1, electrical2))`
    and `sigma_T = T * ln(10)/10 * sigma_db`. For each device,
    `electrical = abs(d_db_d_voltage_mv) * ATTENUATOR_FVOA_NOISE_RMS_MV / gain`.
    This source constant defaults to 10 mV RMS after the amplifier, applied to
    every FVOA; zero disables the electrical term. The full calibrated curve's
    local slope predicts variation along the curve; stored `rms_db` estimates
    its accuracy. The contributions and the two devices are assumed independent.
    Calibration errors remain correlated across samples, and the combined
    uncertainty is not temporal RMS. No bandwidth or averaging correction is
    inferred from electrical RMS. Throughput includes this and laser uncertainty
    in `tp_err`; `tp_pd_err` remains PD-only. The error propagation does not alter nominal transmission.
  - There is no separate `attensettings` command; calibration coefficients live
    on `atten/<laser>/coeff`.

(atten-calibrate)=
### `atten/calibrate`
The implementation flow, bridge-normalization sequence, and retained-record
ownership are documented in `attenuator_calibration.md`.
Status queries return a coherent snapshot without waiting for numerical fitting.
The state remains `running` during fitting; each completed physical fit becomes
available without waiting for the other fit.

- **No payload -> compact calibration state:**
  ```json
  {
    "state": "inactive",
    "mode": "none",
    "physical": "dac1",
    "fit": "none",
    "n": 128,
    "t_ms": 300,
    "complete_pct": 0,
    "point": "1/128",
    "mv": 0.0,
    "other_mv": 3300.0,
    "error": 0,
    "dac1": {
      "valid": true,
      "accepted": true,
      "points": 12,
      "fvoa_50pct_mv": 3144.95,
      "slope_inv_fvoa_mv": 0.00303104,
      "max_atten_db": 48.36,
      "max_calibrated_db": 45.0,
      "max_atten_sigma_db": 0.29,
      "corr": 0.999,
      "rms_db": 0.1,
      "max_abs_db": 0.2,
      "correction_coeff": [0.12, -0.03, 0.01, 0.0, 0.0, 0.0]
    },
    "dac2": {"valid": false}
  }
  ```
- **Record data query:** retained calibration acquisition records are available
  as metadata plus fixed binary MQTT record chunks, independent of best-effort
  telemetry:
  ```
  atten/calibrate/records/<dac1|dac2>
  atten/calibrate/records/<dac1|dac2>/<chunk>
  ```
  The metadata query has no chunk suffix. The response is not JSON and is
  MQTT-only because the serial response printer is string-oriented. The
  metadata response starts with `<4s 15B>`: magic `HAC4`, version, kind
  (`0=metadata`), physical index, state, mode, fit-valid, fit-accepted,
  overflow, record-size, records-per-chunk, record-count, record-chunk-count,
  reference-valid, reference-record, and bridge-count. It is followed by
  `bridge-count` little-endian `<2B>` bridge entries containing
  `before_record` and `after_record` indices. The reference and bridge entries
  name roles for retained raw records; they are not separate copied records.

  Each numbered chunk response contains only raw records and no header. Chunk
  `0` starts at record `0`; subsequent chunks use the fixed
  `records-per-chunk` value reported by metadata. Version 3 uses record size
  27 bytes, and each raw record has little-endian layout `<6f 3B>`:
  `sweep_mv`, `other_mv`, `laser_pct`, `signal_mv`, `signal_err_mv`, `max_mv`,
  `event`, `classification`, and `segment`. Event codes are `0=point`,
  `1=initial_probe`, and `2=bridge_probe`; classification codes are `0=ok`,
  `1=saturated`, `2=below_snr`, and `3=adc_error`. State codes are
  `0=inactive`, `1=running`, `2=complete`, `3=error`; mode codes are
  `0=none`, `1=tib_auto`. The Python tool decodes records directly into a
  NumPy raw record array with those firmware names. It also adds convenience
  `fvoa_mv` and `other_fvoa_mv` columns derived from DAC millivolts and the
  default FVOA drive gain; those columns are host-side coordinates, not
  additional firmware measurements. Bridge scale, scaled signal, transmission,
  dB attenuation, fit inclusion, and residuals are derived from the raw records
  and the accepted bridge boundaries after acquisition.
- **Payload:** start automatic TIB calibration for the logical pair belonging to
  a laser.
  ```json
  {
    "laser": "1430yj",
    "output": "yj_ao",
    "fiber": "M",
    "dwell_ms": 300,
    "persist": true
  }
  ```
- **Payload:** stop/cancel any calibration.
  ```json
  {"stop": true}
  ```
  `stop:true` takes priority over accompanying start fields, whose values are
  ignored even if invalid. Unknown top-level fields are still rejected.

- **Telemetry topic:** `dt/<device>/atten`
- **Telemetry payload:** attenuator calibration emits one best-effort JSON
  message per significant state transition, DAC setpoint, retained
  measurement record, bridge measurement, and fit result. Calibration continues
  if telemetry is dropped; authoritative acquisition data is queried through
  `atten/calibrate/records`.
  ```json
  {
    "event": "point",
    "state": "running",
    "mode": "tib_auto",
    "physical": "dac1",
    "attenuator": 2,
    "complete_pct": 10,
    "record_count": 4,
    "segment": 0,
    "sweep_mv": 1983.0,
    "other_mv": 1764.0,
    "laser_pct": 100.0,
    "i": 4,
    "classification": "ok",
    "signal_mv": 124.0,
    "signal_err_mv": 0.1,
    "max_mv": 124.6
  }
  ```
  Other `event` values include `start`, `physical_start`, `initial_probe_set`,
  `point_set`, `bridge_probe_set`, `initial_probe`, `point`, `bridge_probe`,
  `fit`, `complete`, `stop`, and `error`. Fit input and residual diagnostics
  are retained in calibration state and queried through
  `atten/calibrate/records` instead of being emitted as an end-of-run telemetry
  burst.

- **Notes:**
  - State names are `inactive`, `running`, `complete`, and `error`.
    A canceled calibration returns to `inactive`. If automatic acquisition
    completes but the fit is not accepted, state is `complete`, fit is `failed`,
    coefficients are not persisted, and retained data remains available for lab
    analysis. Fit state is `ok` when both physical attenuators are accepted,
    `failed` after an unsuccessful fit, and `none` before a fit exists.
    Accepted calibration coefficients are applied to runtime attenuator control;
    they are persisted to NVS only when `persist` is requested.
  - One static dataset is retained through stop, errors, rejected fits, and host
    cleanup. Raw records, references, bridges, run identity, and completed fit
    results are cleared only by an accepted new start or reboot. A status query
    does not start a run. Rejected requests preserve the data; an accepted start
    clears it before hardware setup, even if that setup subsequently fails.
  - Acquisition completion stops the calibration-owned laser before fitting.
    A failed stop reports an error and preserves both data and shutdown ownership
    for an explicit retry. Stop also shuts down an active acquisition's source;
    shutdown failure is returned as a command error.
  - Fitting runs at the lowest application priority, without holding the
    calibration mutex. Stop or a replacement start cancels at a numerical loop
    boundary and waits for fitting to release the data. Cancellation before
    installation prevents application/persistence; it does not undo an already
    installed calibration. Status and records remain readable during fitting.
  - Python downloads the record prefix described by each physical device's
    initial metadata, allowing acquisition to append concurrently. Starting a
    replacement run invalidates an in-progress multi-request download.
  - TIB automatic calibration uses `laser`, `output`, and `fiber`; the laser
    selects the logical attenuator pair and outbound route input, while `fiber`
    selects the photodiode route as in `measure_throughput`.
  - Start requires the selected photodiode to be powered, with valid sampler
    data and a configured dark. `dwell_ms` is rounded to whole 50 ms attempts;
    the 100 ms FVOA settling wait is additional. Calibration does not measure a
    private dark or turn on the photodiode.
  - Records marked saturated, below-SNR or ADC-error remain available as
    diagnostics. Classification `ok` alone does not guarantee inclusion in the
    fitted prefix. `points` includes the above-limit supporting anchor;
    residual RMS/max and correlation score support within `max_calibrated_db`.
  - Both physical fits must pass before either is installed. A rejected fit
    leaves the prior coefficients/RMS intact. `max_fit_residual_db` is a per-device
    maximum absolute residual acceptance threshold. Fit details include the
    calibrated limit and correction coefficients; per-device telemetry also
    reports spans omitted from the bounded aggregate status reply.

The [calibration operation page](attenuator_calibration.md) owns acquisition,
bridge normalization, fit weighting, the 65 dB floor constraint and model
validation. The [attenuator model](attenuator.md) defines runtime conversion and
uncertainty; the [Python guide](python_helpers.md#calibration-datasets-and-manual-grids)
explains retained-record inspection and fitting tools.

### `atten/<laser>/settings`

Query the two physical fit acceptance limits with no payload. Set either or both:

```json
{"dac1":{"max_fit_residual_db":2.0},"dac2":{"max_fit_residual_db":2.0},"persist":true}
```

Each limit is finite and strictly positive; defaults are 2 dB. Unknown nested
fields are errors. Updates are rejected while calibration is active. Replies
contain `dac1` and `dac2` objects with `max_fit_residual_db`; no DAC is moved.
`pcb.atten_settings(laser, dac1={"max_fit_residual_db":1.0}, persist=True)` uses the
same schema. Omitted devices retain their limit. Persistence uses separate NVS
records at 0x0140–0x0145, leaving calibration/dark records and settings schema 13
unchanged so an OTA rollback can still read the existing calibration.

(pd)=
### `pd`
- **Topic:** `cmd/<device>/req/pd` or `cmd/<device>/req/pd/<yj|hk>`
- **No payload -> photodiode values and the public monitoring window:**
  ```json
  {
    "yj": {
      "raw": 0,
      "mv": 0.0,
      "net_mv": 0.0,
      "net_err_mv": 0.0,
      "power_uw": 0.0,
      "power_err_uw": 0.0,
      "dark_mv": 0.0,
      "dark_err_mv": 0.0,
      "window": {
        "duration_ms": 0,
        "failed_samples": 0,
        "mean_mv": 0.0,
        "mean_net_mv": 0.0,
        "rms_mv": 0.0,
        "mean_net_err_mv": 0.0,
        "min_mv": 0.0,
        "max_mv": 0.0,
        "power_uw": 0.0,
        "power_err_uw": 0.0
      },
      "pd_powered": true,
      "pd_on_s": 0
    },
    "hk": {}
  }
  ```
- **Single-channel query:** `pd/yj` returns only the selected channel:
  ```json
  {"yj": {}}
  ```

- **Notes:**
  - `pd` queries both channels. `pd/yj` and `pd/hk` query only one channel.
    In auto power mode, a query enables the selected photodiode relay or relays.
  - `raw`, `mv`, `net_mv`, `net_err_mv`, `power_uw`, and `power_err_uw` are the
    latest successful conversion and its reading/offset error. An ADC failure
    leaves this diagnostic state unchanged; throughput does not re-emit it.
  - `window` is the fixed public diagnostic window, separate from throughput
    and autolevel. Its duration is set by `PHOTODIODE_FIXED_WINDOW_MS` in
    `app/src/photodiode.h`.
  - The internal configurable window used by dark measurement and attenuator
    calibration is not exposed through the command API.
  - Dark measurement and forced dark updates are done through
    `pd/dark/<channel>`, not through `pd` or `pd/settings`.

(pddark)=
### `pd/dark`
- **Topic:** `cmd/<device>/req/pd/dark/<yj|hk>`
- **No payload -> active and lowest dark windows:**
  ```json
  {
    "channel": "yj",
    "pending": false,
    "duration_ms": 0,
    "dark": {
      "duration_ms": 0,
      "failed_samples": 0,
      "mean_mv": 0.0,
      "mean_net_mv": 0.0,
      "rms_mv": 0.0,
      "mean_net_err_mv": 0.0,
      "min_mv": 0.0,
      "max_mv": 0.0,
      "power_uw": 0.0,
      "power_err_uw": 0.0
    },
    "lowest_dark": {}
  }
  ```
- **Payload:** measure, force, or reset one channel's dark.
  ```json
  {"duration_ms": 1000, "persist": false}
  ```
  ```json
  {"dark_mv": 0.0, "rms_mv": 1.5, "persist": true}
  ```
  ```json
  {"reset_lowest": true}
  ```

- **Notes:**
  - `duration_ms` arms a sampler-owned dark capture using the internal
    configurable photodiode window and returns immediately. Query
    `pd/dark/<channel>` to see `pending:false` and the resulting dark window.
    Collection starts only after 10 continuous seconds of PD power on and
    relevant lasers off; already elapsed settling time is credited. This wait
    is additional to the requested duration (maximum 2,000 ms). Interruptions
    discard the partial window and restart settling. Clipped/all-invalid
    captures are rejected without replacing the previous dark.
  - `dark_mv` forces a user-specified dark. `rms_mv` may be included with
    `dark_mv`; if omitted, firmware uses
    `PHOTODIODE_FORCED_DARK_RMS_DEFAULT_MV` from `app/src/photodiode.h`.
  - `duration_ms` and `dark_mv` are mutually exclusive. Durations must be
    greater than zero and no larger than `APP_PD_DARK_DURATION_MAX_MS` in
    `app/src/app_settings.h`.
  - `reset_lowest:true` resets the lowest-dark record to the active **measured**
    dark, or the new result when combined with a duration capture. Forced/default
    darks cannot be promoted, including after persistence/reboot. Combining
    `dark_mv` and `reset_lowest:true` is rejected before either value changes.
    Existing minima and optional persistence are otherwise unchanged.
  - `persist` defaults false. Duration captures are rejected during attenuator
    calibration. They stop all throughput and its owned laser before capture;
    stop failure aborts capture. A relevant manual laser still emitting rejects
    capture; unrelated routes remain unchanged.
  - Captured `rms_mv` is single-reading scatter; `mean_net_err_mv` and
    `pd.dark_err_mv` report dark-mean uncertainty. For a forced dark, supplied
    `rms_mv` is offset uncertainty. Recapture dark after cadence/rate changes;
    saved records do not encode the acquisition rate. See the
    [uncertainty audit](photodiode_notes.md).

(pd-settings)=
### `pd/settings`
- **Topic:** `cmd/<device>/req/pd/settings/<yj|hk>`
- **No payload -> one channel's photodiode settings:**
  ```json
  {
    "channel": "yj",
    "noisewarn_mv": 3.0,
    "responsivity_a_per_w": 0.93,
    "transimpedance_v_per_a": 2.0e10,
    "power": "auto",
    "powered": false,
    "autooff_s": 300,
    "off_in_s": null
  }
  ```
- **Payload:** update one channel's photodiode settings.
  ```json
  {
    "noisewarn_mv": 3.0,
    "responsivity_a_per_w": 0.93,
    "transimpedance_v_per_a": 2.0e10,
    "power": "auto",
    "autooff_s": 300,
    "persist": true
  }
  ```

- **Current set fields:**
  - `noisewarn_mv`
  - `responsivity_a_per_w`
  - `transimpedance_v_per_a`
  - `power`
  - `autooff_s`
  - `persist`

- **Notes:** not all settings need to be included when setting; failure on any
  settable setting results in none being set. YJ and HK settings use separate
  command keys and separate app NVS records. `power` is the relay intent for
  this channel: `auto`, `override_on`, or `override_off`. `autooff_s` is the
  channel's automatic power-off delay used when firmware auto-enables the
  relay; `off_in_s` is `null` unless a channel auto-off countdown is armed.
  `powered` is the read-only actual state of this channel. Querying settings
  neither enables it nor rearms auto-off; unlike `pd`, it is suitable for preflight.
  Dark and lowest-dark values are queried and updated through
  `pd/dark/<channel>`.

  `transimpedance_v_per_a` is the effective gain at the ADC input, combining
  detector datasheet transimpedance with the divider and intervening analog gain.
  Its allowed range is `1e5` to `1e12` V/A. Defaults are `2.0e10` V/A
  for YJ and `9.5e5` V/A for HK; do not apply the divider again in power conversion.
  `noisewarn_mv` is ADC-input RMS scatter in the fixed 500 ms window, including
  real optical changes. The 10 mV default warning level corresponds nominally to 0.538 pW RMS
  for YJ and 17.3 nW RMS for HK using the default responsivities.
  Zero disables the noise warning. PD power, attenuator, laser-output, and
  actual MEMS changes defer noise warnings on both channels until all changing
  operations finish, five seconds elapse, and a fresh 500 ms window is acquired
  (at least two valid samples). Further changes restart this wait. An eligible
  window above threshold warns immediately, at most once per channel per
  60 seconds. This affects warning eligibility only; samples, reported RMS,
  throughput, and the separate ten-second dark-capture rule continue normally.
  Dark bounds are +/-2048 mV and noise RMS bounds are 0-2048 mV.

(measure-throughput)=
### `measure_throughput`
- **Payload:** start monitoring.
  ```json
  {
    "autolevel": true,
    "initial_level": 0.5,
    "laser": "<lasername>",
    "fiber": "M",
    "output": "yj_ao",
    "max_flux_ph_s": 1.0e12,
    "off_in_s": 300,
    "format": "binary"
  }
  ```
- **Payload:** start monitoring an externally supplied/calibration input.
  ```json
  {
    "autolevel": false,
    "laser": "none",
    "channel": "yj",
    "input": "yj_cal",
    "output": "yj_ao",
    "fiber": "M",
    "format": "binary"
  }
  ```
- **Payload:** stop monitoring.
  ```json
  {
    "stop": "yj"
  }
  ```

`measure_throughput` is the only command that starts or stops photodiode
streaming. It measures throughput by comparing the route-corrected optical power at the
selected photodiode with the route- and attenuator-corrected laser power
estimate.

`autolevel:true` lets firmware adjust the selected laser output level percent
and logical attenuator to keep the photodiode signal in the useful
ADC/photodiode range. `autolevel:false` streams the selected photodiode level
and derived values without adjusting laser level or attenuation during monitoring.
For a named laser, passive monitoring can start before any laser command, with
bank power off or on. It continues through bank power-on, controller preparation,
TEC startup, and the first current command; no preliminary STOP or positive-current
command is required. The source-current estimate starts at zero, and throughput
is undefined (`null` in JSON, NaN in binary) while delivered source power is zero.
PD measurements remain available throughout. These source estimates describe
commanded state, not an optical measurement; actual control/communication faults
still stop the stream.

`initial_level` is an optional fraction from 0 to 1, accepted only with
`autolevel:true`. Firmware alone supplies the default, 0.5. Startup sets maximum
calibrated attenuation, then requests this fraction of modeled nominal emission.
The inverse curve selects current, bounded by `min_autolevel_current_ma` and nominal
current and quantized to 0.1 mA.
Thus `initial_level:0` starts at the autolevel minimum; it does not turn the laser
off. This applies to both compiled dimming priorities. Throughput ignores stored
`tune_nm` and retains the live TEC target when already prepared; cold preparation
still applies `default_operating_temp_c`.
Stopping an autolevel operation also stops the laser it was using, even if
manual laser level or attenuation changes have since disabled automatic adjustments.
A purely passive measurement leaves manual laser output unchanged when stopped. Continuing the
same source with `autolevel:false` retains an existing operation's laser
shutdown obligation; replacing its source first stops that autolevel laser.
Bank power, TECs and unrelated lasers are left unchanged. This differs from an
explicit `laser stop:true` or laser auto-off, which uses `disable_tec_at_autooff`.
Valid `stop` requests take priority over accompanying start fields, whose values
are ignored even if invalid. Unknown top-level fields are still rejected.

HK and YJ can both stream, with one measurement per photodiode channel and
**only one autolevel owner**. A second autolevel start is rejected before changing
routes or outputs. External instrument paths combine the light and influence
both PDs; manually enabled additional lasers are not separated by this system.

`stop:"yj"`, `stop:"hk"`, or `stop:"all"` stops the selected measurements and
attempts every owned laser shutdown. Failed shutdown disables streaming/control
and retains the laser identity for an explicit stop retry. Expiry, PD power loss,
and operational source/relay faults use the same stop path. Numerical laser
estimates continue to use confirmed setpoints; acquisition checks owner health
separately. A single failed read warns; five seconds without a response while
in use faults the owner. Recovery does not restart measurements. See
[communication and power lifetime](laser_operation.md#communication-and-power-lifetime).

Both `format:"json"` and `format:"binary"` are supported. Firmware defaults to
JSON; Python and the notebook default to binary. Binary channel and wavelength
identify the source, including both 1430 nm lasers, and a flag bit carries the
actual autolevel state. Individual DAC setpoints are streamed as `dac1_mv` and `dac2_mv`; laser
percentage is not streamed.

For a known laser, `output` is required and `input` is inferred unless supplied.
Optional `channel` must match that laser. Passive `laser:"none"` requires explicit
`channel:"yj"` or `"hk"` and `autolevel:false`. Its `input` and `output` are an
optional pair: omit both to leave launch switching untouched (for example,
astrophysical illumination), or supply both to route external calibration light.
An explicit launch must select that channel's AO or FEI output.

The command validates launch and return routes before preparing the monitor.
Preparation checks exclusions, quiesces the target stream, and stops an owned
source if replacing it. The command then applies the optional launch route and
**always** applies `yj_mm/sm -> yj_pd` or `hk_mm/sm -> hk_pd`. These use independent
switches. Only then does the monitor enable PD power and start measurement.
Route/start failure stops the prepared monitor and attempts its owned laser
shutdown; routing failures can leave some MEMS switches changed and report that.
Invalid input or an exclusion failure leaves an existing run untouched.

All measurements divide detected power and its error by the selected return
transmission. A known laser selects its route/laser override when present;
unknown illumination selects the route's `generic` override or compiled generic
default (MM 0.98, SM 0.60). Passive PD voltage, corrected power, errors, and
detector S/N remain available; source power, wavelength, delivered power, and
throughput are NaN/null because emission is unknown. `launch_route_tx` reports
the resolved transmission for an explicitly selected launch; it is NaN/null for a
return-only capture. The acquisition selector remains `laser:"none"`; only
route-loss storage uses `generic`. A return-only example:
```json
{"laser":"none","channel":"hk","fiber":"S","autolevel":false,"format":"binary"}
```

Dark capture or active attenuator calibration rejects all starts. Taking a dark
or starting calibration stops existing throughput; there is no automatic resume.

`max_flux_ph_s` remains an optional autolevel limit in photons/s, after the
dynamic attenuator pair and **before** static route losses. Stream quantities
use power: nW for detected/delivered light and µW for the estimated laser output
before attenuation. No actual laser power readback exists.

The stream is nominally **20 Hz per channel**: one fresh ADC conversion per
50 ms, without overlapping or reused samples. It does not use the fixed 500 ms
PD window. The monitor selects its previous/current source context using the
monotonic acquisition start and last input-change time; the PD module owns no
source context. `t_ms` is the estimated UTC conversion midpoint. A delayed consumer can skip
intermediate readings; a failed ADC conversion produces no record or adjustment.
Timestamps expose gaps. The latest diagnostic PD state remains available and
its windows count failed conversions. ADC warnings are limited to one per
channel per 10 seconds. See [the sampling/error audit](photodiode_notes.md).

**Telemetry topics:** `dt/<device>/yj_tput`, `dt/<device>/hk_tput`.

**JSON fields:**
```json
{
  "channel": "yj_m",
  "laser": "1028y",
  "autolevel": true,
  "t_ms": 0,
  "tp": 0.2,
  "tp_err": 0.0100498756211,
  "tp_pd_err": 0.001,
  "pd_power_nw": 0.4,
  "pd_power_err_nw": 0.002,
  "delivered_power_nw": 2,
  "delivered_power_err_nw": 0.1,
  "laser_output_power_uw": 1000,
  "laser_output_power_err_uw": 30,
  "pd_route_tx": 0.5,
  "launch_route_tx": 0.2,
  "atten_tx": 0.01,
  "pd_mv": 100,
  "pd_net_mv": 90,
  "pd_net_err_mv": 0.5,
  "laser_current_ma": 50,
  "atten_db": 20,
  "wavelength_nm": 1028,
  "atten1_db": 12,
  "atten2_db": 8,
  "dac1_mv": 2447.314,
  "dac2_mv": 2378.027,
  "atten1_slope_db_per_fvoa_mv": 0.012,
  "atten2_slope_db_per_fvoa_mv": 0.018,
  "pd_raw": 1600,
  "pd_ontime_s": 1,
  "laser_current_ontime_s": 2,
  "flags": []
}
```

`channel` is `yj_m`, `yj_s`, `hk_m`, or `hk_s`. On-times are integer seconds:
PD relay continuous on-time and the laser module's current-emission on-time.
Nonfinite values are JSON `null`; finite values use 12 significant digits.

**Binary layout:** 227 bytes, little-endian, Python `struct` format
`<8sQ24dh2QB`. The channel is zero-padded ASCII. Float values are IEEE-754 doubles.

```text
char[8] channel
uint64 t_ms
float64 tp
float64 tp_err
float64 tp_pd_err
float64 pd_power_nw
float64 pd_power_err_nw
float64 delivered_power_nw
float64 delivered_power_err_nw
float64 laser_output_power_uw
float64 laser_output_power_err_uw
float64 pd_route_tx
float64 launch_route_tx
float64 atten_tx
float64 pd_mv
float64 pd_net_mv
float64 pd_net_err_mv
float64 laser_current_ma
float64 atten_db
float64 wavelength_nm
float64 atten1_db
float64 atten2_db
float64 dac1_mv
float64 dac2_mv
float64 atten1_slope_db_per_fvoa_mv
float64 atten2_slope_db_per_fvoa_mv
int16 pd_raw
uint64 pd_ontime_s
uint64 laser_current_ontime_s
uint8 flags  # bit 0: overrange; bit 1: autolevel; remaining bits zero
```

The two signed local slopes are in dB per **post-amplifier** drive mV and are
also present in JSON. They are evaluated by the firmware model at confirmed DAC
settings; unavailable values are null/NaN. These slopes allow host temporal-noise
estimates without evaluating another attenuator model.

**Field interpretation:**

- `pd_mv`, `pd_net_mv`, `pd_net_err_mv` and `pd_raw` describe one conversion,
  not the rolling window. `pd_power_nw` includes the return-route correction;
  `delivered_power_nw` includes attenuator and launch-route transmission.
- `tp` is corrected return/delivered power. `tp_pd_err` includes only detector
  uncertainty; `tp_err` also includes modeled source and attenuator uncertainty.
  These uncertainties are not temporal RMS. Zero/invalid delivered power makes
  throughput null/NaN.
- ADC input ≥2000 mV sets JSON `flags:["overrange"]` / binary bit 0. Retain
  detector/throughput values only as nominal lower bounds; their errors and S/N
  are unavailable. Calibration's ADC-rail classification is separate.
- `atten1_db`, `atten2_db`, `dac1_mv` and `dac2_mv` describe the source context
  associated with the conversion, including previous settings for a delayed
  sample. DAC values are confirmed setpoints, not independent voltage readings.
  An unnamed source leaves these fields null/NaN.
- JSON uses null and binary uses NaN for unavailable floating-point values.

See [throughput and autolevel operation](throughput.md) for the equations,
route-loss selection, timestamp association, feedback decisions and ownership.
[Mathematics and uncertainty](mathematics.md) distinguishes reported errors,
window RMS and temporal noise, including correlated and omitted terms.
