# Laser calibration

`laser_calibration.c` owns current acquisition, fitting, application and optional
persistence after FVOA calibration. The existing throughput thread advances it;
no new thread or heap is used. See [commands](commands.md#lasercalibrate) for
prerequisites, payloads and the field/commissioning workflows.

## Model and identifiability

At fixed DAC settings in segment $j$, the signed dark-subtracted voltage is
$V_i=b+G_j P(I_i)$. Same-run zero-current windows establish residual baseline $b$.
Measured same-current overlaps establish relative segment scale $S_j$, without
assuming a linear laser curve or using FVOA attenuation to infer curve shape:

```{math}
\frac{S_j}{S_{j-1}}=\frac{\overline{V}_{j}-b}{\overline{V}_{j-1}-b},\qquad
U_i=\frac{V_i-b}{S_j},\qquad
P_i=P_{ref}\frac{U_i}{\overline{U}_{ref}}.
```

The three reference windows define $\overline U_{ref}$ and turn each relative
scale into the recorded `segment_mv_per_mw`. PD gain, FVOA scale and unknown fixed
loss cancel from this curve inference. They remain separate assumptions for loss
estimation. Absolute power still depends on the vendor anchor; the experiment
cannot distinguish a multiplicative driver-current error from efficiency.
`current_cal_pct`, PD gain and the configured dark are not changed.

The runtime curve, for $I>t$, is

```{math}
x=I-t,\qquad P(I)=x[\eta+q(I-I_e)].
```

Below threshold, modeled power is zero. `threshold_current_ma` is the corrected
curve's zero; `efficiency_mw_per_ma` retains the **uncorrected straight-line fit's
slope** on the selected support. `efficiency_mw_per_ma2` is the quadratic
coefficient. The installed `efficiency_ref_ma` ($I_e$) is a curve coordinate,
independent of the datasheet triplet. Setting $q=0$ recovers the linear law.
Changing datasheet inputs later does not change this installed curve. The vendor
reference need not lie in a linear region, and its stated maximum power is not
interpreted as a nonlinearity specification.

The independent reference triplets `(ref_mw, ref_ma, ref_temp_c)` are
`(43.8,250,25)` for 1028y, `(10,52,25)` for 1270j, `(10,49,25)` for both 1430s,
`(10,47,25)` for 1510h and `(3.18,120,25)` for 2330k. They are separate from
nominal current and mutable model settings. Current is commanded mA on the
0.1 mA driver grid.

## Fit and interval selection

For each candidate current interval, scale current to $z\in[-1,1]$ and fit

```{math}
P(z)=P_{ref}+c_1[T_1(z)-T_1(z_{ref})]+c_2[T_2(z)-T_2(z_{ref})],
\qquad T_1(z)=z,\quad T_2(z)=2z^2-1.
```

The anchor is exact. A direct weighted two-column normal solve uses the existing
[`calibration_solve_2x2`](calibration_fitting.md), with diagonal scaling before
inversion. The linear candidate sets $c_2=0$. On the same selected observations,
let $d_i=I_i-R$, where $R=\texttt{ref_ma}$, and let $W_i$ be inverse normalized
mean variance. Preserve the uncorrected anchored line's slope:

```{math}
\eta=\frac{\sum W_i d_i(P_i-P_{ref})}{\sum W_i d_i^2},\qquad
M=\frac{\sum W_i d_i^3}{\sum W_i d_i^2},\qquad
s=\eta-qM.
```

Here $s$ is the corrected curve's slope at the datasheet reference. Round $q$ to
its stored float precision before deriving the remaining coefficients:

```{math}
t=R-\frac{2P_{ref}}{s+\sqrt{s^2-4qP_{ref}}},\qquad
I_e=\begin{cases}R,&q=0\\2R-t+M,&q\ne0.\end{cases}
```

This rationalized root selects the increasing branch. The derived coordinate
reproduces the quadratic while retaining $\eta$; it is not an independently fitted
parameter. It never divides by $q$, and $M$ comes from the existing normal matrix.
$I_e$ may exceed the measured or allowed current range and is not quantized to
a driver register. Scoring uses the actual stored curve. There is no iterative
laser optimizer, fixed 2% seed window, or compiled-efficiency deviation guard.

For a window with $n_i$ surviving readings and recorded population RMS $r_i$,
$s_i^2=r_i^2n_i/(n_i-1)$ when $n_i>1$. Pool off-state within-window variances to
obtain $d^2$, bounded below by ADC quantization variance. Mean weights are
$w_i=n_i/\max(s_i^2,d^2)$; single-reading means use the pooled floor. The baseline
is weighted by the same rule. Normalized mean variance is $1/(w_iG_j^2)$.
Overlaps and reference repeats are pooled with the corresponding scaled weights.

Enumerate contiguous intervals whose endpoints are measured usable currents.
Every usable observation inside each interval participates; individual outliers
are never removed to improve acceptance. An interval must:

- contain the reference and nominal currents and at least six distinct currents;
- have a nonnegative modeled threshold at least 0.1 mA below its lower endpoint;
- have positive derivative from threshold through its upper endpoint;
- keep every absolute fractional mean residual within `max_fit_residual_pct`
  (default **5%**), and every mean's three-standard-error precision within the
  same tolerance. This precision check includes pooled baseline uncertainty.

Choose the widest passing span, then the lower floor. Prefer a linear curve if
both orders pass the same interval; otherwise compare residual RMS. Curvature
may extend the useful range, but cannot excuse residuals beyond the requested
tolerance. If no interval passes, retain a diagnostic candidate and its failure
reason. Fitting never changes acquisition completion or discards raw records.

On application, the lower endpoint becomes `min_autolevel_current_ma`; the upper
endpoint becomes the existing `max_current_ma`. The maximum cannot fall below
nominal or reference current. If a bad high-power region contains either required
point, the run rejects instead of silently discarding that point. The automatic
floor avoids the knee without redefining the modeled threshold. Manual current
commands retain their existing meaning. Runtime power requests, tuning power
budgets and automatic power steps use the corrected forward model and inverse.

`rms_fraction` and `max_residual_fraction` describe mean-curve departure, not
within-window temporal noise. With normal matrix $H$ and root sensitivity
$J_t=\partial t/\partial(c_1,c_2)$, `threshold_se_ma` is
$\sqrt{J_t H^{-1}J_t^T}$ (the one-column equivalent for a line).
This is **conditional** on measured normalization and the fixed reference. It
excludes common overlap/anchor uncertainty, baseline uncertainty in the fit
covariance, systematic drift and vendor uncertainty. It is not rescaled by
reduced chi-square and should not be read as absolute calibration accuracy.

## Acquisition and bounded storage

Start checks selected PD power, configured dark, both dark-busy flags, reference
envelope and optical routes before replacing the retained dataset. It stops
throughput and laser outputs, routes the loopback and starts at high calibrated
attenuation. TEC qualification requires three one-second checks within 0.1 C of
`ref_temp_c`, with a 60 s timeout. Temperature drift discards the partial window
and waits again. Completion/STOP leave TEC policy unchanged.

After an off-state window, step down from the configured maximum. The grid combines
33 linear currents toward the seed threshold, 20 logarithmic overdrive currents,
nine currents through the knee to zero and the reference. Deduplicate on the
0.1 mA grid; the six default profiles fit the 64-entry grid. Dwell is 100–2000 ms,
default 1000 ms, rounded up to the PD's 50 ms sample interval. Each actuator change
settles for 100 ms and resets the window to exclude conversions already in flight.

Range toward `pd_target_mv` (default 1500 mV), using at most 16 initial probes
and four probes on subsequent segments. Accepted ranged means are 65–115% of
target, or below target at zero attenuation. Peaks must remain below 1850 mV.
When a descending-current mean falls below target/6, retain it, reduce attenuation
at the same current and record the overlap. Three reference windows use one DAC
pair. Continue to zero. A provisional accepted fit may select up to eight interior
currents in the lower-bound bracket at that segment's exact DAC pair; capture a
new zero window and refit before application. Probes, saturation, below-SNR and
ADC-error records remain available but do not contribute to curve fitting.

| Storage | Bound |
|---|---:|
| Raw windows | 128 × 24 = 3072 bytes |
| Current grid | 64 × 2 = 128 bytes |
| Sweep segments | 8 |
| Other-output reservation | 24 of the 128 windows |
| Wire metadata, including 16-byte support mask | 428 bytes |
| Laser numerical scratch | 1496 bytes on the checked build |
| Shared data / scratch unions | 7680 / 3584 bytes, unchanged |

Records/results/metadata reuse the existing calibration unions. A full buffer
ends acquisition as incomplete; no record is overwritten. The curve reference
adds one double to each property/fit copy without another per-point array.
The self-contained policy record is 152 bytes; see [settings](settings.md#scoped-laser-calibration-restore)
for old-record handling and explicit reinstallation before/after flashing.

Partial ADC failures use surviving samples. Zero surviving samples or no fresh
window within dwell + 3 s is terminal. At least one off-state window needs two
samples to establish empirical variance. STOP cancels/joins numerical work,
stops emission and preserves records. A failed stop retains a shutdown obligation
for explicit retry. Fitting runs at the existing lowest application priority
without the calibration mutex held. Starts/stops join before reusing scratch.

## Temporal noise and static loss

Only accepted core support contributes to noise fitting. With measured segment
gain $G_j$ in mV/mW and corrected power $P_i$:

```{math}
s_i^2=d^2+G_j^2 C^2+(G_jP_i)^2 f^2.
```

The existing scaled two-term nonnegative variance solve checks the interior, both
axes and the origin. It performs one initial solve and two variance-weighted
passes, using $\operatorname{Var}(s_i^2)=2v_i^2/(n_i-1)$ as the independent-Gaussian
approximation. Negative measured excess variances stay signed to avoid upward
bias. Rank-deficient noise estimates cannot be adopted.

These are effective source-referred terms: illuminated PD/FVOA noise not removed
by the off-state windows remains included. No assumed electrical FVOA noise is
subtracted. `update_noise=false` reports estimates without changing settings;
`true` requests both noise terms with an accepted core. PD noise is unchanged.

With `determine_static_loss=true`, the reference and frozen PD conversion $G$
in mV/mW give

```{math}
P_{PD}=\frac{V_{ref}-b}{G},\qquad
L_{combined}=10\log_{10}\frac{P_{ref}}{P_{PD}}-A_{FVOA}-L_{loopback},\qquad
L_{outbound}=L_{combined}-L_{return}.
```

`loopback_loss_db` describes external cable/attenuators only. Return transmission,
PD gain, reference power and FVOA model remain assumptions. The result replaces
the whole selected named outbound loss; it is not an incremental correction.
Negative/nonfinite losses reject. Loss can be *estimated* despite core rejection,
but cannot be *applied* without an accepted core.

`other_output=true` adds an emission-off `awaiting_output` pause with PD auto-off
inhibited. Physically reconnect the other AO/FEI output to the same return and
continue, optionally supplying a different external loss. Its own dark/reference
triplet/zero windows estimate that output's loss without repeating the sweep.
Automatic first-stage changes survive a later stage failure.

## Application, restoration and notebook review

The field default `apply=true` installs an accepted core automatically. Noise and
loss are requested separately; all requested estimates validate before settings
mutation. `persist=true` additionally saves the current laser policy and requested
route overrides. Core acceptance, RAM application and NVS persistence have distinct
outcomes; a flash failure cannot roll back an already active RAM update.

The commissioning notebook starts with `apply=False,persist=False`, archives raw
data first, and shows one summary plus normalized power/curve/residual plots.
Selected points come from the transmitted mask. Below-floor extrapolation, the
uncorrected weighted line (with its own intercept), corrected curve, vendor trace
and source-power assumptions are explicit. Detailed raw-window plots are optional.
Reviewed installation sends all four coefficients through `laser/settings`, with
optional noise in that write and separate existing route-loss writes. Saved fits
remain installable after reboot or dataset replacement. Target/diode/archive
checks belong to Python, and each acknowledgement/error/readback is saved without
rewriting the original calibration result.

The notebook preserves historical S154C readings and labels the original base
attenuation and later JIB path adjustment separately. Digitized vendor traces in
`doc/datasheets/<serial>_power_vs_current.txt` retain source/precision comments;
they are not raw vendor samples. The notebook compares linear and quadratic fits
over identical report intervals. Earlier archives can be normalized offline from
measured overlaps for comparison, with unknown membership explicitly labeled;
their original failed result is never replaced. Optional 500/1500 mV target
comparison helps test PD-level dependence. It cannot by itself isolate diode
curvature from detector or path effects.

Restore returns to compiled threshold, efficiency, curve reference, zero correction, maximum,
automatic floor, reference and noise defaults, and removes the selected named
outbound override. It preserves nominal current, current calibration, identity,
other TEC policy, PD return and the other output. This is not an undo to a pre-run
snapshot. Applied/persisted flags and independent errors report partial outcomes.

## Status and retained data

Status fields: `state`, `laser`, `output` (current stage), `records`,
`dwell_ms`, `current_ma`, `acquisition_complete`, `overflow`, `shutdown_pending`,
`error`. States are `inactive`, `running`, `fitting`, `awaiting_output`, `complete`,
`stopped`, `error`. Completion concerns acquisition of the current stage.

- `fit`: `valid`, `accepted`, `applied`, `persisted`, `error`, `apply_error`,
  `persist_error`, `n`, `threshold_ma`, `efficiency_mw_per_ma`,
  `efficiency_mw_per_ma2`, `efficiency_ref_ma`, `floor_ma`, `max_ma`, `baseline_mv`,
  `rms_fraction`, `max_residual_fraction`, `threshold_se_ma`.
- `noise`: `valid`, `applied`, `persisted`, `error`, `constant_mw`, `fractional`.
- `routes`: two objects, requested output then other output; each has `accepted`,
  `applied`, `persisted`, `error`, `apply_error`, `persist_error`, `combined_db`,
  `outbound_db`. Unrequested results have false flags. Invalid numbers are null.

All JSON errors are text/null. Internal errno values remain C implementation
contracts. Expected guards have specific text such as `pd off`, `dark required`,
`too non-linear` or `fit failed`; unexpected errno uses `unexpected error (<ENAME>)`.
Flags describe actions by this run, not guarantees that settings were never edited
later. Read current settings to establish present configuration.

Binary retrieval is MQTT only. `laser/calibrate/records` returns **428 bytes**,
little endian, magic `HLC1`, version byte **3**:

| Offset | Contents |
|---:|---|
| 5–15 | State, laser index, PD channel, fiber ASCII, first-FEI flag, options (loss=1, noise=2, other=4, persist=8, apply=16), record size=24, count, records/chunk, segment count, reserved |
| 16 | `uint32 dwell_ms`, `uint64 started_ms` (uptime) |
| 28 | 15 doubles: reference triplet, maximum, nominal, seed threshold, current-cal %, PD mV/mW, configured dark mean/RMS, return transmission, first/other loopback dB, compiled threshold/efficiency |
| 148 | Six floats: two DAC full scales, two limits, observed temperature min/max |
| 172, 244 | Each FVOA: six doubles (f50, slope, max attenuation, calibrated max, gain, RMS) and six float correction coefficients |
| 316 | Two doubles: residual tolerance %, PD target mV |
| 332 | Eight doubles: measured segment gain mV/mW |
| 396 | Two doubles: same-run residual baseline mV, pooled dark variance mV² |
| 412 | 16-byte fit-membership bitmask, low bit first; one bit per retained record |

`laser/calibrate/records/<chunk>` returns headerless `<IffHHHhBBBB` records:
elapsed uptime ms, net mV, population RMS mV, current in 0.1 mA, two confirmed
12-bit DAC codes, signed ADC maximum, good/failed counts, segment and flags.
Flag bits 0–1 mean OK/saturated/below-SNR/ADC-error; bits 2–4 mean
sweep/probe/overlap/reference/dark/refinement; bit 7 means other output.
Other-output segment is 255. DAC mV is `code * full_scale_mv / 4096`.
Use advertised chunk capacity. The host checks run ID, uptime and stable metadata
around download so replaced data cannot silently mix with an earlier run.

Starting either calibration replaces the single retained dataset; reboot loses
it. Applied settings survive replacement, but raw data and results are not saved
to NVS. Download and archive before starting another run.
