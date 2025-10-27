# Zephyr COO Template

Caltech Optical Observatories (COO) standardized template for Zephyr RTOS firmware projects. This template provides a consistent foundation with persistent settings, watchdog support, networking, and QEMU emulation built-in from day one.

## Quick Start

```bash
# Create workspace
west init -m https://github.com/mikelangmayr/zephyr-coo-template --mr main my-project
cd my-project && west update

# Build and test in QEMU (no hardware needed)
cd zephyr-coo-template
west build -b qemu_x86 app
west build -t run  # Press Ctrl+A, X to exit

# Or build for hardware
west build -b nucleo_f302r8 app --pristine
west flash
```

## COO Commons Library

The [lib/coo_commons](lib/coo_commons/) library provides production-ready utilities shared across all COO projects:

### PID Controller
Reusable proportional-integral-derivative loops for temperature and motion control with configurable gains, anti-windup protection, and output limiting.

### Network Stack
Complete networking support with high-level connection manager integration (L4 events, DHCP/static IP) and low-level socket utilities (TCP/UDP create, connect with timeout, send/recv with retry).

### JSON Utilities
Structured message handling including telemetry encoding, command parsing with hierarchical keys (`device/setting`), message type detection (GET/SET/RESPONSE), and standard error responses.

### MQTT Client
Production-ready wrapper with automatic retry, subscription management (up to 4 topics), optional TLS, event-driven callbacks, and QoS support (0, 1, 2).

**Usage Example:**
```c
#include <coo_commons/mqtt_client.h>
#include <coo_commons/network.h>

void on_mqtt_message(const struct mqtt_publish_param *pub) {
    LOG_INF("Topic: %.*s", pub->message.topic.topic.size,
            pub->message.topic.topic.utf8);
}

int main(void) {
    // Initialize network
    coo_network_init(NULL);
    coo_network_wait_ready(0);

    // Connect to MQTT broker
    struct mqtt_client client;
    coo_mqtt_init(&client, "device-id");
    coo_mqtt_set_message_callback(on_mqtt_message);
    coo_mqtt_add_subscription("sensors/#", MQTT_QOS_1_AT_LEAST_ONCE);
    coo_mqtt_connect(&client);
    coo_mqtt_run(&client);

    return 0;
}
```

**Configuration:**
Enable modules in [app/prj.conf](app/prj.conf):
```
CONFIG_COO_COMMONS=y   # Always enabled (includes PID)
CONFIG_COO_NETWORK=y   # Requires CONFIG_NETWORKING=y
CONFIG_COO_JSON=y      # Requires CONFIG_JSON_LIBRARY=y
CONFIG_COO_MQTT=y      # Requires CONFIG_MQTT_LIB=y
```

For MQTT, configure broker in Kconfig:
```
CONFIG_COO_MQTT_BROKER_HOSTNAME="mqtt.example.com"
CONFIG_COO_MQTT_BROKER_PORT="1883"
CONFIG_COO_MQTT_PAYLOAD_SIZE=512
```

See headers in [include/coo_commons/](include/coo_commons/) for full API documentation.

## Template Features

- **Persistent Settings** - NVS-backed configuration survives reboots
- **Watchdog** - Automatic system recovery from hangs
- **QEMU Support** - Test logic without hardware (`qemu_x86`, `qemu_cortex_m3`)
- **CI/CD Ready** - GitHub Actions with Zephyr SDK caching
- **Custom Boards** - Example board definitions and device tree overlays
- **Out-of-tree Components** - Drivers and libraries without modifying Zephyr
- **Documentation** - Doxygen API docs and Sphinx user docs

## Prerequisites

