# Implemented Commands

This page is derived from `app/src/command.c`. It is a comparison artifact, not
a replacement for `commands.md`.

## Global Rules

- MQTT request prefix: `cmd/hsfib-tib/req/`.
- Default MQTT response prefix: `cmd/hsfib-tib/resp/`.
- MQTT `response_topic` overrides the default when supplied and fitting the
  fixed buffer.
- MQTT `correlation_data` up to 16 bytes is copied into a fixed static buffer
  and echoed exactly in responses.
- While serial guard is active, safe MQTT GETs are accepted, but MQTT SET/action
  commands and legacy side-effect GET handlers are rejected.
- Empty MQTT payload is GET.
- Non-empty MQTT payload is SET unless it includes `msg_type:"get"`.
- Serial `<key>` is GET.
- Serial `<key> <payload>` is SET.
- Serial supports raw JSON, `key=value` fields, and selected shorthand forms.
- All handlers run in `command_executor_thread()` and enqueue one response to
  `outbound_queue`.

## Dispatch Table

| Command key | MQTT request topic | Default response topic | Serial form |
| --- | --- | --- | --- |
| `help` | `cmd/hsfib-tib/req/help` | `cmd/hsfib-tib/resp/help` | `help` |
| `ip` | `cmd/hsfib-tib/req/ip` | `cmd/hsfib-tib/resp/ip` | `ip [payload]` |
| `mqtt` | `cmd/hsfib-tib/req/mqtt` | `cmd/hsfib-tib/resp/mqtt` | `mqtt [payload]` |
| `time` | `cmd/hsfib-tib/req/time` | `cmd/hsfib-tib/resp/time` | `time [payload]` |
| `reboot` | `cmd/hsfib-tib/req/reboot` | `cmd/hsfib-tib/resp/reboot` | `reboot <payload>` |
| `serialguard` | `cmd/hsfib-tib/req/serialguard` | `cmd/hsfib-tib/resp/serialguard` | `serialguard [payload]` |
| `memsroute` | `cmd/hsfib-tib/req/memsroute` | `cmd/hsfib-tib/resp/memsroute` | `memsroute [payload]` |
| `mems` | `cmd/hsfib-tib/req/mems` | `cmd/hsfib-tib/resp/mems` | `mems` |
| `mems/<switch>` | `cmd/hsfib-tib/req/mems/<switch>` | `cmd/hsfib-tib/resp/mems/<switch>` | `mems/<switch> [payload]` |
| `split/<channel>` | `cmd/hsfib-tib/req/split/<channel>` | `cmd/hsfib-tib/resp/split/<channel>` | `split/<channel> [payload]` |
| `laserbank/poweron` | `cmd/hsfib-tib/req/laserbank/poweron` | `cmd/hsfib-tib/resp/laserbank/poweron` | `laserbank/poweron [payload]` |
| `laserbank/poweroff` | `cmd/hsfib-tib/req/laserbank/poweroff` | `cmd/hsfib-tib/resp/laserbank/poweroff` | `laserbank/poweroff [payload]` |
| `laserbank/clearfaults` | `cmd/hsfib-tib/req/laserbank/clearfaults` | `cmd/hsfib-tib/resp/laserbank/clearfaults` | `laserbank/clearfaults [payload]` |
| `laser/...` | `cmd/hsfib-tib/req/laser/...` | `cmd/hsfib-tib/resp/laser/...` | `laser/... [payload]` |
| `atten/<laser>/<setting>` | `cmd/hsfib-tib/req/atten/<laser>/<setting>` | `cmd/hsfib-tib/resp/atten/<laser>/<setting>` | `atten/<laser>/<setting> [payload]` |
| `pd` | `cmd/hsfib-tib/req/pd` | `cmd/hsfib-tib/resp/pd` | `pd [payload]` |
| `pdsettings/<channel>` | `cmd/hsfib-tib/req/pdsettings/<channel>` | `cmd/hsfib-tib/resp/pdsettings/<channel>` | `pdsettings/<channel> [payload]` |
| `temp` | `cmd/hsfib-tib/req/temp` | `cmd/hsfib-tib/resp/temp` | `temp` |
| `status` | `cmd/hsfib-tib/req/status` | `cmd/hsfib-tib/resp/status` | `status` |

## Command Details

### `help`

- GET only. Payload ignored.
- Response: `{"help":"help,ip,mqtt,time,temp,status,reboot,serialguard,memsroute,mems,split,laser,laserbank,power,atten,pd,pdsettings"}`.
- No hardware side effects, no settings writes, no direct publish.
- Handler: `help_get()` in `app/src/command.c`.
- Mismatch: help text is implementation-derived, not a full copy of
  `commands.md`.

### `ip`

- GET returns stored/manual IP settings, active IPv4 status, and NTP source.
- SET fields: `trydhcpfirst`, `preferdhcpdns`, `preferdhcpntp`, `ip`,
  `subnet`, `gateway`, `dns`, `ntp`, `persistent`.
