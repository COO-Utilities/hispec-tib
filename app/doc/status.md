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
- ST Nucleo TODO
- rPi **W5500-EVB-PICO2**

### MEMS Switches (5V GPIO control via PCAL6416AHF, ad I2C 16x GPIO expander)
- 3.3V i2c, 5V gpio, 25mA max drive
- Address 0x20 (ADDR tied to ground) or 0x21 (use 0x20)
- Configure ase open drain for FFSW as they have 5V pullups
- Configure as push-pull for FFLS
- Require 2 pins per FFSW or FFLS

For board files:
- W5500: Use I2C1 on pins GP2 (SDA) and GP3 (SCL) (per existing design notes)
- W5500: I2C0 on pins GP4 (SDA) and GP5 (SCL) (per Zephyr code)
- TODO Sort out which and what to use on the Nucleo

At the drive level transistors are 
- SI3552DV (for FFLS)
  - N & P MOSFET
  - 2x each for FFSW
  - 2x 4.7k resistors for pullups
- DMP3085LSD (for FFSW)
  - 2x P  MOSFET
  - 1x each for FFLS
  - 1x 4.7k resistor for status line if using

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
- TODO Sort out which and what to use on the Nucleo

Breadboard Considerations:
- AF prototype board has 10K pullups, may need to get rid of as LL shifter boards also have pullups

### TIB & CAL Attenuator Drive DAC7578SPW 8 chan DAC driving 3x OPA2991 2 channel OpAmp
- 0 - 4095 (Vref, default=Vcc)
- Must not exceed Vmax of attenuator (6V for FVOA, so safe). Imax is 36.66 mA
- Use Vcc=3.3 with 1.51x opamp gain to avoid LL shifting
- OpAmp supplies required current to attenuator.
- I2C addr: 0x4C floating pin, 0x48 GND, 0x4A VCC) (use 0x4c float, default)

For board files:
- W5500: I2C1 on GP2 (SDA) and GP3 (SCL) (per existing design notes)
- W5500: I2C0 on pins GP4 (SDA) and GP5 (SCL) (per Zephyr code)
- TODO Sort out which and what to use on the Nucleo

Breadboard considerations:
- Kit board has 10K pullups, do we need to get rid of as have pullups on 3.3v side of LL translation?
- FVOA (resistive, typ~3.5V 80mA @ full atten)
- MSOA (resistive, typ~0.5-3.25V, DNE 4.5V, something like 24-38 mA, datasheet unclear)

### Laser Diodes MODBUS + ST3485EIY
- Use a UART with 485 driver chip (ST3485EIY? @ 3.3V )
- Termination resistor (TODO ?K) at NH8 NH8
- 5V and ground to the NH8 from the LD bank
- Need HW flow control pins on microcontroller

For board files:
- W5500: UART1 GP8 (TX) & GP9 (RX) 3.3v, TODO need to assign HW flow control pins
- TODO Sort out what to use on the Nucleo

### Laser Bank Power Enable
- 3.3V, GPIO to enable of power driver

For board files:
- W5500: TODO
- Nucleo: TODO

### Power Switch
- 3.3V, GPIO to opto in AC switch, current-limiting resistor in case of short

For board files:
- W5500: gpio0 6
- TODO Sort out what to use on the Nucleo

### DS18B20 1Wire Temperature Sensor

For board files:
- W5500: TODO
- Nucleo: TODO

---
## 2. Design Intent and Philosophy

The project is designed to create a **reliable, networked controller** capable of:
- **Following** MQTT commands over TCP
- **Publishing sensor data** (photodiode voltages) via MQTT

### Key design philosophies: (aka potential GPT slop)
- **Minimal moving parts**: minimal tasks, minimal inter-task coordination complexity.
- **Single Ownership**: Devices (photodiodes, attenuators, switches) are instantiated once, lifetime-managed explicitly.
- **Clear concurrency model**: Only genuinely asynchronous hardware polling (photodiodes) is task-separated.
- **Memory efficiency**: Avoid dynamic allocations where possible; careful use of `string_view`, fixed buffers, and compile-time constants.
- **Future resilience**: Structured around extensibility (e.g., laser diode driver support, additional sensors).
- **Zephyr Realism**: Pragmatic use of mutexes where needed, but prefer single-task device ownership when possible.
- **Avoid runtime exceptions**: C++ exceptions are disabled; error handling is done via return codes and checking object states.

---
## 3. Code Structure and Instructions

# TODO review code and update what follows


### High-Level Architecture:

| Area                  | Description                                                                                      |
|-----------------------|--------------------------------------------------------------------------------------------------|
| `main.cpp`            | Initializes hardware, tasks, queues, and the shared context object                               |
| `hardware_context.h`  | Defines the shared `HardwareContext` passed to tasks                                             |
| `executor_task.cpp`   | Main command dispatcher: acts on inbound mKTL commands and builds replies                        |
| `photodiode_task.cpp` | Asynchronously polls photodiode voltages and sends updates via the PUB queue                     |
| `photodiode.cpp`      | A photodiode object                                                                              |
| `maiman.cpp`          | A Maiman laser diode object                                                                      |
| `attenuator.cpp`      | An attenuator object                                                                             |
| `coms_task.cpp`       | Manages Zyre beaconing, incoming WHISPER handling, actually sending ack/replys, and outbound PUB |
| `pico_zyre.cpp`       | Provides lightweight Zyre protocol support (ENTER broadcast, WHISPER receiving, PUB sending)     |
| `mems_switching.cpp`  | Abstracts control over MEMSSwitch devices and the MEMSRouter class                               |
| `mktl_keys.h`         | Centralized mKTL key prefixes/suffixes for matching against inbound commands                     |

### Key libraries:

- **nlohmann/json**: Used for lightweight JSON parsing of inbound mKTL messages
- **Pico SDK**: Base microcontroller support
- **WIZnet ioLibrary Driver**: Raw SPI Ethernet stack for W5500
- **FreeRTOS**: Task scheduling and resource sharing

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
