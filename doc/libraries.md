# Libraries and Local Wrappers

## Zephyr Subsystems

- GPIO, I2C, ADC, DAC, UART, Modbus, settings/NVS, watchdog, console, networking,
  MQTT, SNTP, sensor, and 1-Wire APIs are used directly.
- Command parsing uses Zephyr JSON descriptors plus small app/library helpers.
- The DS18B20 ambient sensor uses the Zephyr sensor API.

## App-Local Domain Modules

- `devices.c`: board strap detection and board-profile setup.
- `mems_switching.c`: MEMS switch state, route lookup, and duty-cycle tick work.
- `attenuator.c`: DAC-backed logical attenuator channels.
- `maiman.c`: raw/scaled Modbus register wrapper for Maiman drivers.
- `lasers.c`: laser-bank GPIO, Maiman profile, status, and tuning helpers.
- `photodiode.c`: ADS1115 sampling, dark calibration, and telemetry.
- `app_settings.c`: Zephyr settings ownership for app-level persistent state.
- `app_warning.c`: best-effort warning publication.

## `lib/coo_commons`

- `network.c`: IPv4 Ethernet bootstrap with DHCP/static/fallback profile logic.
- `mqtt_client.c`: MQTT 5 connect/process/subscription wrapper over Zephyr MQTT.
- `json_utils.c`: keyed primitive extraction helpers for constrained JSON
  command payloads.
- `pid.c`: generic PID helper, currently not central to the app runtime.

## Driver Stubs

- `drivers/gpio/ds2408`: project-local Zephyr GPIO driver for the DS2408
  1-Wire open-drain GPIO expander.

## Open items

- Decide whether `coo_commons` remains in this repository or is split into a
  reusable shared module.
- Add test coverage around `json_utils.c`, command normalization, and network
  profile selection.
- Review the DAC7578 integration against the two-DAC hardware description.

## LLM-resolved items requiring human review

- Old notes listed DS2408 as only an example/library idea. A project-local
  Zephyr GPIO driver now exists and is used by the Nucleo overlay.
- Old notes listed SNTP and settings as future library concerns. Both now have
  app implementations.
