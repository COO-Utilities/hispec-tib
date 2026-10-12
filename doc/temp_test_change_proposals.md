# Freezer-test approved software follow-up

9 October 2026. The owner approved A–H as eight commits, one per item. Status below records implementation.
The owner requires explicit approval for execution-flow and API changes.
The separately approved 19–21°C heater band is implemented with its documentation;
the follow-up below is separately approved. Hardware execution and flashing are not authorized by it.

## Evidence and scope

The existing surface is largely sufficient. Engineering status already exposes
`op_started`, `tec_started`, `curr_meas_ma`, raw state/lock bits, and the configured
limits. Internally, `LASER_CONTROL_FAULT` already latches an output fault.
`HispecFibPCBError.response` already retains the firmware error response.
The problems are incomplete use of this information and weak presentation.

The Oct 9 2330k sequence establishes an autonomous stop after an acknowledged
start, with a warning already emitted:

| UTC | Archived observation |
| --- | --- |
| 00:26:23 | Default TEC target set to 18°C |
| 00:26:25.830 | Start command acknowledged |
| 00:26:29.520 | `laser_output_fault` warning, about 3.69 s later |
| 00:27:07.636 | Engineering status: LD and TEC stopped, current measured 0, temperature 28.49°C, no current blocking lock |
| 00:28:45.230 | Another start acknowledged |
| 00:28:47.655 | Another output-fault warning |
| 00:28:48.754 | TEC stopped, temperature 14.28°C |
| 00:29:55 | Allowed range set to 16–38°C |
| 00:30:16.407 | Start acknowledged |
| 00:30:19.611 | Another output-fault warning |
| 00:30:29.052 | Compact status retained 120 mA set current and 2.948 mW estimate, with zero measured TEC current/voltages and `tec_not_started` |
| 00:31:57 | Default target raised to 25°C |
| 00:33:24 | After a later start, both LD and TEC started at 24.97°C, measured diode current 73.1 mA |
| 00:33:58 onward | Later target reduction to 19°C operated successfully |

Startup undershoot and a controller thermal trip are well-supported hypotheses.
The exact transient trip temperature and lock bit were not captured. A late
clear lock read does not erase the observed autonomous stop. The notebook log
captured the warnings, but they were easy to miss in its display arrangement.

B adds one compact output-state field, and H adds one PCB-owned persisted thermistor
coefficient. No replacement safety state machine, automatic retry, automatic TEC
ramp, or optical-estimate zero/null semantics are included. `i_mA` remains set current; `power_mw` remains its model estimate.

## A. Improve existing diagnostics without changing sequencing

Status: implemented; offline verification recorded in commit.

Owners: `lasers.c`, `laser_command.c`, and `_decode_ok_or_raise()` in
`tools/hispec_fibpcb.py`. Update `doc/commands.md` and relevant docstrings together.

Use the existing `error` string, `rc`, warning message, and warning context.
Include laser identity and the operation that failed. Where state/lock/temperature
evidence was already obtained by the failing operation, include that evidence.
Do not issue new Modbus reads solely to enrich an error. Distinguish a transport
failure from a controller refusal when the available evidence permits it;
otherwise report the ambiguity. Thermal descriptions must cover too cold and
too hot. Preserve existing diagnostic codes and best-effort warning delivery.

Follow-up: bounded text replaces the public diagnostics record/formatter.
Controller observations remain private; existing MQTT errors and warnings carry
the evidence. Nested STOP/programming failures retain their detail through restore.

Format Python exception text with its topic, firmware error and available `rc`.
Retain the existing `.topic` and `.response` attributes and exception class.
Example shape: `laser: 1510h start: controller thermal block ... (rc=-5)`;
actual wording must describe observed evidence rather than guessing a cause.

Why merited: the previous `laser operation failed` string hid even the existing
return code from a normal notebook exception display. This is an incremental
diagnostic improvement included in the explicit A–H approval.

Verification: temporary synthetic error-response checks and representative
blocked/transport paths; confirm unchanged request count, waits, response fields,
and exception type. Firmware build. Hardware error examples only in a later
authorized session.

## B. Clarify public-start readiness and output state

Status: implemented; offline verification recorded in commit.

`ready` and `blocked_reason` describe known barriers to the public start operation,
not manual controller preparation or historical faults. Bank auto can power an idle
bank; explicit override-off blocks start. Applicable controller locks and identity
failures remain blockers. A recoverable stop can therefore be ready with no blocking
reason. No diagnostic read clears the existing safety latch.

