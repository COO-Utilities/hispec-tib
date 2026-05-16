# Implemented Commands

This page is derived from `app/src/command.c`. It is a comparison artifact, not
a replacement for `commands.md`.

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
- MQTT and serial share the same schema-based request classification in
  `command_infer_msg_type()`. The internal names `MSG_GET` and `MSG_SET` are
  dispatch-slot names, not user-visible protocol verbs.
- Empty/no-payload requests are queries except no-payload actions such as `reboot`
  and `laserbank/clearfaults`, plus laserbank topic-suffix actions.
- Non-empty payload requests are effect/action requests except documented query
  shapes for `status`, laser query endpoints, and `memsroute/route_loss`.
- The old MQTT `msg_type` payload convention is not used by command ingress.
- Pure queries are not recorded as `lastcommand`; known effect-capable requests
  are recorded before handler execution.
- Serial supports raw JSON, `key=value` fields, and selected shorthand forms.
- All handlers run in `command_executor_thread()` and enqueue one response to
  `outbound_queue`.
- Data-less success returns `{"status":"ok"}`. Data-bearing success returns the
  data object. Failures include an `error` key.

## Dispatch Table

| Command key | MQTT request topic | Default response topic | Serial form |
| --- | --- | --- | --- |
| `help` | `cmd/<device>/req/help` | `cmd/<device>/resp/help` | `help` |
| `ip` | `cmd/<device>/req/ip` | `cmd/<device>/resp/ip` | `ip [payload]` |
| `mqtt` | `cmd/<device>/req/mqtt` | `cmd/<device>/resp/mqtt` | `mqtt [payload]` |
| `time` | `cmd/<device>/req/time` | `cmd/<device>/resp/time` | `time [payload]` |
| `reboot` | `cmd/<device>/req/reboot` | `cmd/<device>/resp/reboot` | `reboot` |
| `serialguard` | `cmd/<device>/req/serialguard` | `cmd/<device>/resp/serialguard` | `serialguard [payload]` |
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

## Command Details

### `help`

- Query only. Payload ignored.
- Response: `{"help":"help,ip,mqtt,time,temp,status,reboot,serialguard,memsroute,mems,split,measure_throughput,laser,laserbank,atten,pd,pdsettings"}`.
- No hardware side effects, no settings writes, no direct publish.
- Handler: `help_get()` in `app/src/command.c`.
- Mismatch: the response is a static summary, not a full endpoint list or
  device-info payload.

### `ip`

- Query returns stored/manual IP settings, active IPv4 status, and NTP source.
- Effect request fields: `trydhcpfirst`, `preferdhcpdns`, `preferdhcpntp`, `ip`,
  `subnet`, `gateway`, `dns`, `ntp`, `persistent`.
- Validation: bools must parse as bools; string fields must fit fixed IPv4
  buffers; unsupported DHCP/DNS/NTP fields are reported in a data response.
- Data-less success response: `{"status":"ok"}`.
- Partial support response: `{"dhcp":"ok|unsupported","dns":"ok|unsupported","ntp":"ok|unsupported"}`.
- Side effects: applies runtime network changes through `network_reconfigure()`,
  updates runtime settings after successful apply, optional settings
  persistence, and NTP changes schedule SNTP sync.
- Blocking: runtime network reconfigure can wait for DHCP; settings writes may
  block. No direct publish.
- Handler: `ip_get()`, `ip_set()` in `app/src/command.c`.

### `mqtt`

- Query returns `broker` as `<host-or-ip>:<port>` and `dns_supported`.
- Effect request fields: `broker`, optional `persistent`.
- Validation: broker must be one `<host-or-ip>:<port>` value; hostname
  requires DNS support and must resolve before settings are updated unless
  numeric IPv4; port must be 1..65535.
- Data-less success response: `{"status":"ok"}`.
- Side effects: updates runtime broker settings; optional persistence; main
  loop reconnects later. If the new broker fails its first connection attempt,
  main restores the prior broker setting and emits `mqtt_broker_revert`.
- Blocking: hostname resolution and settings writes may block. No direct
  publish.
- Serial shorthand: `mqtt <host-or-ip>:<port> [persistent]`.
- Handler: `mqtt_get()`, `mqtt_set()` in `app/src/command.c`.

### `time`

