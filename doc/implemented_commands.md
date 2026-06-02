# Implemented Commands

This page is derived from the app command spec table in `app/src/command.c`,
the common dispatch helpers, and the current command handlers. It is a
comparison artifact, not a replacement for `commands.md`.

## Global Rules

- MQTT request prefix: `cmd/<device>/req/`.
- Default MQTT response prefix: `cmd/<device>/resp/`.
- `<device>` is selected from the board strap: `tib` uses `hsfib-tib`,
  `cal_hk` uses `hsfib-rcal`, `cal_yj` uses `hsfib-bcal`, and `as` uses
  `hsfib-as`.
- MQTT `response_topic` overrides the default when supplied and fitting the
  fixed buffer.
- MQTT `correlation_data` up to 16 bytes is copied into a fixed static buffer
  and echoed exactly in responses.
- Retained MQTT requests are ignored with an error response to avoid replaying
  actions when the device reconnects.
- MQTT and serial share the same schema-based request classification selected
  by the app command spec table and applied by command dispatch. The internal
  names `MSG_GET` and `MSG_SET` are dispatch-slot names, not user-visible
  protocol verbs.
- Empty/no-payload requests are queries except no-payload actions such as `reboot`
  and `laserbank/clearfaults`, plus laserbank topic-suffix actions.
- Non-empty payload requests are effect/action requests except documented query
  shapes for `status`, laser query endpoints, `memsroute/route_loss`, and
  `pd` dark-status.
- The old MQTT `msg_type` payload convention is not used by command ingress.
- Pure queries are not recorded as `lastcommand`; supported effect-capable
  requests are recorded by command dispatch before handler execution and
  persisted in a fixed NVS record.
- Serial supports raw JSON, `key=value` fields, and selected shorthand forms.
- Dispatcher built-ins (`help`, and when enabled, `serialguard` and `reboot`)
  are handled in `command_dispatch.c`. Serial `help` prints directly; MQTT
  `help`, `serialguard`, and `reboot` enqueue one response to `outbound_queue`.
- App command handlers run in `coo_cmd_runtime_executor_thread()` and enqueue one
  response to `outbound_queue`.
- App support predicates reject unsupported command families before their
  hardware/domain handlers run.
- Data-less success returns `{"status":"ok"}`. Data-bearing success returns the
  data object. Failures include an `error` key.

## Dispatch Table

| Command key | MQTT request topic | Default response topic | Serial form |
| --- | --- | --- | --- |
| `help` | `cmd/<device>/req/help` | `cmd/<device>/resp/help` | `help` (dispatcher built-in) |
| `ip` | `cmd/<device>/req/ip` | `cmd/<device>/resp/ip` | `ip [payload]` |
| `mqtt` | `cmd/<device>/req/mqtt` | `cmd/<device>/resp/mqtt` | `mqtt [payload]` |
| `time` | `cmd/<device>/req/time` | `cmd/<device>/resp/time` | `time [payload]` |
| `reboot` | `cmd/<device>/req/reboot` | `cmd/<device>/resp/reboot` | `reboot` |
| `serialguard` | `cmd/<device>/req/serialguard` | `cmd/<device>/resp/serialguard` | `serialguard [payload]` (dispatcher built-in) |
| `memsroute` | `cmd/<device>/req/memsroute` | `cmd/<device>/resp/memsroute` | `memsroute [payload]` |
| `memsroute/route_loss` | `cmd/<device>/req/memsroute/route_loss` | `cmd/<device>/resp/memsroute/route_loss` | `memsroute/route_loss <payload>` |
| `mems` | `cmd/<device>/req/mems` | `cmd/<device>/resp/mems` | `mems` |
| `mems/<switch>` | `cmd/<device>/req/mems/<switch>` | `cmd/<device>/resp/mems/<switch>` | `mems/<switch> [payload]` |
| `split` | `cmd/<device>/req/split` | `cmd/<device>/resp/split` | `split <payload>` |
| `split/<channel>` | `cmd/<device>/req/split/<channel>` | `cmd/<device>/resp/split/<channel>` | `split/<channel>` |
| `measure_throughput` | `cmd/<device>/req/measure_throughput` | `cmd/<device>/resp/measure_throughput` | `measure_throughput <payload>` |
| `laserbank/power` | `cmd/<device>/req/laserbank/power` | `cmd/<device>/resp/laserbank/power` | `laserbank/power[/mode] [payload]` |
| `laserbank/clearfaults` | `cmd/<device>/req/laserbank/clearfaults` | `cmd/<device>/resp/laserbank/clearfaults` | `laserbank/clearfaults` |
| `laserbank/heater` | `cmd/<device>/req/laserbank/heater` | `cmd/<device>/resp/laserbank/heater` | `laserbank/heater[/mode] [payload]` |
| `laser` | `cmd/<device>/req/laser` | `cmd/<device>/resp/laser` | `laser <payload>` |
| `laser/tune` | `cmd/<device>/req/laser/tune` | `cmd/<device>/resp/laser/tune` | `laser/tune <payload>` |
| `laser/status` | `cmd/<device>/req/laser/status` | `cmd/<device>/resp/laser/status` | `laser/status <payload>` |
| `laser/engstatus` | `cmd/<device>/req/laser/engstatus` | `cmd/<device>/resp/laser/engstatus` | `laser/engstatus <payload>` |
| `laser/settings` | `cmd/<device>/req/laser/settings` | `cmd/<device>/resp/laser/settings` | `laser/settings <payload>` |
| `atten/<laser>/<setting>` | `cmd/<device>/req/atten/<laser>/<setting>` | `cmd/<device>/resp/atten/<laser>/<setting>` | `atten/<laser>/<setting> [payload]` |
| `pd` | `cmd/<device>/req/pd` | `cmd/<device>/resp/pd` | `pd [payload]` |
| `pdsettings/<channel>` | `cmd/<device>/req/pdsettings/<channel>` | `cmd/<device>/resp/pdsettings/<channel>` | `pdsettings/<channel> [payload]` |
| `temp` | `cmd/<device>/req/temp` | `cmd/<device>/resp/temp` | `temp` |
| `status` | `cmd/<device>/req/status` | `cmd/<device>/resp/status` | `status [payload]` |