Add one compact `output_state`: `stopped`, `running`, `stopped_fault`, or `unknown`.
Intentional STOP, acknowledged zero current and confirmed bank-off are stopped.
Running means established nonzero output operation, not measured optical emission.
Stopped-fault requires positive evidence of an unexpected stop after nonzero operation;
uncertain writes/observations do not establish operation from retained setpoints.
The member adds at most 31 bytes. Existing estimates and shutdown obligations remain.

Verification: zero/full stop, auto/override bank modes, current locks versus stop
history, autonomous stop, ambiguous writes, explicit recovery and payload size.

## C. Detect an LD-only stop in the existing background check

Status: implemented, including C2; offline verification recorded in commit.

Owner: the existing laser auto-off worker in `lasers.c`.

The worker previously checked only TEC-started state during established operation.
Extend that same opportunistic check to LD-started state. If either is confirmed
stopped after established operation, set the existing control-fault latch and
emit its existing one-shot warning with the observed reason. Preserve the current
nonblocking bus-lock attempt, worker cadence, foreground recheck and auto-off
obligation. Do not poll extra drivers when the existing active predicate is false.
Do not add a second scheduler or retry/restart a stopped diode.

Why merited: foreground status already detects both kinds of stop, whereas the
background check could miss LD-only shutdown until a status command. This change
adds a controller read on active checks and therefore changes execution flow and
bus load. It was explicitly approved; target contention/timing verification remains.

The approved C commit includes the counter refinement formerly called **C2**: when readback
positively confirms a stopped LD or TEC, stop that component's existing on-time
counter. Retain setpoints and shutdown deadlines. Failed reads leave operation
uncertain and must not stop accounting as though shutdown were confirmed.
Apply it at foreground status, temperature polls and background observations;
preserve each component independently and retain accumulated durations.

Verification: temporary active/TEC-off/LD-off/read-failure/foreground-stop-race
cases and counter cases. Build, followed by authorized timing/stop tests on
hardware. No new start wait or immediate post-START verification is included.

## D. Keep temperature observation during heater override

Status: implemented; offline verification recorded in commit.

Owner: `laserbank_tempcontrol.c` using existing polling and cache.

Keep the heater output forced as selected. When bank power is already on,
continue the existing opportunistic temperature poll and summary update during
`override_off` and `override_on`. Heater override alone must not power the bank.
Bank `override_off` still prevents reads from unpowered drivers and must invalidate
or age out values normally. Preserve the current cadence, warning policy and
all existing mode names. Do not add separate observation controls.

Why merited: the previous override branch returned before polling, making the
heater cache stale precisely when observing passive cooling. Existing bank and
heater modes already provide the requested independent control. This rearranges
the cycle and adds override-mode I/O, included in the explicit A–H approval.

Verification: both heater overrides with bank on/off, transition back to auto,
bus contention, stale readings, and override output enforcement. Update the
heater diagram, command docs, notebook guidance and Python docstring together.

## E. Use idle probes for `temps().laserbank_c`

Status: implemented; offline verification recorded in commit.

Owners: the existing temperature command and its direct temperature-read owner.

Keep the existing response shape. Compute `laserbank_c` from successfully read
channels whose TEC is stopped, excluding actively controlled TECs. Return the
existing unavailable representation (`null` on the wire, `None` in Python)
when all TECs are active or the bank is off/no valid idle probe is available.
Keep individual probe readings and foreground read behavior. Do not silently
switch this command to a differently aged cache.

Why merited: an actively regulated laser temperature is not an enclosure
temperature. This is a field-meaning change requiring approval. The heater's
existing idle aggregate already demonstrates the intended interpretation.

Verification: mixed idle/active channels, all active, bank off, partial failures,
and ordinary idle behavior. Update temperature command documentation, examples,
and the Python `temps()` docstring.

## F. Remove notebook bank cycling at low power

Status: implemented; fake-client notebook verification recorded in commit.

Keep existing firmware bank `auto` policy and ambient behavior. Replace the notebook's
bank-off/auto pair with `laser(name, stop=True)` for all six channels. Respect
`disable_tec_at_autooff`; no new boot-time bank power-on or settings rewrite.
Keep other low-power actions and reset heater mode to auto on every transition,
as selected by the owner. Report partial failures and continue best effort without
using a bank power cycle as fallback. Preserve capture/save behavior and quick checks.
Update notebook guidance and the archived-replay/commissioning prompt together.

Verification: recording fake-client helper execution; six stop requests, heater
auto, no bank-off request, and visible partial failures. No live notebook execution.

## G. A small PCB power convenience method

Status: implemented; fake-client helper verification recorded in commit.

Approved new host API:

```python
pcb.laser_bank_power_mw(names: Sequence[str] | None = None) -> float
```

