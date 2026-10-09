# Freezer-test software follow-up for explicit approval

9 October 2026. These are reviewable proposals, not implemented changes.
The owner requires explicit approval for execution-flow and API changes.
The separately approved 19–21°C heater band is implemented with its documentation;
the following items remain outside that approval.

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

No additional compact status fields, replacement state machine, persistence,
automatic retry, automatic TEC ramp, or optical-estimate zero/null semantics are
proposed. `i_mA` remains set current; `power_mw` remains its model estimate.

## A. Improve existing diagnostics without changing sequencing

Owners: `lasers.c`, `laser_command.c`, and `_decode_ok_or_raise()` in
`tools/hispec_fibpcb.py`. Update `doc/commands.md` and relevant docstrings together.

Use the existing `error` string, `rc`, warning message, and warning context.
Include laser identity and the operation that failed. Where state/lock/temperature
evidence was already obtained by the failing operation, include that evidence.
Do not issue new Modbus reads solely to enrich an error. Distinguish a transport
failure from a controller refusal when the available evidence permits it;
otherwise report the ambiguity. Thermal descriptions must cover too cold and
too hot. Preserve existing diagnostic codes and best-effort warning delivery.

Format Python exception text with its topic, firmware error and available `rc`.
Retain the existing `.topic` and `.response` attributes and exception class.
Example shape: `laser: 1510h start: controller thermal block ... (rc=-5)`;
actual wording must describe observed evidence rather than guessing a cause.

Why merited: the current `laser operation failed` string hides even the existing
return code from a normal notebook exception display. This is an incremental
diagnostic improvement, but text consumed by users changes and needs approval.

Verification: temporary synthetic error-response checks and representative
blocked/transport paths; confirm unchanged request count, waits, response fields,
and exception type. Firmware build. Hardware error examples only in a later
authorized session.

## B. Reflect the existing latched fault in readiness

Owner: `lasers.c` status construction. No new public fields.

Preserve `ready` and `blocked_reason`, but make `ready` false while the existing
control-fault latch is set. Preserve a currently observed specific blocking
reason when present. When current controller conditions look usable but the
latch remains, report `blocked_reason="control_fault"`. Prioritize an observed
thermal/electrical lock over the generic `tec_not_started` explanation, while
retaining bank-off and identity checks first. Keep the existing latch-clear
conditions; a passive status read must not clear a fault.

Why merited: compact readiness currently depends on current controller bits,
which can recover after an autonomous stop. The application already knows that
control faulted. Returning a ready state that ignores that latch is misleading.
This is a semantic API change and a new reason value, requiring explicit approval.
It does not change the meaning of set current or estimated optical power.

Verification: temporary cases for fault present/current locks clear, thermal
block with TEC stopped, bank off, serial mismatch, normal readiness, and existing
successful recovery. Check both compact and engineering output and document the
reason priority.

## C. Detect an LD-only stop in the existing background check

Owner: the existing laser auto-off worker in `lasers.c`.

The worker currently checks TEC-started state during established operation.
Extend that same opportunistic check to LD-started state. If either is confirmed
stopped after established operation, set the existing control-fault latch and
emit its existing one-shot warning with the observed reason. Preserve the current
nonblocking bus-lock attempt, worker cadence, foreground recheck and auto-off
obligation. Do not poll extra drivers when the existing active predicate is false.
Do not add a second scheduler or retry/restart a stopped diode.

Why merited: foreground status already detects both kinds of stop, whereas the
background check can miss LD-only shutdown until a status command. This proposal
adds a controller read on active checks and therefore changes execution flow and
bus load. It needs separate approval and contention/timing verification.

Counter refinement is a separately selectable part, **C2**: when readback
positively confirms a stopped LD or TEC, stop that component's existing on-time
counter. Retain setpoints and shutdown deadlines. Failed reads leave operation
uncertain and must not stop accounting as though shutdown were confirmed.
Inspect the counter owners before patching to preserve these distinctions.

Verification: temporary active/TEC-off/LD-off/read-failure/foreground-stop-race
cases and counter cases. Build, followed by authorized timing/stop tests on
hardware. No new start wait or immediate post-START verification is included.

## D. Keep temperature observation during heater override

Owner: `laserbank_tempcontrol.c` using existing polling and cache.

Keep the heater output forced as selected. When bank power is already on,
continue the existing opportunistic temperature poll and summary update during
`override_off` and `override_on`. Heater override alone must not power the bank.
Bank `override_off` still prevents reads from unpowered drivers and must invalidate
or age out values normally. Preserve the current cadence, warning policy and
all existing mode names. Do not add separate observation controls.

Why merited: the current override branch returns before polling, making the
heater cache stale precisely when observing passive cooling. Existing bank and
heater modes already provide the requested independent control. This rearranges
the cycle and adds override-mode I/O, so it needs explicit approval.

Verification: both heater overrides with bank on/off, transition back to auto,
bus contention, stale readings, and override output enforcement. Update the
heater diagram, command docs, notebook guidance and Python docstring together.

## E. Use idle probes for `temps().laserbank_c`

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

## F. Power the laser bank by default at startup

Owners: current boot path and laser bank power owner, after confirming actual
startup ordering and stored heater-mode handling.

Proposed behavior: after board-profile selection and normal laser initialization,
request bank power on in the existing `auto` bank mode, independently of heater
mode. This powers monitoring electronics only; it does not request emission or
TEC regulation. Preserve the explicit bank override-off command. Do not introduce
a new setting or change boot behavior for board profiles without the laser bank.

Why merited: the present heater auto-entry can power the bank, but heater override
can bypass that path. Keeping the idle bank powered improves thermal continuity
and monitoring, as the owner requested. Boot sequencing is a flow change and
requires separate approval. Idle input-power cost was not measured in these
archives and should be checked.

Verification: inspect the complete boot path before the bounded patch; test the
applicable board profile with each persisted heater mode, initialization failures,
and bank override-off. Document startup behavior and retain owner boot TODOs.

## G. A small PCB power convenience method

Proposed new host API, subject to approval:

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

## H. Correct the engineering thermistor unit label

The existing engineering `ntc_t_coeff` is populated from the controller's B25/100
readback, while the similarly named diode property uses a coefficient per °C.
Propose replacing only the engineering field with `ntc_b25_100_k`, updating its
Python dataclass, serializer, docs and consumers together. Leave the diode
property's per-°C coefficient as a separate quantity. No alias or register write.
This is a naming/schema change requiring approval; it can be deferred without
blocking the heater patch or the report.

Verification: search all consumers, confirm archived engineering readback units
against the driver, and use a temporary parsing check. Do not rewrite archives.

## Approval and validation boundary

Approve items individually (including C2 separately), or select a subset.
All remain unimplemented. Each approved item should be a bounded patch with
updated documentation and relevant temporary checks. Live commissioning,
flashing, diode settings writes, and automated target ramps are separate actions
and are not implied by accepting this document.
