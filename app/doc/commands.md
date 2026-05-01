# Control and Telemetry Interface Specification
Draft 0.1


## MQTT Topics (roles)

- **Device subscribes:** `cmd/<device>/req/#` *(all request endpoints live under this prefix)*
- **Device publishes (responses):** `cmd/<device>/resp/...`
- **Device publishes (telemetry):** `dt/<device>/...`
- **Device publishes (warnings):** `dt/<device>/warning`

### Global (applies to all commands)

- Any response may be: `{"status":"error", "msg":<error message>}`

- Req/resp also use MQTT5 request/response metadata:
  - **On requests (publisher → device):**
    - `response_topic`: where the device should publish the response (this doc assumes it’s under `cmd/<device>/resp/...`)
    - `correlation_data`: opaque bytes echoed back in the response so the requester can match replies to requests
  - **On responses (device → publisher):**
    - `correlation_data`: copied from the request
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
power on
```

Payload rules:

- A payload beginning with `{` is copied unchanged into `Command.payload`; it is
  not parsed and rebuilt by the serial layer.
- Payloads containing `=` use `serial_payload_from_key_values()`, for example
  `state=A stopafter_s=30`.
- Known compact forms use `serial_payload_from_shorthand()`, for example
  `power on`, `serialguard off`, or `mems/yj_cal_laser A 0.5 30`.
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
- `help`
- `memsroute`
- `mems`
- `mems/<switchname>`
- `measure_tput`
- `laser`
- `lasersettings`
- `laserbank/poweron`
- `laserbank/poweroff`
- `laserbank/clearfaults`
- `atten`
- `attensettings`
- `pd`
- `pdsettings`
- `ip`
- `mqtt`
- `serialguard`
- `time`
- `temp`
- `status`
- `reboot`
- `split`
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
an error. Current warning emitters include MQTT command rejection while serial
guard is active and attenuator DAC-range clamping.

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
    TODO flip this so it is dest:source and dest that have no source are "no source"
    {"active_routes": {
            "<source.name>":"<dest.name>"
      }
    }
     ```

### `mems`
- **Request topic:** `cmd/<device>/req/mems`
  - Query: No payload
- **Response topic:** `cmd/<device>/resp/mems/`
  - Query result:
    ```json
    {
      "<switchname>":{
        "state":"A|B|A?|B?|?",
        "duty_cycle":0.0,
        "toggle_rate_hz":0.0,
        "stopafter_s":0
      },
      ...
    }
    ```
    Note duty_cycle, toggle_rate_hz, stopafter_s are omitted if not toggling. 
    TODO this response bloated to likely beyond what is reasonable MQTT 

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


### `measure_tput`
- **Request topic:** `cmd/<device>/req/measure_tput`
  - Payload:
    ```json
    {
        "autolevel": true,
        "laser": "<lasername>",
        "fiber": "<fibername>",
        "stopafter_s": 300,
        "format": "json"|"binary"
      }
    ```
    or
    ```json
    {
    "stop": "yj"|"hk"|"all"
    }
    ```
- **Response topic:** `cmd/<device>/resp/measure_tput`
  - Result: `{"status": "success"}`
    Causes telemetry to be published on `yj_tput` or `hk_tput` topic (or stops it).


**Telemetry topics (published):**
- `dt/<device>/yj_tput`
- `dt/<device>/hk_tput`

**Telemetry payload:**
  ```json
  {
    "tp": 0.0,
    "tp_err": 0.0,
    "laser": 0.0,
    "at1": 0.0,
    "at2": 0.0,
    "adc": 0.0,
    "time": 0,
    "waveelngth_nm": 0.0,
    "fiber": "M"|"S"
  }
  ```
  or 35 bytes of binary data

  ```binary
    {
      float64_t tp,
      float64_t tp_err,
      uint16_t laser,
      uint16_t at1,
      uint16_t at2,
      uint16_t adc,
      uint64_t time,
      uint16_t wavelength_nm,
      char fiber
    }
  ```

*(The `yj` vs `hk` stream depends on the requested laser.)*

**Notes (behavior):**
- throughput (tp) is in [0-1], though NaN is offscale, greater than unity would indicate a noise artifact
- Units for values are TBD
- Turning on any other laser to that photodiode disables measurement.
- Switching the MEMS route may impact the measurement but will not stop it.
- Changing laser power/attenuation disables autolevel; run the command again to re-enable.
- Powers photodiodes and lasers as needed.
- Auto-off timing:
  - PD: updates no sooner than `max(stopafter_s, pd_autooff_s)`
  - Laserbank: updates no sooner than `max(stopafter_s, laserbank_autooff_s)`
  - Laser: updates no sooner than `max(stopafter_s, laser_autooff_s)`

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
    ```json
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

### `laserbank/poweron`
- **Request topic:** `cmd/<device>/req/laserbank/poweron`
  - Payload: `{"autooff_s": 0.0}`
- **Response topic:** `cmd/<device>/resp/laserbank`
  - Response: `{"status": "success"}`

- **Notes:** powers on the laser bank + starts TECs; does nothing if already powered (no reinit); restarts laserbank auto-off timer; bank will not power down while a laser is emitting.

### `laserbank/poweroff`
- **Request topic:** `cmd/<device>/req/laserbank/poweroff`
- **Response topic:** `cmd/<device>/resp/laserbank`
  - Response: `{"status": "success"}`


### `laserbank/clearfaults`
- **Request topic:** `cmd/<device>/req/laserbank/clearfaults`
- **Response topic:** `cmd/<device>/resp/laserbank`
  - Response: `{"status": "success"}`

- **Notes:** cycles power on the laser bank if any drive is in overcurrent protection mode (equivalent to checking each laser for `overcurrent_fault` and, if found, calling poweroff then poweron).


### `laserbank/autowarm`
- **Request topic:** `cmd/<device>/req/laserbank/autowarm/[on,off]`
-   - Payload: `{"alloffabove_ambienttemp": 0.0, "bankonbelow_ambienttemp":0.0,
                 "auxonbelow_tectemp": 0.0, auxoffabove_tectemp": 0.0, auxonoperating_ambienttemp": 0.0}`
- **Response topic:** `cmd/<device>/resp/laserbank/autowarm`
  - Response: `{"status": "success"}`
  - Response: `{settings...}`

- **Notes:** Maintains power of bank and aux heater to ensure that lasers may be turned on at a moment's notice 

### `atten`
- **Request topic:** `cmd/<device>/req/atten`
  - Set:
    ```json
    {   "name": "<lasername><optional suffix>",
        "value": 0.0,
        "unit": "db"
    }
    ```
  - Query:
    ```json
    {   "name": "<lasername>",
        "unit": "db"
    }
    ```
    Where:
    - `name` is `<lasername>` + `"" | "1" | "2"` *(or `1|2` in your shorthand)*
    - `unit` is `"db" | "volt" | "%"` (case-insensitive)
    - `volt` is an error for *total* attenuation
    - if the attenuator is unspecified (total), `value` is total
    
- **Response topic:** `cmd/<device>/resp/atten`
  - Set result: `{"status": "success"}`
  - Query result:
    ```json
    {    "<lasername>:": 0.0,
         "<lasername>1": 0.0,
         "<lasername>2": 0.0,
         "unit": "db"
    }
    ```


### `attensettings`
- **Request topic:** `cmd/<device>/req/attensettings`
  ```json
  {
      "name": "<lasername>1"|"<lasername>2",
      "settings": {
        "offset": 0
      }
    }
  ```

- **Response topic:** `cmd/<device>/resp/attensettings`
  - Response: `{"status": "success"}` 

- **Notes:** unsettable ignored; not all settings required; drive-restart requirements TBD.

### `pd`
- **Request topic:** `cmd/<device>/req/pd`
  - Optional payload: `{"unit": "power"}`

    `unit` is `"power" | "volts"` (case-insensitive), defaults to power`.

- **Response topic:** `cmd/<device>/resp/pd`
  ```json
  {   "unit": "power",
      "yjvalue": 0.0,
      "yjvalue_err": 0.0,
      "hkvalue": 0.0,
      "hkvalue_err": 0.0,
      "time": 0,
      "uptime": 0
    }
  ```

- **Notes:** powers photodiodes as needed; resets photodiode auto-off timer.

### `pdsettings`
- **Request topic:** `cmd/<device>/req/pdsettings`
  - Set:
      ```json
      {  "autooff_s": 0.0,
         "yj": {
           "nep_uwprthz": 7.5e-09,
           "noise_rms_mV": 3.0,
           "dark_mv": 0.0,
           "qe_y": 0.0,
           "qe_j": 0.0,
           "qe_yj": 0.0,
           "qe_1430": 0.0,
           "qe_1028": 0.0,
           "qe_1270": 0.0,
           "saturation_uw": 1.1e-5,
           "gain_v_p_uw": 47500.0
         },
         "hk": {
           "nep_uwprthz": 2.11e-3,
           "noise_rms_mV": 1.0,
           "dark_mw": 0.0,
           "qe_1430": 0.0,
           "qe_1510": 0.0,
           "qe_2330": 0.0,
           "qe_h": 0.0,
           "qe_k": 0.0,
           "qe_hk": 0.0,
           "saturation_uw": 1.6194331984,
           "gain_v_p_uw": 3.0875
         }
       }
      ```
  - Get: No payload

- **Response topic:** `cmd/<device>/resp/pdsettings`
  - Set result: `{"status": "success"}` 
  - Get result: Full set of settings as described in set JSON

- **Notes:** not all settings need to be included when setting; failure on any settable setting results in none being set; QE values are in `[0, 1]`.

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
     {     "status": "success"|"partial",
           "ntp": "unsupported",
           "dns": "unsupported",
           "dhcp": "unsupported"
         }
     ```
TODO add diode stabilized flag and time until setting
  - Query result:
    ```json
    {     "preferdhcpntp": true,
          "preferdhcpdns": true,
          "source": "<source>",
          "sourceonnextboot": "<source>",
          "trydhcpfirst": true,
          "source_settings": {
            "<source>": {
              "ip": "<ip>",
              "ntp": "<ip>",
              "dns": "<ip>",
              "subnet": "<subnet>",
              "gateway": "<gateway>"
            }
          }
        }
    ```

- **Notes:** 
  - unsupported features don’t error; partial config accepted
  - IP precedence: `temporary_override` → `persistent manual setting` → `dhcp` (if enabled) → `compiled`.
  - partial comes with keys indicating which settings are not supported.
  - unsupported have unsupported in place of an ip
  - source names are: `temporary_override`, `persistent_manual`, `dhcp`, `compiled`.

### `mqtt`
- **Request topic:** `cmd/<device>/req/mqtt`
  - Set:
    ```json
    {
      "broker": "<ipv4-or-hostname>:<port>>",
      "persistent": true
    }
    ```
    `host` is accepted as an alias for `broker`.
  - Query: No payload

- **Response topic:** `cmd/<device>/resp/mqtt`
  - Set result: `{"status":"success","apply":"reconnect"}`
  - Query result:
    ```json
    {"broker":"<value>:<port>", "dns_supported":true}
    ```

- **Notes:**
  - Broker value may be numeric IPv4 or hostname.
  - If DNS is not compiled in, hostname values are rejected.
  - Successful set updates runtime settings and triggers MQTT reconnect behavior.

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
  - While active, MQTT commands are rejected before dispatch and logged.
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

- **Response topic:** `cmd/<device>/resp/stauts`
  ```json
  {   "fwversion": "<githash>",
      "bootcount": 0,
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
          "name": "yj_cal_laser",
          "state": "A",
          "duty_cycle": 0.33,
          "numerator": 33,
          "denominator": 100,
          "tick_ms": 2
        },
        {
          "name": "yj_forward_retro",
          "state": "B",
          "duty_cycle": 1.0,
          "numerator": 100,
          "denominator": 100,
          "tick_ms": 2
        },
        {
          "name": "yj_ao_fei",
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
  - The fixed routes are `yj_split -> as_split` and `hk_split -> as_split`,
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
