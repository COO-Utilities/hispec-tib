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
  - **On responses (device → publisher):**
    - `correlation_data`: copied from the request when it is 16 bytes or less
    - `qos`: response QoS
- Commands have serial port duals. Serial commands use a simpler line format
  for interactive bring-up and debugging.

## Serial Command Form

Serial commands are one line, whitespace separated, and intended for a human at
the console.

Top-level implementation path:

1. `coo_cmd_runtime_serial_thread()` reads one console line.
2. `coo_cmd_runtime_handle_serial_line()` splits the line into `<key>` and optional payload.
3. `coo_cmd_normalize_serial_payload()` turns non-JSON serial payloads into the same JSON shape used by MQTT.
4. `coo_cmd_runtime_executor_thread()` dispatches the command through the app command table.
5. `coo_cmd_runtime_drain_outbound()` prints serial responses with `coo_cmd_print_serial_response()`.

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
mems/yj_cal_laser state=A duty_cycle=0.5 toggle_rate_hz=17 stopafter_s=30
split channel=yj ratio1=0.33 ratio2=0.33 stopafter_s=300
laserbank/power/override_on
```

Payload rules:

- A payload beginning with `{` is copied unchanged into `Command.payload`; it is
  not parsed and rebuilt by the serial layer.
- Payloads containing `=` use `serial_payload_from_key_values()`, for example
  `state=A stopafter_s=30`.
- Known compact forms use `serial_payload_from_shorthand()`, for example
  `serialguard off` or `mems/yj_cal_laser A 0.5 30`.
- Handlers parse and validate the normalized JSON exactly as they do for MQTT.

Serial response format:

```text
cmd/<device>/resp/<key>
        {"same":"payload MQTT would publish, wrapped at print time"}
```

The first line is the MQTT response topic. The following lines are the response
payload, tab-indented and wrapped at 80 columns by `print_serial_response()`.

Any non-empty serial command refreshes serial override. While active, MQTT
commands are rejected before dispatch and receive an error response when MQTT is
available. The override expires after `serialguard_s` seconds without another
serial command; `serialguard off` or `serialguard seconds=0` disables the
override. JSON payloads are accepted for MQTT parity, but should not be needed
for normal serial operation.

---

## Command Endpoints
- [`help`](#help)
- [`memsroute`](#memsroute)
- [`memsroute/route_loss`](#route-loss)
- [`mems`](#mems)
- [`mems/<switchname>`](#mems-switchname)
- [`measure_throughput`](#measure-throughput)
- [`laser`](#laser)
- [`laser/tune`](#laser-tune)
- [`laser/status`](#laser-status)
- [`laser/engstatus`](#laser-engstatus)
- [`laser/settings`](#laser-settings)
- [`laserbank/power`](#laserbank-power)
- [`laserbank/clearfaults`](#laserbank-clearfaults)
- [`laserbank/heater`](#laserbank-heater)
- [`atten/<laser>/value`](#atten-value)
- [`atten/<laser>/valuedb`](#atten-valuedb)
- [`atten/<laser>/coeff`](#atten-coeff)
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

---

(warning-publication)=
### Warning Publication
- **Publish topic:** `dt/<device>/warning`
- **Top-level helper:** `coo_cmd_runtime_warning_emit(command_runtime_get(), code, msg, context)`
- **Queue behavior:** best-effort MQTT through `OUT_TARGET_MQTT_BEST_EFFORT`;
  logs locally and drops if MQTT is unavailable or the outbound queue is full.
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

Warnings do not imply command failure unless the command response also reports
an error. Warning delivery is intentionally lossy and is not mirrored into
sticky status fields. Current warning emitters include MQTT command rejection
while serial guard is active and attenuator DAC-range clamping.

(help)=
### `help`
- **No payload -> command summary:**
  ```json
  {"help":"<summary of commands and device info>"}
  ```

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
    "output": "<dest>"
  }
  ```
- **Notes:** The no-payload response lists every destination present in the active
  board route table. Each value is an array of currently connected sources
  because one destination may receive multiple sources through combining optics.
  A destination with no currently active source reports `["no source"]`. Active
  routes are read from current switch state and are not persisted.

(route-loss)=
### `memsroute/route_loss`
- **Payload:** set one route-loss record.
  ```json
  {
    "route": "yj_sm_to_yj_pd",
    "1430yj": 0.93,
    "persistent": true
  }
  ```
  or:
  ```json
  {
    "route": "yj_sm_to_yj_pd",
    "1430yj": "0.32 dB",
    "persistent": false
  }
  ```
  or:
  ```json
  {
    "route": "yj_calin_to_yj_split",
    "split": ["0.32 dB", "0.32 dB", 0.93],
    "persistent": true
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
      "state": "A|B|A?|B?|?",
      "duty_cycle": 0.0
    }
  }
  ```
- **Notes:** The all-switch query is intentionally compact and includes only
  state and duty cycle so the TIB eight-switch response fits the fixed MQTT
  payload buffer. Use `mems/<switchname>` for requested/actual toggle rate and
  stop-after details.

(mems-switchname)=
### `mems/<switchname>`
- **Topic:** `cmd/<device>/req/mems/<switchname>`
- **No payload or payload -> one MEMS switch state.**

  Static payload:
  ```json
  {"state":"A"}
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
    "toggle_rate_hz": 17,
    "stopafter_s": 30
  }
  ```

  Response:
  ```json
  {
    "state": "A|B|A?|B?",
    "duty_cycle": 0.0,
    "requested_toggle_rate_hz": 0.0,
    "toggle_rate_hz": 0.0,
    "stopafter_s": 0
  }
  ```
- **Notes:**
  - `duty_cycle` is only valid with `state:"A"`.
  - `toggle_rate_hz` is optional; if omitted the switch uses its current
    requested toggle rate.
  - Requested `toggle_rate_hz` is stored separately from the actual
    firmware-quantized rate.
  - `{"state":"A","duty_cycle":0.0}` is valid and equivalent to static `B`.
  - `stopafter_s` max is 4 hours.
  - `?` suffix means the state has not yet been pulsed this boot; on first boot
    all switches will be reported as `A?`.
  - `duty_cycle`, `toggle_rate_hz`, and `stopafter_s` are omitted if not
    toggling.
  - `toggle_rate_hz` is the actual quantized rate. If it differs from requested
    by more than rounding noise, the firmware emits `mems_rate_quantized` on
    `dt/<device>/warning`.


(measure-throughput)=
### `measure_throughput`
- **Payload:** start monitoring.
  ```json
  {
    "autolevel": true,
    "laser": "<lasername>",
    "fiber": "M",
    "stopafter_s": 300,
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

Firmware uses each photodiode channel's configured `responsivity_a_per_w` and
`transimpedance_v_per_a` from `pdsettings/<yj|hk>` with the active laser
wavelength estimate. It does not interpolate wavelength curves at runtime. The
photodiode sampler owns ADC reads and dark tracking. The throughput monitor
owns streaming output, autolevel decisions, and throughput math.

**Telemetry topics (published):**
- `dt/<device>/yj_tput`
- `dt/<device>/hk_tput`

**Telemetry payload (`format:"json"`):**
```json
{
  "channel": "yj_m",
  "laser": "1430yj",
  "autolevel": true,
  "time": 0,
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
  "pd_mean_mv_1s": 0.0,
  "pd_rms_mv_0p5s": 0.0,
  "laser_current_ma": 0.0,
  "atten_db": 0.0,
  "wavelength_nm": 1430.0,
  "pd_ontime_s": 0.0,
  "laser_current_ontime_s": 0.0,
  "flags": []
}
```

`channel` combines the photodiode channel and fiber class with an underscore,
for example `yj_m`, `yj_s`, `hk_m`, or `hk_s`. `time` is Unix time in
milliseconds from the firmware clock. `pd_ontime_s` is the tracked on-time of
the photodiode power relay for that channel since boot; it does not infer
pre-boot relay state.

**Telemetry payload (`format:"binary"`):**

Binary telemetry is little-endian and contains the fields below in order. The
first field is a zero-padded 8-byte ASCII channel/fiber label such as `yj_m`.

```text
char[8] channel
uint64 time_ms
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
float32 pd_mv
float32 pd_net_mv
float32 pd_mean_mv_1s
float32 pd_rms_mv_0p5s
float32 laser_current_ma
float32 atten_db
float32 wavelength_nm
float32 pd_ontime_s
float32 laser_current_ontime_s
```

**Notes:**
- `tp` is unitless. `NaN` means offscale or insufficient information; values
  above unity are reported rather than clamped.
- Flux values are photons per second.
- Route transmissions default to `1.0` when no route-loss record is stored.
- Both outbound laser route loss and inbound photodiode route loss are applied
  when estimating throughput.
- The current firmware lookup names the inbound photodiode route as
  `<yj|hk>_<mm|sm>_to_<yj|hk>_pd` from `fiber:"M"|"S"` and the outbound laser
  route as `<lasername>_to_<M|S>`. These names are route-loss record keys, not
  MEMS route-table entries.
- The 1 s mean controls autolevel. Below 20% usable range, firmware requests
  3x flux. Above 80%, it requests 1/3 flux.
- Five consecutive saturated samples or five consecutive below-dark samples
  trigger immediate autolevel adjustment.
- Flux is raised by decreasing logical attenuation first, then raising laser
  output level percent. Flux is decreased by increasing logical attenuation
  first, then lowering laser output level percent.
- At start with `autolevel:true`, attenuation is set to maximum before laser
  power is raised.
- Starting a monitor powers the required photodiode and laser-bank outputs as
  needed. Shutting down the required photodiode power stops that monitor.
- Changing the monitored laser output or its logical attenuator disables
  autolevel for the affected monitor; run the command again to re-enable it.
- Dark measurement must not be started while an autolevel throughput monitor is
  running on that photodiode.


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
    "tec_on_s": 0,
    "emit_on_s": 0,
    "emit_total_s": 0,
    "temp_c": 0.0,
    "current_ma": 0.0,
    "level": 0.0,
    "power_mw": 0.0,
    "nominal_nm": 0.0,
    "tuned_nm": 0.0,
    "tune_nm": 0.0,
    "tec_ma": 0.0,
    "diode_v": 0.0,
    "tec_v": 0.0,
    "offin_s": 0,
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
  the laser bank as needed, prepares the TEC, sets the laser current, and restarts the auto-off timer. Setting level
  0 stops emission and writes driver current to 0. Laser output current is never persisted by app settings. The Maiman
  driver may retain its own current register, so firmware writes 0 whenever emission is disabled or the bank is turned off.
  `tec_on_s`, `emit_on_s`, and `emit_total_s` are firmware-owned counters. `emit_total_s` is persisted when emission
  stops cleanly. `autooff_s` is optional and non-persistent; if supplied, it overrides the default configured through
  `laser/settings` for this start.

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
  The request is stored by firmware and used on future positive `laser` level
  commands. It is best-effort: large shifts are clamped by the TEC temperature
  range and allowed current adjustment.


(laser-status)=
### `laser/status`
- **Payload -> compact operational status:**
  ```json
  {"name":"<lasername>"}
  ```
  Response shape is the same as `laser` query.

Compact operational status. This is the preferred status payload for normal users and is the source used by the
optional laser section of `status`.

(laser-engstatus)=
### `laser/engstatus`
- **Payload -> detailed engineering status:**
  ```json
  {"name":"<lasername>"}
  ```

Detailed engineering status derived from the Maiman status query used in `refrence_docs_examples/lasers.py`.
Includes raw state, lock, and TEC-state registers, measured diode/TEC voltage and current, driver limits, PID,
serial/device-id verification, and interlock flags. This command may be slower than `laser/status` because it reads
many Modbus registers.


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
      "emit_total_s": 0.0
    }
  }
  ```
- **Payload:** update laser settings.
  ```json
  {
    "name": "<lasername>",
    "settings": {
      "nominal_current_ma": 0.0,
      "max_current_ma": 0.0,
      "efficiency_mw_per_ma": 0.0,
      "wavelength_nm": 0.0,
      "current_set_calibration_pct": 0.0,
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
    }
  }
  ```

- **Notes:**
  - It is the user's responsibility to ensure the triple of (nominal_current_ma, default_operating_temp_c, wavelength_nm) are aligned and in sync as these form the basis of wavelength tuning
  - Settings are checked when a laser is first talked to at each boot
  - A mismatch between those the driver stores in its eeprom and controllers NVRAM will trigger a warning in the log and the driver values will be programmed.
  - If the laser bank is off, firmware powers it, applies driver-backed settings, verifies them as practical, and then
    restores the previous bank power state. If `laserbank/power` is `override_off`, driver-backed settings changes return
    an error.
  - it is **encouraged** to send only the settings that requested changed.
  - The overcurrent threshold is the maximum current the driver will allow the laser to run at and requires physically 
    adjusting a potentiometer on the driver. It has a (weak) temperature dependence and is not a fixed value.
  - Changes to settings will disable laser emission and may disable the TEC (stops emission + any throughput measurement using that laser)
  - failures to set will leave all settings unchanged (rollback is performed or an error emitted)
  - Unsettable (attempts to set are silently ignored):
    - `name`,`model`, `serial`
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
    - TEC max current is clipped to datasheet maximums that are compiled in.


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
  {"override":"auto|override_on|override_off"}
  ```
  Suffix requests use
  `cmd/<device>/req/laserbank/power/auto`,
  `cmd/<device>/req/laserbank/power/override_on`, or
  `cmd/<device>/req/laserbank/power/override_off`.

- **Notes:** `auto` is the default at boot. In `auto`, power to the laser bank is handled by the bank heater and commands
  interacting with laser drivers. `override_on` forces bank power on. `override_off` stops all laser emission, writes
  driver currents to 0 as practical, and powers the bank off; commands that need a live driver return an error while the
  override is active.

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


(laserbank-heater)=
### `laserbank/heater`
- **No payload -> laser-bank heater state:**
  ```json
  {
    "heater_mode": "auto|override_on|override_off",
    "heater_on": false,
    "bank_power": true,
    "ambient_valid": true,
    "ambient_c": 0.0,
    "valid_temps": 6,
    "stale_temps": 0,
    "any_disabled_below_15c": false,
    "any_disabled_above_off_threshold": false,
    "all_tecs_enabled": false,
    "all_tecs_enabled_ms": 0,
    "last_error": 0,
    "last_poll_age_ms": 0
  }
  ```
- **Payload or topic suffix -> laser-bank heater state after update:**
  ```json
  {"override":"auto|override_on|override_off"}
  ```
  Suffix requests use
  `cmd/<device>/req/laserbank/heater/auto`,
  `cmd/<device>/req/laserbank/heater/override_on`, or
  `cmd/<device>/req/laserbank/heater/override_off`.

- **Notes:** `auto` is the default at boot. In `auto`, the TIB-only control
  thread powers the laser bank so the Maiman temperature monitors can
  initialize, polls TEC temperatures at a fixed interval, and drives the
  laser-bank heater. Any disabled TEC below 15 C turns the heater on. Any
  disabled TEC above the ambient-dependent off threshold turns it off. If all
  TECs remain enabled for at least one control interval, the heater is turned
  off. `override_on` and `override_off` force the heater state and suspend the
  automatic warmup policy. While a heater override is active, firmware emits
  `laserbank_heater_override` on `dt/<device>/warning` every 20 minutes.
  If the off-board DS2408 relay expander is offline, set requests return an I/O
  error because the heater relay cannot be driven.

(atten)=
(atten-value)=
(atten-valuedb)=
(atten-coeff)=
### `atten`
- **Top-level handlers:** `atten_setting_get()`, `atten_setting_set()`
- **Topics:**
  - `cmd/<device>/req/atten/<laser>/value`
  - `cmd/<device>/req/atten/<laser>/valuedb`
  - `cmd/<device>/req/atten/<laser>/coeff`
  - Responses use the same key under `cmd/<device>/resp/...`.
- **No payload to `value` or `valuedb` -> attenuator setting:**
  ```json
  {
    "db": 12.5,
    "linear": 0.0562,
    "voltage1": 1234.0,
    "voltage2": 0.0,
    "db1": 12.5,
    "db2": 0.0
  }
  ```
- **Payload to `value`:** set total linear transmission through the logical
  attenuator.
  ```json
  {"value":0.25}
  ```
- **Payload to `valuedb`:** set total attenuation in dB.
  ```json
  {"value":12.5}
  ```
- **No payload to `coeff` -> model coefficients:**
  ```json
  {"dac1":[0.001953125,0.0],"dac2":[0.001953125,0.0]}
  ```
- **Payload to `coeff`:** set the linear model coefficients for the two physical
  attenuators that make up the logical attenuator.
  ```json
  {
    "dac1": [0.001953125, 0.0],
    "dac2": [0.001953125, 0.0],
    "persistent": true
  }
  ```

- **Notes:**
  - `<laser>` is one of `1028y`, `1430yj`, `1430hk`, `1510h`, or `2330k`.
  - Each logical attenuator is a pair of physical FVOAs. Total set commands use
    the full modeled range of the first physical attenuator before using the
    second, and override any individual physical set point made through the C
    attenuator API.
  - `value` is a unitless linear transmission fraction in `(0, 1]`.
  - Coefficients are loaded from persistent settings during
    `setup_attenuators()`. They define `b = slope * voltage + offset` for the
    attenuation model `transmission = (erf(4) + erf(4 - b)) / (2 * erf(4))`.
  - `persistent` is optional and defaults to false. A non-persistent coefficient
    update changes runtime behavior until reboot or a later coefficient command.
  - There is no separate `attensettings` command; calibration coefficients live
    on `atten/<laser>/coeff`.

(pd)=
### `pd`
- **No payload -> photodiode values:**
  ```json
  {
    "yjvalue": 0.0,
    "yjvalue_err": 0.0,
    "hkvalue": 0.0,
    "hkvalue_err": 0.0,
    "yj_raw": 0,
    "hk_raw": 0,
    "yj_mv": 0.0,
    "hk_mv": 0.0,
    "yj_noise_rms_mv": 0.0,
    "hk_noise_rms_mv": 0.0,
    "yj_mean_mv_1s": 0.0,
    "hk_mean_mv_1s": 0.0,
    "yj_rms_mv_0p5s": 0.0,
    "hk_rms_mv_0p5s": 0.0,
    "uptime": 0
  }
  ```
- **Payload -> dark measurement state:** measure dark without storing.
  ```json
  {
    "action": "measure_dark",
    "channel": "yj",
    "duration_ms": 1280,
    "store": false
  }
  ```
  Response:
  ```json
  {
    "state": "measuring",
    "channel": "yj",
    "stored_on_complete": true,
    "duration_ms": 60000,
    "samples": 0,
    "target_samples": 3000
  }
  ```
- **Payload -> dark measurement state:** measure dark and persist it.
  ```json
  {
    "action": "measure_dark",
    "channel": "hk",
    "duration_ms": 1280,
    "store": true
  }
  ```
- **Payload -> dark measurement progress/result:** complete results include the
  measured mean/RMS/min/max.
  ```json
  {
    "action": "dark_status",
    "channel": "yj"
  }
  ```
- **Payload:** reset lowest-ever dark tracking.
  ```json
  {
    "action": "reset_lowest_dark",
    "channel": "yj",
    "persistent": true
  }
  ```

- **Notes:**
  - `measure_dark` starts or restarts the selected channel's dark measurement
    and returns immediately with `state:"measuring"`.
  - Dark level is updated only after an explicit `measure_dark` with
    `store:true` completes.
  - `duration_ms` is rounded to the nearest supported sample count at the
    monitor thread cadence. The response reports both actual `duration_ms` and
    exact `samples`.
  - `dark_status` returns `state:"measuring"`, `state:"complete"`, or an error
    response. Complete results include measured mean/RMS/min/max.
  - `measure_dark` with `store:false` leaves stored calibration unchanged; its
    completed statistics are available through `dark_status`.
  - `lowest_dark_mv` is updated only when a stored dark measurement is lower
    than the previous stored lowest value.
  - Active monitoring tracks a simple residual RMS after smoothing. If it
    exceeds the configured warning threshold, the firmware emits
    `photodiode_noise` on `dt/<device>/warning`.
  - Power estimates subtract stored dark mV and use `responsivity_a_per_w` and
    `transimpedance_v_per_a`.

(pdsettings)=
### `pdsettings`
- **Topic:** `cmd/<device>/req/pdsettings/<yj|hk>`
- **No payload -> one channel's photodiode settings:**
  ```json
  {
    "channel": "yj",
    "dark_mv": 0.0,
    "lowest_dark_mv": 0.0,
    "lowest_dark_valid": false,
    "dark_measurement": "idle",
    "dark_measurement_duration_ms": 0,
    "dark_measurement_samples": 0,
    "dark_measurement_target_samples": 0,
    "noise_rms_mV": 3.0,
    "responsivity_a_per_w": 0.93,
    "transimpedance_v_per_a": 5.0e10
  }
  ```
- **Payload:** update one channel's photodiode settings.
  ```json
  {
    "noise_rms_mV": 3.0,
    "dark_mv": 0.0,
    "responsivity_a_per_w": 0.93,
    "transimpedance_v_per_a": 5.0e10,
    "persistent": true
  }
  ```

- **Current set fields:**
  - `dark_mv`
  - `noise_rms_mV`
  - `responsivity_a_per_w`
  - `transimpedance_v_per_a`
  - `persistent`

- **Notes:** not all settings need to be included when setting; failure on any
  settable setting results in none being set. YJ and HK settings use separate
  command keys and separate persistent settings keys. Dark and lowest-dark
  values are persisted through the settings subsystem.

(ip)=
### `ip`
- **No payload -> IP configuration:**
  ```json
  {
    "source": "<source>",
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
      "source": "<source>",
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
    "persistent": true
  }
  ```

- **Notes:**
  - Unsupported features don’t error; supported changes are still applied and
    partial status reports unsupported fields.
  - IP precedence: runtime settings → compiled static defaults → fallback
    service profile.
  - If `trydhcpfirst` is true and DHCP is compiled in, DHCP is tried before the
    runtime static profile.
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
    "persistent": true
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
  {"serialguard_s":30,"active":true,"remaining_ms":12000}
  ```
- **Payload:** update serial guard configuration.
  ```json
  {
    "seconds": 30,
    "persistent": true
  }
  ```
  `value` is accepted as an alias for `seconds`.

- **Notes:**
  - Any non-empty serial command activates or refreshes the guard.
  - Serial shorthand: `serialguard seconds=60` or `serialguard off`.
  - While active, MQTT requests that may change hardware or runtime state are
    rejected before dispatch and logged. Safe read-only MQTT requests are
    allowed.
  - The guard uses the named scheduled action `serial_guard_expire`.
  - `seconds:0` disables serial override.

(time)=
### `time`
- **No payload -> firmware time:**
  ```json
  {
    "utc": 0,
    "uptime": 0
  }
  ```
- **Payload:** set firmware time.
  ```json
  {"linuxtime_ms":0}
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
  temperatures.

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
    "fwversion": "<githash>",
    "bootcount": 0,
    "board_type": "tib|cal_yj|cal_hk|as|unknown",
    "board_valid": true,
    "mems_switches": 8,
    "relay_gpio_error": 0,
    "ip": "<response of ip command query>",
    "temp_c": 0.0,
    "pd_ontime": 0,
    "laserbank_ontime": 0,
    "lasers": {
      "<lasername>": {
        "power_mw": 0.0,
        "tec_on_time_s": 0,
        "offin_s": 0
      }
    },
    "attens": {
      "<attenname>": {
        "level_%": 0.0
      }
    },
    "lastcommand": {
      "name": "<cmdname>",
      "source": "mqtt",
      "time": 0
    }
  }
  ```
- **Notes:** `ip`, `lasers`, and `attens` are omitted unless requested.


(reboot)=
### `reboot`
- **No payload:** schedule a reboot.

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
    "ratio1": 0.0,
    "ratio2": 0.0,
    "stopafter_s": 0
  }
  ```
- **No payload to `split/yj` or `split/hk` -> get splitter state.**
- **Availability:** only available when the AS board strap is selected.

  Response:
  ```json
  {
    "channel": "yj",
    "ratio_ask": [0.33, 0.33, 0.34],
    "ratio_actual": [0.33, 0.33, 0.34],
    "ratio_out": [0.33, 0.33, 0.34],
    "split_transmission": [1.0, 1.0, 1.0],
    "switches": [
      {
        "name": "yj_as1",
        "state": "A",
        "duty_cycle": 0.33,
        "numerator": 33,
        "denominator": 100,
        "tick_ms": 2
      },
      {
        "name": "yj_as2",
        "state": "B",
        "duty_cycle": 1.0,
        "numerator": 100,
        "denominator": 100,
        "tick_ms": 2
      },
      {
        "name": "yj_as3",
        "state": "A",
        "duty_cycle": 0.66,
        "numerator": 66,
        "denominator": 100,
        "tick_ms": 2
      }
    ],
    "stopsin_s": 0
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
  - The user sets only `ratio1` and `ratio2`, both as floats from `0.0` to
    `1.0`. They must sum to `<= 1.0`; `ratio3` is computed internally as the
    remaining fraction.
  - `ratio1` maps to the direct branch selected by SW1. The remaining light is
    sent through the downstream branch. SW2 is held on the splitter branch.
    SW3's selected-state numerator is `ratio1 + ratio2`, so its output-2
    interval starts after SW1's output-1 deadtime.
  - Users cannot set `toggle_rate_hz` for `split`. The firmware uses the
    fastest period allowed by `MEMS_SWITCH_MAX_TOGGLE_HZ`, then quantizes the
    requested ratios to integer MEMS ticks.
  - `ratio_ask`, `ratio_actual`, `ratio_out`, and `split_transmission` are
    arrays ordered as `[ratio1, ratio2, ratio3]`.
  - `ratio_ask` is the requested output split. `ratio_actual` is the MEMS duty
    split after transmission correction and integer tick quantization.
    `ratio_out` is the estimated optical output split after applying
    `split_transmission`.
  - Each switch report gives the selected route state, the selected-state
    duty-cycle float, and the exact integer timing as
    `numerator / denominator` ticks with `tick_ms` milliseconds per tick.
  - If the attained ratio differs from the requested ratio because MEMS timing
    is quantized, the firmware emits `split_ratio_quantized` on
    `dt/<device>/warning`.
  - The route-loss split tuple sets `split_transmission`. Set all three split
    transmissions to the same value, or leave them unset, to disable relative
    split correction.
