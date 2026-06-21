# Control and Telemetry Interface Specification
Draft 0.1


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
- Commands have serial port duals. Serial commands use a simpler line format
  for interactive bring-up and debugging.
- Board-specific commands are rejected before their domain handler runs when
  the selected board strap does not provide that hardware.
- Command keys are exact-match by default. Only endpoint families documented
  with topic suffixes, such as `atten/<laser>/coeff`, `split/yj`, or
  `laserbank/power/<mode>`, opt into prefix matching. Unknown top-level payload
  keys are rejected before the domain handler runs.

## Serial Command Form

Serial commands are one line, whitespace separated, and intended for a human at
the console.

Top-level implementation path:

1. `coo_cmd_runtime_serial_thread()` reads one console line.
2. `coo_cmd_runtime_handle_serial_line()` splits the line into `<key>` and optional payload.
3. `coo_cmd_normalize_serial_payload()` turns non-JSON serial payloads into the same JSON shape used by MQTT.
4. Dispatcher built-ins handle `help` and, when enabled,
   `serialguard`. Serial `help` prints directly and does not enter the command
   queues.
5. `coo_cmd_runtime_executor_thread()` dispatches app-owned commands through the
   app command table.
6. `coo_cmd_runtime_drain_outbound()` prints queued serial responses with
   `coo_cmd_print_serial_response_pretty()`. The simpler
   `coo_cmd_print_serial_response()` remains available as a fallback renderer.

No-payload serial request form is just the key:

```text
status
mems/yj_cal_laser
split/yj
```

Requests with payload use the key followed by a payload. There are no `get` or
`set` keywords in the serial command set.

```text
serialguard seconds=60
mems/yj_cal_laser state=A duty_cycle=0.5 cycle_ms=400 off_in_s=30
split channel=yj ratio1=0.25 ratio2=0.25 cycle_ms=800 off_in_s=300
laserbank/power/override_on
```

Payload rules:

- A payload beginning with `{` is copied unchanged into `Command.payload`; it is
  not parsed and rebuilt by the serial layer.
- Payloads containing `=` use `serial_payload_from_key_values()`, for example
  `state=A off_in_s=30`.
- Known compact forms use `serial_payload_from_shorthand()`, for example
  `serialguard off`, `serialguard 60`, or `mems/yj_cal_laser A 0.5 30`.
- Handlers parse and validate the normalized JSON exactly as they do for MQTT.

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

