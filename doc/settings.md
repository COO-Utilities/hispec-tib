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
Command dispatch keeps last-command diagnostics in RAM for the current boot;
command traffic does not write a history record to flash.

## Stored NVS Records

Current app NVS records include:

- Schema marker.
- Board type.
- Boot count.
- IP settings as one record.
- MQTT broker host/port as one record.
- One attenuator coefficient record per logical channel, including `rms_db`
  for each physical FVOA. Coefficients and RMS are restored as one record.
- One photodiode settings record per photodiode channel, including active dark
  window result, lowest stored dark window result, noise threshold,
  responsivity, transimpedance, relay power intent, and auto-off delay.
  The compiled auto-off default is 36000 seconds (ten hours); persisted policies
  keep their saved values across firmware updates.
- Last successfully applied PD relay state, one byte per channel at NVS IDs
  `0x0240`/`0x0241`, separate from calibration/settings. Only changes write flash.
  AUTO restores this state at boot (missing records mean off); persisted overrides
  take precedence. No dark or response calibration is saved by relay use.
- Laser-bank heater policy.
- One laser policy record per laser channel, including laser calibration/user
  intent and the operator-confirmed Maiman driver serial used as a physical
  association check, plus `fractional_noise`, `constant_noise_mw`, and
  `min_autolevel_current_ma`.
- One laser total-emitting counter record per laser channel.
- One laser thermistor-coefficient record per channel (`ntc_coeff_pct_per_c`,
  double in %/°C at 25°C, NVS IDs 0x0380–0x0385). Missing or invalid values
  retain the compiled coefficient. Reset deletes this record with channel policy.
- One independent laser reference triplet per channel (`ref_mw`, `ref_ma`,
  `ref_temp_c`, three doubles at NVS IDs 0x03c0–0x03c5). Missing/invalid values
  retain the compiled vendor anchor; reset deletes the record.
- One autolevel upper-current bound per laser (`max_autolevel_current_ma`, a
  double at NVS IDs 0x03e0–0x03e5). A missing bound defaults to the restored
  policy's nominal current. Keeping this separate preserves the existing policy
  records and calibration; reset deletes it with the other channel records.
- One route-loss table-entry record per configured route/source or split output,
  up to 32 records per PCB (16 per TIB channel). AS split records live on their
  own PCB. Broadband TIB CAL/return records use source `generic`.
- One MEMS intent record containing per-switch static state intent, per-switch
  direct-toggle restart metadata, and per-channel split restart metadata.

The PCB owns the thermistor coefficient; Maiman EEPROM retains driver-side values.
Profile preparation synchronizes the PCB value at the first explicit start after
bank power-up, and driver-backed settings updates program it immediately through
the existing stop/power flow. `persist:true` saves app NVS intent; it adds no
controller EEPROM SAVE operation. START/STOP retain their existing controller
persistence behavior. Settings queries always report PCB configuration;
engineering status reports controller readbacks while powered and null while off.
Before preparation, readbacks can differ from configuration. The coefficient
uses internal LD NTC 0x007F for programming and readback. External NTC 0x008A
is unused.

## Capacity and endurance