Default to the six configured lasers. Validate names and reject duplicates before
any request. Read each selected laser through the existing engineering-status
method. Sum `diode_v * curr_meas_ma + abs(tec_v * tec_ma)` in mW. Use measured
diode current, not `i_mA`. Return zero for a selected driver confirmed unpowered
and for an explicitly empty selection. If a powered driver's required reading is
missing/nonfinite or `read_rc` is nonzero, return NaN for the total rather than
silently returning a partial sum. Existing request/transport exceptions propagate.
Do not write settings, start TECs, or change bank power.

Why merited: this turns the owner's useful notebook quick check into one small,
documented convenience method without a new firmware endpoint or return type.
Engineering reads are sequential and are not a simultaneous power snapshot.
The result estimates electrical power delivered to the diode and TEC outputs;
it excludes driver losses, heater power and other bank input consumption.

Verification: temporary recorded-status examples for units, signed TEC power,
zero/off, invalid readings, empty selection, duplicate/unknown names and failures.
Document these semantics alongside existing host methods. No permanent test
framework is proposed.

## H. Synchronize thermistor coefficients in datasheet units

Status: implemented, including the owner-authorized correction from unused
external NTC 0x008A to internal LD NTC 0x007F for programming and readback.
No external NTC is installed.

`ntc_coeff_pct_per_c` uses resistance %/°C at 25°C for configured settings and
engineering readback. Internally `alpha_pct = -100 * B / 298.15²`; writes round B
to integer kelvin and normalize the accepted coefficient. Invalid or
nonrepresentable values are rejected. One representation is exposed.

Settings always report PCB configuration. Engineering configuration readbacks
are actual while powered, including before first start, and null while off.
Shared names remain `max_current_ma`, `current_cal_pct` and `tec_curr_lim_a`.
Serial mismatches return readable status with a start/program blocker;
device-ID/read failures remain errors. The correction changes no schema or NVS
layout, readiness meaning, sequencing or calibration values.

PCB settings own coefficient intent and persistence in the existing separate
eight-byte per-channel NVS record. Existing profile preparation synchronizes it
after the TEC current limit, before target/bounds/PID, at the first explicit
start after bank power-up and on explicit driver-backed settings updates.
MCU startup adds no programming or controller EEPROM SAVE. Compiled defaults
use vendor values: −4.4 %/°C for FLC and B=3900 K for Aero; they are not
unit-specific thermistor measurements.

The report and uncommitted review deck quantify relative R25-only, coefficient-only
and joint fits, including heating/cooling differences, under the owner's common
idle-temperature assumption. Nominal R25 variation can explain the mean offsets
within supplied ±5% limits. The cold span cannot separate absolute R25/B errors.
The report compares diode sheets, two unit reports, controller limits and PID
profiles and provides relative/independently referenced replacement procedures.
It changes no PID, temperature/current limit, QE or optical-power coefficient.

### Completed bounded debugger investigation

The owner authorized TEC-only 1270j trials and then the internal-register
correction. Internal B was read on all six; LD 0x0088 and TEC 0x007E calibration
both read 100%. At digital targets 18/25/32°C, factory 100% matched reported
temperature within about 0.02°C. A reversible 99% trial lowered reported
temperature by approximately 0.17/0.26/0.31°C, including during cooling.
This approximately scales the Celsius regulation target; controller circuitry
remains unconfirmed. Near-equilibrium zero TEC telemetry is compatible with the
coarse 100 mA / 0.1 V readback resolution.

Retain factory 100%. There is no observed regulation error to correct. Keep
`current_cal_pct` for LD and add no TEC setting or calibration-field rename.
0x007E remains an engineering adjustment to investigate if future settled
readback misses its digital target. Its demonstrated target shift does not
calibrate physical R25 or establish electrical current accuracy.

Emission stayed off. The trial restored and verified TEC calibration, original
target/interlock permission, all LD/TEC states, PCB settings and bank/heater
policy. No NTC coefficient, external sensor, LD current, explicit EEPROM SAVE
or firmware flash was written by the agent. User breakpoints/session remain.
Raw evidence and limitations are in [the report](temp_test_result.md#two-calibration-addresses-and-the-live-test).

Verification: register-address/source checks, conversion/model sanity checks,
restoration assertions, unchanged archive hashes, Nucleo build and visual review
of the presentation. Post-flash profile synchronization/failure paths and
independent physical temperature/resistance calibration remain hardware checks.

## Approval and validation boundary

A–H are approved for implementation, one coherent commit each; C includes C2.
Each commit updates its documentation and uses temporary task-specific verification.
Live commissioning, flashing, controller writes during this development session,
and automated ramps remain separate actions. The presentation remains uncommitted.
