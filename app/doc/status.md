# HISPEC FIB PCB Controller

## Summary
This codebase implements a networked embedded controller for the PCBs in HISPEC's FIB switching PCBs. The PCBs principally
control MEMS fiber switches to route light for the instrument, though they are also variously equipped with 
attenuators, and one has both an ADC to readout photodiodes and a MODBUS connection to control a calibration laser diode bank.

hardware.md has hardware details.

Commanding is done via MQTT and roughly follows the AWS IoT guidelines for MQTT-based networked control:
https://iotatlas.net/en/implementations/aws/command/command1/
https://docs.aws.amazon.com/iot/latest/developerguide/mqtt.html
https://docs.aws.amazon.com/whitepapers/latest/designing-mqtt-topics-aws-iot-core/designing-mqtt-topics-aws-iot-core.html

The code emphasizes static memory layout, task separation, clear ownership of devices, and lightweight error
handling (no runtime exceptions). Networking is DHCP-based with fallback to static and automatically recovers after link loss.

Command dispatch flows through a central `executor_task`, structured for extensibility. JSON payloads are parsed with
a friendlified wrapper around the Zephyr JSON parser and done in `TODO`.

## Background 
Originally intended for targeting the [W5500-EVB-PICO2 board](https://wiznet.io/products/evaluation-boards/w5500-evb-pico2) using FreeRTOS, the code was retargeted to 
Zephyr both for its batteries-included development environment and for its broad support for ST32 boards. The FIB PCBs 
switched to the ST32, specifically the [ST32Nucleaoxxx]() board, as in order to increase overlap with the HISPEC SPEC
subsystem's temperature controller.


HISPEC's FIB system uses these boards in four locations:
1. The Trunk Interface Box (TIB), which has
   1. MMF Switches (FFLS)
   2. 6 SMF Switches (FFSW)
   3. 12 SMF variable attenuators (2x per laser channel) (FVOA)
   4. 1 off-board 6 laser diode bank controlled via Modbus (Maiman electronics SF8250-ZIF14 + NH8 hub)
   5. off-board power relays to toggle PD power and a auxiliary heater for the laser bank
   6. on board power switch to toggle LD bank power
2. The Achromatic Splitter
   1. SMF switches (FFSW) for muxing lign in the YJ (3x) and HK (3x) channels
3. The YJ Calibration switch
   1. (smaller core) MMF switches (FFSW)
   2. 5 SMF switches (FFSW)
   3. 2 SMF variable attenuators (FVOA)
4. The HK calibration switch, same as the YJ switch but with a slightly different, but electrically identical
   model of fiber components (FFSW & FVOA).

## Software
Before making edits understand Zephyr's task structure and device initialization flow.


## Direction
### Architecture:

- System listens for commands over serial and network. There is a serial “ignore network for X after receipt of serial cmd” command which expires X after the last serial command.
- Expect only a small set of named global scheduled actions (no full scheduler subsystem).
- Current implementation note: `app_scheduled_actions.c` owns the named delayed actions for serial override expiry and delayed reboot.
- Current implementation note: MQTT remains connected during serial override; MQTT command execution is rejected at ingress and logged.
- Core runtime components: 
  - watchdog timer, 
  - network manager, 
  - command dispatcher, 
  - sNTP
  - MQTT interaction/handler, 
  - MEMS switch pin toggler task (switch modulation and pin pulses)
  - photodiode monitoring / output-power-determination task
  - maybe a housekeeping/current-state task
- Photodiode monitoring and laser power output-determination are tightly coupled but can be separate tasks.
- Console/status messages to be published to MQTT (configurable verbosity) and not just serial console
- Debugging: use onboard ST32 debugger features for learning, development, and possible Grafana integration of telemetry.
- Implementation target: C (Zephyr). Later port to C++ on zephyer if desired for personal learning and exploration.
- Avoids “tedious” state machines because of maintenance fragility; lean toward simpler task patterns with explicit housekeeping unless state machine is unavoidable.
- Many algorithmic details (attenuator linearity, exact control loops, quantization rules) will need lab verification before finalizing implementations.
  - Current implementation note: `mems/<switch>` accepts optional `toggle_rate_hz`; requested and actual quantized rates are reported separately.
  - Current implementation note: `split` is implemented as an AS-PCB-specific system command using fixed `yj_split -> as_split` and `hk_split -> as_split` MEMS routes.

### Features:
- Laser bank control: temperature regulation algorithms, output-power control, detected-power auto-level tuning for photodiodes.
- Switching/splitter control: determine switching frequencies and quantize requested switch frequencies to supported rates.
- Photodiode handling:
  - Auto-determination of warm-up behavior and running measurements (e.g., 1 s running average).
  - Noise/fault detection (e.g., residuals after linear fit indicate photodiode fault).
  - Decision: dark-level measurement must be an explicit user command (to avoid drift/bias from passive collection). Commands planned: measure dark and store, measure dark without updating stored value, reset lowest-ever dark value.
  - Maintain stored lowest-ever dark value and a running average; but avoid auto-updating dark from passive data because of long-term bias risks.
- Calibration:
  - Auto-calibrate attenuators in loopback mode. Plan to use linear fitting or analytical model parameterization, but must verify attenuator behavior experimentally.
- Settings persistence:
  - Persist calibrations and states across reboots; store values and fall back to hard-coded defaults. No additional persistence layers required.
  - Current implementation note: the detected PCB board type is persisted as `tib/board/type`; if a later boot detects a different valid board type, all other app settings are cleared and the boot behaves as a first boot for the new hardware.
  - Current implementation note: attenuator calibration coefficients are stored per logical attenuator channel as `tib/atten/<channel>/db2volt/<index>` and `tib/atten/<channel>/volt2db/<index>`, then loaded by `setup_attenuators()`.
- Error handling & observability:
  - Error heuristics: if a command can detect a problem it should do as much safe work as possible and report known failures.
  - Warnings are to be easy to publish from anywhere in the code as fire-and-forget MQTT messages.
  - Current implementation note: `app_warning_emit()` logs warnings locally and publishes best-effort MQTT warnings on `dt/hsfib-tib/warning`.
- Help/UX: if memory permits, include a help command that emits plain-English descriptions of MQTT commands and payloads (not their return values).

## Unstarted work is principally in
- persistent settings storage for remaining laser/power/relay state,
- json/MQTT standardization,
- power management (diodes & laser),
- auxiliary heater control,
- attenuator calibration,
- and user command coverage.
- porting more advanced python laser code to maiman driver

### Other:
- Command & control API and code stubs are mostly documented; remaining work is the software architecture and some algorithms.
- Might try later using AI to auto-generate optional UI module (local display)


## Present Code Structure

Note well that information below this point is not intended to be determinative of future direction, especially where it conflicts with the above of commands.md 


| Area                              | Description                                                                                                                                                                     |
|-----------------------------------|---------------------------------------------------------------------------------------------------------------------------------------------------------------------------------|
| `main.c`                          | Initializes hardware, tasks, queues, network and runs MQTT TODO needs tidy and refactor after sorting out libraries                                                             |
| `command.c`                       | command callbacks, exercising of hardware, where JSON gets parsed and created (mostly), the dispatch system `Command` creation from MQTT is in `main.c` which is rather muddled |
| `photodiode.c`                    | a zephyr task that continuously reads the ADC to monitor the two photodiods at a fixed rate and ships date to an output queue                                                   |
| `attenuator.c`                    | functions to init/get/set attenuators                                                                                                                                           |
| `mems_switching.c`                | MEMS Switch control and a MEMS switch router to manage connecting the light paths                                                                                               |
| `maiman.cpp`                      | Maiman laser diode driver control                                                                                                                                               |
| `devices.c`                       |                                                                                                                                                                                 |
| `coo_commons/json_utils.c`        | JSON parsing and creation (nicer wrapping of Zephyr)                                                                                                                            |
| `coo_commons/mqtt_client.c`       | nicer mqtt wrapping of zephyr functions                                                                                                                                         |
| `coo_commons/network.c`           | standardized Zephyer startop (maybe, maybe this is overkill and should just be in the project)                                                                                  |
| `coo_commons/commanding.c`        | potential Future home for command dispatch                                                                                                                                      |
| `modules/dac/dac7578` | DAC device driver library, others (e.g. ADS1115 ADC) are mainlined in Zephyr                                                                                                    |


### main.c
 - Some Settings management stubs,  probably GPT via ML.
   - setting_handler(name, size, read_cb, cb_args)
   - Zephyr macro that neglects to provision for setting, committing, or exporting settings
 - structs
   - mqtt client
 - threads
   - executor thread & stack & function
     - gets from inbound q calls dispatch_command on the command and shoves result into outbound q
       - queues defined as extern in command.h, via Zephyr macro in command.c
       - drops result if outbound is full
         - thinking is that repeating a command is preferable than ignoring or undoing a command
     - TODO this is either tight or implicit coupling between code parts that could be librarized and used across projects
     - TODO is very greedy, does that matter?
   - photodiode thread & publish work delayable & publish handler function
     - publish handler loads up outbound q with photodiode q until pd q is empty or out q full then reschedules itself
 - watchdog
   - TODO FIX: not configurable, no provision for nor discussion of hardware sequencing or implied runtime status
   - callback
   - initialization
 - mqtt_command_handler
   - builds command struct from received mqtt message and puts into inbound q
   - does some json parsing but insufficient to eliminate later need for it
     - I saw this as unavoidable as individual callbacks are the only ones that know how to parse their data 
   - rejects malformed commands or if input command q is full by putting a response in the outbound q
   - TODO should be refactored and moved to a library
   - TODO needs to include error message into command error response
   - TODO it is rather muddled that this is hear but all the command stuff is in command.c
 - main()
   -  TODO need to ensure that things are setup so that DHCP falls back to a static IP and that if the link goes down 
      (e.g. cable unplug) it comes back up automatically. I'm not clear if I need to use the connection manager to 
      ensure this. 
   - TODO need to ensure that MQTT properly stops/restarts across link failures.
   - TODO coo_mqtt library should bundle more stuff (i. subscription, cmd handler, and mqtt portions of main loop)
   - initializes watchdog
   - calls devices_ready()
     - TODO review this in devices.c
   - sets up mems objects with `setup_mems_switches_and_routes()`
     - TODO this needs integration with state persistence and last known mems setting
   - sets up attenuators with `setup_attenuators()`
     - TODO this needs integration with state persistence and last known setting
     - Suspect that rule should be on restart attens default to DAC=0 TODO should decide
       - if restart changes persisted value notify via MQTT
   - Sets up settings subsystem and restores
     - TODO this certainly needs integration with the setup of the mems and attens and so should be earlier
   - while (1)
     - mqtt connect
     - mqtt subscribe
     - while mqtt connected
       - feed watch dog (so mqtt server down triggers reboot loop!!!!!) TODO major flaw
         - GPT slop from refactor: why is WTD feed w/ an interval
       - while outbound q not empty
         - publish message
       - call mqtt process
     - call mqtt disconnect
 coo commons network does not have the necessary includes to prevent also needing several <zephyr/net/ in main!!
TODO: Incorporate UUID generation: https://github.com/zephyrproject-rtos/zephyr/tree/main/samples/subsys/uuid
TODO: Verify how I'm doing logging makes sense: https://github.com/zephyrproject-rtos/zephyr/tree/main/samples/subsys/logging/logger
TODO: Incorporate settings persistence: https://github.com/zephyrproject-rtos/zephyr/blob/main/samples/subsys/settings/src/main.c#L392
TODO: Verify I'm dealing with networking properly and setup DHCP with fallback to static. see https://github.com/zephyrproject-rtos/zephyr/tree/main/samples/net/common

### command.c
- defines some json types TODO should be in json utils, no?

- defines laser_t enum
- get_laser_channel()
  - converts a laser name string to a `laser_t` enum
- wait_laser_boot()
  - sleeps a bit of time to allow for laser booting

- power functions
  - power_enabled()
    - checks if the power_gpio for the lasers is enabled 
  - enable_power()
    - enables power for lasers
    - returns true iff power went from off->on 
  - disable_power()
    - disables power for lasers
    - returns true iff power went from on->off

- TODO need to add power control for laser diode bank

- defines a dispatch system
  - a dispatch table with callbacks for get/set for command endpoints
  - defines the DispatchEntry struct
  - a find_dispatch() function
  - a OutMsg dispatch_command(const Command*) function that
    - finds the dispatch entry
    - deals with no entry or lack of a get or set
    - calls the function and returns its response (or the appropriate missing function response)
- defines a key name / setting name pair out of a character string
  - TODO this should certainly be in the area that defines the IoT/MQTT endpoint&command schema 
- defines parse_msg_type_from_payload(const char *payload, enum MsgType *out)
  - looks in the json payload for 'msg_type' and sets the type appropriately.
  - that it is looking for 'msg_type' relies on implicitly macro work and variable names, which coming back to is clever 
    of Zephyr but without more usages or explanation, VERY unclear to the uninitiated
  - TODO this seems like a lot of code for low return
- _msg_builder()
  - takes in a `Command`, `MsgType` and message payload string and builds and MQTT `OutMsg`
  - TODO should probably be renamed MQTT_msg_builder()
- defines some core command responses
  - invalid_command_response
  - unknown_response
  - unsupported_response
  - busy_response
  - All take a `Command` and return an appropriate `OutMsg`
- defines `OutMsg memsroute_get(`Command`)
  - gets the active routes via `mems_router_active_routes()`
  - builds a json message payload from them of form {'active_routes': {<input_name>: output_name, ...}}
- defines `OutMsg memsroute_set(`Command`)
    - parses command payload JSON of form `{"input":"<src>","output":"<dst>"}`
    - gets the route via `mems_router_get_route()`
    - walks through route and gets each switch in route with `mems_router_find_switch()` setting each to required state
    - builds response with `_msg_builder()` w/ payload of {'status': 'OK'} on completion or {'error': <error message>} in event of failure
- defines `OutMsg mems_set(`Command`)
  - parses mems switch name from command key with `parse_key_pair` '<don't care>\<mems_switch_name>'
  - parses desired state from `{"state":"A"|"B"}`
  - optional `duty_cycle` and `stopafter_s` configure toggling (state A only)
  - returns switch state as `A|B|A?|B?` plus `duty_cycle`, `toggle_rate_hz`, and `stopafter_s`
- defines `OutMsg splitting_set(`Command`)
  - parses `channel`, `ratio1`, and `ratio2`; computes `ratio3 = 1 - ratio1 - ratio2`
  - gets the fixed channel route with `mems_router_get_route()`
  - applies exact integer MEMS tick duties with `mems_switch_set_state_ticks()`
  - reports requested and actual split ratios as arrays plus per-switch numerator/denominator/tick timing
- `OutMsg laser_setting_get(const struct Command *cmd)`
  - parses laser name and setting name from command key with `parse_key_pair` '<laser>\<setting>'
  - gets laser channel with `get_laser_channel()` 
    - TODO get rid of hardcoded int offset (+5) and make work with laser enum
  - gets register address for setting with `maiman_get_register_address()`
  - enables laser power and waits for boot if necessary
  - reads the setting value with `maiman_read_u16()`
  - builds response with `_msg_builder()` w/ payload of {<setting>:<value>} or {"error": <errmsg>}
- `OutMsg laser_setting_set(const struct Command *cmd)`
  - parses laser name and setting name from command key with `parse_key_pair` '<laser>\<setting>'
  - parses the setting value as a uint16 from {"value": uint16}
  - gets laser channel with `get_laser_channel()`
    - TODO get rid of hardcoded int offset (+5) and make work with laser enum
  - gets register address for setting with `maiman_get_register_address()`
  - enables laser power and waits for boot if necessary
  - writes the setting value with `maiman_write_u16()`
  - builds response with `_msg_builder()` w/ payload of {"status":"OK"} or {"error": <errmsg>}
    - note that no readback of the setting or range checking is done. 
      behavior of the driver with bad settings is as specified in datasheet
      TODO: verify this is ok.
- TODO: verify that get/set ops with laser are operative with overcurrent alarm and if not account for that
- `OutMsg atten_setting_get(const struct Command *cmd)`
  - parses attenuator key `atten/<laser>/<setting>` with `parse_atten_key()`
  - gets atten channel with `get_laser_channel()`
  - if setting is "value"|"valuedb":
    - gets voltage and db setting of attenuator with  `attenuator_get()`
    - builds response with `_msg_builder()` w/ payload of {"voltage": float, "db": float}
  - if setting is "coeff":
    - builds response with `_msg_builder()` w/ payload of {"db2volt": [float,..], "volt2db": [float,..]} 
  - errors get a response built with {"error": <errmsg>}
- `OutMsg atten_setting_set(const struct Command *cmd)`
  - parses attenuator key `atten/<laser>/<setting>` with `parse_atten_key()`
  - gets atten channel with `get_laser_channel()`
  - if setting is "value"|"valuedb":
    - parses the setting value as a float from {"value": float}
    - uses `attenuator_set()` to set the attenuation as either a dB or raw level
  - if setting is "coeff":
    - parses json payload for quadratic coeff for {"db2volt": [float,float,float], "volt2db": [float,float,float], "persistent": bool}
    - fetches the current attenuation as dB
    - updates the coeffs
    - resets to the attenuation to the same dB
    - updates `app_settings_update_attenuator_channel()` and persists only when requested
  - builds response with `_msg_builder()` w/ payload of {"status": "OK"} or {"status":"OK","persistent":bool}; errors get a response built with {"error": <errmsg>}
- status_get()
  - builds response with `_msg_builder()` w/ payload of {"laser_power": "true" | "false"}
- power functions
  - power_qet()
  - power_set()
    - enables power GPIO
  - sleep_set()
    - set how long until power is automatically turned off
    - TODO not yet emplemented

### photodiode.c
- defines the two zephyr adc device tree configs for each channel
  - yj = adc ch 0
  - hk = adc ch 2
  - todo need to make sure channel numbers hooked zephyr properly(
- defines the photodiode monitoring thread
  - waits until device is ready
  - uses zephyr to set up yj channel for read, reads, then does same on hk channel 
    - relys on blocking behavior of adc_read and conversion time TODO this is sound as we have time margin but need to 
      verify no race condition with IC polling otherwise need to pad conversion interval or use HW interrupt
  - uses zephyr log macro to log errors
  - builds `OutMsg` on hardcoded `dt/hsfib-tib/photodiode` with QoS = 0
    payload includes raw counts, millivolts, dark-subtracted millivolts, power estimate, residual RMS noise, dark settings, and uptime
    - TODO move pub address to, at a minimum, a central IoT/MQTT point in this file, ideally for whole project/kernel config
    - TODO finiaize time format
    - TODO consider adding an "hk_time_offset" value
    - TODO consider not emitting if photodiode power is off or flagging in JSON
  - put message in the `photodiode_queue` for consumption elsewhere, drop sample if queue full
  - sleep until the net polling interval don't worry about the overflow ever 300Myr
  - active noise monitoring uses residual RMS after smoothing and emits a warning without failing the sample
  - explicit dark calibration commands live in `pd_set()`
    - dark measurements are requested by the command path and latched by the
      regular photodiode sampler thread; commands do not read the ADC directly
      and do not block for the requested interval
    - requested `duration_ms` is rounded to the nearest supported whole sample
      count at the sampler cadence
    - `measure_dark` with `store:false` starts a measurement without changing settings
    - `measure_dark` with `store:true` updates stored dark and lowest-ever dark when complete if lower
    - `dark_status` reports measuring/complete/error state and completed statistics
    - `reset_lowest_dark` clears lowest-ever tracking for the selected channel

### attenuator.c
- attenuator struct
  - double  coeff_db_to_volt[3] 
  - double  coeff_volt_to_db[3] [x0+x1*v+x2*v^2]
  - double  voltage
  - struct dac_channel_cfg cfg
- attenuator_init
  - populates the attenuator config struct
  - calls dac_channel_setup which sets channel to 0 if not configured
  - subsequent calls look like they would not trigger an IC reset, don't expose, just reset the  TIB
- attenuator_get
  - reads dac value
  - return value is in either volts or db (as per x0+x1*v+x2*v^2 where x_i =coeff_volt_to_db[i])
- attenuator_set
  - uses value to comput atten voltage via coeff_db_to_volt[0]+coeff_db_to_volt[1]*value+coeff_db_to_volt[2]*value*value
  - if raw treats value as voltage 
  - clips setting voltage to be in between [0, MAX_VOLTAGE]
- TODO: investigate and attenuation auto-calibration enhancement
  - consider adding function that auto-gatheres coeffs
    - set atten to 0. increase laser power until detection
    - set atten to max. Increase laser point to detection, if at laser limit walk atten back until detection.
    - take samples with detections
    - make fit and adopt coeff

### mems_switching.c
- Implements the mems switch and mems router concepts. routes are a series of switches and required states to connect an input to an output. Multiple paths are not supported. Multiple active routes are.
- Router owns a fixed-rate toggler task that runs every `MEMS_SWITCH_ELECTRICAL_PULSE_MS`.
- mems_switch struct
  - gpio device and pin A and B
  - state fields (actual state, target state, state-known-this-boot)
  - toggle fields (duty cycle, attained toggle rate, remaining duration)
  - char name[]
- mems_route structs 
  - mems_route, mems_route_step|id|key
    - key: pointers to input and output names
    - id: char arrays for input and output location names
    - step: pointer to a switch name and a char state (the required state)
    - the route itself
- mems_router struct
  - *switches[] - pointers to mems switch structs
  - *routes - pointer to the selected board's immutable route table
  - num_switches & num_routes
- void mems_switch_init(..., float configured_toggle_rate_hz)
  - populate switch struct with GPIO pins and quantized per-switch toggle-rate data
  - configure both GPIO pins inactive on init
- int mems_switch_set_state(struct mems_switch *sw, char state)
  - schedules static state A/B for next toggler tick
- int mems_switch_set_state_with_duty(...)
  - configures duty-cycle toggling with max-duration enforcement
  - repeated set with same profile extends remaining duration without resetting phase
- int mems_switch_get_state(const struct mems_switch *sw, char *out_state)
  - returns known state (or target state if not yet known this boot)

- void mems_router_init(struct mems_router *router, struct mems_switch **switches, uint8_t num_switches, const struct mems_route *routes, uint8_t num_routes)
  - load the mems_router struct with active switch pointers and the selected static route table

- struct mems_switch *mems_router_find_switch(const struct mems_router *router, const char *name)
  - find a switch by name and return a pointer to it if found, null otherwise

- const struct mems_route *mems_router_get_route(const struct mems_router *router, const char *input, const char *output)
  -  find/return the route from input to output and return a pointer to it (or NULL if not found)

- uint8_t mems_router_active_routes(const struct mems_router *router, struct mems_route_key *out_keys, uint8_t max_keys);
  - List all routes whose switches are ALL in the expected configured static state.
  - Returns the number of active routes found, up to max_keys.
  - Each result is a mems_route_key with an (input, output) pair.

### maiman.c
- TODO see https://github.com/CaltechOpticalObservatories/hispec-fib/blob/develop/ait/photonic_testing/photonic_testing.py
- TODO see https://github.com/CaltechOpticalObservatories/hispec-fib/blob/develop/ait/photonic_testing/maiman_modbus/...
- control of the Maiman lasers via modbus
  - NB Zephyer modbus init (modbus_init_client) is handled in devices.c
- defines MaimanRegister struct, laser_address type, maiman_driver type (a wrapper around a node id), and a register_table 
- defines device bitmasts and register addresses
- defines a function to init the device and read and write registers
  - TODO need to init with appropriate laser properties
- defines user interface functions to interact with the device, core functions
  - maiman_start_device/maiman_stop_device
  - maiman_set_current/maiman_get_current
  - maiman_is_operation_started
  - TODO create a collective status function
    - see hispec-fib.ait.photonic_testint.photonic_testing.Laser.status() for inspiration 
  - TODO Make it clear if setting/getting laser current fails e.g. if overcurrent is tripped
  - TODO still need to actually implment TEC startup and actual command flow as per python test code
    - get_current_protection_threshold (it is a function of a potentiometer on the device)
    - flag if OCP value is above LASER_DNE value and warn
    - set TEC current limit, set current limit to laser DNE w/ safety margin
    - set the tec pid coeffs to defaults
    - set the drive current to 0
    - disable the interlock
    - start the device
  - TODO need to reinit the device when the power is turned off
  - TODO implement power_down of the device
    - current to 0
    - stop device
    - stop tec
    - enable interlock
  - TODO add setting current as a percentage of the laser's current
    - TODO ass LASER properties struct, populate in devices.c (actual lasers are specific to TIB not mainman driver)

### devices.c
- instantiates adc, dac, modbus_name, and gpio dev via device tree
- detects the physically assembled PCB from the four active-low board-type strap GPIOs in `devices_detect_board_type()`
  - exactly one strap must be active
  - no active strap leaves board type `unknown`
  - multiple active straps also leave board type `unknown` after logging an error
  - Nucleo strap pins are TIB D35/PA3, CAL YJ D37/PE15, CAL HK D36/PB10, and AS D38/PE6
- instantiates attenuators, mems_switches, and mems_router according to the detected board profile
  - TIB: 8 MEMS switches, TIB route table, 6 logical attenuator channels, laser bank, photodiodes, laser power GPIO, relay box
  - AS: 6 MEMS switches and AS split/cal routes
  - CAL YJ/HK: 7 MEMS switches, CAL route table, and the single H/CAL logical attenuator channel
- defines MEMS routes as static file-scope tables selected by board type; routes are not registered or copied at boot.
- `devices_ready()` checks only devices that should exist on the selected board profile

 

---

### Explicit Developer Instructions:

- **Review major code (`tip-pico2/*`) carefully.**  
  Each is intended to be (strongly) modular and typically owns its domain of hardware or communication.
- **The true command and control flow is in `executor_task.cpp`.**  
  New commands, hardware devices, and features must be added by expanding the dispatcher tables there. It is presently unfinished
- **Device initialization (photodiodes, attenuators, switches) happens in `main.cpp`.**  
  All shared objects are passed through `HardwareContext`.
- **Zyre network behavior (discovery and messaging) is in `pico_zyre.cpp`.**  
  See `ZyreBeacon::tick()` and `maybe_send_enter()` for dynamic IP broadcasting.
- **Message framing and unframing follows strict multipart framing rules (see `ZyreFramer::decode` and `encode`).**  
  Always validate or extend message parsing at the framer level, not inside tasks.
  The executor_task is responsible for properly packaging results into the JSON payload for a Response. This is out of scope for pico_zyre
- **Look carefully at the key naming convention in `mktl_keys.h` when matching or dispatching mKTL commands.**  
  Prefixes and suffixes are split for flexibility and clarity.
- **No C++ exceptions are permitted at runtime.**  
  Any new library usage or parsing must use fail-safe, non-throwing patterns.
- **Networking is dynamic with DHCP.**  
  If the Ethernet link is dropped and recovered, DHCP automatically restarts.

If reviewing with an AI or static analysis tool:
- Ask it to **trace main task interactions** (command-in queue, response-out queue, pub-out queue)
- Review each task's control loop separately for sleeping and blocking behavior
- Be aware that socket numbers and uses (e.g., WHISPER sockets) are **hardwired constants** in `pico_zyre`
- Recognize that **memory layout is static**; dynamic malloc is avoided wherever possible.

---

## 4. Outstanding Tasks and Future Work

| Area                               | Description                                                                                                                                                                                                                        |
|------------------------------------|------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------|
| **Laser Diode Driver Integration** | Preliminary work only. status() and config() commands are missing.                                                                                                                                                                 |
| **Full Command Dispatch Coverage** | Only a subset of mKTL command space is wired. Need to extend `executor_task` for things like system status, laser commands, and calibration writes, persistence of settings to flash, and packaging of results into response JSON. |
| **EEPROM / Flash Storage**         | Settings (e.g., calibration curves) are not persisted across reboots. This needs to be added.                                                                                                                                      |
| **SNTP Integration**               | SNTP integration could offer powerful debugging capabilities and should be added.                                                                                                                                                  |
| **Formal Testing**                 | Unit tests for command parsing, error paths, and hardware failure modes are recommended. No automated tests exist yet.                                                                                                             |
| **Power Management**               | No explicit low-power or watchdog behavior implemented yet. Watchdog is a requirement.                                                                                                                                             |
| **Optimization**                   | Opportunity exists to reduce startup latency and shrink final binary by tuning linker script and trimming unused libraries.                                                                                                        |
| **CMake cleanup**                  | The cmake setup is ok, but could really use some refinement to clean up the integration of the W5500 ioLibrary_driver and either a patch or specific commit.                                                                       |
---

# Closing

This codebase is designed for clarity, reliability, and growth.  
It is **production capable**, assuming that additional hardware interfaces are finalized and final edge case handling (network loss, hardware faults) are addressed.

**Explicit instruction to future developers**:
- **Read the code**.
- **Understand the task structure** before adding complexity.
- **Preserve the principle** of static, memory-safe embedded C++ design.

---
