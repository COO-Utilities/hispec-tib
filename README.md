# HiSPEC-TIB - Zephyr RTOS Firmware

High-Speed Spectroscopic Camera for Palomar (HiSPEC) - Telescope Interface Box (TIB) firmware built on Zephyr RTOS using the COO (Caltech Optical Observatories) standardized template.

## Overview

The HiSPEC-TIB controls optical routing, laser calibration sources, and attenuators for the HiSPEC spectrograph. This firmware runs on a WIZnet W5500-EVB-Pico2 board (RP2350 microcontroller) and provides MQTT-based remote control over Ethernet.

### Hardware Features

- **MEMS Optical Switches**: 8 dual-channel fiber switches for beam routing
- **Maiman Laser Controllers**: Modbus control of calibration laser sources
- **Optical Attenuators**: DAC-controlled variable optical attenuators with polynomial calibration
- **Photodiode Monitoring**: ADC-based optical power monitoring at 50Hz
- **Watchdog**: Automatic recovery from system hangs
- **Persistent Settings**: NVS-backed configuration storage

## Quick Start

```bash
# Create workspace
west init -m https://github.com/mikelangmayr/hispec-tib --mr main hispec-zephyr
cd hispec-zephyr && west update

# Build for W5500-EVB-Pico2
cd hispec-tib
west build -b w5500_evb_pico2/rp2350a/m33 app
west flash
```

## COO Commons Library

This project uses the [lib/coo_commons](lib/coo_commons/) library for production-ready utilities:

### MQTT Client
Production-ready MQTT 5.0 wrapper with automatic retry, subscription management, event callbacks, and QoS support (0, 1, 2).

**MQTT Command Interface:**
- **Subscribe topic**: `cmd/hsfib-tib/req/#`
- **Response topic**: Provided in MQTT 5.0 `response_topic` property
- **Broker**: `jebcontrol.caltech.edu:1883`

### Network Stack
Complete networking support with connection manager integration (L4 events, DHCP with static IP fallback).

### JSON Utilities
Structured message handling for telemetry encoding, command parsing with hierarchical keys (`device/setting`), and message type detection (GET/SET/RESPONSE).

### PID Controller
Reusable proportional-integral-derivative loops for temperature and motion control.

**Usage Example:**
```c
#include <coo_commons/mqtt_client.h>
#include <coo_commons/network.h>

// Initialize network and MQTT
coo_network_init(NULL);
coo_network_wait_ready(K_FOREVER);

struct mqtt_client client;
coo_mqtt_init(&client, "hsfib-tib");
coo_mqtt_add_subscription("cmd/hsfib-tib/req/#", MQTT_QOS_2_EXACTLY_ONCE);
coo_mqtt_set_message_callback(on_mqtt_message);
coo_mqtt_connect(&client);
```

**Configuration in [app/prj.conf](app/prj.conf):**
```
CONFIG_COO_COMMONS=y
CONFIG_COO_NETWORK=y
CONFIG_COO_MQTT=y
CONFIG_COO_MQTT_BROKER_HOSTNAME="jebcontrol.caltech.edu"
CONFIG_COO_MQTT_BROKER_PORT="1883"
```

## MQTT Command Protocol

All commands follow a standardized JSON format with MQTT 5.0 properties:

### Common Fields
```json
{
  "msg_type": "get|set",
  "value": "<depends on command>"
}
```

### MEMS Switch Routing
**Topic**: `cmd/hsfib-tib/req/memsroute`
```json
{
  "msg_type": "set",
  "value": ["input_name", "output_name"]
}
```

### Individual MEMS Control
**Topic**: `cmd/hsfib-tib/req/mems/<name>`
```json
{
  "msg_type": "set|get",
  "value": "state"
}
```

### Laser Flux Control
**Topic**: `cmd/hsfib-tib/req/laser###/flux`
```json
{
  "msg_type": "set|get",
  "value": <flux_value>
}
```

### Attenuator Control
**Topic**: `cmd/hsfib-tib/req/atten###/value[dB]`
```json
{
  "msg_type": "set|get",
  "value": <attenuation_in_dB>
}
```

**Topic**: `cmd/hsfib-tib/req/atten###/coeff`
```json
{
  "msg_type": "set|get",
  "value": [coeff0, coeff1, ..., coeffN]
}
```

### System Status
**Topic**: `cmd/hsfib-tib/req/status`
```json
{
  "msg_type": "get"
}
```
**Response**:
```json
{
  "version": 1,
  "uptime": <seconds>,
  "a_laser_is_on": "true|false|error:...",
  "time": <unix_timestamp>
}
```

## Application Architecture

### Thread Structure
- **Main Thread**: MQTT event loop, network management, watchdog feeding
- **Executor Thread**: Command dispatch and execution
- **Photodiode Thread**: 50Hz optical power sampling
- **Photodiode Publisher**: 100Hz work queue for telemetry publishing

### Message Queues
- `inbound_queue`: MQTT commands → Executor
- `outbound_queue`: Executor/Photodiode → MQTT publisher
- `photodiode_queue`: ADC samples → Publisher

## Building

**For Hardware:**
```bash
west build -b w5500_evb_pico2/rp2350a/m33 app
west flash
```

**Debug Build:**
```bash
west build -b w5500_evb_pico2/rp2350a/m33 app -- -DEXTRA_CONF_FILE=debug.conf
```

**Clean Rebuild:**
```bash
west build -b w5500_evb_pico2/rp2350a/m33 app --pristine
```

## Device Tree Configuration

Hardware is configured via [app/boards/w5500_evb_pico2_rp2350a_m33.overlay](app/boards/w5500_evb_pico2_rp2350a_m33.overlay):