- Validation: bools must parse as bools; string fields must fit fixed IPv4
  buffers; unsupported DHCP/DNS/NTP fields return partial status.
- Response: success with `apply:"reboot_required"` for network changes,
  `apply:"immediate"` for NTP-only changes, or partial support status.
- Side effects: updates runtime settings; optional settings persistence; NTP
  changes schedule SNTP sync.
- Blocking: settings writes may block. No direct publish.
- Handler: `ip_get()`, `ip_set()` in `app/src/command.c`.

### `mqtt`

- GET returns `broker` as `<host-or-ip>:<port>` and `dns_supported`.
- SET fields: `broker`, optional `persistent`.
- Validation: broker must be one `<host-or-ip>:<port>` value; hostname
  requires DNS support unless numeric IPv4; port must be 1..65535.
- Response: `{"status":"success","apply":"reconnect"}`.
- Side effects: updates runtime broker settings; optional persistence; main
  loop reconnects later.
- Blocking: settings writes may block. No direct publish.
- Serial shorthand: `mqtt <host-or-ip>:<port> [persistent]`.
- Handler: `mqtt_get()`, `mqtt_set()` in `app/src/command.c`.

### `time`

- GET returns UTC ms, cycle ticks, uptime, and SNTP status.
- SET field: `linuxtime_ms`.
- Validation: `linuxtime_ms` must parse as unsigned 64-bit milliseconds.
- Side effects: SET calls `clock_settime()`.
- Blocking: no bus I/O; no settings writes; no direct publish.
- Serial shorthand: `time <linuxtime_ms>`.
- Handler: `time_get()`, `time_set()` in `app/src/command.c`.

### `reboot`

- SET schedules a named delayed reboot action after 250 ms.
- GET is unsupported, so empty MQTT payload or bare serial `reboot` does not
  reboot.
- SET payload is not parsed; any non-empty MQTT payload dispatches SET.
- Response: `{"status":"success"}` or schedule error.
- Side effects: calls `sys_reboot(SYS_REBOOT_COLD)` from the scheduled action.
- Handler: `reboot_set()` in `app/src/command.c`.
- Mismatch: intended docs read like a no-payload action.

### `serialguard`

- GET returns configured holdoff seconds, active state, and remaining ms.
- SET fields: `seconds` or `value`, optional `persistent`.
- Validation: seconds/value must parse as unsigned 32-bit.
- Side effects: updates serial guard setting; optional persistence; serial SET
  refreshes the active guard window.
- While active, serial guard rejects MQTT SET/action commands. Safe read-only
  MQTT GETs are allowed; laser-bank power and raw laser register GETs remain
  blocked because those legacy GET handlers can have side effects.
- Serial shorthand: `serialguard off`, `serialguard <seconds> [persistent]`.
- Handler: `serial_guard_get()`, `serial_guard_set()` in `app/src/command.c`.

### `memsroute`

- GET returns `{"active_routes": {"<output>":["<input>", "..."]}}`; outputs
  with no active source report `["no source"]`.
- SET fields: `input`, `output`.
- Validation: route must exist in current board profile and every route switch
  must exist.
- Side effects: sets MEMS switch requested states through the router.
- Blocking/enqueue: can lock router state and schedule MEMS delayable work; no
  direct publish.
- Handler: `memsroute_get()`, `memsroute_set()` in `app/src/command.c`.

### `mems` and `mems/<switch>`

- `mems` GET returns all active profile switches.
- `mems/<switch>` GET returns one switch.
- `mems/<switch>` SET fields: `state`, optional `duty_cycle`,
  `toggle_rate_hz`, `stopafter_s`.
- Validation: state is `A` or `B`; `duty_cycle` only valid with state `A`;
  `toggle_rate_hz` must be greater than zero; `stopafter_s` must be in range.
- Response: state, duty cycle, requested and quantized toggle rate, stop-after.
- Side effects: updates router-owned MEMS switch state and schedules toggler.
- Enqueue: can enqueue `mems_rate_quantized` warning.
- Serial shorthand: `mems/<switch> A [duty_cycle] [stopafter_s]`.
- Handler: `mems_get()`, `mems_set()` in `app/src/command.c`.

### `split/<yj|hk>`

- GET channel can come from key or payload field `channel`.
- SET fields: `ratio1`, `ratio2`, optional `channel`, optional `stopafter_s`.
- Rejected fields: `ratio3`, `toggle_rate_hz`.
- Validation: channel is `yj` or `hk`; ratios are 0.0..1.0 and sum <= 1.0.
- Response: requested ratios, actual quantized ratios, switch tick details, and
  `stopsin_s`.
- Side effects: applies three MEMS switches on AS split routes.
- Enqueue: can enqueue `split_ratio_quantized` warning.
- Board restriction: requires routes present in active board profile, normally
  the AS profile.
- Handler: `splitting_get()`, `splitting_set()` in `app/src/command.c`.

