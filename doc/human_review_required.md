# Human Review Required

This is the central owner-review list for current code-vs-doc mismatches,
source TODOs, and behavior decisions.

LLMs Agents: Do NOT change heading names in this file.

## Locked-down code

## PCB Validation
- [ ] Bench-validate high-signal fit support after removing the upper transmission
  exclusion. Valid readings at/above the measured reference now enter both fits
  and signed residual statistics. Verify the open-region plots and installed RMS
  in a fresh calibration; model, 55 dB boundary and reference selection are unchanged.
- [ ] Run the notebook's `dac1_reversal` program after detector preparation:
  alternate 2822.241211 / 2848.022461 mV with DAC2 at zero, retaining startup and
  repeated visits. Compare final-30-second means, noise and drift before choosing
  a device-failure threshold or attributing the asymmetry to the DAC1 capacitor.
- [ ] After flashing, replay saved calibration records with the laser disabled
  and confirm health polls and record/status downloads continue throughout both
  fits without CRC/transport errors or false health timeouts. Fitting now uses
  lowest application priority with no calibration mutex held over calculation.
  Host replay matches pre-change coefficients/metrics; concurrent host checks
  cover cancellation, replacement, shutdown failure and retained wire records.
  Measure target fitting/cancellation time and verify laser shutdown before fit.
  Confirm notebook stop/error cleanup retains records until the next start.
- [ ] Verify acknowledged STOP followed by status with lock bit `0x0002` no
  longer emits `laser_output_fault`; hard faults and active interlock still do.
  Host tests exercise the production status path; repeat with the controller.
- [ ] After flashing UART-backed 1-Wire and USART2 FIFO, verify cold DS2408
  discovery/startup outputs and the first DS18B20 acquisition. Repeat concurrent
  1028y status reads, relay commands and temperature polling; check presence
  failures, corrupted replies, USART2 overruns and faults. All Zephyr patches,
  build hooks and Maiman cross-bus locks are removed; stock-driver ownership and
  accepted 3.3 V reset timing are documented in zephyr/README.md and hardware.md.
  Host tests cover public timeout cancellation and initialization on the next
  requested transaction. Verify response loss followed by a later request with
  the controller; emission is unnecessary for these communication checks.
  Repeat the original dark/calibration sequence under the agreed laser limits.
- [ ] Bench-validate both `TP_AUTOLEVEL_DIM_PRIORITY` choices, initial-level
  startup, minimum-current fallback, and preference for lower attenuation.
  Verify actual Maiman TEC bound expand/target/narrow ordering and rejection
  recovery with the installed modules. Host tests cover register order and
  failures; firmware builds do not establish optical response or hardware timing.
- [ ] Bench-validate 55 dB fitting with the first above-limit support point and
  correction refits from six terms down to one. Check selected terms, calibrated
  residuals, individual autolevel limits and continuous residual-to-floor
  continuation using `atten_scan`. Offline replay checks numerical behavior;
  fresh acquisition and embedded fitting duration still need bench validation.
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
- Investigate detector startup and illuminated settling separately. The September 21
  19:05 UTC noise capture saved a clipped dark (mean 53.8 mV, RMS 319.5 mV), then
  a stable dark trace near -2 mV. Notebook preparation now waits five seconds and
  rejects unusable dark results before illumination; firmware dark acceptance and
  the actual hardware stabilization time still need investigation. The same run
  shows a long transition after 182 seconds of laser emission, so laser warmup alone
  does not explain all settling. Keep the sampler-owned notification TODO below.
- Investigate DAC1 reference/sweep disagreement before changing acquisition:
  capture `cal_1028y_20260917T213847_376797Z.npz`, reference record 7 and first
  sweep record 11 both command DUT 0 / companion 2810.156 mV, but raw peaks are
  1767.688 / 2047.938 mV. The repeated sweep clips through DUT 2050 mV despite
  warm PD context. Trace command/window timing; no reference-reselection loop.
- Audit successful noise/default-autooff/TEC-autooff-policy settings, settings
  no-ops, and `laser/tune` that release a throughput stream while leaving its
  laser emitting. They can discard measurement shutdown responsibility. Model
  and envelope edits now stop emission; failed settings updates retain the owned
  shutdown for explicit retry. The other ownership cases are intentionally deferred.
- Raw attenuator calibration records and completed fit results now survive
  stop/error cleanup until a new start or reboot. Host lifecycle and binary-read
  tests cover this; PCB/notebook validation remains in the item above.
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
