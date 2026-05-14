# Settings and Persistence

## Ownership

`app_settings.c` owns app-level persistent settings under named top-level
Zephyr settings subtrees. It initializes defaults, loads stored values, and
protects the runtime snapshot with a mutex.

Maiman modules own their EEPROM-backed driver parameters. Laser diode property
tables in `laser_properties.h` are compile-time defaults and estimates.

## Stored Keys

Current app settings include:

- `board/type`
- `serial/holdoff_s`
- `boot/count`
- `ip/trydhcpfirst`
- `ip/preferdhcpdns`
- `ip/preferdhcpntp`
- `ip/ip`
- `ip/subnet`
- `ip/gateway`
- `ip/dns`
- `ip/ntp`
- `mqtt/broker`
- `atten/<channel>/physical/<physical>/slope`
- `atten/<channel>/physical/<physical>/offset`
- `pd/yj/dark_mv`
- `pd/yj/lowest_dark_mv`
- `pd/yj/lowest_dark_valid`
- `pd/yj/noise_warn_rms_mv`
- `pd/yj/gain_v_per_uw`
- `pd/hk/dark_mv`
- `pd/hk/lowest_dark_mv`
- `pd/hk/lowest_dark_valid`
- `pd/hk/noise_warn_rms_mv`
- `pd/hk/gain_v_per_uw`

## Board-Type Reset Policy

The detected board type is treated as immutable hardware identity. If a stored
board type exists and a later boot detects a different valid board type,
`app_settings_note_board_type()` clears all other app settings and persists the
new board type. This prevents settings from one physical PCB profile from being
silently reused on another.

## Defaults

- IP defaults come from Zephyr network config symbols.
- MQTT defaults come from `CONFIG_COO_MQTT_BROKER_HOSTNAME` and
  `CONFIG_COO_MQTT_BROKER_PORT`; persistence uses one `mqtt/broker`
  `<host-or-ip>:<port>` value.
- Serial guard defaults to 30 s.
- Attenuator coefficients default to a linear `b = slope * voltage + offset`
  model that maps the 0-4096 mV DAC span onto `b = 0..8` until
  calibrated/stored.
- Photodiode dark defaults to 0 mV. YJ and HK have different default gain/noise
  warning values.

## Persistence Side Effects

Settings writes happen synchronously through Zephyr settings APIs and may block
the caller. Command handlers that set `persistent:true` can therefore block in
the executor thread.

Photodiode stored dark updates happen in the sampler thread when a user-started
dark measurement completes with `store:true`.

If settings loading fails after a board has already been initialized in the
field, treat it as a human-intervention fault. At minimum, inspect logs and
reinitialize settings before trusting persisted calibration or network intent.
A first boot with no stored settings is normal and should use defaults.

## Intentionally Not Persisted

- Active routes are derived from current MEMS switch state and route tables.
- Laser output state is not restored after reboot. Laser-bank heater mode is a
  persisted policy and defaults to `auto`; in auto mode it may power the bank
  after boot for temperature monitoring. The heater starts off unless auto mode
  or an override turns it on.
- DS2408 relay output state is not restored after reboot.

## Not Currently Persisted

- MEMS switch state.
- AS split requested/actual state.
- Laser output current, temperature, pulse, or tuning state.
- Last command metadata.