### `laserbank/poweron`

- GET and SET both call the same side-effect handler.
- Payload ignored.
- Board restriction: TIB only.
- Side effects: enables laser-bank power GPIO and sleeps 1000 ms when it
  transitions on.
- Response: `status`, `laser_power`, `transitioned`.
- Handler: `laserbank_poweron()` in `app/src/command.c`.

### `laserbank/poweroff`

- GET and SET both call the same side-effect handler.
- Payload ignored.
- Board restriction: TIB only.
- Side effects: disables laser-bank power GPIO.
- Response: `status`, `laser_power`, `was_powered`, `transitioned`.
- Handler: `laserbank_poweroff()` in `app/src/command.c`.

### `laserbank/clearfaults`

- GET and SET both call the same side-effect handler.
- Payload ignored.
- Board restriction: TIB only.
- Side effects: power-cycles the laser bank, sleeps for the fault-clear off
  interval, then sleeps 1000 ms if power is re-enabled.
- Response: `status`, `laser_power`, `was_powered`, `off_ms`,
  `fault_detection`.
- Handler: `laserbank_clearfaults()` in `app/src/command.c`.

### `laser/...`

- Intended handler reads or writes one raw Maiman 16-bit register.
- SET field: `value` as uint16.
- Accepted register names come from `maiman_get_register_address()`, including
  `CURRENT`, `FREQUENCY`, `DURATION`, `LOCK_STATUS`, measured current/voltage
  and temperature registers, TEC registers, PID coefficients, and aliases in
  `maiman.c`.
- Board restriction: TIB only.
- Side effects: powers on the laser bank if needed, sleeps 1000 ms on power
  transition, then performs blocking Modbus I/O.
- Handler: `laser_setting_get()`, `laser_setting_set()` in `app/src/command.c`.
- Mismatch: current key parsing likely prevents valid documented laser topics
  from resolving to a laser id.

### `atten/<laser>/value` and `atten/<laser>/valuedb`

- GET returns total `db`, total `linear` transmission, both physical DAC
  voltages, and both physical modeled dB values.
- SET field: `value` float.
- `value` sets total linear transmission in `(0, 1]`; `valuedb` sets total
  attenuation dB.
- Board restriction: TIB supports all logical channels below `NUM_ATTENUATORS`;
  CAL profiles support only logical channel 4.
- Side effects: blocks on DAC I2C and can clamp DAC range.
- Enqueue: can enqueue `attenuator_clamped` warning.
- Handler: `atten_setting_get()`, `atten_setting_set()` in
  `app/src/command.c`.

### `atten/<laser>/coeff`

- GET returns `dac1` and `dac2` coefficient arrays.
- SET fields: `dac1[2]`, `dac2[2]`, optional `persistent`.
- Validation: both arrays must contain exactly two floats: slope and offset for
  `b = slope * voltage + offset`.
- Side effects: updates runtime coefficients, reapplies current attenuation,
  and optionally persists coefficients.
- Blocking: DAC I2C and settings writes may block.
- Handler: `atten_setting_get()`, `atten_setting_set()` in
  `app/src/command.c`.

### `pd`

- GET field: optional `unit` with `power` or `volts`; for MQTT this requires
  `msg_type:"get"` if payload is non-empty.
- GET response includes YJ/HK values, errors, raw counts, mV, noise, and uptime.
- SET fields: `action`, `channel` or key suffix, plus action-specific fields.
- Actions:
  - `measure_dark`: optional `duration_ms`, optional `store`.
  - `dark_status`: no additional fields.
  - `reset_lowest_dark`: optional `persistent`.
- Board restriction: TIB only.
- Side effects: starts or reads sampler-owned dark calibration state; optional
  persistence is performed by photodiode/settings code.
- Handler: `pd_get()`, `pd_set()` in `app/src/command.c`.

### `pdsettings/<yj|hk>`

- GET returns channel dark settings, lowest dark, dark measurement state,
  noise warning threshold, and gain.
- SET fields: optional `persistent` plus at least one of `dark_mv`,
  `noise_rms_mV`, `gain_v_p_uw`.
- Validation: dark is -5000..5000 mV; noise is 0..5000 mV; gain is
  0.000001..1000000000.
- Board restriction: TIB only.
- Side effects: updates runtime photodiode settings and optional persistence.
- Handler: `pd_settings_get()`, `pd_settings_set()` in `app/src/command.c`.

### `temp`

- GET only.
- Response: valid ambient temperature and age, or error with `last_error`.
- Side effects: none; reads cached state from temperature thread.
- Handler: `temp_get()` in `app/src/command.c`.
- Mismatch: documented alarm set behavior is not implemented.

### `status`

- GET only.
- Response: firmware version, boot count, uptime, board type/validity, MEMS
  switch count, network ready/IP, and laser power.
- Side effects: none.
- Handler: `status_get()` in `app/src/command.c`.
- Mismatch: `commands.md` documents a much larger payload.
