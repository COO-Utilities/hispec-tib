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
- One attenuator coefficient record per logical channel, including `rms_db`
  for each physical FVOA. Coefficients and RMS are restored as one record.
- One photodiode settings record per photodiode channel, including active dark
  window result, lowest stored dark window result, noise threshold,
  responsivity, transimpedance, relay power intent, and auto-off delay.
- Laser-bank heater policy.
- One laser policy record per laser channel, including laser calibration/user
  intent and the operator-confirmed Maiman driver serial used as a physical
  association check, plus `fractional_noise`, `constant_noise_mw`, and
  `min_autolevel_current_ma`.
- One laser total-emitting counter record per laser channel.
- One laser thermistor-coefficient record per channel (`ntc_coeff_pct_per_c`,
  double in %/°C at 25°C, NVS IDs 0x0380–0x0385). Missing or invalid values
  retain the compiled coefficient. Reset deletes this record with channel policy.
- One route-loss table-entry record per configured route/source or split output,
  up to 32 records per PCB (16 per TIB channel). AS split records live on their
  own PCB. Broadband TIB CAL/return records use source `generic`.
- One MEMS intent record containing per-switch static state intent, per-switch
  direct-toggle restart metadata, and per-channel split restart metadata.

The autolevel-minimum addition changed the laser-policy record size under
schema 13. An incorrect record size selects compiled policy defaults for that
channel. Schema 14 now resets all older app settings as described below.

The thermistor-coefficient addition introduced a separate eight-byte record
under schema 13 without changing the laser-policy/calibration layout. The PCB
owns the configured coefficient; Maiman EEPROM retains driver-side values.
Profile preparation synchronizes the PCB value at the first explicit start after
bank power-up, and driver-backed settings updates program it immediately through
the existing stop/power flow. `persist:true` saves app NVS intent; it adds no
controller EEPROM SAVE operation. START/STOP retain their existing controller
persistence behavior. Settings queries always report PCB configuration;
engineering status reports controller readbacks while powered and null while off.
Before preparation, readbacks can differ from configuration. The coefficient
uses internal LD NTC 0x007F for programming and readback. External NTC 0x008A
is unused; the mapping correction changes no NVS layout or configured values.

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
- Physical FVOA coefficients default to the nominal 0-3300 mV DAC span, gain
  1.533, and `FVOA_DEFAULT_MAX_ATTEN_DB` until calibrated/stored. The
  `max_atten_db` coefficient is a per-FVOA leakage floor, not a logical
  two-FVOA attenuator limit. Each physical model defaults to `rms_db=2.0`;
  accepted autocalibration replaces it with the final fit's existing residual
  RMS. Manual replacement without RMS returns to that default.
- Photodiode dark windows default to 0 mV with 0 mV RMS. YJ and HK have
  different default gain/noise warning values.
- Laser optical-power uncertainty defaults to `fractional_noise=0.001` and
  `constant_noise_mw=0.0031 * (max_current_ma - threshold_current_ma) * efficiency_mw_per_ma`,
  using `default_laser_props` from the compiled table, never mutable settings.
  The resulting floors are 0.13505925 mW (1028), 0.041912 mW (1270),
  0.038688 mW (1430yj), 0.039432 mW (1430hk), 0.040176 mW (1510), and
  0.00913911 mW (2330). These app-only fields do not program Maiman.
- Laser `ld_serial` is the compiled manufacturer diode identity, distinct from
  the numeric expected Maiman driver serial. It is read-only and not persisted.
- Laser NTC defaults use vendor values: −4.4 %/°C for FLC and B=3900 K for Aero.
  Valid persisted property and NTC overrides continue to replace compiled defaults.
- Laser expected serials default to the initial known driver/diode association.
  Operators may update the value through `laser/settings` after confirming a
  replacement driver is physically associated with the intended diode.
- Laser `min_autolevel_current_ma` defaults to threshold plus 0.1 mA rounded up
  to the current-register grid. Validation normalizes it upward after threshold
  changes and rejects an empty range through nominal current. It is app policy,
  not the Maiman current limit. `initial_level` is a per-measurement fraction,
  defaults to 0.5 in firmware, and is not persisted.
- Laser settings `autooff_s` supplies the manual-command default; an explicit
  command value overrides it. Throughput owns its separate `off_in_s` deadline.
- Route-loss records default to absent. Lookup uses an exact route/source
  override, then a compiled source default, then that route's compiled generic
  default, then unity. [Hardware defaults](hardware.md#tib-route-loss-defaults)
  distinguish provisional YJ totals from nominal HK values. MM/SM generic
  defaults are transmission 0.98/0.60. Constants occupy no RAM/NVS slots;
  overrides replace the whole route, including transmission 1.0 (API loss 0).
  Passive measurements explicitly select `generic`; named sources never inherit
  a stored generic override. CAL launch loss does not imply known emission.
  Throughput latches effective losses at start; restart to pick up changes.
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
`pd/settings/<channel>` owns photodiode response settings and relay power intent;
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

Schema v14 replaces the obsolete `none` loss selector with `generic` and reduces
the override table to 32 slots. On first boot with an older or missing marker,
firmware clears **all app NVS records**, writes v14, and uses defaults. This
includes calibration, network settings, board/switch intent, counters, and other
app records, not just route losses. Reapply needed settings from reviewed lab
records; there is no migration. Maiman EEPROM is separate and is not cleared.
The earlier v13 reset invalidated calibrations from the mean-only clipping test
or incomplete six-term correction basis. Coefficient records still include the
physical-attenuator residual RMS and laser uncertainty introduced in v12/v11.

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