- Query returns `utc` milliseconds from `CLOCK_REALTIME` and `uptime`
  milliseconds from `k_uptime_get()`.
- Effect request field: `linuxtime_ms`.
- Validation: `linuxtime_ms` must parse as unsigned 64-bit milliseconds.
- Data-less success response: `{"status":"ok"}`.
- Side effects: effect requests call `clock_settime()`.
- Blocking: no bus I/O; no settings writes; no direct publish.
- Serial shorthand: `time <linuxtime_ms>`.
- Handler: `time_get()`, `time_set()` in `app/src/command.c`.

### `reboot`

- No-payload action. MQTT empty payload and bare serial `reboot` schedule a
  named delayed reboot action after 250 ms.
- Payload is ignored if a caller supplies one.
- Data-less success response: `{"status":"ok"}`.
- Side effects: calls `sys_reboot(SYS_REBOOT_COLD)` from the scheduled action.
- Handler: `reboot_set()` in `app/src/command.c`.

### `serialguard`

- Query returns configured holdoff seconds, active state, and remaining ms.
- Effect request fields: `seconds` or `value`, optional `persistent`.
- Validation: seconds/value must parse as unsigned 32-bit.
- Data-less success response: `{"status":"ok"}`.
- Side effects: updates serial guard setting; optional persistence; serial
  effect requests refresh the active guard window.
- While active, serial guard rejects MQTT effect/action requests. It also blocks
  all `laserbank/*` and `laser` read-like requests even if they look read-only.
- Serial shorthand: `serialguard off`, `serialguard <seconds> [persistent]`.
- Handler: `serial_guard_get()`, `serial_guard_set()` in `app/src/command.c`.

### `memsroute`

- Query returns `{"active_routes": {"<output>":["<input>", "..."]}}`; outputs
  with no active source report `["no source"]`.
- Effect request fields: `input`, `output`.
- Validation: route must exist in current board profile and every route switch
  must exist.
- Data-less success response: `{"status":"ok"}`.
- Side effects: sets MEMS switch requested states through the router.
- Blocking/enqueue: can lock router state and update state applied by the MEMS
  router thread; no direct publish.
- Handler: `memsroute_get()`, `memsroute_set()` in `app/src/command.c`.

### `memsroute/route_loss`

- Query payload fields: `route`, `laser`.
- Effect request fields: `route`, one laser-name key containing either linear
  transmission or a string loss in dB, optional `persistent`.
- Request classification: a payload containing `laser` is treated as query;
  otherwise the request is treated as an effect request.
- Validation: route and laser names must fit fixed route-loss record buffers;
  transmission must be in `(0, 1]`; dB loss must be non-negative.
- Query response: `tx`, `loss_db`, and `configured`.
- Data-less effect success response: `{"status":"ok"}`.
- Side effects: updates one app-owned route-loss record and optionally persists
  it under `routeloss/<route>/<laser>`.
- Handler: `memsroute_get()`, `memsroute_set()` route-loss branch in
  `app/src/command.c`.

### `mems` and `mems/<switch>`

- `mems` query returns all active profile switches with compact `state` and
  `duty_cycle`.
- `mems/<switch>` query returns one switch with state, duty cycle,
  requested/actual toggle rate, and stop-after.
- `mems/<switch>` effect request fields: `state`, optional `duty_cycle`,
  `toggle_rate_hz`, `stopafter_s`.
- Validation: state is `A` or `B`; `duty_cycle` only valid with state `A`;
  `toggle_rate_hz` must be greater than zero; `stopafter_s` must be in range.
- Effect success returns the same one-switch state object rather than the global
  data-less `ok` response.
- Side effects: updates router-owned MEMS switch state applied by the MEMS
  router thread.
- Enqueue: can enqueue `mems_rate_quantized` warning.
- Serial shorthand: `mems/<switch> A [duty_cycle] [stopafter_s]`.
- Handler: `mems_get()`, `mems_set()` in `app/src/command.c`.

### `split`

- `split/<yj|hk>` query reads one splitter channel.
- `split` effect request fields: `channel`, `ratio1`, `ratio2`, optional `stopafter_s`.
- Rejected fields: `ratio3`, `toggle_rate_hz`.
- Validation: channel is `yj` or `hk`; ratios are 0.0..1.0 and sum <= 1.0.
- Effect success returns requested ratios, actual quantized ratios, switch tick
  details, and `stopsin_s`.
