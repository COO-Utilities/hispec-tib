# Human Review Required

This is the central owner-review list for current code-vs-doc mismatches,
source TODOs, and behavior decisions.

LLMs Agents: Do NOT change heading names in this file.


## Locked-down code

## PCB Validation
- [ ] Validate the integrated OTA flow before making MCUboot the only build:
  provision a confirmed baseline from compiled defaults; exercise USB
  confirmed-image reflash, matching-ELF debug/attach and cold reset; then normal
  and maximum-size network updates, exact-hash confirmation and rollback.
  Exercise all board profiles, command admission, pending-slot protection,
  wrong/late confirmation, network loss, running hangs and power cuts during
  swap/revert. Observe bank power and measure the 30-second MCUboot to 15-second
  application IWDG handoff/feed gaps. USB updates may install a confirmed image
  directly; they do not need to test rollback. Once these results are recorded,
  make MCUboot/sysbuild the default and remove the temporary standalone build
  and its instructions. Software builds do not validate hardware behavior.
  See [firmware updates and OTA](ota.md).
- [ ] Resolve the drawing/as-built differences exposed by the documentation
  audit against the PCB evidence committed in `2fd8e87`. Record the assembly
  revision and measured result before changing firmware assumptions:
  - Schematic page 39 has 2.43 kΩ / 4.7 kΩ feedback, implying gain 1.5170;
    `ATTENUATOR_DEFAULT_GAIN` is 1.533. Confirm installed resistor values and
    measured gain, then decide whether the drawing, default or calibration
    interpretation needs correction.
    - PCB has schematic values with 1-2% tolerance parts
  - NoTIB placement connects ASR1 optical C to ASR2 and B to ASR3; blue uses
    B to ASB2 and C to ASB3. Firmware's two channels use the same first-switch
    state interpretation. Confirm electrical-state-to-optical-port mapping and
    actual red patch before deciding whether routing or drawing labels are wrong.
    - The intent was ASx1B>ASx2A>ASx2B/C=xCAL/xSPLIT1 & ASx1C>ASx3A>ASx3B/C=xSPLIT2/xSPLIT3, 
      The labels FC17 and FC20 are off
      like both the labels and code are off. People will assemble per labels:
      - ASR1C>ASR2A>ASR2BC=RCAL/RSPLIT1
      - ASR1B>ASR3A>ASR3BC=RSPLIT2/3
      - ASB1B>ASB2A>ASB2BC=BCAL/BSPLIT1 
      - ASB1C>ASB3A>ASB3BC=BSPLIT2/3
  - Confirm the CAL `nm` route's physical source/termination: the drawing calls
    the branch Dark. Confirm physical AO/FEI and splitter-output port assignments
    before treating the API-route figures as an as-built patch certificate.
    - nm=narcissus mirror, another (the formal) name for a "dark" source
  See [electrical evidence](hardware.md#source-drawings-and-revision-boundaries)
  and [fiber paths](hardware_profiles.md).

- [ ] Bench-validate revised laser calibration after FVOA calibration: thermal
  qualification/settling, calibrated FVOA weighting, corrected curve and automatic
  floor and full configured range, AO/FEI loss, effective noise, recabling/STOP, default restoration
  and NVS reboot behavior. Multi-level captures below show that the upper-current
  compression persists at low PD signal. Its physical origin remains unresolved;
  reference anchoring does not establish power independently of the vendor report.
  - 2026-10-23 full-range acceptance revision: calibration now preserves maximum
    and nominal current, fits through the configured maximum with its reference
    inside support, and searches only the floor. Residuals allow 5% plus three
    measured SEs; immutable unit-specific vendor traces allow 5% of vendor
    reference power plus three fitted-power SEs. Cross-attenuation/gain-shift
    vetoes are removed; GLS uncertainty and raw diagnostics remain. Repeat/dark
    controls run once after selection; failure rejects the whole calibration.
    The fresh 18:19/18:24/18:28 UTC captures completed with the ranging fallback.
    Offline real-C replay of those records rejects 1028y through 250 mA (best
    full-range candidate has 10.04% raw maximum residual); 1430yj accepts 6–60 mA
    with threshold 4.266 +/- 0.006 mA conditional SE. The lowest 1270j candidate,
    9.8–65 mA, passes curve/vendor tests but fails the final dark check: 0.594 mV
    residual versus 0.484 mV allowance. The earlier 11.7–65 estimate checked darks
    during interval selection; the approved once-after-selection rule does not
    retry that floor. This is a control/model inconsistency, not proof of diode
    nonlinearity or a component-level physical diagnosis.
    Temporary real-C saved/synthetic replays, acquisition/failure/application
    simulations, independent dense GLS/covariance checks, all six vendor-table
    compression bounds, offline plot/manual-install checks, normal/OTA builds
    and strict Sphinx pass. Flash grows 2040 bytes; static RAM, shared buffers,
    settings and retained-data layouts are unchanged. No permanent test tooling
    was added. The notebook marks threshold/SE, floor and configured maximum,
    separates supported curve from extrapolation, preserves all diagnostics and
    refuses partial-range manual installs. Owner options, outputs and captures
    are preserved. Evidence is in `/private/tmp/laser-fullrange-20261023/`.
    No hardware action or OTA was performed for this revision; on-target behavior,
    control interpretation and the 1028y measurement-chain cause remain bench
    validation work.
  - 2026-10-23 morning follow-up: the 179-window 1028y run accepted only
    43.9–191.1 mA; the 168-window 1270j run stopped before fitting with
    `usable pd range not found`. Its 19/219/1840 mV probes were valid, including
    the final 1931 mV peak. Preferred 1850 mV ranging headroom was incorrectly
    treated as a validity requirement. Fix `0a810f3` falls back to the closest
    valid retained probe, restores its exact DAC pair and takes fresh windows;
    fitting and classification thresholds are unchanged. Targeted real-C probe
    checks, full acquisition simulations, normal/OTA builds and strict Sphinx
    pass. Shared buffers and static RAM are unchanged; OTA flash grows 112 bytes.
    OTA succeeded: firmware `0a810f371ffa-dirty`, boot 8, image 482748 bytes,
    confirmed hash `6a5eaa451c959c5783d5277dd5c8b1bfca28af0fca93163591fc85be6ce41017`.
    All six laser policies and both PD settings match pre-update readback; bank
    policy restored to `auto`, all laser outputs stopped. No curve/noise/loss
    applied. A fresh physical acquisition with the fallback remains to be run.
    The notebook now has three explicit source subsections per channel, clear
    acquisition/fit outcomes and explanations in absent-residual panels. Offline
    morning and earlier accepted/rejected replays pass; original captures and
    all diagnostic panels are retained. Local OTA snapshots and checks are in
    `/private/tmp/laser-notebook-morning/`.
  - 2026-10-22/23 multi-level revision (`51891ed`, settling recovery fix `fbe64e7`):
    every emitting-current portion is sampled at high/low PD signal, with a third
    upper-band level, early/two-late windows, same-DAC/current returns and three
    dark groups. The fit uses calibrated transmission and shared per-DAC-pair
    FVOA RMS uncertainty, not overlap-derived gains. Wavelength coordinates are
    the independent `wave_ref_nm/ma/temp_c` triplet. Normal/OTA builds, strict
    Sphinx, temporary real-C acquisition/failure/fit/runtime/dispatcher checks,
    dense-covariance crosscheck and offline notebook replay pass. Shared data and
    scratch remain 7680/3584 bytes; static RAM grows 232 bytes and flash about
    4.7 kB versus the preceding image. On-target stack margins remain unmeasured.
  - Both new OTAs **succeeded**. Final firmware `fbe64e711b90-dirty`, signed image
    482636 bytes, confirmed MCUboot hash
    `7d7524f9c7e8411e8fecc160d6e0d41f273da54fe7561eca489cca6751eea599`.
    The six policies were explicitly reinstalled for the 168-byte record after
    the first update and verified across a further reboot. Existing physical
    wavelength predictions were preserved. Final readback matches all six
    policies except elapsed-emission counters; source stopped at 0 mA, bank and
    PD policies `auto`. No fitted curve, noise or route loss was applied.
  - New 1028y captures: `laser_cal_1028y_20261023T052755_242609Z.pkl` has 192
    windows at 2 s settling, accepted only 48.3–220.6 mA; the 191-window
    `laser_cal_1028y_20261023T053942_629881Z.pkl` at 5 s settling is rejected.
    Both acquisitions completed without overflow. The latter's identical
    27.3 mA / DAC 3129/3000 return changes 176.48 to 195.91 mV (+11.0%), while
    the measured dark varies only about 0.25 mV. This is time/history dependence
    in the measurement chain, not proof of which component changed.
  - Within fixed DAC pairs, the new 133/250-mA ratios are 0.797–0.853 across
    305–1423 mV reference signals, versus 0.742–0.756 in the two earlier captures
    and about 0.52 in the vendor trace. Constant gain normalization cancels in
    this check. Driver readback follows commanded upper-band current. Neither
    the fitter revision nor simple high-PD saturation alone explains the curves.
    The commissioning notebook retains all diagnostic panels, compares these
    captures and separates observed darks from the rejected candidate baseline.
    Evidence is under `tools/commissioning_data/laser_multilevel_20261023/`.
    Independent optical/current metrology and isolating attenuator/path history
    remain bench questions; do not treat the rejected fit's gain shifts or
    conditional threshold SE as physical estimates.
  - 2026-10-22 API/curve follow-up (`b3a0056`): manual calibration now uses the
    complete four-coefficient `laser/settings` update, independent of the retained
    dataset and vendor triplet. Removed the retained-run apply API and lab aliases.
    Levels, initial autolevel and tuning use emission fractions. Efficiency is the
    uncorrected anchored line's slope on the selected support; the independent
    `efficiency_ref_ma` preserves the corrected curve. Fixed both the missing
    command-registry options and the required numeric-parser presence flag.
    Normal/OTA builds, strict Sphinx, real dispatcher/JSON-handler checks, saved
    and synthetic fits, acquisition/failure simulation, runtime level/inverse/
    validation checks, NVS layout/loading, and offline notebook replay/application
    checks pass. Shared buffers and the 1496-byte laser scratch are unchanged;
    static RAM grows 100 bytes versus the preceding ELF.
  - Follow-up OTA **succeeded**: `b3a0056b3196-dirty`, 477928-byte signed image,
    confirmed MCUboot hash
    `c9ac7ef7535cd13e4bd7efc7fdc7ddf9e5b3b7f504179cc45fa4f7488635c755`.
    The real command accepted the new options and returned `pd off`; removed
    arguments and incomplete curves returned explanatory strings. All six saved
    policies were explicitly reinstalled into the 152-byte record and matched
    readback after a further reboot. Existing NTC register quantization changed
    the displayed -4.4 coefficients to -4.399651 on 1028y/2330k; no new NTC policy
    was introduced. PD settings are unchanged. Bank remains `override_off`, both
    PDs off; no optical sweep or new fitted-curve adoption was performed. The
    parameter probe reported `ENOTSUP` and one transport retry recovered; upload,
    boot and confirmation completed. Local before/after/reboot snapshots are in
    `tools/commissioning_data/laser_policy_ota_20261023T011000Z`.
  - 2026-10-22 iteration: normal and OTA Nucleo builds and strict Sphinx build pass.
    Temporary real-C saved-data/synthetic fits, acquisition/failure/application
    simulations, 5000 forward/inverse checks, packet/host decoding and offline
    notebook replay/plots pass. The 87-window 1028y archive yields 46 selected
    windows, 17.1–250 mA, threshold 13.436 mA and maximum residual 4.12% at the
    new 5% tolerance. This replay is not another physical calibration.
  - Static RAM increases 16 bytes versus the pre-iteration ELF; shared data/scratch
    remain 7680/3584 bytes. On-target stack/timing margins remain unmeasured.
  - OTA **succeeded** on `hsfib-tib` at `hispec.caltech.edu`, firmware
    `03d32f8171fe-dirty`, 477880-byte image, confirmed MCUboot hash
    `5dfe910057a649fdea47ca6a395f5f52268e3b2cd624f420eec08e7d54f9ecac`.
    Acquisition was stopped and bank-off acknowledged before upload. Exact hash,
    confirmed/inactive/nonpending OTA state, board readiness, all six correction
    settings, per-channel PD power and new calibration/error replies were queried
    after boot. Bank remains `override_off`; both PDs are off. No optical sweep or
    calibration-setting adoption was performed. The MCUMgr parameter probe logged
    `ENOTSUP`, but upload, trial boot and confirmation completed successfully.
    Build label is dirty because existing notebook/ledger edits remain local.
    See [laser calibration](laser_calibration.md) for the next bench workflow.
- [ ] After flashing the generic TIB loss correction, verify schema 14's full
  app NVS reset, reapply needed settings, and bench-check generic CAL/return
  overrides and persistence. YJ 34.5/41/62/1 dB whole-route defaults remain
  provisional; equal AO/FEI loss needs optical validation. The Nucleo build,
  temporary C/host checks, and saved noise/failed-hold replay passed offline;
  no flashing, hardware commands, or physical persistence checks were performed.

- [ ] After flashing the FPU/runtime-float and priority changes, measure DWT
  call durations and stack margins; stacks have not been reduced. Verify MEMS 0
  splitter toggle/cleanup timing, PD 1 cadence and Modbus/system 2 completion
  under both-channel streaming and MQTT/command load. Repeat the failing notebook
  operation and at least 100 transactions. Exercise missing/corrupt replies,
  disconnection and later recovery without emission where possible; record PD
  cadence and shared-throughput stream gaps during synchronous laser waits.
  Builds and numerical replay do not establish target timing or fault recovery.
  Offline verification: normal and isolated OTA builds passed; all six installed
  FVOAs passed every DAC code, paired transmission error stayed below 14.17 ppm,
  and three of 33,006 inverse requests moved by one code (repeatably). All 88
  saved/partial/noisy fit replays retained exact double coefficients/metrics.
  PD replay covered 127,100 window phases, invalid samples and ADC extremes;
  maximum saved-capture net-mean change was 0.00055 mV.
- [ ] Commissioning changes: bench-check continuous 10 s dark settling after PD
  power/laser transitions, forced-dark lowest gating, and measured reset_lowest
  persistence. Existing historical minima are unchanged. Verify no-light startup
  and reboot laser STOP with Modbus RX free to run on the system queue.

- Bench verification (2026-10-03, firmware 5489c84): periodic LPTIM gained
  4.40 ms/min idle and 4.37 ms/min with both PD streams and command load.
  RTC remained continuous across three normal SNTP updates (UTC corrections
  3.84–4.14 ms). 200 zero-current commands interspersed with 733 cross-node
  status queries produced no communication errors or board warnings; both
  20 Hz streams had 46–52 ms UTC intervals and no overrange samples. Laser
  settings were unchanged and PD settings restored. The remaining ~80 ms
  host-time warning matched the Mac's offset against both NIST and Apple NTP.
  Normal Nucleo build and 23 temporary RTC-guard boundary/error checks passed.
  Full autocal/notebook and long-run queue/stack validation remain; the original
  Modbus failure was not reproduced or proven fixed by these changes.

- [ ] Execute the scope investigation only with fresh bench authorization and
  verified probe floors/patch/actuator identities. Temporal-noise and excess-
  variance results remain conditional on explicit bandwidth and independence.


## Command/API Mismatches
- [x] Python CLI `atten-db` calls `atten()` and prints the full `AttenuatorState`,
  including dB loss. Offline CLI dispatch and request/response checks pass.

- [ ] Decide throughput shutdown ownership across tuning and policy-only
  settings changes. `laser_tune_set()` releases a matching monitor before
  applying the tune, including a failed tune. Successful
  `throughput_monitor_update_laser_settings()` also releases it; some settings
  (for example fractional noise or auto-off policy) do not stop emission.
  `release_locked()` clears the monitor deadline and owned-laser shutdown
  obligation. An autolevel source can therefore remain emitting after its
  stream disappears, outside the original measurement expiry. Decide whether
  to stop the owned source or retain ownership while disabling feedback;
  reconcile `laser_command.c`, `throughput_monitor.c`, `lasers.c` and the
  [operation contract](throughput.md#lifetime-manual-changes-and-failure).
  This audit documents the current path without changing behavior.


### LLM Resolved; Human Review Requested


## Decisions To Make
- [ ] Confirm the attenuator leakage-floor constraint. The previous calibration
  description used the last three normalized points' mean; current
  `estimate_max_atten_db()` uses `max(mean, 65 dB)` while retaining their measured
  standard error. The 65 dB lower bound is a prior on the model, not measured
  optical support. Decide whether this prior and its uncertainty treatment are
  intended, or whether the estimator must change. The
  [calibration mathematics](attenuator_calibration.md#records-telemetry-and-fit)
  now distinguishes this constraint from the calibrated operating range.

- Review the freezer report's protection comparison: physical FLC OCP readings
  were 261.5–265.5 / 131.3–134.3 mA versus sheet maxima 250 / 120 mA; compiled
  acceptance ceilings also exceed vendor maxima. Normal commanded maxima agree.
  No pot adjustment or limit changes were made. Decide protection settings from
  the exact hardware/vendor requirements, not from archive readback alone.
- Decide whether a controlled PID comparison is merited for Aero DFB diodes
  using 100/1000/0 rather than Maiman's 20/1000/1000 DFB example. FLC already
  uses that example. 

## TODOs
- Investigate detector startup and illuminated settling separately. The September 21
  19:05 UTC noise capture saved a clipped dark (mean 53.8 mV, RMS 319.5 mV), then
  a stable dark trace near -2 mV. Firmware now requires ten continuous seconds
  of PD on and relevant lasers off before collecting a dark, and rejects clipped
  captures. Verify the physical stabilization time; the code does not establish it. The same run
  shows a long transition after 182 seconds of laser emission, so laser warmup alone
  does not explain all settling. Keep the sampler-owned notification TODO below.

- Play with and explore laser/tune. make sure it doesn't break autooff for laser or with throughput measuring

- Add sampler-owned illumination-change notification with settling duration.
  Laser/attenuator changes report the event; readings and windows remain marked
  settling until their acquisitions are clear of it. Consumers wait or skip.
  Keep the current local 100 ms autocalibration wait until this shared change.

- algorithmic/proceedural status (e.g. atten aotocalibration or throughput monitor 
  autoranging notices) should be going to console AND mqtt and that was the whole point 
  of a combined dispatch helper. this needs an app-wide reevaluation.
- Status needs to gain things we actually want.
- Lab-investigate safe laser-bank and external-relay shutdown on fatal faults.
  The current fatal path halts and relies on watchdog reset; formal shutdown
  safety is deferred pending hardware exploration.
