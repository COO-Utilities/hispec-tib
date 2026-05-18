# Settings and Persistence

## Ownership

`app_settings.c` owns app-level persistent settings under app-assigned Zephyr
NVS numeric IDs. It initializes defaults, loads stored values, and protects the
runtime snapshot with a mutex. The app does not use the string-keyed Zephyr
settings layer for its own persistence.

Maiman modules own their EEPROM-backed driver parameters. Laser diode property
tables in `laser_properties.h` are compile-time defaults and estimates.

## Stored NVS Records

Current app NVS records include:

- Schema marker.
- Board type.
- Serial guard holdoff.
- Boot count.
- IP settings as one record.
- MQTT broker host/port as one record.
- One attenuator coefficient record per logical channel.
- One photodiode settings record per photodiode channel.
- Laser-bank heater policy.
- One laser policy record per laser channel.
- One laser total-emitting counter record per laser channel.
- One route-loss table-entry record per configured route/laser output, up to
  the fixed route-loss table limit.

## Board-Type Reset Policy

The detected board type is treated as immutable hardware identity. If a stored
board type exists and a later boot detects a different valid board type,
`app_settings_note_board_type()` clears all other app settings and persists the
new board type. This prevents settings from one physical PCB profile from being
silently reused on another.

## Defaults

- IP defaults come from Zephyr network config symbols.
- MQTT defaults come from `CONFIG_COO_MQTT_BROKER_HOSTNAME` and
  `CONFIG_COO_MQTT_BROKER_PORT`; persistence stores host and port directly.
- Serial guard defaults to 30 s.
- Attenuator coefficients default to a linear `b = slope * voltage + offset`
  model that maps the 0-4096 mV DAC span onto `b = 0..8` until
  calibrated/stored.
- Photodiode dark defaults to 0 mV. YJ and HK have different default gain/noise
  warning values.
- Route-loss records default to absent. Missing route-loss settings are treated
  as loss-free transmission, `1.0`.

## Persistence Side Effects

NVS writes happen synchronously through Zephyr NVS and may block the caller.
Command handlers that set `persistent:true` can therefore block in the executor
thread.

Photodiode stored dark updates happen in the sampler thread when a user-started
dark measurement completes with `store:true`.

If NVS loading fails after a board has already been initialized in the field,
treat it as a human-intervention fault. At minimum, inspect logs and
reinitialize storage before trusting persisted calibration or network intent. A
first boot with no app schema marker clears the old storage layout, writes the
current schema marker, and uses defaults.

## Intentionally Not Persisted

- Active routes are derived from current MEMS switch state and route tables.
- Laser output state is not restored after reboot. Laser-bank heater mode is a
  persisted policy and defaults to `auto`; in auto mode it may power the bank
  after boot for temperature monitoring. The heater starts off unless auto mode
  or an override turns it on.
- DS2408 relay output state is not restored after reboot; relay outputs default
  inactive/off when the off-board expander is online during app setup.

## Not Currently Persisted

- MEMS switch state.
- AS split requested/actual state.
- Laser output current, temperature, pulse, or tuning state.
- Last command metadata.
