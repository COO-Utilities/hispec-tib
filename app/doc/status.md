# Project Summary

This project implements a networked embedded controller for the PCBs in HISPEC's FIB subsystem. The PCBs principally
control MEMS fiber switches to right light for the instrument, though they are also variously equipped with 
attenuators, connection to control a calibration laser diode bank, and also read out photodiodes.

Originally intended for targeting the [W5500-EVB-PICO2 board](https://wiznet.io/products/evaluation-boards/w5500-evb-pico2) using FreeRTOS, the code was retargeted to 
Zephyr both for its batteries-included development environment and for its broad support for ST32 boards. The FIB PCBs 
switched to the ST32, specifically the [ST32Nucleaoxxx]() board, as in order to increase overlap with the HISPEC SPEC
subsystem's temperature controller.

The control system follows the AWS IoT guidelines for MQTT-based networked control:
- [TODO insert AWS description]

The code emphasizes static memory layout, task separation, clear ownership of devices, and lightweight error 
handling (no runtime exceptions). Networking is DHCP-based with fallback to static and automatically recovers after link loss.

Command dispatch flow through a central `executor_task`, structured for extensibility. JSON payloads are parsed with 
a friendlified wrapper around the Zephyr JSON parser and done in `TODO XXX`.

Unstarted work is principally in:
- persistent settings storage, 
- NTP time integration,
- watchdog timer integration,
- json/MQTT standardization,
- power management (diodes & laser),
- selection of pins for the Nucleo,
- and broader command coverage. 

Anyone making edits is encouraged to understand the Zephyr task structure and device initialization flow before 
making changes.

---
## 1. Hardware

FIB uses this system in 4 locations:
1. The Trunk Interface Box (TIB), which has
   2. 2 MMF Switches (FFLS)
   3. 6 SMF Switches (FFSW)
   4. 12 SMF variable attenuators (2x per laser channel) (FVOA)
   4. 1 off-board 6 laser diode bank controlled via Modbus (Maiman electronics SF8250-ZIF14 + NH8 hub)
   5. 1 off-board power switch to toggle PD power
   6. one board power switch to toggle LD bank power
2. The Achromatic Splitter
   3. 6 SMF switches (FFSW) for muxing lign in the YJ (3x) and HK (3x) channels
2. The YJ Calibration switch
   3. 2 (smaller core) MMF switches (FFSW)
   4. 5 SMF switches (FFSW)
   5. 2 SMF variable attenuators (FVOA)
6. The HK calibration switch, same as the YJ switch but with a slightly different, but electrically identical 
model of fiber components (FFSW & FVOA). 

All have an on-board temperature sensor and are interacted with via ethernet when deployed. USB-C is available   

### Microcontroller
- ST Nucleo H563ZI on a STM32H5 Nucleo-144 board (MB1404)
  - See user manual UM3115
- rPi **W5500-EVB-PICO2**

### MEMS Switches (5V GPIO control via PCAL6416AHF, ad I2C 16x GPIO expander)
- 3.3V i2c, 5V gpio, 25mA max drive
- Address 0x20 (ADDR tied to ground) or 0x21 (use 0x20)
- Configure as open drain for FFSW as they have 5V pullups
- Configure as push-pull for FFLS
- Require 2 pins per FFSW or FFLS

For board files:
- W5500: Use I2C1 on pins GP2 (SDA) and GP3 (SCL) (per existing design notes)
- W5500: I2C0 on pins GP4 (SDA) and GP5 (SCL) (per Zephyr code)
- Nucleo: (todo finalize)
  - CN7 2 D15 I2C_A_SCL PB8 I2C1_SCL
  - CN7 4 D14 I2C_A_SDA PB9 I2C1_SDA

At the drive level transistors are 
- SI3552DV (for FFLS)
  - N & P MOSFET
  - 2x each for FFSW
  - 2x 4.7k resistors for pullups
- DMP3085LSD (for FFSW)
  - 2x P  MOSFET
  - 1x each for FFLS
  - 1x 4.7k resistor for status line if using

### TIB & CAL Attenuator Drive DAC7578SPW 8 chan DAC driving 3x OPA2991 2 channel OpAmp
- 0 - 4095 (Vref, default=Vcc)
- Must not exceed Vmax of attenuator (6V for FVOA, so safe). Imax is 36.66 mA
- Use Vcc=3.3 with 1.51x opamp gain to avoid LL shifting
- OpAmp supplies required current to attenuator.
- I2C addr: 0x4C floating pin, 0x48 GND, 0x4A VCC) (use 0x4c float, default)
- Mustmake sure that the LDAC pin is tied to ground to ensure synchronus mode

For board files:
- W5500: I2C1 on GP2 (SDA) and GP3 (SCL) (per existing design notes)
- W5500: I2C0 on pins GP4 (SDA) and GP5 (SCL) (per Zephyr code)
- Nucleo: (TODO finalize)
    - CN7 2 D15 I2C_A_SCL PB8 I2C1_SCL
    - CN7 4 D14 I2C_A_SDA PB9 I2C1_SDA
    - (can use CN9 9 & 11 if reconfiguring jumpers)