- Side effects: applies three MEMS switches on AS split routes.
- Enqueue: can enqueue `split_ratio_quantized` warning.
- Board restriction: requires routes present in active board profile, normally
  the AS profile.
- Handler: `splitting_get()`, `splitting_set()` in `app/src/command.c`.

### `measure_throughput`

- Action only.
- Start fields: `laser`, `fiber`, optional `autolevel`, optional
  `stopafter_s`, optional `format` with `json` or `binary`.
- Stop field: `stop` as `yj`, `hk`, or `all`.
- Data-less success response: `{"status":"ok"}`.
- Side effects: starts/stops throughput stream publication on `yj_tput` or
  `hk_tput`; can enable photodiode power and, with autolevel enabled, set
  attenuation and laser current.
- Binary telemetry is emitted as a fixed little-endian frame. JSON telemetry
  includes Unix time, channel/fiber label, flux estimates, PD windows, current
  attenuation, PD on-time, and laser-current on-time.
- Handler: `measure_throughput_set()` in `app/src/command.c` and
  `throughput_monitor_thread()` in `app/src/throughput_monitor.c`.

### `laserbank/power`

- Query returns current laser-bank power override mode and GPIO power state.
- Effect request accepts `{"override":"auto|override_on|override_off"}`,
  `{"mode":"auto|override_on|override_off"}`, raw text, or a topic suffix such
  as `laserbank/power/override_on`.
- Effect success returns the same data shape as the query.
- Board restriction: TIB only.
- Side effects: `override_on` powers the bank and waits for Maiman boot;
  `override_off` best-effort writes all currents to 0 before powering the bank
  off. `auto` returns bank power to demand-driven control.
- Handler: `laserbank_power()` in `app/src/command.c` and laser-bank domain
  helpers in `app/src/lasers.c`.

### `laserbank/clearfaults`

- No-payload action; ingress classifies this as an action. The dispatch table
  still points both internal slots at the same handler.
- Payload is ignored if supplied.
- Board restriction: TIB only.
- Side effects: if the bank is powered and any driver reports overcurrent,
  power-cycles the laser bank, sleeps for the fault-clear off interval, then
  sleeps for bank boot after re-enabling power.
- Response: `{"off_ms":0}` when no cycle was needed, or
  `{"off_ms":250}` when the power cycle was performed.
- Handler: `laserbank_clearfaults()` in `app/src/command.c`.

### `laserbank/heater`

- Query with no suffix reports heater auto/override control status.
- Effect request accepts `override`/`state` string values `auto`, `override_on`, or
  `override_off`, including topic suffixes. The misspelled `overide_*` forms
  are accepted.
- Effect success returns the same data shape as the query.
- Board restriction: TIB only.
- Side effects: updates the persisted laser-bank heater mode and wakes
  `laserbank_control_thread()`. `auto` runs the warmup policy; `override_on`
  and `override_off` force heater state from the control thread. Override mode
  emits a best-effort warning every 20 minutes.
- Response: heater mode, heater/bank state, ambient state, temperature freshness
  counts, control flags, and last error.
- Handler: `laserbank_heater()` in `app/src/command.c`.

### `laser`

- Query payload: `{"name":"<laser>"}`. Returns compact operational status.
- Effect payload: `{"name":"<laser>","level":0..100,"autooff_s":<optional>}`.
- Request classification: payloads with `level` are effect requests; payloads without
  `level` are queries.
- Data-less effect success response: `{"status":"ok"}`.
- Board restriction: TIB only.
- Side effects: effect requests can power the bank, program TEC/current, stop an active
  throughput monitor using that laser, and arm/reset firmware auto-off.
- Handler: `laser_get()`, `laser_set()` in `app/src/command.c`; hardware work
  is delegated to `app/src/lasers.c`.

### `laser/tune`, `laser/status`, `laser/engstatus`, `laser/settings`

- `laser/tune` query/effect requests manage the persisted wavelength tune offset. Payloads
  with `tune_nm` or `delta_nm` are effect requests; name-only payloads are queries.
- `laser/status` is an alias of the compact `laser` query.
- `laser/engstatus` returns raw Maiman engineering status and measured driver
  values; unavailable numeric values are JSON `null`.
