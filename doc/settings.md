# Settings and Persistence

## Ownership

This document is the consolidated inventory of firmware values that survive a
reboot. It is intended to save readers from piecing together persistence
behavior from command handlers, runtime modules, and NVS IDs.

`app_settings.c` owns the app settings machinery: defaults, direct Zephyr NVS
records, runtime snapshots, and bulk invalidation when a board or firmware
schema change requires settings to be rebuilt. Other modules hand values to this
machinery when they need app-owned intent, calibration, or counters restored.

Maiman modules own their EEPROM-backed driver parameters. Laser diode property
tables in `laser_properties.h` are compile-time defaults and estimates.
Command dispatch owns the separate last-command NVS record used for boot/status
visibility.

## Stored NVS Records

Current app NVS records include:

- Schema marker.
- Board type.
- Boot count.
- Last known UTC time in milliseconds.
- IP settings as one record.
- MQTT broker host/port as one record.
- Last command metadata as one command-dispatch record.
- One attenuator coefficient record per logical channel.
- One photodiode settings record per photodiode channel, including active dark
  window result, lowest stored dark window result, noise threshold,
  responsivity, transimpedance, relay power intent, and auto-off delay.
- Laser-bank heater policy.
- One laser policy record per laser channel, including laser calibration/user
  intent and the operator-confirmed Maiman driver serial used as a physical
  association check.
- One laser total-emitting counter record per laser channel.
- One route-loss table-entry record per configured route/laser output, up to
  the fixed route-loss table limit.
- One MEMS intent record containing per-switch static state intent, per-switch
  direct-toggle restart metadata, and per-channel split restart metadata.

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
- Serial guard defaults to 30 s from Kconfig but is runtime-only.
- Last known UTC defaults to unset. Once SNTP or a `time` command sets the
  realtime clock, the value is persisted and restored on later boots until a
  fresher time source updates it.
- Attenuator coefficients default to
  `b = gain * (slope * dac_mv + offset)`, with DAC output millivolts in the
  0-3300 mV span and default gain 1.533, until calibrated/stored.
- Photodiode dark windows default to 0 mV with 0 mV RMS. YJ and HK have
  different default gain/noise warning values.
- Laser expected serials default to the initial known driver/diode association.
  Operators may update the value through `laser/settings` after confirming a
  replacement driver is physically associated with the intended diode.
- Route-loss records default to absent. Missing route-loss settings are treated
  as loss-free transmission, `1.0`.
- MEMS switch intent defaults to absent. A switch with no stored intent defaults
  to A at boot. Toggle and split restart metadata defaults to dormant/absent.

## Persistence Side Effects

NVS writes happen synchronously through Zephyr NVS and may block the caller.
Command handlers that set `persist:true` can therefore block in the executor
thread.

Photodiode active dark windows, forced dark values, lowest-dark records, and
persisted dark updates happen only through `pd/dark/<channel>`. Duration-based
dark captures are armed by the command and committed later by the photodiode
sampler thread.
`pdsettings/<channel>` owns photodiode response settings and relay power intent;
it does not update or report dark summaries. `persist:true` on `pd/dark` writes
that dark settings snapshot to NVS.

MEMS static state requests and route applications update the persisted
per-switch intent once per command. Toggle and split commands persist the
commanded request metadata once per command. Runtime toggle/split progress is
not written back to NVS, so reboot resume, when enabled by
`CONFIG_RESUME_TOGGLE_STATE_AT_BOOT`, restarts the original commanded duration
rather than preserving elapsed or remaining time.

If NVS loading fails after a board has already been initialized in the field,
treat it as a human-intervention fault. At minimum, inspect logs and
reinitialize storage before trusting persisted calibration or network intent. A
first boot with no app schema marker clears the old storage layout, writes the
current schema marker, and uses defaults.

Schema v6 adds the per-laser `expected_serial` association field. Older schema
markers are not migrated; firmware clears the old app settings layout, writes
the v6 marker, and uses defaults.

## Intentionally Not Persisted

- Active routes are derived from current MEMS switch state and route tables.
  Applying a route persists the affected switch states as static switch intent,
  not as a named active-route object.
- Laser output state is not restored after reboot. Laser-bank heater mode is a
  persisted policy and defaults to `auto`; in auto mode it may power the bank
  after boot for temperature monitoring. The heater starts off unless auto mode
  or an override turns it on.
- DS2408 relay output state is not restored after reboot; relay outputs default
  inactive/off when the off-board expander is online during app setup.
- Serial guard active state and runtime holdoff changes are not restored after
  reboot.

## Not Currently Persisted

- Laser output current, temperature, pulse, or tuning state.