Breadboard considerations:
- Kit board has 10K pullups, do we need to get rid of as have pullups on 3.3v side of LL translation?
- FVOA (resistive, typ~3.5V 80mA @ full atten)
- MSOA (resistive, typ~0.5-3.25V, DNE 4.5V, something like 24-38 mA, datasheet unclear)


### TIB Photodiode Monitoring ADC (ADS1115 16bit 4 channel muxed ADC)
- Use channels A0 and A2
- Run device at 128 SPS, ±6.144 range, 187.5 uV LSB
- Sample each at 50 Hz muxing between the two 
- PD coax terminated with 50 Ohm and fed to ADC as singled-ended input (gives 0-5V range from 0-10V PDs)
- I2C addr: 0x48 (ADDR=gnd) or 0x49 (ADDR=Vcc)
- Uses 2-channels of LL shifting for i2c 3.3-5V
- TODO Elec: Consider clamping Ain at MAX Vdd+.3 (say .2V above 5V with a shottkey(?) diode)

For board files:
- W5500: I2C0 on pins GP4 (SDA) and GP5 (SCL) (per existing design notes)
- W5500: I2C1 on GP2 (SDA) and GP3 (SCL) (per Zephyr code)
- Nucleo: (TODO Finalize)
    - CN9 19 D69 I2C_B_SCL PF1 I2C2
    - CN9 21 D68 I2C_B_SDA PF0 I2C2

Breadboard Considerations:
- AF prototype board has 10K pullups, may need to get rid of as LL shifter boards also have pullups

### Laser Diodes MODBUS + ST3485EIY
- Use a UART with 485 driver chip (ST3485EIY? @ 3.3V )
- Termination resistor (TODO ?K) at NH8 NH8
- 5V and ground to the NH8 from the LD bank
- Need HW flow control pins on microcontroller

For board files:
- W5500: UART1 GP8 (TX) & GP9 (RX) 3.3v, TODO need to assign HW flow control pins
- TODO Sort out what to use on the Nucleo
  - UART4/5/7/8/9/12 or USART1/2/3/6/10/11 not LPUART1
  - CN9 4 D52 USART_B_RX PD6 USART2
  - CN9 6 D53 USART_B_TX PD5 USART2

### Laser Bank Power Enable
- 3.3V, GPIO to enable of power driver

For board files:
- W5500: TODO
- Nucleo: CN9 13 D72 IO PB2 -

### Power Switch
- 3.3V, GPIO to opto in AC switch, current-limiting resistor in case of short

For board files:
- W5500: gpio0 6
- Nucleo: CN9 15 D71 IO PE9 -

### DS18B20 1Wire Temperature Sensor
- 3.3v digital temp sensor for good measure

For board files:
- W5500: TODO
- Nucleo: CN9 30 D64 IO PG1 - (can be configured as UART9_TX)

Nucleo board pins in use:
- USB
  - PB13 USB PD controller side for the CC1 pin
  - PA12 USB differential pair P
  - PA11 USB differential pair M
  - PB14 USB PD controller side for the CC2 pin
- STLINK-V3EC
  - PC3 USB PD controller side for the CC1 pin
  - PC4 USB PD controller side for the CC2 pin
  - PB15 USB differential pair P
  - PB14 USB differential pair M
- RMII interface for ETH

---
## 2. Design and Approach

The project is designed to create a controller capable of:
- **Handling** HIPSEC's fiber PCB system requirements  
- **Following** MQTT commands over TCP
- **Publishing sensor data** (photodiode voltages) via MQTT

### Approach
- Low Complexity: KISS, minimal `tasks`, minimal inter-task coordination complexity.
- Devices (photodiodes, attenuators, switches) are instantiated once, lifetime-managed explicitly.
- Clear concurrency model: Only asynchronous hardware polling (photodiodes) is task-separated.
- Memory efficiency: Avoid dynamic allocation; careful use of `string_view`, fixed buffers, and compile-time constants.
- Structured around extensibility (e.g., laser diode driver support, additional sensors).
- Zephyr: Use Zephyr's tooling wherever possible vs rolling our own
- C++ exceptions are disabled: error handling is done via return codes and checking object states.

---
## 3. Code Structure

### main.c
 - Some Settings management stubs,  probably GPT via ML.
   - setting_handler(name, size, read_cb, cb_args)
   - Zephyr macro that neglects to provision for setting, comitting, or exporting settings
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
   - TODO coo_mqtt libary should bundle more stuff (i. supcription, cmd handler, and mqtt portions of main loop)
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
TODO: Incorporate settings persistance: https://github.com/zephyrproject-rtos/zephyr/blob/main/samples/subsys/settings/src/main.c#L392
TODO: Veryfy I'm dealing with networking properly and setup DHCP with fallback to static. see https://github.com/zephyrproject-rtos/zephyr/tree/main/samples/net/common