The [shared flash layout](ota.md#flash-layout) assigns NVS 192 KiB in bank 2:
24 erase sectors of 8192 bytes, with 16-byte programming alignment. Zephyr NVS
rotates garbage collection through these sectors and keeps one sector available
for copying live records. It does not require the STM32 high-cycle-data area.
Payload CRC and the optional lookup cache are disabled; NVS metadata retains its
CRC and the flash controller supplies hardware ECC.

The maximum live inventory for schema 14, with every record slot populated, is:

| Records | Count | Payload bytes | Aligned bytes including NVS metadata |
|---|---:|---:|---:|
| Schema, identity, boot count, IP, MQTT, heater policy | 6 | 246 | 400 |
| Attenuator coefficients, fit policies, operational opens | 18 | 1008 | 1344 |
| PD calibration/settings and last relay states | 4 | 274 | 384 |
| Laser policies, runtime totals, NTC, references, upper bounds | 30 | 1296 | 2016 |
| Route-loss overrides | 32 | 1792 | 2560 |
| MEMS intent | 1 | 272 | 288 |
| **Total** | **91** | **4888** | **6992** |

Live records therefore occupy about 3.6% of the partition, or 3.7% after the
spare sector and fixed sector metadata are excluded. Obsolete records and
garbage-collection markers occupy additional space until sectors are reclaimed;
the live-record total is not an instantaneous free-space measurement.

The seven-year design budget uses 2557 days at the following daily workload:

- TIB: 1000 changed eight-byte laser-runtime saves and 50 changed route intents.
  This charges every ordinary laser/attenuator command as a runtime save, even
  though ordinary attenuator changes and many laser operations do not save one.
- AS: 100 changed split intents. CAL: 100 changed route intents.
- Every profile: one boot-count increment, 200 actual relay transitions per PD
  channel, and a daily rewrite reserve for all remaining configuration records.
  Explicit calibration/settings saves must fit within that reserve.

A host simulation using the workspace's actual NVS implementation, the full
91-record inventory, and the target erase/program geometry gives:

| Profile | Maximum erases on any sector in seven years | Margin against 10000 cycles |
|---|---:|---:|
| TIB | 864 | 11.6× |
| AS / CAL | 636 | 15.7× |

The acceptance limit is fewer than 1000 erases on every sector under this
workload, giving at least tenfold margin against the normal-flash endurance
rating in the [STM32H563 datasheet, flash memory characteristics](https://www.st.com/resource/en/datasheet/stm32h563vi.pdf).
This is a workload projection, not a measurement of existing flash wear or a
substitute for the datasheet's temperature and retention conditions.

Reads do not consume program/erase cycles. Settings getters use RAM snapshots;
NVS reads occur during restoration, saved-value comparisons and garbage
collection. Repeated `pd` queries while a relay remains on do not write flash.
An AUTO query that turns it on saves one one-byte state record, and a later
auto-off saves another. Identical records are not programmed again. Command
history and runtime MEMS toggle progress never write flash.

## Attenuator default selection

TIB uses the twelve November 4 installed-device models and operational opens
listed in [calibration closeout](calibration_closeout.md#compiled-attenuator-defaults-adopted-november-4).
The stored board identity selects defaults before coefficient/open records load;
first detection and board-type resets select them through the existing hook.
Saved valid coefficient, open and fit-policy records independently override their
defaults. CAL keeps the nominal model and 1250 mV opens, including its shared
logical index with TIB 1510. The NVS schema and record formats are unchanged.

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
- UTC is restored from a valid RTC calendar on warm boot. There is no NVS
  last-time fallback; its former record ID is reserved. Manual/SNTP updates use
  the serialized RTC guard described in [architecture](architecture.md).
- FVOA outputs initialize closed at a 3300 mV request, quantized to the highest
  representable DAC code. Operational `open1_mv`/`open2_mv` default to 1250 mV;
  they constrain modeled full open, never engineering-voltage commands. Updates
  do not move outputs. Open settings use records 0x0180–0x0185; fit acceptance
  policies use 0x0140–0x0145. Both retain schema 14.
- Physical FVOA coefficients default to the nominal 0-3300 mV DAC span, gain
  1.533, and `FVOA_DEFAULT_MAX_ATTEN_DB` until calibrated/stored. The
  `max_atten_db` coefficient is a per-FVOA leakage floor, not a logical
  two-FVOA attenuator limit. Each physical model defaults to `rms_db=2.0`;
  accepted autocalibration replaces it with the final fit's existing residual
  RMS. Manual replacement without RMS returns to that default.
- Photodiode dark windows default to 0 mV with 0 mV RMS. YJ and HK have
  different default gain/noise warning values.
- Laser power models default to vendor-only reference-anchored quadratic fits.
  Optical-power uncertainty defaults to the per-laser fractional RMS adopted
  from the November 4 active calibration and `constant_noise_mw=0` for all six
  lasers. Startup and explicit calibration restore use the same defaults;
  valid saved overrides still replace them at boot. See the coefficients,
  noise values and provenance in [laser calibration](laser_calibration.md#compiled-defaults-adopted-november-4-2026).
  These app-only fields do not program Maiman.
- Laser `ld_serial` is the compiled manufacturer diode identity, distinct from
  the numeric expected Maiman driver serial. It is read-only and not persisted.
- Laser NTC defaults use vendor values: −4.4 %/°C for FLC and B=3900 K for Aero.
  Valid persisted property and NTC overrides continue to replace compiled defaults.
- Laser expected serials default to the initial known driver/diode association.
  Operators may update the value through `laser/settings` after confirming a
  replacement driver is physically associated with the intended diode.
- Laser `min_autolevel_current_ma` defaults to threshold plus 0.1 mA rounded up
  to the current-register grid. Validation normalizes it upward after threshold
  changes. `max_autolevel_current_ma` defaults to nominal current and rounds down
  to the register grid. Validation rejects an empty automatic range or a ceiling
  above the engineering maximum. These bounds are app policy and do not restrict
  manual current/level commands. `initial_level` is a per-measurement fraction,
  defaults to 0.5 in firmware, and is not persisted.
- Laser settings `autooff_s` supplies the positive manual-command default;
  command `off_in_s` overrides one operation. Zero laser default updates and
  zero command durations are rejected. Changing the default restarts a started
  laser's countdown from now without starting idle hardware. PD settings behave
  similarly for powered AUTO channels, but PD `autooff_s=0` cancels its countdown.
  Unchanged values and unrelated edits do not rearm. Returning a powered PD to
  AUTO arms a missing countdown and preserves a running one; an off PD stays off.
  Releasing a measurement/calibration PD hold starts a fresh configured interval.
  Calibration rejects timer writes but permits power-only overrides.
  Finite throughput owns its `off_in_s` deadline. Laser settings writes disable
  feedback while retaining collection and owned-source shutdown. PD read-only
  `off_in_s` is available through `pd`; `pd/settings` reports policy without
  that countdown or rearming. `pd` queries can enable AUTO power and rearm it.
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

Photodiode active dark windows, forced dark values and lowest-dark records are
changed through `pd/dark/<channel>`. Duration-based
dark captures are armed by the command and committed later by the photodiode
sampler thread.
`pd/settings/<channel>` owns photodiode response settings and relay power intent;
it does not replace or report dark summaries. Both `pd/dark` and `pd/settings`
with `persist:true` save the complete channel record, including its current dark,
lowest-dark, response and power policy. A save failure leaves applied RAM values
in place and is reported by the synchronous command or, for a completed measured
dark, the `photodiode_dark_persist_failed` warning.

MEMS static state requests and route applications update the persisted
per-switch intent once per command, including all accepted steps of a partially
failed route. A save error does not undo queued switch changes; it is returned
unless an earlier route-step error takes precedence. NVS logs the storage error
in either case. Identical records do not program flash. Toggle and split commands persist the
commanded request metadata once per command. Runtime toggle/split progress is
not written back to NVS, so reboot resume, when enabled by
`CONFIG_RESUME_TOGGLE_STATE_AT_BOOT`, restarts the original commanded duration
rather than preserving elapsed or remaining time.

If NVS loading fails after a board has already been initialized in the field,
treat it as a human-intervention fault. At minimum, inspect logs and
reinitialize storage before trusting persisted calibration or network intent. A
first boot with no app schema marker clears the old storage layout, writes the
current schema marker, and uses defaults.

### Schema reset

The current schema is v14. On boot with a different or missing schema marker,
firmware clears **all app NVS records**, writes v14, and uses defaults. This
includes calibration, network settings, board/switch intent, counters, and other
app records, not just route losses. Reapply needed settings from reviewed lab
records; there is no migration. Maiman EEPROM is separate and is not cleared.

## Intentionally Not Persisted

- Last-command diagnostics are boot-local; retired NVS ID `0x0009` is reserved.
  External command logs and watchdog boot telemetry provide cross-boot history.
- Active routes are derived from current MEMS switch state and route tables.
  Applying a route persists the affected switch states as static switch intent,
  not as a named active-route object.
- Laser output state is not restored after reboot. Laser-bank heater mode is a
  persisted policy and defaults to `auto`; in auto mode it may power the bank
  after boot for temperature monitoring. The heater starts off unless auto mode
  or an override turns it on.
- PD relay state is restored after device initialization: persisted override
  intent wins, otherwise AUTO restores the last confirmed on/off state. AUTO
  receives a fresh full countdown; on-time starts again at this boot. The DS2408
  driver may turn outputs off during early initialization, so restoration does
  not guarantee uninterrupted PD power. Ordinary reboot preparation preserves PD
  intent; a settings-erasing reboot explicitly turns them off and clears it.
  Heater policy is separate.
- Serial guard active state and runtime holdoff changes are not restored after
  reboot.

- Active output/current and live TEC/tuner results. The stored `tune_delta_nm`
  request is persisted configuration and is applied by a later positive level
  command; restoring it does not resume emission.

## Scoped laser calibration restore

`laser/calibrate` with `restore_defaults:true`, `laser` and `output` restores
compiled threshold/efficiency/curve reference/quadratic correction/maximum/nominal/autolevel bounds/datasheet reference/noise
settings and removes only that exact named outbound route override. Independent wavelength references,
driver identity and current calibration, other TEC policy, return loss and other-output overrides
remain. Optional persistence writes the laser records and deletes the route's NVS
record; a volatile restore does not erase saved values. Laser/route application
and persistence errors are reported separately. Raw calibration data occupies one
shared RAM dataset for attenuator/laser runs, never NVS. See
[laser calibration](laser_calibration.md) for mathematical assumptions and stages.

The self-contained laser policy record is 168 bytes: the quadratic coefficient
is a float at offset 140, and `efficiency_ref_ma` is a double at offset 144.
`wave_ref_nm` replaces the previous wavelength field at offset 40; the independent
`wave_ref_ma` and `wave_ref_temp_c` doubles are appended at offsets 152 and 160.
Size and offsets are build-asserted. Datasheet references load independently and
do not redefine the installed curve. The closeout keeps that policy layout and
schema unchanged; the separate autolevel-ceiling record does not require calibration
reinstallation. Older records of a different length still fail the existing validation.
Compiled wavelength defaults use the coordinates printed in each diode's test report.
