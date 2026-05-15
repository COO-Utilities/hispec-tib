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

- Any response may be: `{"status":"error", "msg":<error message>}`

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

1. `command_serial_thread()` reads one console line.
2. `command_parse_serial_line()` splits the line into `<key>` and optional payload.
3. `normalize_serial_payload()` turns non-JSON serial payloads into the same JSON shape used by MQTT.
4. `command_executor_thread()` dispatches the command through `dispatch_command()`.
5. `command_drain_outbound_queue()` prints serial responses with `print_serial_response()`.

Query form is just the key:

```text
status
mems/yj_cal_laser
split/yj
```

Set form is the key followed by a payload. There are no `get` or `set`
keywords in the serial command set.

```text
serialguard seconds=60
mems/yj_cal_laser state=A duty_cycle=0.5 toggle_rate_hz=17 stopafter_s=30
split channel=yj ratio1=0.33 ratio2=0.33 stopafter_s=300
laserbank/poweron
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
- [`lasersettings`](#lasersettings)
- [`laserbank/poweron`](#laserbank-poweron)
- [`laserbank/poweroff`](#laserbank-poweroff)
- [`laserbank/clearfaults`](#laserbank-clearfaults)
- [`laserbank/heater`](#laserbank-heater)
- [`atten/<laser>/value`](#atten)
- [`atten/<laser>/valuedb`](#atten)
- [`atten/<laser>/coeff`](#atten)
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
- Warnings: `dt/<device>/warning`

---

Command items below are **one request topic** (subscribed by the device) and
their **matching response topic** (published by the device). The warning topic
is publish-only.

### Warning Publication
- **Publish topic:** `dt/<device>/warning`
- **Top-level helper:** `app_warning_emit()`
- **Queue behavior:** best-effort MQTT through `OUT_TARGET_MQTT_BEST_EFFORT`;
  logs locally and drops if MQTT is unavailable or the outbound queue is full.
- **Payload:**
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

### `help`
- **Request topic:** `cmd/<device>/req/help`
  - No payload

- **Response topic:** `cmd/<device>/resp/help`
  - Result: `{"help": <a nice string of commands and info, in essence a summary of this file>}`

### `memsroute`
- **Request topic:** `cmd/<device>/req/memsroute`
  - Set:
  ```json
      {"input":"<source>",
        "output":"<dest>"
      }
  ```
  - Query: No payload

- **Response topic:** `cmd/<device>/resp/memsroute`
  - Set result: `{"status":"OK"}`
  - Query result:
    ```json
    {
      "active_routes": {
        "<dest.name>": ["<source.name>", "..."]
      }
    }
    ```
    The response lists every destination present in the active board route
    table. Each value is an array of currently connected sources because one
    destination may receive multiple sources through combining optics. A
    destination with no currently active source reports `["no source"]`.
    Active routes are read from current switch state and are not persisted.

(route-loss)=
### `memsroute/route_loss`
- **Request topic:** `cmd/<device>/req/memsroute/route_loss`
  - Set one route-loss record:
    ```json
    {"route":"yj_sm_to_yj_pd","1430yj":0.93,"persistent":true}
    ```
    or:
    ```json
    {"route":"yj_sm_to_yj_pd","1430yj":"0.32 dB","persistent":true}
    ```
  - Query one route-loss record:
    ```json
    {"route":"yj_sm_to_yj_pd","laser":"1430yj"}
    ```
- **Response topic:** `cmd/<device>/resp/memsroute/route_loss`
  - Set result: `{"status":"success"}`
  - Query result:
    `{"status":"success","tx":0.93,"loss_db":0.3188,"configured":true}`

Route-loss records are app settings keyed by route name and laser name. Missing route-loss
records are treated as loss-free transmission, `tx = 1.0`. Numeric values are
linear transmission in `(0, 1]`. Strings ending in `dB`, `db`, or `DB` are route
loss in dB and convert to `tx = 10^(-loss_db / 10)`. Route loses are only used on the TIB for throughput monitoring. 


### `mems`
- **Request topic:** `cmd/<device>/req/mems`
  - Query: No payload
- **Response topic:** `cmd/<device>/resp/mems/`
  - Query result:
    ```text
    {
      "<switchname>":{
        "state":"A|B|A?|B?|?",
        "duty_cycle":0.0
      },
      ...
    }
    ```
    The all-switch query is intentionally compact and includes only state and
    duty cycle so the TIB eight-switch response fits the fixed MQTT payload
    buffer. Use `mems/<switchname>` for requested/actual toggle rate and
    stop-after details.

(mems-switchname)=
### `mems/<switchname>`
- **Request topic:** `cmd/<device>/req/mems/<switchname>`
  - Query: No payload
  - Set:
    - Static: `{"state":"A"}` or `{"state":"B"}`
    - Toggle: `{"state":"A","duty_cycle":[0.0-1.0],"toggle_rate_hz":<hz>,"stopafter_s":<seconds>}`
    - `duty_cycle` is only valid with `state:"A"`.
    - `toggle_rate_hz` is optional; if omitted the switch uses its current requested toggle rate.
    - Requested `toggle_rate_hz` is stored separately from the actual firmware-quantized rate.
    - `{"state":"A","duty_cycle":0.0}` is valid and equivalent to static `B`.
    - `stopafter_s` max is 4 hours

- **Response topic:** `cmd/<device>/resp/mems/<switchname>`
  - Query/set result:
    ```json
    {
      "state":"A|B|A?|B?",
      "duty_cycle":0.0,
      "requested_toggle_rate_hz":0.0,
      "toggle_rate_hz":0.0,
      "stopafter_s":0
    }
    ```
    `?` suffix means the state has not yet been pulsed this boot, note that on first boot all switches wil be reported as A?
    duty_cycle, toggle_rate_hz, stopafter_s are omitted if not toggling.
    `toggle_rate_hz` is the actual quantized rate. If it differs from requested
    by more than rounding noise, the firmware emits `mems_rate_quantized` on
    `dt/<device>/warning`.


(measure-throughput)=
### `measure_throughput`
- **Request topic:** `cmd/<device>/req/measure_throughput`
  - Payload:
    ```json
    {
      "autolevel": true,
      "laser": "<lasername>",
      "fiber": "M",
      "stopafter_s": 300,
      "format": "json"
    }
    ```
    or
    ```json
    {
      "stop": "yj"
    }
    ```
- **Response topic:** `cmd/<device>/resp/measure_throughput`
  - Start or stop result: `{"status":"success"}`
  - Error result: `{"status":"error","msg":"<error message>"}`

`measure_throughput` is the only command that starts or stops photodiode
streaming. It measures throughput by comparing the route-corrected flux at the
selected photodiode with the route- and attenuator-corrected laser flux
estimate.

`autolevel:true` lets firmware adjust the selected laser drive current and
logical attenuator to keep the photodiode signal in the useful ADC/photodiode
range. `autolevel:false` streams the selected photodiode level and derived
values without changing laser current or attenuation.

Firmware uses precomputed photodiode response values for each supported
laser/photodiode combination; it does not interpolate wavelength curves at
runtime. The photodiode sampler owns ADC reads and dark tracking. The
throughput monitor owns streaming output, autolevel decisions, and throughput
math.

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

**Notes (behavior):**
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
  current. Flux is decreased by increasing logical attenuation first, then
  lowering laser current.
- At start with `autolevel:true`, attenuation is set to maximum before laser
  power is raised.
- Starting a monitor powers the required photodiode and laser-bank outputs as
  needed. Shutting down the required photodiode power stops that monitor.
- Changing the monitored laser output or its logical attenuator disables
  autolevel for the affected monitor; run the command again to re-enable it.
- Dark measurement must not be started while an autolevel throughput monitor is
  running on that photodiode.


### `laser`
- **Request topic:** `cmd/<device>/req/laser`
  - Set:
    ```json
    {"name": "<lasername>",
      "level": 0.0,
      "unit": "mw"
     }
    ```

    `unit` is `"mw" | "%"`.

  - Query: `{"name": "<lasername>"}`

- **Response topic:** `cmd/<device>/resp/laser`
  - Set result: `{"status": "success"}`
  - Query result:
    ```json
    {
    "name": "<lasername>",
    "powered": true,
    "timeon_s": 0.0,
    "totaltimeon_s": 0.0,
    "timeemitting_s": 0.0,
    "temp": 0.0,
    "current_ma": 0.0,
    "power_%": 0.0,
    "power_mw": 0.0,
    "wavelength_nm": 0.0,
    "attenuated_power_mw": 0.0,
    "tec_ma": 0.0,
    "voltage": 0.0,
    "tec_v": 0.0,
    "overcurrent_fault": false
    }
    ```

- **Notes:** turns on laser (+ laser bank power) as needed; validates range; restarts laser auto-off timer. `timeon_s` is time with TEC running.


### `lasersettings`
- **Request topic:** `cmd/<device>/req/lasersettings`
  - Set:
    ```text
    {     "name": <lasername>,
          "settings": {
            "nominal_current_ma": 0.0,
            "max_current_ma": 0.0,
            "efficiency_mw_per_ma": 0.0,
            "wavelength_nm": 0.0,
            "operating_temp_c": 0.0,
            "tec_pid": {
              "p": 0.0,
              "i": 0.0,
              "d": 0.0
            },
            "dlambda_dT_nm_per_k": 0.0,
            "dlambda_dA_nm_per_ma": 0.0,
            "autooff_s": 0.0
          }
    }
    ```

- Query: `{"name": "<lasername>" }`

- **Response topic:** `cmd/<device>/resp/lasersettings`
  - Set result: `{"status": "success"}`
  - Query result:
    ```json
    {
          "name": "<lasername>",
          "settings": {
            "name": "<string>",
            "model": "<string>",
            "nominal_current_ma": 0.0,
            "max_current_ma": 0.0,
            "dne_current_ma": 0.0,
            "threshold_current_ma": 0.0,
            "efficiency_mw_per_ma": 0.0,
            "wavelength_nm": 0.0,
            "test_monitor_current_ua": 0.0,
            "operating_temp_range_c": {
              "min": 0.0,
              "max": 0.0
            },
            "operating_temp_c": 0.0,
            "thermistor_kohm": 0.0,
            "isolation_db": 0.0,
            "tec_max_current_a": 0.0,
            "tec_pid": {
              "p": 0.0,
              "i": 0.0,
              "d": 0.0
            },
            "ntc_t_coefficient_per_c": 0.0,
            "dlambda_dT_nm_per_k": 0.0,
            "dlambda_dA_nm_per_ma": 0.0,
            "autooff_s": 0.0
          }
        }
    ```

- **Notes:**
  - settings are kept in sync with the driver when powered
  - driver will not be powered just to update settings
  - some settings require a drive restart (stops emission + any throughput measurement using that laser)
  - failures to set will leave all settings unchanged
  - Unsettable (attempts to set are silently ignored):
    - `name`
    - `model`
    - `dne_current_ma`
    - `threshold_current_ma`
    - `test_monitor_current_ua`
    - `operating_temp_range_c`
    - `thermistor_kohm`
    - `isolation_db`
    - `tec_max_current_a`
    - `ntc_t_coefficient_per_c`

(laserbank-poweron)=
### `laserbank/poweron`
- **Request topic:** `cmd/<device>/req/laserbank/poweron`
  - Payload: optional; empty payload is accepted.
- **Top-level handler:** `laserbank_poweron()`
- **Response topic:** `cmd/<device>/resp/laserbank/poweron`
  - Response: `{"status":"OK","laser_power":true,"transitioned":true|false}`

- **Notes:** powers on the TIB laser-bank power GPIO; does nothing if already
  powered. Heater auto mode may also power the bank for temperature monitoring.

(laserbank-poweroff)=
### `laserbank/poweroff`
- **Request topic:** `cmd/<device>/req/laserbank/poweroff`
- **Top-level handler:** `laserbank_poweroff()`
- **Response topic:** `cmd/<device>/resp/laserbank/poweroff`
  - Response: `{"status":"OK","laser_power":false,"was_powered":true|false,"transitioned":true|false}`


(laserbank-clearfaults)=
### `laserbank/clearfaults`
- **Request topic:** `cmd/<device>/req/laserbank/clearfaults`
- **Top-level handler:** `laserbank_clearfaults()`
- **Response topic:** `cmd/<device>/resp/laserbank/clearfaults`
  - Response: `{"status":"OK","laser_power":true,"was_powered":true|false,"off_ms":250,"fault_detection":"power_cycle_only"}`

- **Notes:** currently performs a bounded power cycle of the TIB laser-bank
  power GPIO. Overcurrent-specific fault detection is not wired yet because the
  `maiman.h` status bit for that condition is not defined.


(laserbank-heater)=
### `laserbank/heater`
- **Request topic:** `cmd/<device>/req/laserbank/heater`
  - Query: no payload
  - Set: `{"override":"auto|override_on|override_off"}` or topic suffix
    `laserbank/heater/auto|override_on|override_off`
- **Response topic:** `cmd/<device>/resp/laserbank/heater`
  - Response:
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

### `atten`
- **Top-level handlers:** `atten_setting_get()`, `atten_setting_set()`
- **Request topic:** `cmd/<device>/req/atten/<laser>/value`
  - Set total linear transmission through the logical attenuator:
    ```json
    {"value": 0.25}
    ```
  - Query:
    empty payload

- **Request topic:** `cmd/<device>/req/atten/<laser>/valuedb`
  - Set total attenuation in dB:
    ```json
    {"value": 12.5}
    ```
  - Query:
    empty payload

- **Request topic:** `cmd/<device>/req/atten/<laser>/coeff`
  - Set the linear model coefficients for the two physical attenuators that
    make up the logical attenuator:
    ```json
    {
      "dac1": [0.001953125, 0.0],
      "dac2": [0.001953125, 0.0],
      "persistent": true
    }
    ```
  - Query:
    empty payload

- **Response topic:** `cmd/<device>/resp/atten/<laser>/<setting>`
  - Value query:
    `{"db":12.5000,"linear":0.0562,"voltage1":1234.0000,"voltage2":0.0000,"db1":12.5000,"db2":0.0000}`
  - Coeff query: `{"dac1":[slope,offset],"dac2":[slope,offset]}`
  - Set result: `{"status":"OK"}` or `{"status":"OK","persistent":true}`

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

### `pd`
- **Request topic:** `cmd/<device>/req/pd`
  - Optional payload: `{"unit": "power"}`

    `unit` is `"power" | "volts"` (case-insensitive), defaults to `"power"`.
  - Measure dark without storing:
    ```json
    {
      "action": "measure_dark",
      "channel": "yj",
      "duration_ms": 1280,
      "store": false
    }
    ```
  - Measure dark and persist it:
    ```json
    {
      "action": "measure_dark",
      "channel": "hk",
      "duration_ms": 1280,
      "store": true
    }
    ```
  - Retrieve dark measurement progress/result:
    ```json
    {
      "action": "dark_status",
      "channel": "yj"
    }
    ```
  - Reset lowest-ever dark tracking:
    ```json
    {
      "action": "reset_lowest_dark",
      "channel": "yj",
      "persistent": true
    }
    ```

- **Response topic:** `cmd/<device>/resp/pd`
  - `measure_dark` start response:
    ```json
    {
      "status": "measuring",
      "channel": "yj",
      "stored_on_complete": true,
      "duration_ms": 60000,
      "samples": 0,
      "target_samples": 3000
    }
    ```
  - `dark_status` complete response includes the measured mean/RMS/min/max.
  ```json
  {
    "unit": "power",
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

- **Notes:**
  - `measure_dark` starts or restarts the selected channel's dark measurement
    and returns immediately with `status:"measuring"`.
  - Dark level is updated only after an explicit `measure_dark` with
    `store:true` completes.
  - `duration_ms` is rounded to the nearest supported sample count at the
    monitor thread cadence. The response reports both actual `duration_ms` and
    exact `samples`.
  - `dark_status` returns `status:"measuring"`, `status:"complete"`, or
    `status:"error"`. Complete results include measured mean/RMS/min/max.
  - `measure_dark` with `store:false` leaves stored calibration unchanged; its
    completed statistics are available through `dark_status`.
  - `lowest_dark_mv` is updated only when a stored dark measurement is lower
    than the previous stored lowest value.
  - Active monitoring tracks a simple residual RMS after smoothing. If it
    exceeds the configured warning threshold, the firmware emits
    `photodiode_noise` on `dt/<device>/warning`.
  - Power estimates subtract stored dark mV and use `gain_v_p_uw`.

### `pdsettings`
- **Request topic:** `cmd/<device>/req/pdsettings/<yj|hk>`
  - Set:
      ```json
      {
        "noise_rms_mV": 3.0,
        "dark_mv": 0.0,
        "gain_v_p_uw": 47500.0,
        "persistent": true
      }
      ```
  - Get: No payload

- **Response topic:** `cmd/<device>/resp/pdsettings/<yj|hk>`
  - Set result: `{"status": "success"}`
  - Get result:
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
      "gain_v_p_uw": 47500.0
    }
    ```

- **Current set fields:**
  - `dark_mv`
  - `noise_rms_mV`
  - `gain_v_p_uw`
  - `persistent`

- **Notes:** not all settings need to be included when setting; failure on any
  settable setting results in none being set. YJ and HK settings use separate
  command keys and separate persistent settings keys. Dark and lowest-dark
  values are persisted through the settings subsystem.

### `ip`
- **Request topic:** `cmd/<device>/req/ip`
  - Set:
    ```json
    {   "ip": "<ip>",
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

    - Query: No payload

- **Response topic:** `cmd/<device>/resp/ip`
  - Set result:
     ```json
     {
       "status": "success",
       "apply": "immediate"
     }
     ```
     Unsupported capability fields return `status:"partial"` with per-feature
     `dhcp`, `dns`, and `ntp` values.
  - Query result:
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

- **Notes:**
  - Unsupported features don’t error; supported changes are still applied and
    partial status reports unsupported fields.
  - IP precedence: runtime settings → compiled static defaults → fallback
    service profile.
  - If `trydhcpfirst` is true and DHCP is compiled in, DHCP is tried before the
    runtime static profile.
  - partial comes with keys indicating which settings are not supported.
  - network-affecting changes are applied at runtime; ordinary changes do not
    require reboot.
  - source names are: `unknown`, `compiled`, `static`, `fallback`, `dhcp`.

### `mqtt`
- **Request topic:** `cmd/<device>/req/mqtt`
  - Set:
    ```json
    {
      "broker": "<ipv4-or-hostname>:<port>",
      "persistent": true
    }
    ```
  - Query: No payload

- **Response topic:** `cmd/<device>/resp/mqtt`
  - Set result: `{"status":"success","apply":"reconnect"}`
  - Query result:
    ```json
    {"broker":"<value>:<port>", "dns_supported":true}
    ```

- **Notes:**
  - Broker value must be one `<host-or-ip>:<port>` string.
  - If DNS is not compiled in, hostname values are rejected.
  - Hostname values must resolve before settings are updated. Numeric IPv4
    broker values do not require DNS.
  - Successful set updates runtime settings and triggers MQTT reconnect
    behavior. If the new broker cannot connect, firmware restores the prior
    broker and emits a best-effort `mqtt_broker_revert` warning.

### `serialguard`
- **Request topic:** `cmd/<device>/req/serialguard`
  - Set:
    ```json
    {
      "seconds": 30,
      "persistent": true
    }
    ```
    `value` is accepted as an alias for `seconds`.
  - Query: No payload

- **Response topic:** `cmd/<device>/resp/serialguard`
  - Set result: `{"status":"success"}`
  - Query result:
    ```json
    {"serialguard_s":30, "active":true, "remaining_ms":12000}
    ```

- **Notes:**
  - Any non-empty serial command activates or refreshes the guard.
  - Serial shorthand: `serialguard seconds=60` or `serialguard off`.
  - While active, MQTT SET/action commands are rejected before dispatch and
    logged. Safe read-only MQTT GETs are allowed. Legacy GET handlers with
    side effects, including laser-bank power and raw laser register reads, stay
    blocked under serial guard until those command shapes are corrected.
  - The guard uses the named scheduled action `serial_guard_expire`.
  - `seconds:0` disables serial override.

### `time`
- **Request topic:** `cmd/<device>/req/time`
  - Query: No payload
  - Set: `{"linuxtime_ms": 0}`

- **Response topic:** `cmd/<device>/resp/time`
  - Query result:
    ```json
    {     "utc": 0,
          "ticks": 0,
          "uptime": 0
     }
    ```
  - Set result: `{"status": "success"}`

- **Notes:** set time may be overwritten later by NTP if configured and responding.

### `temp`
- **Request topic:** `cmd/<device>/req/temp`
  - Query: No payload
  - Set alarm: `{"alarm_level": 0.0}`

- **Response topic:** `cmd/<device>/resp/temp`
  - Query result: `{"ambient_c": 0.0, "laserbankavg_c": NaN| 0.0, "laser[name]_c": NaN| 0.0}`
  - Set result: `{"status": "success"}`

- **Notes:** if above alarm level, all commands except this one return an alarm error. Laserbank temperature is not available if power is off or laser TEC is running.

### `status`
- **Request topic:** `cmd/<device>/req/status`
  - Optional payload:
    ```json
    {   "ip": true,
        "lasers": true,
        "attens": true
      }
    ```
    - **Note:** ip lasers and attens are not included unless requested, key is not required

- **Response topic:** `cmd/<device>/resp/status`
  ```json
  {   "fwversion": "<githash>",
      "bootcount": 0,
      "board_type": "tib|cal_yj|cal_hk|as|unknown",
      "board_valid": true,
      "mems_switches": 8,
      "relay_gpio_error": 0,
      "ip": "<response of ip command query>",
      "temp_c": 0.0,
      "pd_ontime": 0,
      "pd_offin_s": 0,
      "laserbank_ontime": 0,
      "laserbank_offin_s": 0,
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


### `reboot`
- **Request topic:** `cmd/<device>/req/reboot`
- **Response topic:** `cmd/<device>/resp/reboot`
  - Response: `{"status": "success"}`

### `split`
- **Request topic:** `cmd/<device>/req/split`
  - Set:
    ```json
    {
      "channel": "yj",
      "ratio1": 0.0,
      "ratio2": 0.0,
      "stopafter_s": 0
    }
    ```
  - Query one channel: `cmd/<device>/req/split/yj` or
    `cmd/<device>/req/split/hk` with no payload
  - Only available when the AS board strap is selected. The AS board registers
    routes `yj_calin -> yj_split` and `hk_calin -> hk_split`.

- **Response topic:** `cmd/<device>/resp/split` for set, or
  `cmd/<device>/resp/split/<channel>` for per-channel query
  - Set result: same shape as query result.
  - Query result:
    ```json
    {
      "status": "success",
      "channel": "yj",
      "requested_ratio": [0.33, 0.33, 0.34],
      "actual_ratio": [0.33, 0.33, 0.34],
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
  - `requested_ratio` and `actual_ratio` are arrays ordered as
    `[ratio1, ratio2, ratio3]`.
  - Each switch report gives the selected route state, the selected-state
    duty-cycle float, and the exact integer timing as
    `numerator / denominator` ticks with `tick_ms` milliseconds per tick.
  - If the attained ratio differs from the requested ratio because MEMS timing
    is quantized, the firmware emits `split_ratio_quantized` on
    `dt/<device>/warning`.