## Implementation Map

This section intentionally avoids restating payload and response schemas. Use
`commands.md` for protocol behavior; this page records where the behavior lives,
which slow resources it can touch, and known implementation-specific caveats.

### `help`

- Owner: command-dispatch built-in in `lib/coo_commons/command_dispatch.c`.
- Notes: no hardware side effects, no NVS writes, no direct publish.
- Serial response prints directly from the dispatcher and bypasses the command
  queues so it can exceed the normal MQTT payload budget.
- MQTT response is intentionally compact: device ID, request prefix, response
  prefix, and command keys from built-ins and app command specs with help
  metadata.
- App-specific help text lives on the static command spec table in
  `app/src/command.c`; help entries can report commands as unsupported for the
  current board profile.

### `ip`

- Owner: `ip_get()`, `ip_set()` in `app/src/command.c`, with runtime network
  apply in `lib/coo_commons/network.c`.
- Side effects: can reconfigure IPv4, update runtime settings, optionally write
  NVS, and schedule SNTP sync after NTP setting changes.
- Blocking: DHCP waits, DNS/NTP validation, and NVS writes can block.
- Serial shorthand is described in the command spec and normalized by command
  dispatch.

### `mqtt`

- Owner: `mqtt_get()`, `mqtt_set()` in `app/src/command.c`.
- Side effects: updates runtime broker settings and optional NVS persistence;
  `main()` reconnects later and can restore the prior broker after a failed
  first connection.
- Blocking/enqueue: hostname resolution and NVS writes can block; failed first
  connection emits `mqtt_broker_revert`.
- Serial shorthand is described in the command spec and normalized by command
  dispatch.

### `time`

- Owner: `time_get()`, `time_set()` in `app/src/command.c`.
- Side effects: effect requests update Zephyr's realtime clock and persist the
  last known UTC time for boot-time restore.
- Blocking: no bus I/O, NVS writes can block, no direct publish.
- Serial shorthand is described in the command spec and normalized by command
  dispatch.

### `reboot`

- Owner: command-dispatch built-in in `lib/coo_commons/command_dispatch.c`.
- Side effects: schedules a dispatcher-owned non-cancelable `k_work_delayable`
  item, calls the app reboot-prepare hook, then calls
  `sys_reboot(SYS_REBOOT_COLD)` after the response window.
- Optional payload: `{"erase_non_ip_settings":true}` or serial shorthand
  `reboot erase_non_ip_settings` erases persisted app settings except IP
  settings and boot count immediately before reboot.
- While reboot is pending, later commands are rejected before app handlers run.

### `serialguard`

- Owner: command-dispatch built-in in `lib/coo_commons/command_dispatch.c`,
  enabled by `CONFIG_COO_CMD_SERIAL_GUARD`.
- Side effects: updates runtime-only holdoff; serial activity refreshes the
  active guard window. No NVS persistence is supported.
- Guard behavior: active guard rejects MQTT effect/action requests. The app
  command table marks which MQTT queries may pass through the guard.
- Serial shorthand is implemented in command dispatch:
  `serialguard off`, `serialguard 60`, and `serialguard seconds=60`.

### `memsroute` and `memsroute/route_loss`

- Owner: `memsroute_get()`, `memsroute_set()` in
  `app/src/mems_command.c`.
- Side effects: route changes update router-owned MEMS switch requests applied
  by `mems_router_thread()`.
- Route-loss side effects: updates app-owned route-loss records and optional
  NVS persistence under `routeloss/<route>/<name>`.
- Blocking: can lock router/settings state; no direct publish.

### `mems` and `mems/<switch>`

- Owner: `mems_get()`, `mems_set()` in `app/src/mems_command.c`, with switch
  timing owned by `app/src/mems_switching.c`.
