# Laser calibration

`laser/calibrate` runs after FVOA calibration with an external loopback to the
selected SM/MM photodiode return. `laser_calibration.c` owns acquisition, fitting,
application and optional persistence. `laser_command.c` parses commands. The
existing throughput thread advances the operation; no extra thread or heap is
used. See [commands](commands.md#lasercalibrate) for the interface.

## Model and identifiability

At fixed DAC settings in segment j, fit signed dark-subtracted voltage as

```{math}
V_{ij}=b+a_j(I_i-t).
```

`t` is threshold in commanded mA, `b` is the residual baseline and `a_j` absorbs
PD gain, static transmission and FVOA transmission. Their absolute scales cannot
be separately recovered from this sweep. `current_cal_pct` is held fixed: a
multiplicative driver-current error remains degenerate with efficiency. The
independent vendor reference defines

```{math}
\eta=\frac{P_{\rm ref}}{I_{\rm ref}-t}\quad[\mathrm{mW/mA}].
```

This assumes the reference lies in the validated linear region at `ref_temp_c`.
It does not establish absolute power independently of that reference. No PD
responsivity, transimpedance, configured dark, or current-calibration setting is
refined. The reference fields are independent of nominal current and the mutable
threshold/efficiency model; changing one does not silently move the reference.

Compiled `(ref_mw, ref_ma, ref_temp_c)` values are `(43.8,250,25)` for 1028y,
`(10,52,25)` for 1270j, `(10,49,25)` for both 1430s, `(10,47,25)` for 1510h and
`(3.18,120,25)` for 2330k. These represent each diode's vendor test point.

For each trial `(t,b)`, profile each nuisance gain analytically:

```{math}
a_j=\frac{\sum_i w_{ij}(I_i-t)(V_{ij}-b)}{\sum_i w_{ij}(I_i-t)^2}\quad[\mathrm{mV/mA}].
```

Five weighted sums per segment suffice. Analytic derivatives of this quotient
feed a two-parameter damped Gauss–Newton solve. The [shared two-parameter optimizer](calibration_fitting.md) uses 30 iterations,
bounded trials and a symmetric 2x2 solve. Attenuator weighting, point order and
six-term correction fitting remain with their existing owner.

Weights use temporal variance of each window mean, bounded below by the
observed off-state variance. Off/zero-current windows constrain one shared `b`.
Configured dark-offset uncertainty is common to the run and is not incorrectly
averaged as independent noise on every point. Each contributing segment needs
three distinct current levels; the joint fit needs six distinct levels.

For a window with $n_i$ valid readings and sample variance $s_i^2$, its mean
weight is $w_i=n_i/\max(s_i^2,d^2)$, where $d^2$ is the pooled off-state
within-window variance, bounded below by quantization variance. A single-reading
window can supply a mean but cannot establish the empirical noise floor.

The reported `threshold_se_ma` is the local normal-matrix estimate

```{math}
\sigma_t=\sqrt{\frac{H_{11}}{H_{00}H_{11}-H_{01}^2}},\qquad
H=J^T J,
```

including the shared baseline constraint and derivatives through profiled gains.
A singular matrix fails the fit. This estimate is not scaled by reduced chi-square
and does not include vendor-reference uncertainty or unmodeled optical drift.
The separate mean-curve diagnostic is

```{math}
\texttt{rms\_fraction}=\sqrt{\frac1N\sum_{i\in\mathrm{fit}}
 \left[\frac{V_i-b-a_{j(i)}(I_i-t)}{a_{j(i)}(I_i-t)}\right]^2}.
```

It is unweighted fractional scatter of window means around the fitted curve,
not within-window temporal noise.

Seed the fit with the upper 75% of reference overdrive, from
`seed_threshold + .25*(ref_ma-seed_threshold)` through `ref_ma`. Extend downward
and upward from the reference; three distinct consecutive current levels with
both >2% and >3 standard-error departure end a linear interval. Repeated reference
windows cannot count as three departures. A boundary needs precision sufficient
to establish 2% linearity. Fit the selected interval again. The default acceptance
guards are threshold change <= `max(1 mA, .05*ref_ma)` and efficiency within ±25%
of the compiled efficiency. The reference must agree with the line. The lowest
validated linear current becomes `min_autolevel_current_ma`, subject to the
existing threshold + 0.1 mA/grid/nominal-current validator. It may decrease or
increase. `linear_max_ma` is diagnostic; configured maximum current is unchanged.

## Acquisition and bounded storage

Start validates the reference/current/temperature envelope, selected route,
PD power and configured dark, then cancels previous calibration and throughput.
It stops laser outputs to exclude other diodes sharing the selected input. It
selects launch and return routes and high calibrated attenuation, starts the TEC
at `ref_temp_c`, and waits for three one-second checks within 0.1 C (60 s timeout).
A later drift discards the partial window and waits again. TEC operation continues
after completion/stop; calibration does not rewrite the stored TEC setpoint.

Capture an off-state window, then step down from the quantized configured maximum.
The grid combines 33 linear points from maximum to the seed threshold, 20
log-spaced overdrive points from 0.1 mA to reference overdrive, nine points from
threshold to zero, and the exact reference. Deduplicate on the 0.1 mA grid; the
six default profiles use at most 64 points. Dwell defaults to 1000 ms (100–2000 ms,
rounded up to the PD's 50 ms sample interval). Allow 100 ms settling after writes
and reset the configurable PD window to exclude conversions already in flight.

Hunt from high attenuation toward a mean near 1500 mV with peaks below 1850 mV,
using at most 16 initial probes. When a descending-current window falls below
250 mV, retain it, reduce attenuation at the same current, and retain the overlap
with a new segment. Later range searches have four probes. Each record includes
raw peak, valid/failed sample counts, population RMS, event and confirmed DAC
codes; probes and clipped/low-SNR points remain available but do not fit the curve.
Three reference windows use one DAC pair. Continue through the knee to zero.
A provisional fit can select up to eight interior points in the lower linearity
bracket, reusing that segment's exact DAC pair, then capture zero again and refit.

There are at most eight sweep segments and 128 records total, including probes,
repeated anchors and zero windows. Each record occupies 24 bytes: **3072 bytes**.
A request for the other output reserves 24 records, leaving 104 for the sweep.
Metadata is <=512 bytes; the laser numerical workspace is <=1024 bytes. Dataset,
results, 64-entry current grid and fitting workspace reuse the attenuator's
**7680-byte data union and 3584-byte scratch union**. A full buffer ends acquisition
as incomplete; no record is overwritten. The shared telemetry buffer is reused.

A partial ADC failure uses surviving samples. Fitting needs at least one off-state
window with two surviving samples to establish empirical variance. Zero surviving samples or no fresh
window within dwell + 3 s is terminal. Stop cancels/join numerical work before
releasing ownership, stops emission, and retains records. Failed STOP retains a
shutdown obligation for explicit retry. Numerical fitting runs at the existing
lowest application priority with no calibration mutex held. Manual laser starts,
settings/attenuator/route/PD mutations and throughput starts are busy while this
operation owns the path, including the recabling pause. Zero/STOP or bank-off can
cancel it. An accepted attenuator start can replace it after confirmed shutdown.

Starting either attenuator or laser calibration replaces the one raw dataset.
Applied settings survive replacement; raw data and results are not persisted.
Download before another start. Acquisition completion, fit acceptance, RAM
application and successful NVS persistence are distinct status facts. Applied
flags describe this run's actions; later manual edits or restoration do not rewrite
historical fit results. Query laser/route settings for current configuration.

## Temporal noise

The PD retains population RMS. With n surviving readings, use unbiased sample
variance `s² = n/(n-1) * rms²` for n > 1. Pool off-state within-window variances to
estimate `d²`; use only accepted linear-region windows for

```{math}
s_{ij}^2=d^2+\left(\frac{a_j}{\eta}\right)^2 C^2+[a_j(I_i-t)]^2 f^2.
```

Fit the two nonnegative variance terms with scaled two-column least squares,
checking the interior, both axes and the origin. Three passes use variance-based
weights after the first solve: for modeled reading variance $v_i$, the Gaussian
independent-sample approximation gives $\operatorname{Var}(s_i^2)=2v_i^2/(n_i-1)$.
Keep negative measured excess variances signed;
clipping them would bias both noise estimates upward. Reject a rank-deficient
noise fit. Mean-curve residual RMS is reported separately.

These are effective source-referred noise estimates: any illuminated PD/FVOA noise
not removed by the off-state measurement remains included. Do not subtract the
assumed 10 mV FVOA electrical noise or a static model RMS. `update_noise=false`
reports estimates only; `true` applies them only with an accepted core fit and
valid noise fit. It never updates PD noise settings.

## Static loss and the other output

`determine_static_loss=true` requires an explicit, nonnegative
`loopback_loss_db`, for the **external cable/attenuators only**. With reference
voltage corrected by the same-run baseline and snapshotted optical conversion gain $G$ in mV/mW (including detector
responsivity, divider and known wavelength correction):

```{math}
P_{\rm PD}=\frac{V_{\rm ref}-b}{G},\qquad
L_{\rm combined}=10\log_{10}\frac{P_{\rm ref}}{P_{\rm PD}}-A_{\rm FVOA}-L_{\rm loopback},
\qquad L_{\rm outbound}=L_{\rm combined}-L_{\rm return}.
```

`L_combined` includes outbound plus configured SM/MM return. The return loss is
held fixed, so only the selected exact named-laser outbound route is replaced.
This is a complete route override, not an additional correction to the old loss.
A negative/nonfinite inferred outbound loss is rejected. Reference anchoring can
succeed independently of core-fit acceptance, using its own off-state baseline.

With `other_output=true`, apply accepted first-stage results, stop emission and
enter `awaiting_output`. PD auto-off remains inhibited through this pause so
continuation can acquire without changing the protected PD settings. Physically reconnect the external loopback to the other
AO/FEI output, keeping the same return. `continue:true` reroutes, repeats thermal
qualification, captures its own dark, ranges at `ref_ma`, captures three reference
windows and final zero. It does not repeat the current sweep. Continue may replace
that output's external loopback loss. A stop/error in this stage preserves the
first-stage core/noise/route changes and persistence.

## Restore compiled defaults

`restore_defaults:true` with `laser` and `output` stops calibration/throughput and
the selected source, restores compiled threshold, efficiency, autolevel floor,
reference triplet and both noise settings, and removes the selected named outbound
route override. Removing the override exposes the compiled route default.
Current limits, `current_cal_pct`, driver identity, other TEC policy, the return
loss and other-output override are preserved. If preserved limits make the
compiled core invalid, that part reports failure; route restoration is independent.

`persist:false` changes RAM only; old saved values return after reboot.
`persist:true` writes laser policy/reference records and deletes the matching
route NVS entry. Separate error/applied/persisted fields report partial work;
flash failure does not imply a RAM rollback. A second persistent restore also
finds the old route record after an earlier volatile removal.

## Validation boundary

The fit assumes stable optical paths within each segment and an accurate vendor
reference at the stated temperature. Offline numerical checks cannot establish
physical settling, thermal stability, reference accuracy, target stack margin or
persistence over power cycles. These remain [bench-review items](human_review_required.md).