Follow the [Zephyr Getting Started Guide](https://docs.zephyrproject.org/latest/getting_started/index.html) to install:

- West: `pip3 install west`
- CMake >= 3.20
- Python >= 3.9
- Zephyr SDK
- QEMU: `apt install qemu-system-x86 qemu-system-arm` or `brew install qemu`

Set environment:
```bash
export ZEPHYR_TOOLCHAIN_VARIANT=zephyr
export ZEPHYR_SDK_INSTALL_DIR=~/zephyr-sdk-0.16.x
```

## Building

**For QEMU:**
```bash
west build -b qemu_x86 app        # x86 emulation
west build -b qemu_cortex_m3 app  # ARM Cortex-M3
west build -t run                 # Run in emulator
```

**For Hardware:**
```bash
west build -b nucleo_f302r8 app   # STM32 Nucleo
west build -b custom_plank app    # Custom nRF52840 board
west flash                        # Flash to connected device
west flash --runner jlink         # Use specific debugger
```

**Debug Build:**
```bash
west build -b $BOARD app -- -DEXTRA_CONF_FILE=debug.conf
```

**Clean Rebuild:**
```bash
west build -b $BOARD app --pristine
```

## Testing

```bash
west twister -T tests --integration  # Run integration tests
west twister -T app --integration    # Run application tests
```

## Persistent Settings & Watchdog

The template demonstrates NVS and watchdog in [app/src/main.c](app/src/main.c):

```c
#include <zephyr/settings/settings.h>
#include <zephyr/drivers/watchdog.h>

// Initialize and load settings from flash
settings_subsys_init();
settings_load();

// Save settings (persists across reboots)
settings_save_one("app/key", &value, sizeof(value));

// Initialize watchdog (5 second timeout)
watchdog_init(&wdt, &wdt_channel);

// Feed periodically in main loop
wdt_feed(wdt, wdt_channel);
```

Settings are stored in the `storage_partition` defined in board device tree overlays.

## Troubleshooting

**Build fails with "Zephyr not found":**
```bash
cd my-project
west update
west list  # Verify configuration
```

**"No CMAKE_C_COMPILER could be found":**
```bash
export ZEPHYR_TOOLCHAIN_VARIANT=zephyr
export ZEPHYR_SDK_INSTALL_DIR=~/zephyr-sdk-0.16.x
```

**QEMU doesn't run:**
```bash
# Ubuntu/Debian
sudo apt-get install qemu-system-x86 qemu-system-arm

# macOS
brew install qemu
```

**Settings don't persist:**
- Verify `CONFIG_SETTINGS_NVS=y` in prj.conf
- Check board device tree has `storage_partition` defined
- Try clean build: `west build -b $BOARD app --pristine`

**Enable verbose logging:**
```bash
west build -b $BOARD app -- -DEXTRA_CONF_FILE=debug.conf
# Or in prj.conf: CONFIG_LOG_DEFAULT_LEVEL=4
```

## Documentation

Build API and user documentation locally:

```bash
cd doc
pip install -r requirements.txt

doxygen    # API docs → _build_doxygen/html/index.html
make html  # User docs → _build_sphinx/html/index.html
```

CI automatically publishes to GitHub Pages.

## Project Structure

```
zephyr-coo-template/
├── app/                    # Application entry point and configuration
│   ├── src/main.c         # Demonstrates NVS, watchdog, sensors
│   ├── boards/            # Board overlays (nucleo, custom_plank, QEMU)
│   └── prj.conf           # Kconfig options
├── lib/
│   ├── coo_commons/       # Shared library (PID, network, JSON, MQTT)
│   └── custom/            # Example custom library
├── include/
│   ├── coo_commons/       # COO commons public headers
│   └── app/lib/           # Other public headers
├── drivers/               # Custom drivers (blink LED, example sensor)
├── boards/                # Custom board definitions (custom_plank)
├── tests/                 # Integration tests
├── doc/                   # Doxygen + Sphinx
└── .github/workflows/     # CI with QEMU builds
```

## Workflow

1. Fork/clone this template
2. Customize device tree and board files for your hardware
3. Develop application logic using COO commons library
4. Test in QEMU for rapid iteration
5. Deploy to hardware with `west flash`

## Links

- [Zephyr Documentation](https://docs.zephyrproject.org/)
- [West Tool](https://docs.zephyrproject.org/latest/develop/west/index.html)
- [Device Tree Guide](https://docs.zephyrproject.org/latest/build/dts/index.html)