## Command Endpoints
- [`help`](#help)
- [`catalog`](#catalog)
- [`memsroute`](#memsroute)
- [`memsroute/route_loss`](#route-loss)
- [`mems`](#mems)
- [`mems/<switchname>`](#mems-switchname)
- [`measure_throughput`](#measure-throughput)
- [`laser`](#laser)
- [`laser/tune`](#laser-tune)
- [`laser/status`](#laser-status)
- [`laser/settings`](#laser-settings)
- [`laserbank/power`](#laserbank-power)
- [`laserbank/clearfaults`](#laserbank-clearfaults)
- [`laserbank/heater`](#laserbank-heater)
- [`atten/<laser>`](#atten)
- [`atten/<laser>/coeff`](#atten-coeff)
- [`atten/calibrate`](#atten-calibrate)
- [`pd`](#pd)
- [`pdsettings/<yj|hk>`](#pdsettings)
- [`ip`](#ip)
- [`mqtt`](#mqtt)
- [`serialguard`](#serialguard)
- [`time`](#time)
- [`temp`](#temp)
- [`status`](#status)
- [`reboot`](#reboot)
- [`split`](#split)
- Telemetry: `yj_tput`, `hk_tput`
- Warnings: [`dt/<device>/warning`](#warning-publication)
- Boot telemetry: [`dt/<device>/boot`](#boot-telemetry)

---

(warning-publication)=
### Warning Publication
- **Publish topic:** `dt/<device>/warning`
- **Top-level helper:** `coo_cmd_runtime_emit(command_runtime_get(), &args)`
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
    "uptime_s": 0
  }
  ```

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
- **Serial no payload -> full interactive command help.**
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

(catalog)=
### `catalog`
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
  input/output pairs for `memsroute` and route-bearing commands. `lasers` is
  populated on TIB and empty on non-TIB board profiles.

(memsroute)=
### `memsroute`
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
### `memsroute/route_loss`
- **Payload:** set one route-loss record.
  ```json
  {
    "route": "yj_sm_to_yj_pd",
    "1430yj": 0.93,
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
    "split": ["0.32 dB", "0.32 dB", 0.93],
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
      "1028y": 1.0,
      "1270j": 1.0,
      "1430yj": 0.93,
      "1430hk": 1.0,
      "1510h": 1.0,
      "2330k": 1.0
    }
  }
  ```
  For a split route:
  ```json
  {
    "route": "yj_calin_to_yj_split",
    "split": [0.93, 0.93, 1.0]
  }
  ```

Route-loss records are app settings keyed by route name and laser name or split. Missing route-loss
records are treated as loss-free transmission, `tx = 1.0`. Numeric values are
linear transmission in `(0, 1]`. Strings ending in `dB`, `db`, or `DB` are route
loss in dB and convert to `tx = 10^(-loss_db / 10)`. The split identifier must be a three-tuple, though dB loss and
transmission may be mixed. Route losses are used on the TIB for throughput monitoring
and the AS for splitting fraction correction.


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
  For example, `mems/yj_cal_laser state=B` returns:
  ```json
  {"state":"B"}
  ```
  and `mems/yj_cal_laser state=A` returns:
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


(measure-throughput)=
### `measure_throughput`
- **Payload:** start monitoring.
  ```json
  {
    "autolevel": true,
    "laser": "<lasername>",
    "fiber": "M",
    "output": "yj_ao",
    "max_flux_ph_s": 1.0e12,
    "off_in_s": 300,
    "format": "json"
  }
  ```
- **Payload:** start monitoring an externally supplied/calibration input.
  ```json
  {
    "autolevel": false,
    "laser": "none",
    "input": "yj_cal",
    "output": "yj_ao",
    "fiber": "M",
    "format": "json"
  }
  ```
- **Payload:** stop monitoring.
  ```json
  {
    "stop": "yj"
  }
  ```

`measure_throughput` is the only command that starts or stops photodiode
streaming. It measures throughput by comparing the route-corrected flux at the
selected photodiode with the route- and attenuator-corrected laser flux
estimate.

`autolevel:true` lets firmware adjust the selected laser output level percent
and logical attenuator to keep the photodiode signal in the useful
ADC/photodiode range. `autolevel:false` streams the selected photodiode level
and derived values without changing laser level or attenuation.

`output` is optional for normal laser monitoring. When supplied, firmware
selects the outbound MEMS route before starting the monitor. The route input is
inferred from `laser` unless `input` is supplied explicitly. `laser:"none"` is
for monitoring externally supplied light and requires `input`, `output`, and
`autolevel:false`; throughput and emitted-flux fields that require a known
laser are reported as `null` in JSON or NaN in binary.

`max_flux_ph_s` is optional and valid only with `autolevel:true`. It limits the
estimated emitted photon flux after the calibrated logical attenuator pair, so
the limit uses the current laser flux estimate multiplied by
`attenuator_estimate_transmission()`.

Firmware uses each photodiode channel's configured `responsivity_a_per_w` and
`transimpedance_v_per_a` from `pdsettings/<yj|hk>` with the active laser
wavelength estimate. It applies the nearest nominal-laser photodiode
multiplicative correction coefficient; the current firmware table uses `1.0`
for every nominal laser wavelength. The photodiode sampler owns ADC reads and
dark tracking. The throughput monitor owns streaming output, autolevel
decisions, and throughput math.

Transient ADC read/write errors are treated as missing photodiode samples:
firmware emits a `photodiode_adc_error` warning, leaves the last good rolling
value intact for streaming consumers, and counts the failed sample in the
active photodiode windows. A window becomes unusable only when all attempted
samples in that window fail.

**Telemetry topics (published):**
- `dt/<device>/yj_tput`
- `dt/<device>/hk_tput`

**Telemetry payload (`format:"json"`):**
```json
{
  "channel": "yj_m",
  "laser": "1430yj",
  "autolevel": true,
  "t_ms": 0,
  "tp": 0.0,
  "tp_err": 0.0,
  "tp_rms_err": 0.0,
  "pd_flux_ph_s": 0.0,
  "pd_flux_err_ph_s": 0.0,
  "laser_flux_ph_s": 0.0,
  "laser_flux_err_ph_s": 0.0,
  "pd_route_tx": 1.0,
  "laser_route_tx": 1.0,
  "atten_tx": 1.0,
  "pd_raw": 0,
  "pd_mv": 0.0,
  "pd_net_mv": 0.0,
  "pd_mean_net_mv": 0.0,
  "pd_mean_net_err_mv": 0.0,
  "laser_current_ma": 0.0,
  "atten_db": 0.0,
  "wavelength_nm": 1430.0,
  "pd_ontime_s": 0,
  "laser_current_ontime_s": 0,
  "flags": []
}
```

`channel` combines the photodiode channel and fiber class with an underscore,
for example `yj_m`, `yj_s`, `hk_m`, or `hk_s`. `t_ms` is Unix time in
milliseconds from the firmware clock. `pd_ontime_s` is the current continuous
integer on-time in seconds of the photodiode power relay for that channel.

**Telemetry payload (`format:"binary"`):**

Binary telemetry is little-endian and contains the fields below in order. The
first field is a zero-padded 8-byte ASCII channel/fiber label such as `yj_m`.

```text
char[8] channel
uint64 t_ms
float64 tp
float64 tp_err
float64 tp_rms_err
float64 pd_flux_ph_s
float64 pd_flux_err_ph_s
float64 laser_flux_ph_s
float64 laser_flux_err_ph_s
float64 pd_route_tx
float64 laser_route_tx
float64 atten_tx
int16 pd_raw
float64 pd_mv
float64 pd_net_mv
float64 pd_mean_net_mv
float64 pd_mean_net_err_mv
float64 laser_current_ma
float64 atten_db
float64 wavelength_nm
uint64 pd_ontime_s
uint64 laser_current_ontime_s
```

**Notes:**
- `tp` is unitless. `NaN` means offscale or insufficient information; values
  above unity are reported rather than clamped.
- Flux values are photons per second.
- `pd_mv` is the instantaneous raw ADC millivolt reading and `pd_net_mv` is
  the instantaneous dark-subtracted value. `pd_mean_net_mv` and
  `pd_mean_net_err_mv` come from the photodiode sampler's fixed monitoring
  window. Its duration is set by `PHOTODIODE_FIXED_WINDOW_MS` in
  `app/src/photodiode.h`.
- `atten_tx` and `atten_db` are dynamic logical attenuator terms normalized to
  the modeled 0 V FVOA state. Static assembly and route losses belong in
  `memsroute/route_loss`.
- Route transmissions default to `1.0` when no route-loss record is stored.
- Both outbound laser route loss and inbound photodiode route loss are applied
  when estimating throughput.
- The current firmware lookup names the inbound photodiode route as
  `<yj|hk>_<mm|sm>_to_<yj|hk>_pd` from `fiber:"M"|"S"` and the outbound laser
  route as `<lasername>_to_<M|S>`. These names are route-loss record keys, not
  MEMS route-table entries.
- The fixed-window net mean controls autolevel. Below 20% usable range, firmware requests
  3x flux. Above 80%, it requests 1/3 flux.
- Five consecutive saturated samples or five consecutive below-dark samples
  trigger immediate autolevel adjustment.
- Flux is raised by decreasing logical attenuation first, then raising laser
  output level percent. Flux is decreased by increasing logical attenuation
  first, then lowering laser output level percent.
- At start with `autolevel:true`, attenuation is set to maximum before laser
  power is raised.
- Starting a monitor powers the required photodiode unless
  `pdsettings/<channel>.power` is `override_off`; in that mode the command
  fails with `photodiode power override_off`. While a monitor is running,
  photodiode auto-off is inhibited and `pdsettings/<channel>.off_in_s` reports
  `null`. Shutting down the required photodiode power stops that monitor.
- `off_in_s` is an integer-second monitor auto-stop delay. `0` disables the
  monitor auto-stop.
- Changing the monitored laser output or its logical attenuator disables
  autolevel for the affected monitor; run the command again to re-enable it.
- Starting a monitor with `autolevel:true` while attenuator calibration is
  active is rejected because both paths would own attenuator control.
- Throughput uses the photodiode sampler windows; it does not own or start
  dark commits.


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
    "level": 0.0,
    "power_mw": 0.0,
    "nominal_nm": 0.0,
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
    "level": 0.0,
    "autooff_s": 0
  }
  ```

- **Notes:** `level` is 0-100% of the nominal current range above threshold current. Setting a positive level powers
  the laser bank as needed, prepares the TEC, applies the stored `laser/tune`
  request when `tune_nm` is nonzero, sets the laser current, and restarts the
  auto-off timer. Setting level 0 stops emission and writes driver current to 0;
  it does not clear the stored `laser/tune` request. Laser output current is
  never persisted by app settings. The Maiman
  driver may retain its own current register, so firmware writes 0 whenever emission is disabled or the bank is turned off.
  `ready` reports whether the driver is prepared to operate without a blocking
  SF8025 lock condition. `blocked_reason` is `null` when no blocking condition
  is present, including a ready but idle laser; otherwise it reports a concise
  cause such as `bank_off`, `tec_not_started`, `ld_overcurrent`, or
  `interlock`. Active time fields are integer seconds while active and `null`
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
  temperature or laser current. Future positive `laser` level commands apply
  the stored offset relative to `laser/settings.wavelength_nm`. Reissuing a
  positive `laser` level after changing level does not require retuning because
  firmware reapplies the stored offset. Setting `laser` level 0 stops emission
  without clearing the stored tune request. Tuning is best-effort: large shifts
  are clamped by the TEC temperature range and allowed current adjustment.


(laser-status)=
### `laser/status`
- **Payload -> detailed engineering status:**
  ```json
  {"name":"<lasername>"}
  ```

Detailed engineering status derived from the Maiman status query used in
`refrence_docs_examples/lasers.py`. Includes raw state, lock, and TEC-state
registers, measured diode/TEC voltage and current, driver limits, PID, hard
device-id verification, configured expected driver serial, `blocking_lock`,
`blocked_reason`, and interlock flags. This command may be slower than
the basic `laser` query because it reads many Modbus registers. A serial mismatch is
reported as `serial_ok:false` and `blocked_reason:"driver_identity_mismatch"`.


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
      "expected_serial": 0,
      "nominal_current_ma": 0.0,
      "max_current_ma": 0.0,
      "current_set_calibration_pct": 0.0,
      "threshold_current_ma": 0.0,
      "efficiency_mw_per_ma": 0.0,
      "wavelength_nm": 0.0,
      "operating_temp_range_c": [0.0, 0.0],
      "default_operating_temp_c": 0.0,
      "thermistor_kohm": 0.0,
      "isolation_db": 0.0,
      "tec_max_current_a": 0.0,
      "tec_pid": {
        "p": 0,
        "i": 0,
        "d": 0
      },
      "disable_tec_at_autooff": true,
      "ntc_t_coefficient_per_c": 0.0,
      "dlambda_dT_nm_per_k": 0.0,
      "dlambda_dA_nm_per_ma": 0.0,
      "autooff_s": 0,
      "tune_nm": 0.0,
      "emit_total_s": 0
    }
  }
  ```
- **Payload:** update laser settings.
  ```json
  {
    "name": "<lasername>",
    "settings": {
      "nominal_current_ma": 0.0,
      "expected_serial": 0,
      "max_current_ma": 0.0,
      "efficiency_mw_per_ma": 0.0,
      "wavelength_nm": 0.0,
      "current_set_calibration_pct": 0.0,
      "tec_max_current_a": 0.0,
      "default_operating_temp_c": 0.0,
      "operating_temp_range_c": [0.0, 0.0],
      "tec_pid": {
        "p": 0,
        "i": 0,
        "d": 0
      },
      "disable_tec_at_autooff": true,
      "dlambda_dT_nm_per_k": 0.0,
      "dlambda_dA_nm_per_ma": 0.0,
      "autooff_s": 0
    },
    "persist": true
  }
  ```

- **Notes:**
  - It is the user's responsibility to ensure the triple of
    (`nominal_current_ma`, `default_operating_temp_c`, `wavelength_nm`) is
    aligned and in sync because these values form the baseline for wavelength
    tuning.
  - `default_operating_temp_c` is the persisted TEC startup/baseline setpoint
    applied during driver preparation and TEC start. It is not the live tuned
    TEC setpoint. Tuning may write a different live TEC setpoint when a positive
    `laser` level command applies the stored `tune_nm`, but it does not overwrite
    `default_operating_temp_c`.
  - Changing `default_operating_temp_c` changes the baseline used by future tune
    calculations. Existing `tune_nm` remains stored, but the next positive
    `laser` level command may compute a different TEC/current point from the new
    baseline.
  - Settings are checked when a laser is first talked to at each boot
  - `persist` is optional and defaults to false. Without `persist:true`,
    accepted changes apply to runtime and driver-backed state but are not saved
    in app NVS for the next controller boot.
  - After device-ID and serial checks pass, a mismatch between settings the
    driver stores in its EEPROM and controller NVRAM will trigger a warning in
    the log and the driver values will be programmed.
  - `expected_serial` is the operator-confirmed Maiman driver serial for this
    laser/diode association. It must be nonzero. A serial mismatch blocks
    driver-backed writes until the operator confirms the physical association
    and updates `expected_serial`; firmware does not learn or persist changed
    serials by itself. A device-ID mismatch remains an identity fault.
  - If the laser bank is off, firmware powers it, applies driver-backed settings,
    verifies them as practical, and then restores the previous bank power state.
    Driver-backed settings include `max_current_ma`, `current_set_calibration_pct`,
    `default_operating_temp_c`, `tec_max_current_a`, and `tec_pid`. If
    `laserbank/power` is `override_off`, driver-backed settings changes return
    an error.
  - it is **encouraged** to send only the settings that requested changed.
  - The overcurrent threshold is the maximum current the driver will allow the laser to run at and requires physically 
    adjusting a potentiometer on the driver. It has a (weak) temperature dependence and is not a fixed value.
  - Changes to settings will disable laser emission and may disable the TEC (stops emission + any throughput measurement using that laser)
  - Failures before driver programming completes leave settings unchanged
    (rollback is performed or an error emitted). If programming succeeds but
    restoring the previous bank power state fails, the successfully applied
    settings are still retained and persisted when requested; the command still
    reports the restore error because bank power needs operator attention.
  - Unsettable (attempts to set are silently ignored):
    - `name`, `model`, `serial`
    - `overcurrent_threshold_ma`
  - Non-Driver settings:
    - `autooff_s`
    - `dlambda_dT_nm_per_k`
    - `dlambda_dA_nm_per_ma`
    - `disable_tec_at_autooff`
    - `wavelength_nm`
    - `threshold_current_ma`
    - `efficiency_mw_per_ma`
  - Settings that are informational only (included for datasheet posterity):
    - `isolation_db`
  - Ranges:
    - Operating temp range: limited to [15,40] strong advice to limit to 17,38
    - current_set_calibration: 95 - 105 in steps of .01
    - TEC max current must be greater than zero and no higher than the compiled-in diode datasheet maximum for that laser.


(laserbank-power)=
### `laserbank/power`
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
  `cmd/<device>/req/laserbank/power/auto`,
  `cmd/<device>/req/laserbank/power/override_on`, or
  `cmd/<device>/req/laserbank/power/override_off`.

- **Notes:** `override_off` is the compiled boot default. In `auto`, power to the laser bank is handled by the bank
  heater and commands interacting with laser drivers. `override_on` forces bank power on. `override_off` stops all laser
  emission, writes driver currents to 0 as practical, powers the bank off, and rejects commands that need a live driver
  while the override is active. If the pre-off driver-current shutdown reports a Modbus failure, the command returns an
  error response that still includes the current `mode` and firmware-requested `powered` state. If another laser-bank
  operation occupies the shared Maiman Modbus bus past the command wait budget,
  mode changes return `{"error":"busy"}`.

(laserbank-clearfaults)=
### `laserbank/clearfaults`
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


(laserbank-heater)=
### `laserbank/heater`
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
  `cmd/<device>/req/laserbank/heater/auto`,
  `cmd/<device>/req/laserbank/heater/override_on`, or
  `cmd/<device>/req/laserbank/heater/override_off`.

- **Notes:** `auto` is the default at boot. In `auto`, laser-bank
  temperature-control work powers the bank so the Maiman temperature monitors
  can initialize, polls TEC temperatures at a fixed interval, and drives the
  laser-bank heater through housekeeping relay-power helpers.
  `auto_state` summarizes the internal policy state without exposing the
  control-loop booleans: `waiting_for_temps` means no fresh driver
  temperatures are available; `warming_disabled_tec` means at least one idle TEC
  probe is below the heater-on threshold; `disabled_tec_warm` means at least one
  idle TEC probe is warm enough for heater turnoff; `tecs_running` means all
  driver TECs are enabled; `holding` means no heater state change was requested
  in the latest loop. `idle_tec_temps` counts fresh driver temperature readings
  whose TEC is not started, and `idle_tec_avg_c` averages only those readings;
  actively controlled TEC temperatures remain laser telemetry and are not used
  for this aggregate. `ambient_c` and `idle_tec_avg_c` are `null` when
  unavailable. `poll_age_s` is an integer age in seconds or `null` before the
  first poll. The off threshold is 15 C when ambient is valid and above 15 C,
  otherwise 20 C. If all laser temperatures are stale, auto mode turns the
  heater off when ambient is invalid or at least 15 C. When valid ambient is
  below 15 C, auto mode powers the bank so driver
  temperature monitors can initialize and leaves heater state unchanged until
  valid laser temperature data is available. If all TECs remain enabled for at
  least one control interval, the heater is turned off.
  `override_on` and `override_off` force the heater state and suspend the
  automatic warmup policy. While a heater override is active, firmware emits
  `laserbank_heater_override` on `dt/<device>/warning` every 20 minutes.
  If the off-board DS2408 relay expander is offline, set requests return an I/O
  error because the heater relay cannot be driven.

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
      "gain": 1.533
    },
    "dac2": {
      "fvoa_50pct_mv": 2529.45,
      "slope_inv_fvoa_mv": 0.00158137,
      "gain": 1.533
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
      "gain": 1.533
    },
    "dac2": {
      "fvoa_50pct_mv": 3456.12,
      "slope_inv_fvoa_mv": 0.00247498,
      "gain": 1.533
    },
    "persist": true
  }
  ```
- **Serial form for `coeff`:** send the JSON object after the key. The default
  serial shorthand only builds a `value` payload, so it is not useful for
  coefficient objects. The MQTT payload is the same JSON object without the
  serial key prefix.
  ```text
  atten/1028y/coeff {"dac1":{"fvoa_50pct_mv":3144.95,"slope_inv_fvoa_mv":0.00303104,"gain":1.533},"dac2":{"fvoa_50pct_mv":3456.12,"slope_inv_fvoa_mv":0.00247498,"gain":1.533},"persist":true}
  ```

- **Notes:**
  - On TIB, `<laser>` is one of `1028y`, `1270j`, `1430yj`, `1430hk`, `1510h`,
    or `2330k`. On calibration boards only, the LFC attenuator is addressed as
    `atten/lfc` and `atten/lfc/coeff`.
  - Laser aliases accepted by the laser profile table, such as `1028`, also
    resolve to the matching TIB attenuator channel, but canonical command docs
    use the full logical laser names.
  - Each logical attenuator is a pair of physical FVOAs. Total set commands use
    the full modeled range of the first physical attenuator before using the
    second, and override any individual physical set point made through the C
    attenuator API.
  - `value` is a unitless linear transmission fraction in `(0, 1]`.
  - `v1_mv` and `v2_mv` are DAC-output setpoints in the firmware 0-3300 mV
    drive span. The firmware converts them to DAC codes using the
    board-configured DAC reference transfer, then responses report the applied
    DAC-side millivolts after code quantization and output-rail clipping.
  - Coefficients are loaded from persistent app NVS during
    `setup_attenuators()`. They define the erf coordinate
    `delta = slope_inv_fvoa_mv * (gain * dac_mv - fvoa_50pct_mv)` and
    `transmission = (erf(4) - erf(delta)) / (2 * erf(4))`. Runtime dB/linear
    set commands normalize against the modeled open transmission at DAC 0.
  - `persist` is optional and defaults to false. A non-persistent coefficient
    update changes runtime behavior until reboot or a later coefficient command.
  - There is no separate `attensettings` command; calibration coefficients live
    on `atten/<laser>/coeff`.

(atten-calibrate)=
### `atten/calibrate`
The implementation flow, bridge-normalization sequence, and retained-record
ownership are documented in `attenuator_calibration.md`.

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
      "corr": 0.999,
      "rms_db": 0.1,
      "max_abs_db": 0.2,
      "min_tx": 1.0e-6,
      "max_tx": 0.9,
      "fvoa_span_mv": 2400.0
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
  - TIB automatic calibration uses `laser`, `output`, and `fiber`; the laser
    selects the logical attenuator pair and outbound route input, while `fiber`
    selects the photodiode route as in `measure_throughput`.
  - TIB automatic calibration requires the selected photodiode to already be
    powered and producing valid sampler data. It stops laser emission, sets
    both physical attenuators to the maximum firmware DAC-drive voltage, sets
    the photodiode internal configurable-window duration to `dwell_ms`, and
    then waits that dwell after each attenuator change. It does not measure a
    private calibration dark. Each point uses the photodiode configurable
    window's configured dark-subtracted `mean_net_mv`; updating dark remains a
    separate `pd/dark/<channel>` operation.
  - Automatic calibration is SNR driven, not photodiode-mV-target driven. A
    measurement is usable when it is not ADC/electrical clipped and its
    dark-subtracted signal is at least 5 sigma above the sample mean
    uncertainty. Saturation here means actual ADC clipping near
    5 V, not a merely high photodiode voltage with electrical headroom.
    Low-but-clean points are retained and may be fit inputs. Saturated,
    below-SNR, and ADC-error measurements are retained as records but are not
    fit inputs.
  - Automatic calibration does not use a voltage schedule or datasheet limits
    to choose calibration points. For each physical FVOA it binary-searches the
    companion FVOA to find the lowest usable companion DAC, selects that
    measured initial-probe record as the open reference, linearly sweeps the
    DUT from 0 mV to maximum drive in
    `ATTEN_CAL_SWEEP_STEP_MV` increments, skips saturated bright-side sweep
    records as diagnostics, and bridge-normalizes when the sweep reaches the
    below-SNR dim edge. `ATTEN_CAL_SEARCH_MIN_STEP_MV` is the companion binary
    search resolution, not the DUT sweep step. Bridge normalization holds the
    DUT, selects the latest usable DUT point as the bridge-before record,
    searches the companion FVOA, and records the accepted bridge probe as the
    bridge-after record in the bridge table. The bridge ratio updates the
    segment scale and its uncertainty.
  - Firmware does not try to classify or discard whole nonlinear regions. It
    reports every retained acquisition record, and the fit uses only records
    derived as fit candidates by classification and transmission-domain rules. External analysis can
    inspect all retained records regardless of firmware fit success.
  - Automatic calibration uses the sampler-owned internal photodiode
    configurable window. It does not start a separate photodiode measurement or
    a new calibration thread; the throughput monitor thread advances the state
    machine.
  - The automatic fit derives all normalized quantities from raw records after
    acquisition. It divides each retained signal by the open reference and the
    cumulative bridge segment scale, converts that relative transmission to dB,
    and optimizes the attenuator model directly in dB output space while
    keeping the coefficient names and meanings `fvoa_50pct_mv` and
    `slope_inv_fvoa_mv`. The y uncertainty comes from photodiode mean
    uncertainty, bridge/segment-scale propagation, and the open-reference
    uncertainty; the x uncertainty is the fixed DAC uncertainty, initially
    3 mV. Fit details include point count, correlation, residual RMS/max in dB,
    fitted transmission span, and FVOA-drive span.

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
      "pd_is_off": false,
      "ontime_s": 0
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
    latest sample and its propagated dark error. Invalid latest samples are
    reported with null floating-point values and the raw sentinel.
  - `window` is the fixed public monitoring window used by throughput and
    autolevel. Its duration is set by `PHOTODIODE_FIXED_WINDOW_MS` in
    `app/src/photodiode.h`.
  - The internal configurable window used by dark measurement and attenuator
    calibration is not exposed through the command API.
  - Dark measurement and forced dark updates are done through
    `pd/dark/<channel>`, not through `pd` or `pdsettings`.

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
  - `dark_mv` forces a user-specified dark. `rms_mv` may be included with
    `dark_mv`; if omitted, firmware uses
    `PHOTODIODE_FORCED_DARK_RMS_DEFAULT_MV` from `app/src/photodiode.h`.
  - `duration_ms` and `dark_mv` are mutually exclusive. Durations must be
    greater than zero and no larger than `APP_PD_DARK_DURATION_MAX_MS` in
    `app/src/app_settings.h`.
  - `reset_lowest:true` resets the lowest-dark record to the active dark.
  - `persist` defaults false. Duration captures are rejected while attenuator
    calibration or autolevel throughput owns the configurable window. Dark
    commands do not check laser state, attenuator position, or routes.

(pdsettings)=
### `pdsettings`
- **Topic:** `cmd/<device>/req/pdsettings/<yj|hk>`
- **No payload -> one channel's photodiode settings:**
  ```json
  {
    "channel": "yj",
    "noise_rms_mv": 3.0,
    "responsivity_a_per_w": 0.93,
    "transimpedance_v_per_a": 5.0e10,
    "power": "auto",
    "autooff_s": 300,
    "off_in_s": null
  }
  ```
- **Payload:** update one channel's photodiode settings.
  ```json
  {
    "noise_rms_mv": 3.0,
    "responsivity_a_per_w": 0.93,
    "transimpedance_v_per_a": 5.0e10,
    "power": "auto",
    "autooff_s": 300,
    "persist": true
  }
  ```

- **Current set fields:**
  - `noise_rms_mv`
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
  Dark and lowest-dark values are queried and updated through
  `pd/dark/<channel>`.

(ip)=
### `ip`
- **No payload -> IP configuration:**
  ```json
  {
    "src": "<source>",
    "trydhcpfirst": true,
    "preferdhcpdns": true,
    "preferdhcpntp": true,
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
    "trydhcpfirst": true,
    "preferdhcpntp": true,
    "preferdhcpdns": true,
    "persist": true
  }
  ```

- **Notes:**
  - Unsupported features don’t error; supported changes are still applied and
    partial status reports unsupported fields.
  - IP precedence: runtime settings → compiled static defaults. The compiled
    static defaults are also the last-resort service fallback.
  - If `trydhcpfirst` is true and DHCP is compiled in, DHCP is tried before the
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
  `value` is accepted as an alias for `seconds`. Supplying `persist` is
  rejected; serial guard is runtime-only and is not restored after reboot.

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
  {"unix_ms":0}
  ```

- **Notes:** set time may be overwritten later by NTP if configured and responding.

(temp)=
### `temp`
- **No payload -> temperature status:**
  ```json
  {
    "ambient_c": 0.0,
    "laserbank_c": 0.0,
    "laser": {
      "<lasername>": 0.0
    }
  }
  ```

- **Notes:** Laser diode TEC temperatures are included when the laser bank is powered and the relevant driver registers
  can be read. Unavailable values are returned as JSON `null`. `laserbank_c` is the average of valid laser TEC
  temperatures. On TIB, if the shared Maiman Modbus bus is busy, this command
  returns `{"error":"busy"}` instead of an ambient-only partial response.

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
        "tec_on_s": 0,
        "off_in_s": 0
      }
    },
    "attens": {
      "<attenname>": {
        "level_%": 0.0
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
  `lastcmd` is restored from command-dispatch NVS storage when available.


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

(split)=
### `split`
- **Topics:**
  - `cmd/<device>/req/split`
  - `cmd/<device>/req/split/yj` or `cmd/<device>/req/split/hk`
  - Responses use the same key under `cmd/<device>/resp/...`.
  
- **Payload to `split` -> set splitter state:**
  ```json
  {
    "channel": "yj",
    "ratio1": 0.25,
    "ratio2": 0.25,
    "cycle_ms": 800,
    "off_in_s": 0
  }
  ```
- **No payload to `split/yj` or `split/hk` -> get splitter state.**
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
  - `off_in_s` is integer seconds, with max 4 hours. `0` disables the split
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