### command.c
- defines some json types TODO should be in json utils, no?

- defines laser_t enum
- get_laser_channel()
  - converts a laser name string to a `laser_t` enum
- wait_laser_boot()
  - sleeps a bit of time to allow for laser booting

- power fuctions
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
  - looks in the json paylod for 'msg_type' and sets the type appropriately.
  - that it is looking for 'msg_type' relies on impliciy macro work and variable names, which coming back to is clever 
    of Zephyr but without more usages or explaination, VERY unclear to the uninitiated
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
    - parses command payload JSON of form { "value": [input_string, output_string] } for input and output route names
      - TODO the parsing here looks suspect.
    - gets the route via `mems_router_get_route()`
    - walks through route and gets each switch in route with `mems_router_find_switch()` setting each to required state
    - builds response with `_msg_builder()` w/ payload of {'status': 'OK'} on completion or {'error': <error message>} in event of failure
- defines `OutMsg mems_set(`Command`)
  - parses mems switch name from command key with `parse_key_pair` '<don't care>\<mems_switch_name>'
  - parses the desired switch state out of {"value": "A"|"B"}
  - looks for switch with `mems_router_find_switch()` and gets state with `mems_switch_get_state()`
  - builds response with `_msg_builder()` w/ payload of {"value":"U"|"A"|"B"} or {"error": <errmsg>} 
    - TODO add 'A?' and 'B?' support
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
  - parses atten name and setting name from command key with `parse_key_pair` '<laser>\<setting>'
  - gets atten channel with `get_laser_channel()`
    - TODO get rid of hardcoded int offset (+5) and make work with laser enum
  - if setting is "value"|"valuedb":
    - gets voltage and db setting of attenuator with  `attenuator_get()`
    - builds response with `_msg_builder()` w/ payload of {"voltage": float, "db": float}
  - if setting is "coeff":
    - builds response with `_msg_builder()` w/ payload of {"db2volt": [float,..], "volt2db": [float,..]} 
  - errors get a response built with {"error": <errmsg>}
- `OutMsg atten_setting_set(const struct Command *cmd)`
  - parses atten name and setting name from command key with `parse_key_pair` '<laser>\<setting>'
  - gets atten channel with `get_laser_channel()`
      - TODO get rid of hardcoded int offset (+5) and make work with laser enum
  - if setting is "value"|"valuedb":
    - parses the setting value as a float from {"value": float}
    - uses `attenuator_set()` to set the attenuation as either a dB or raw level
  - if setting is "coeff":
    - parses json payload for quadratic coeff for {"db2volt": [float,float,float], "volt2db": [float,float,float]}
    - fetches the current attenuation as dB
    - updates the coeffs
    - resets to the attenuation to the same dB
  - builds response with `_msg_builder()` w/ payload of {"status": "OK"} or errors get a response built with {"error": <errmsg>}
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
  - gets clock time with `clock_gettime`
    - TODO we want this to be either NTP or tracable to something synchronizable with the FEI `CLOCK_REALTIME` may not be it
  - builds `OutMsg` on hardcoded `dt/hsfib-tib/photodiode` with QoS = 0
    payload of {"yj": <yj reading>, "hk": "<hk reading>, "time": <timestamp in ms>}
    - TODO move pub address to, at a minimum, a central IoT/MQTT point in this file, ideally for whole project/kernel config
    - TODO finiaize time format
    - TODO consider adding an "hk_time_offset" value
    - TODO consider not emitting if photodiode power is off or flagging in JSON
  - put message in the `photodiode_queue` for consumption elsewhere, drop sample if queue full
  - sleep until the net polling interval don't worry about the overflow ever 300Myr
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
- mems_switch struct
  - gpio device and pin A and B
  - char state
  - char name[]
- mems_route structs 
  - mems_route, mems_route_step|id|key
  - key: pointers to input and output names
  - id: char arrays for input and output location names
  - step: pointer to a switch name and a char state (the required state)
  - the route itself
- mems_router struct
  - *switches[] - pointers to mems switch structs
  - routs[] - array of routes
  - num_switches & num_routes
- mems_active_routes struct

#- TODO review rest of code and update what follows


 
### High-Level Architecture:

| Area             | Description                                                                                                                                                                     |
|------------------|---------------------------------------------------------------------------------------------------------------------------------------------------------------------------------|
| `main.c`         | Initializes hardware, tasks, queues, network and runs MQTT TODO needs tidy and refactor after sorting out libraries                                                             |
| `command.c`      | command callbacks, exercising of hardware, where JSON gets parsed and created (mostly), the dispatch system `Command` creation from MQTT is in `main.c` which is rather muddled |
| `photodiode.c`     | a zephyr task that continuously reads the ADC to monitor the two photodiods at a fixed rate and ships date to an output queue                                                   |
| `attenuator.c` | functions to init/get/set attenuators
| `maiman.cpp`     | A Maiman laser diode object     |
| `mems_switching.c` |                                                                                                          |
| `devices.c`     |                                                                                                     |


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