- `laser/settings` query/effect requests manage app-owned diode settings. Payloads with a
  nested `settings` object are effect requests; name-only payloads are queries.
- Data-less effect success response: `{"status":"ok"}`.
- Driver-backed updates temporarily power the bank if needed, unless bank power
  is `override_off`.
- Handlers: `laser_tune_*()`, `laser_status_get()`,
  `laser_engstatus_get()`, and `laser_settings_*()` in `app/src/command.c`.

### `atten/<laser>/value` and `atten/<laser>/valuedb`

- Query returns total `db`, total `linear` transmission, both physical DAC
  voltages, and both physical modeled dB values.
- Effect request field: `value` float.
- `value` sets total linear transmission in `(0, 1]`; `valuedb` sets total
  attenuation dB.
- Data-less effect success response: `{"status":"ok"}`.
- Board restriction: TIB supports all logical channels below `NUM_ATTENUATORS`;
  CAL profiles support only logical channel 4.
- Side effects: blocks on DAC I2C and can clamp DAC range.
- Enqueue: can enqueue `attenuator_clamped` warning.
- Handler: `atten_setting_get()`, `atten_setting_set()` in
  `app/src/command.c`.

### `atten/<laser>/coeff`

- Query returns `dac1` and `dac2` coefficient arrays.
- Effect request fields: `dac1[2]`, `dac2[2]`, optional `persistent`.
- Validation: both arrays must contain exactly two floats: slope and offset for
  `b = slope * voltage + offset`.
- Data-less effect success response: `{"status":"ok"}`.
- Side effects: updates runtime coefficients, reapplies current attenuation,
  and optionally persists coefficients.
- Blocking: DAC I2C and settings writes may block.
- Handler: `atten_setting_get()`, `atten_setting_set()` in
  `app/src/command.c`.

### `pd`

- Query has no documented payload and returns power values, errors, raw counts,
  mV, noise, rolling windows, and uptime.
- Action request fields: `action`, `channel` or key suffix, plus action-specific fields.
- Actions:
  - `measure_dark`: optional `duration_ms`, optional `store`.
  - `dark_status`: no additional fields.
  - `reset_lowest_dark`: optional `persistent`.
- `measure_dark` and `dark_status` return dark-measurement state/result data.
- `reset_lowest_dark` returns `{"status":"ok"}` on success.
- Board restriction: TIB only.
- Side effects: `measure_dark` starts sampler-owned dark calibration state;
  `dark_status` is a pure query; optional persistence is performed by
  photodiode/settings code.
- Handler: `pd_get()`, `pd_set()` in `app/src/command.c`.

### `pdsettings/<yj|hk>`

- Query returns channel dark settings, lowest dark, dark measurement state,
  noise warning threshold, and gain.
- Effect request fields: optional `persistent` plus at least one of `dark_mv`,
  `noise_rms_mV`, `gain_v_p_uw`.
- Validation: dark is -5000..5000 mV; noise is 0..5000 mV; gain is
  0.000001..1000000000.
- Data-less effect success response: `{"status":"ok"}`.
- Board restriction: TIB only.
- Side effects: updates runtime photodiode settings and optional persistence.
- Handler: `pd_settings_get()`, `pd_settings_set()` in `app/src/command.c`.

### `temp`

- Query only.
- Response: `ambient_c`, `laserbank_c`, and a `laser` object keyed by laser
  name. Unavailable numeric values are JSON `null`.
- Side effects: reads cached ambient state and can perform Modbus reads for
  laser TEC temperatures on TIB.
- Handler: `temp_get()` in `app/src/command.c`.

### `status`

- Query only. Payload may request optional sections: `ip`, `lasers`, `attens`.
- Base response fields: firmware version, boot count, board type/validity,
  MEMS switch count, relay GPIO error, ambient temperature, PD power on-time,
  laser-bank power on-time, and `lastcommand`.
- `laserbank_ontime` is integer seconds from runtime-only
  `laserbank_control` tracking of the current bank-power interval.
- Optional `ip` embeds the `ip` query response as JSON.
- Optional `lasers` reads each laser status and reports `power_mw`,
  `tec_on_time_s`, and `offin_s`.
- Optional `attens` reads each available logical attenuator and reports
  `level_%`.
- Side effects: optional sections can perform Modbus and DAC reads.
- Handler: `status_get()` in `app/src/command.c`.
