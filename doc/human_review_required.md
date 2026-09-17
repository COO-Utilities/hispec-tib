# Human Review Required

This is the central owner-review list for current code-vs-doc mismatches,
source TODOs, and behavior decisions.

LLMs Agents: Do NOT change heading names in this file.

## Locked-down code

## PCB Validation
- [ ] After flashing the transport fixes, repeat cold DS18B20 initialization and
  concurrent laser-status/relay/temperature polling; check USART2 overruns and
  Modbus failures, including timeout followed by a new request. Native bus lock
  ordering, RTU handoff/cleanup and automatic patch application have host checks;
  physical timing still requires the PCB. No flash was part of this change.
- [ ] Bench-validate both `TP_AUTOLEVEL_DIM_PRIORITY` choices, initial-level
  startup, minimum-current fallback, and preference for lower attenuation.
  Verify actual Maiman TEC bound expand/target/narrow ordering and rejection
  recovery with the installed modules. Host tests cover register order and
  failures; firmware builds do not establish optical response or hardware timing.
- [ ] Bench-validate restricted 55 dB fitting and the continuous residual-to-floor
  continuation using the existing `atten_scan`. Confirm individual autolevel
  limits and collect the next calibration with the new `max_calibrated_db` field.
- [ ] Verify compact calibration status with both six-term corrections. Numeric
  formatting and omission of three aggregate span/transmission diagnostics keep
  the 1024-byte buffers; those diagnostics remain in per-device fit telemetry.
- [ ] Bench-validate zero-current versus `laser stop=true`, auto-off at zero, and
  retained configuration across STOP. Capture the application Maiman timing logs
  to verify 350 ms busy guards and assess the unchanged 75 ms ACK deadline; see
  `doc/api/maiman_laser.md`. Retain log-drop warnings; application timings cannot
  distinguish late device replies from delayed RX processing.
- [ ] Bench-validate owner communication health and calibration repair: one-second
  checks, five-second sustained-loss shutdown/recovery, relay auto-off during
  restart, and six-term fits with no clipped reference/bridge anchors. Confirm
  Modbus errors and ADC/MEMS timing after removing throughput-rate relay reads.
- [ ] Keep an eye out MEMS loop and ADC loop timing overruns
  - debug build see `timinglog`


## Command/API Mismatches

### LLM Resolved; Human Review Requested
- [ ] Verify Maiman driver serial mismatches report
  `blocked_reason:"driver_identity_mismatch"`, block driver-backed laser setting
  programming, and can be resolved only by operator-updating
  `laser/settings.expected_serial`.


## Decisions To Make
- Validate the implemented direct 20 Hz stream on Rev. 2: timing margin,
  actuator response, illuminated PD noise, and dark-based error estimates.
  No extra settling holdoff is used. 64 SPS builds but remains a hardware
  evaluation option; the default is 250 SPS. See `photodiode_notes.md` for
  the error budget and remaining physical calibration assumptions.

## TODOs
- Audit successful noise/default-autooff/TEC-autooff-policy settings, settings
  no-ops, and `laser/tune` that release a throughput stream while leaving its
  laser emitting. They can discard measurement shutdown responsibility. Model
  and envelope edits now stop emission; failed settings updates retain the owned
  shutdown for explicit retry. The other ownership cases are intentionally deferred.
- Retain raw attenuator calibration records and fit results through stop/error
  cleanup until the next acquisition. A status-payload failure followed by the
  notebook's stop cleanup currently clears the data needed to diagnose it.
- Add sampler-owned illumination-change notification with settling duration.
  Laser/attenuator changes report the event; readings and windows remain marked
  settling until their acquisitions are clear of it. Consumers wait or skip.
  Keep the current local 100 ms autocalibration wait until this shared change.

- algorithmic/proceedural status (e.g. atten aotocalibration or throughput monitor autoranging notices) should be going to console AND mqtt and that was the whole point of a combined dispatch helper. this needs a app-wide reevaluation.

- test `pd/dark/<channel>` measurement, forced dark with optional `rms_mv`, lowest-dark reset, and dark-window persistence over reboot.
- Status needs to gain things we actually want.


- Lab-investigate safe laser-bank and external-relay shutdown on fatal faults.
  The current fatal path halts and relies on watchdog reset; formal shutdown
  safety is deferred pending hardware exploration.


## Deferred Owner-Specified Capabilities