- **I2C0**: DAC7578 (attenuators), PCAL6416A (GPIO expander for MEMS)
- **I2C1**: ADS1115 (photodiode ADCs)
- **UART1**: Modbus for Maiman laser controllers
- **SPI0**: W5500 Ethernet controller
- **Flash**: RP2350 internal flash with NVS storage partition

## Persistent Settings & Watchdog

The application demonstrates NVS and watchdog usage:

```c
// Initialize and load settings from flash
settings_subsys_init();
settings_load();

// Save settings (persists across reboots)
settings_save_one("tib/key", &value, sizeof(value));

// Initialize watchdog (5 second timeout)
watchdog_init(&wdt, &wdt_channel);

// Feed periodically in main loop
wdt_feed(wdt, wdt_channel);
```

Settings are stored in the `storage_partition` defined in the board device tree overlay.

## Network Configuration

### DHCP with Static IP Fallback
The application uses Zephyr's connection manager for automatic network setup:

```c
conn_mgr_all_if_up(true);  // Bring up interfaces with DHCP
coo_network_wait_ready(K_FOREVER);  // Wait for L4 connectivity
```

**Static IP Configuration** (in [app/prj.conf](app/prj.conf)):
```
CONFIG_NET_CONFIG_MY_IPV4_ADDR="192.168.1.111"
CONFIG_NET_CONFIG_MY_IPV4_NETMASK="255.255.255.0"
CONFIG_NET_CONFIG_MY_IPV4_GW="192.168.1.1"
```

## Prerequisites

Follow the [Zephyr Getting Started Guide](https://docs.zephyrproject.org/latest/getting_started/index.html) to install:

- West: `pip3 install west`
- CMake >= 3.20
- Python >= 3.9
- Zephyr SDK

Set environment:
```bash
export ZEPHYR_TOOLCHAIN_VARIANT=zephyr
export ZEPHYR_SDK_INSTALL_DIR=~/zephyr-sdk-0.16.x
```

## Project Structure

```
hispec-tib/
├── app/                           # HiSPEC-TIB application
│   ├── src/
│   │   ├── main.c                # Main application with MQTT loop
│   │   ├── command.c/h           # Command parser and dispatcher
│   │   ├── devices.c/h           # Device initialization
│   │   ├── attenuator.c/h        # Attenuator control via DAC
│   │   ├── maiman.c/h            # Maiman laser Modbus driver
│   │   ├── photodiode.c/h        # Photodiode ADC monitoring
│   │   └── mems_switching.c/h    # MEMS switch routing logic
│   ├── boards/
│   │   └── w5500_evb_pico2_rp2350a_m33.overlay  # Hardware config
│   └── prj.conf                  # Kconfig options
├── lib/
│   └── coo_commons/              # Shared COO library (PID, MQTT, network, JSON)
├── include/
│   └── coo_commons/              # COO commons public headers
├── drivers/                      # Custom drivers (blink LED, sensors)
├── boards/                       # Custom board definitions
├── tests/                        # Integration tests
├── doc/                          # Doxygen + Sphinx documentation
└── .github/workflows/            # CI with Zephyr builds
```

## Troubleshooting

**Build fails with "Zephyr not found":**
```bash
cd hispec-zephyr
west update
west list  # Verify configuration
```

**"No CMAKE_C_COMPILER could be found":**
```bash
export ZEPHYR_TOOLCHAIN_VARIANT=zephyr
export ZEPHYR_SDK_INSTALL_DIR=~/zephyr-sdk-0.16.x
```

**Settings don't persist:**
- Verify `CONFIG_SETTINGS_NVS=y` in prj.conf
- Check board overlay has `storage_partition` defined
- Try clean build: `west build -b w5500_evb_pico2/rp2350a/m33 app --pristine`

**Enable verbose logging:**
```bash
west build -b w5500_evb_pico2/rp2350a/m33 app -- -DEXTRA_CONF_FILE=debug.conf
# Or in prj.conf: CONFIG_LOG_DEFAULT_LEVEL=4
```

**Network not connecting:**
- Check Ethernet cable connection
- Verify W5500 SPI configuration in device tree
- Check DHCP server or configure static IP
- Enable network debug: `CONFIG_NET_LOG_LEVEL_DBG=y`

## Documentation

Build API and user documentation locally:

```bash
cd doc
pip install -r requirements.txt

doxygen    # API docs → _build_doxygen/html/index.html
make html  # User docs → _build_sphinx/html/index.html
```

## Development Notes

### From tib-zephyr Migration
This project was migrated from the original `tib-zephyr` repository to use the COO standardized template. Key improvements:

- **Replaced custom MQTT client** with `coo_commons/mqtt_client` for better maintainability
- **Added coo_commons/network** helper for simplified network initialization
- **Integrated watchdog** for automatic recovery from hangs
- **Added NVS settings** for persistent configuration (stub implementation, ready for expansion)
- **Standardized project structure** following COO template conventions
- **Improved documentation** with consistent formatting and examples

### Original TODOs (from tib-zephyr)
- UUID generation for unique device identification
- Expand settings persistence for device calibration data
- Verify DHCP → static IP fallback behavior across link failures
- Test MQTT reconnection across network disruptions

## Links

- [Zephyr Documentation](https://docs.zephyrproject.org/)
- [West Tool](https://docs.zephyrproject.org/latest/develop/west/index.html)
- [Device Tree Guide](https://docs.zephyrproject.org/latest/build/dts/index.html)
- [W5500-EVB-Pico2 Board](https://docs.zephyrproject.org/latest/boards/wiznet/w5500_evb_pico2/doc/index.html)
- [COO Zephyr Template](https://github.com/mikelangmayr/zephyr-coo-template)

## License

SPDX-License-Identifier: Apache-2.0

Copyright (c) 2025 Caltech Optical Observatories
