# Libraries and Local Wrappers

## Zephyr Subsystems

- GPIO, I2C, ADC, DAC, UART, Modbus, settings/NVS, watchdog, console, networking,
  MQTT, SNTP, sensor, and 1-Wire APIs are used directly.
- Command parsing uses Zephyr JSON descriptors plus small app/library helpers.
- The DS18B20 ambient sensor uses the Zephyr sensor API.

## App-Local Domain Modules

- `devices.c`: board strap detection and board-profile setup.
- `mems_switching.c`: MEMS switch state, route lookup, and duty-cycle tick work.
- `attenuator.c`: paired-DAC logical attenuator channels using zscilib math for
  the FVOA attenuation model.
- `maiman.c`: raw/scaled Modbus register wrapper for Maiman drivers.
- `lasers.c`: laser-bank GPIO, Maiman profile, status, and tuning helpers.
- `laserbank_control.c`: TIB heater auto/override and bank temperature policy.
- `photodiode.c`: ADS1115 sampling, dark calibration, and rolling sample windows.
- `throughput_monitor.c`: measure-throughput streaming, route-loss application,
  and optional photodiode autolevel control.
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
- Add test coverage for the attenuator model and inverse once calibration
  expectations are owner-approved.