- Side effects: updates requested switch state applied by `mems_router_thread()`.
- Enqueue: can emit `mems_rate_quantized` warnings.
- Serial shorthand remains implemented in `app/src/command.c`.

### `split`

- Owner: `splitting_get()`, `splitting_set()` in `app/src/mems_command.c`.
- Side effects: applies the three MEMS switches that make up an AS splitter
  route.
- Enqueue: can emit `split_ratio_quantized` warnings.
- Board restriction: requires routes present in the active board profile,
  normally the AS profile.

### `measure_throughput`

- Owner: `measure_throughput_set()` in `app/src/throughput_command.c` and
  `throughput_monitor_thread()` in `app/src/throughput_monitor.c`.
- Side effects: starts or stops throughput telemetry, can enable photodiode
  power, and with autolevel enabled can set attenuation and laser current.
- Enqueue: telemetry is best-effort through `outbound_queue`; command handlers
  do not publish directly.

### `laserbank/power`

- Owner: `laserbank_power()` in `app/src/laser_command.c`; bank power behavior
  lives in `app/src/lasers.c`.
- Board restriction: TIB only.
- Side effects: override-on powers the bank and waits for Maiman boot;
  override-off best-effort writes currents to 0 before powering the bank off.
- Blocking: Modbus and bank boot/off sleeps can block the command executor.

### `laserbank/clearfaults`

- Owner: `laserbank_clearfaults()` in `app/src/laser_command.c`.
- Board restriction: TIB only.
- Side effects: when the bank is powered and a driver reports overcurrent, the
  command power-cycles the bank and waits through the fault-clear and boot
  intervals.
- Classification note: the dispatch table points both internal slots at the
  action handler; ingress classifies this as a no-payload action.

### `laserbank/heater`

- Owner: `laserbank_heater()` in `app/src/laser_command.c`, persisted mode in
  `app_settings.c`, policy/cadence in `laserbank_tempcontrol.c`, and relay GPIO
  writes through `housekeeping_power_set()`.
- Board restriction: TIB only.
- Side effects: updates persisted heater mode, reschedules heater-policy
  delayable work, and can force the auxiliary heater state when override mode
  is active.
- Enqueue: heater override mode emits a best-effort warning every 20 minutes.

### `laser`

- Owner: request parsing and response shaping in `app/src/laser_command.c`;
  hardware sequencing and state live in `app/src/lasers.c`.
- Board restriction: TIB only.
- Side effects: effect requests can power the bank, program TEC/current, stop an
  active throughput monitor using that laser, and arm or reset firmware
  auto-off handled by laser-owned delayable work.
- Blocking: Maiman Modbus and bank boot/off sleeps can block.

### `laser/tune`, `laser/status`, `laser/engstatus`, `laser/settings`

- Owner: `laser_command.c` handlers with hardware work in `lasers.c` and
  app-owned persisted settings in `app_settings.c`.
- Notes: `laser/status` is the compact `laser` query alias; `laser/engstatus`
  reads raw Maiman engineering state; tune/settings can update app-owned values.
- Side effects: driver-backed settings updates temporarily power the bank if
  needed unless bank power is `override_off`.

### `atten/<laser>/value`, `atten/<laser>/valuedb`, and `atten/<laser>/coeff`

- Owner: `atten_setting_get()`, `atten_setting_set()` in
  `app/src/attenuator_command.c`; DAC behavior in `app/src/attenuator.c`.
- Board restriction: TIB supports all configured logical attenuators; CAL
  profiles support only their configured logical channel.
- Side effects: value changes block on DAC I2C; coefficient changes update
  runtime coefficients, reapply current attenuation, and can persist to NVS.
- Enqueue: value changes can emit `attenuator_clamped`.

### `pd`

- Owner: `pd_get()`, `pd_set()` in `app/src/photodiode_command.c`; sampling,
  short averages, and dark persistence in `app/src/photodiode.c`.
- Board restriction: TIB only.
- Side effects: dark measurement starts the sampler-owned short-average
  accumulator; persistence is delegated to photodiode/settings code.
- Query note: dark-status action is classified as a pure query and does not
  update `lastcommand`.

### `pdsettings/<yj|hk>`

- Owner: `pd_settings_get()`, `pd_settings_set()` in
  `app/src/photodiode_command.c`.
- Board restriction: TIB only.
- Side effects: updates runtime photodiode calibration/settings and optional
  NVS persistence.

### `temp`

- Owner: `temp_get()` in `app/src/command.c`, cached ambient state in
  `housekeeping.c`, and laser-bank temperature reads through `lasers.c`.
- Side effects: reads cached ambient state and can perform Modbus reads for
  laser TEC temperatures on TIB.

### `status`

- Owner: `status_get()` in `app/src/command.c`.
- Notes: base status is cache-oriented; optional sections embed current IP,
  laser, and attenuator data.
- Side effects: optional laser and attenuator sections can perform Modbus and
  DAC reads; large optional responses can exceed the fixed payload buffer.
