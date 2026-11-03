# Laser calibration

`laser_calibration.c` owns acquisition, fitting and optional application after
FVOA calibration, on the existing throughput thread. There is no new thread or
heap allocation. See [commands](commands.md#lasercalibrate) for prerequisites and
payloads. A field call can acquire, accept and install a calibration without Python.

## Acquisition and evidence

The experiment measures every sampled positive current at two fixed DAC pairs:
a high-signal descending sweep and a low-signal ascending sweep. The first block
also measures up to five upper currents at a middle PD level. Changing current
within a block does not re-solve attenuation. After each block, the exact primary
DAC codes and boundary current are revisited. These repeats test actuator history
and drift; they do not determine the normalization.

Start validates PD power, configured dark, neither PD dark operation pending,
reference reachability and both optical routes before replacing old data. It
stops throughput and laser outputs, selects the loopback, sets high calibrated
attenuation, and starts the TEC at `ref_temp_c`. Three one-second temperature checks
within 0.1 C qualify acquisition. A deviation discards the partial window and
restarts qualification, with a 60 s timeout. STOP remains available.

A condition retains **three windows**. The early window starts after the existing
100 ms actuator/ADC guard. The first late window starts no earlier than
`settle_ms` after actuation; a second late window follows it. Waiting is handled
by the existing worker and does not sleep for the whole settling interval under
the command lock. Defaults are `settle_ms=2000`, `dwell_ms=1000`. Dwell rounds up
to the 50 ms PD sample interval. Early windows remain visible but never enter a fit.

The descending grid combines nine linear points from maximum to the seed threshold,
nine logarithmic overdrive points, five points from zero through the seed threshold,
0.9 and 1.1 times the seed threshold, and the exact power-reference current. After
0.1 mA quantization and deduplication it contains at most 24 currents including zero.

Initial ranging uses at most 16 probes; each of at most five subsequent ranges
uses four. The preferred primary mean is 65–115% of `pd_target_mv`, with peak
below 1850 mV. Those are ranging preferences, not measurement-validity limits.
When the probe budget is exhausted or the next attenuation move is negligible,
use the valid probe closest to the target, restoring its exact DAC pair if needed.
Validity uses the normal recorded classification: peak below 2000 mV and mean
above five times its standard error. Thus a 1931 mV peak or a much lower signal
can be usable. Fresh early/late windows follow; probes themselves do not enter
the fit. Ranging fails only when no valid probe was found, except at operational
full open, where dim observations are retained and acquisition continues. Full open
is the frozen pair of per-FVOA open voltages and can have nonzero modeled loss.

A primary block ends early when the mean falls below target/6 **and** another
range can open farther. At operational full open, or after six primary ranges,
low signal does not repeatedly restart the first block. Its companion attenuation
is +6.02 dB (one quarter signal); the upper
middle setting is +2.22 dB (0.6 signal). The next range begins at the old boundary
but resumes acquisition at the next grid current. At the optical floor, acquisition
continues through all remaining currents. A condition without usable signal is
retained; it cannot establish a passing fitted interval.

Initial, intermediate and final zero-current triplets measure baseline history.
There is no provisional-fit-driven refinement. The optional other output reserves
24 windows: dark/reference/end triplets and at most 15 ranging probes.

| Storage or acquisition bound | Size |
|---|---:|
| Raw records | 240 × 24 = 5760 bytes |
| Quantized current grid | 24 × 2 = 48 bytes |
| Exact DAC-pair identities | 16 |
| Primary high/low blocks | 6 |
| First-output worst-case records | 216 |
| Optional other-output reservation | 24 |
| Binary metadata, including 30-byte support mask | 634 bytes |
| Laser data / numerical scratch members | 6664 / 3528 bytes |
| Existing shared data / scratch unions | 7680 / 3584 bytes, unchanged |

The worst-case first-output budget is 138 positive-current records, nine dark
records, 18 revisit records, 15 middle-level records and 36 ranging probes.
The upper middle-level sweep occurs once. Zero-current darks and boundary revisits
are bounded by those blocks; reaching full open cannot repeat them indefinitely.
A full buffer reports incomplete acquisition and never overwrites evidence.
Partial ADC failures use surviving samples; zero valid samples or a window timeout
is an acquisition failure. Fit rejection does not change acquisition completion.
Raw data survive rejection and STOP until either optical calibration starts again
or the controller reboots.

The commissioning notebook provides six explicit source subsections, with
independent YJ/HK routes, run switches and saved-data reviews. Ordinary commands
and progress polling are synchronous; MQTT collection remains independent.
Replay uses static figures, while live widgets have separate setup cells. It reports
acquisition failure separately from fit rejection. When acquisition fails before
fitting, raw diagnostic panels remain available and residual panels explain the
missing curve and report the acquisition error.

## Raw-PD fit and uncertainty

At current $I_i$, elapsed time $t_i$ and exact DAC pair $j$, fit

```{math}
V_i=\sum_{k=0}^{2} w_k(t_i)b_k+T_j Q(I_i)+\epsilon_i,\qquad
T_j=10^{-A_j/10},\qquad Q(I)=K P(I).
```

$A_j$ comes from the frozen calibrated FVOA models and recorded DAC codes. The
three baseline values are shared nuisance parameters constrained by the initial,
intermediate and final same-run dark conditions. $w_k(t)$ linearly interpolates
adjacent measured dark times. A paired condition uses the midpoint of its two
late timestamps, rounded down to whole milliseconds, for both dark knots and
light observations. The fit propagates their shared uncertainty rather
than treating a subtracted baseline as exact. Light observations must be bracketed
by these measurements; no extrapolated time law is used. Interpolation cannot
identify unobserved changes between dark measurements. $K$ combines PD conversion
and unknown fixed path transmission.
The measured `ref_mw` at `ref_ma,ref_temp_c` fixes the power scale. PD conversion
is needed to infer static loss, but not to infer the reference-relative curve.
Absolute power remains conditional on the vendor reference; this experiment cannot
separate an overall current calibration error from optical efficiency.

Scale current to $z\in[-1,1]$. The curve is linear or uses the degree-two Chebyshev
basis $T_1(z)=z$, $T_2(z)=2z^2-1$. When the reference lies inside fitted support,
$Q=R+c_1(z-z_r)+2c_2(z^2-z_r^2)$. When it lies outside support, the measured
reference response $R$ is a separate coordinate and the restricted curve is
$Q=c_0+c_1z+c_2(2z^2-1)$. In both cases $K=R/P_{ref}$: the existing vendor
reference supplies absolute scale. An outside reference is not a demand that the
restricted polynomial extrapolate through that power/current point. Such a fit
must be labeled with both its support and outside reference, and requires human
adoption. No external meter result silently replaces the vendor anchor.

At most seven scalar unknowns are needed, including the three dark knots. A
degree-two Chebyshev model and a quadratic span the same functions; more flexible
knee and high-end shapes remain offline sensitivity checks.

For each window, convert population RMS $r$ to sample variance $s^2=r^2n/(n-1)$.
The pooled dark variance, bounded below by ADC quantization variance, supplies the
floor. For the two late means $v_1,v_2$, fit their average with variance

```{math}
D_i=\max\left[\frac{1}{4}\left(\frac{\max(s_1^2,d^2)}{n_1}
 +\frac{\max(s_2^2,d^2)}{n_2}\right),\ \frac{(v_1-v_2)^2}{2}\right].
```

The second term is deliberately not divided away as independent-sample noise.
Two late windows cannot fully characterize long correlations; the reversed sweep
and identical-condition revisit provide additional checks.

For each exact DAC pair, the combined calibration uncertainty is
$\sigma_A=\operatorname{hypot}(rms_{dB,1},rms_{dB,2})$ and
$\sigma_f=\ln(10)\sigma_A/10$. All records at that pair share one gain uncertainty:

```{math}
C_j=D_j+\sigma_f^2\mu_j\mu_j^T,\qquad
C_j^{-1}=D_j^{-1}-\frac{D_j^{-1}u_j u_j^T D_j^{-1}}{1+u_j^TD_j^{-1}u_j},
\quad u_j=\sigma_f\mu_j.
```

The firmware streams these rank-one corrections into small normal equations.
It never allocates an observation covariance matrix. Different DAC pairs are
assumed independent because the attenuator fit does not provide their covariance;
this approximation is explicit, not a claim that calibration errors are white noise.
Scalar nuisance elimination leaves a two-coordinate solve using the existing
[`calibration_solve_2x2`](calibration_fitting.md). Predicted signals update the gain
covariance for at most 20 iterations, stopping at relative prediction change below
$10^{-6}$. Failure to converge rejects the fit. Numerical work is cancellable and
runs at the existing lowest application priority without the calibration lock.

The profiled common gain shifts are published separately in metadata. They are
neither bridge normalizations nor updates to FVOA coefficients. Mean residuals
and PD-level agreement are evaluated after these uncertainty-constrained shifts.
Plots also show normalization using the unshifted calibrated transmission so the
size and consequence of the shifts remain visible.

## Curve meaning and acceptance

The installed model remains, above threshold,

```{math}
P(I)=(I-t)[\eta+q(I-I_e)].
```

`threshold_current_ma` is the corrected curve's zero. `efficiency_mw_per_ma`
is the **uncorrected linear fit's slope on the same observations**.
`efficiency_mw_per_ma2` is the correction $q$; `efficiency_ref_ma` is the derived
coordinate $I_e$ that reproduces the corrected polynomial while preserving that
linear efficiency. It is independent of the vendor triplet and need not lie on
an attainable current. With zero correction the reference coordinate has no effect.
The rationalized increasing root avoids cancellation near a linear curve; stored
correction uses float precision. The runtime forward model, inverse, level requests
and power budgets use these four coefficients. `value=0.5` requests half nominal
**emission**, not half nominal current.

First search the lowest usable floor through the configured maximum, with at
least six distinct supporting currents on the 0.1-mA driver grid. Prefer a line
when both orders pass at the same floor. If no full-range core curve passes,
search contiguous restricted intervals using the same coverage, residual and
vendor checks. Select the widest passing interval; ties prefer the lower floor,
then a line. All usable late conditions inside an interval participate; no PD
band or individual outlier is dropped. Each fitted current needs two usable PD
levels separated by at least a factor of two.

The threshold must be nonnegative and at least 0.1 mA below the floor. The model
must retain positive derivative through the unchanged engineering maximum, so a
restricted fit does not bypass settings validation. This condition permits safe
model inversion; it does not establish calibration accuracy outside support.
Maximum and nominal current remain unchanged. Adopted support bounds constrain
`min_autolevel_current_ma` and `max_autolevel_current_ma`, not engineering
`value_ma` commands.

For each baseline-subtracted observed signal $v_i$ and predicted PD signal $\mu_i$
including its profiled common gain shift, require

```{math}
|v_i-\mu_i|\leq \tau\mu_i+3\sqrt{D_i},\qquad
\tau=\texttt{max\_fit\_residual\_pct}/100.
```

The default is 5% **beyond three measured standard errors**. Large measured
uncertainty is not itself evidence of nonlinearity. Reported maximum/RMS residuals
remain raw fractions, with no uncertainty subtracted. Cross-attenuation differences
and gain shifts remain diagnostic; neither has a separate acceptance cutoff.
Their uncertainty still contributes to GLS weights and fitted covariance.

After selecting the curve, check all usable exact-current/exact-DAC revisits once
against that selected baseline, even outside its support. A repeat requires
$|s_i-s_j|\leq\tau\max(0,(s_i+s_j)/2)+3\sqrt{D_i+D_j}$.
Failure requires review of the whole calibration; support is not searched again
to evade the control. Near-dark fractional repeat diagnostics are omitted; if no
eligible illuminated comparison exists, the fraction is unavailable (NaN in binary
metadata), not zero variation. The absolute mV consistency test still applies.
Dark movement is represented by
the measured knots, rather than rejected for departing from a global straight
line. Knot residuals remain diagnostic. No universal repeat-derived noise floor
or fitted multiplicative time correction is added to acceptance or fitting.

### Unit-specific vendor comparison

Calibration fine-tunes trusted characterization and detects system upsets requiring
investigation. Immutable piecewise-linear tables in `laser_calibration.c` come from
the existing digitizations in `doc/datasheets/`. Those text files identify each source
report, optical reference plane, temperature and approximate reading precision.
They are not raw vendor measurements. Compression preserves every original point
to within 0.1% of the vendor reference power; the trace is never extrapolated.

| Laser | Report serial | Trace range (mA) | Retained knots | Reference power (mW) |
|---|---|---:|---:|---:|
| 1028y | BF69016 | 0.33–249.01 | 60 | 43.8 |
| 1270j | 8881AFR | 10.04–60.41 | 22 | 10 |
| 1430yj | 8897ACR | 3.99–55.53 | 27 | 10 |
| 1430hk | 8895ACR | 3.99–55.53 | 25 | 10 |
| 1510h | 8882ACT | 5.03–55.53 | 23 | 10 |
| 2330k | D4520009 | 0.96–119.52 | 68 | 3.18 |

The fitted interval must overlap the vendor trace. At each retained vendor knot
inside that interval, require

```{math}
|P_{fit}(I)-P_{vendor}(I)|\leq\tau P_{vendor,ref}+3\sigma_{P_{fit}}(I).
```

The allowance uses vendor **reference power**, not local near-threshold power.
For $P=P_{ref}Q/R$, project its gradient through the full GLS covariance to obtain
$\sigma_P$. This includes fitted baseline, measured reference and common FVOA
uncertainty, conditional on the model and fixed vendor anchor. Threshold SE uses
the same covariance with the implicit-root gradient. Neither SE includes unknown
systematic error or vendor-reference uncertainty. Baseline/gain adjustments do
not identify which diode, driver, attenuator or coupling effect caused a discrepancy.

Every restricted result has `restricted=true`, `accepted=false`, and requires
explicit human adoption, even when its core checks pass. Excluded high-current
behavior remains in the retained records and plots. A tiny passing interval is
not a claim that the full laser or optical path is characterized. If neither a
full-range nor restricted interval passes, retain the physically valid full-range
candidate with the smallest maximum raw residual for review. Report residual,
vendor or control failure with acquisition complete and raw data retained.
Review-needed results automatically write no curve, noise or route-loss settings.

## Noise, static loss and application

On any valid, identifiable fitted support, estimate temporal variance as
$s_i^2=d^2+G_j^2 C^2+(G_jP_i)^2f^2$. The existing scaled, nonnegative two-term solve
checks the interior, axes and origin, with two variance-weighted refinements.
Negative excess variance remains signed to avoid positive bias. These are effective
source-referred noise estimates: illuminated PD and FVOA noise can remain included.
Static attenuation-calibration uncertainty is not temporal noise. These estimates
describe within-window variation; they do not summarize slow repeat changes or
establish intrinsic diode stability. They remain available when automatic curve
acceptance fails. An unidentifiable two-term split is reported as unavailable.
`update_noise=false` leaves estimates diagnostic; `true` requests both terms,
whose automatic application still requires accepted core calibration.

For the reference scale, `combined_loss_db = 10*log10(pd_gain/K)-loopback_loss_db`;
subtract configured return loss to obtain the selected outbound loss. Reference
power, PD gain, return transmission and external cable loss are fixed assumptions.
Negative/nonfinite inferred losses reject. A second output acquires its own dark
and reference after an emission-off recabling pause, without another current sweep.

The field default `apply=true` validates all requested estimates before installing
curve, automatic range and optional noise/loss, preserving maximum/nominal. `persist=true` additionally
writes NVS; `apply=false,persist=true` is invalid. Numerical acceptance, RAM application
and persistence are independent outcomes. First-stage updates survive a later
second-stage failure. A failed core cannot apply noise or route loss.

Commissioning uses `apply=False,persist=False`, saves raw data first, and reviews
the transmitted support, raw PD/current, attenuation chronology, normalized curves,
local slopes, residuals, settling, revisits, darks and vendor/meter comparisons.
Manual installation supplies all four curve coefficients through `laser/settings`;
no retained dataset or laboratory identifier is required. Archive identity checks
belong to Python only. Manual adoption must carry all four model coefficients
and both automatic range bounds, validate them against current engineering
settings, and explicitly acknowledge a review-needed result. Its power panel
separates fitted support, the vendor scale reference and configured maximum;
out-of-support portions are identified as extrapolation. Vendor discrepancy is reported as a
percentage of vendor reference power, with the uncertainty allowance explained.

Restore returns to compiled curve, automatic floor/ceiling, maximum **and nominal**, power reference,
noise and selected outbound-route defaults. It preserves current calibration,
identity, independent `wave_ref_*`, unrelated TEC policy, return and other-output
losses. This is a return to datasheet defaults, not an undo to a previous run.

## Status and retained data

Status exposes `state`, `laser`, `output`, `records`, `dwell_ms`, `settle_ms`,
`current_ma`, `acquisition_complete`, `overflow`, `shutdown_pending`, and `error`.
States are inactive/running/fitting/awaiting_output/complete/stopped/error.
`fit` contains numerical availability, `restricted`, acceptance/application/persistence
flags, text errors, exact supporting window count `n`, the four curve coefficients,
floor/maximum, first fitted dark value (`baseline_mv`), residual RMS/maximum and
threshold SE. The complete shared dark model is in binary metadata.
`noise` reports constant/fractional estimates and independent flags/error.
`routes` contains first and other-output loss outcomes. All errors are strings or
null; unexpected errno uses `unexpected error (<ENAME>)`. Unavailable numbers are null.

`laser/calibrate/records` returns **634 bytes**, little endian, magic `HLC1`,
version **5**. No old wire layout is accepted by the live decoder.

| Offset | Contents |
|---:|---|
| 5–15 | State, laser, PD channel, fiber ASCII, first-FEI, options (loss=1/noise=2/other=4/persist=8/apply=16), record size=24, count, chunk capacity, DAC-pair count, reserved |
| 16 | `uint32 dwell_ms`, `uint64 started_ms` (uptime) |
| 28 | 15 doubles: power-reference triplet, maximum, nominal, seed threshold, current-cal %, PD gain, configured dark mean/RMS, return transmission, first/other loopback dB, compiled threshold/efficiency |
| 148 | Six floats: DAC full scales, limits, observed temperature min/max |
| 172, 244 | Each FVOA: six doubles (f50, slope, max attenuation, calibrated max, gain, RMS) and six float corrections |
| 316 | Two doubles: residual tolerance %, PD target mV |
| 332 | 16 doubles: nominal calibrated segment gain mV/mW, before nuisance shifts |
| 460 | Double pooled dark variance |
| 468 | 30-byte exact fit-support mask, low bit first; both late windows per fitted condition |
| 498 | `uint32 settle_ms` |
| 502 | Three `uint32 baseline_knots_ms` values, in elapsed-time order |
| 514 | Three double fitted `baseline_knots_mv` values |
| 538 | 16 float fractional common-gain shifts |
| 602 | Four doubles: maximum revisit fraction, PD-band fraction, attenuation shift in calibration sigma, dark residual mV |

Unavailable fitted knots have NaN values and zero times. A reader must not
interpolate them; retained measured dark windows still support raw diagnostic
plots after an incomplete or invalid fit. The out-of-support reference conditions
supply scale but are excluded from the reported curve-support mask and `n`.

`laser/calibrate/records/<chunk>` returns headerless `<IffHHHhBBBB` records:
elapsed ms, net mV, population RMS, current in 0.1 mA, two physical DAC codes,
signed ADC maximum, good/failed counts, exact-DAC-pair identity and flags.
Bits 0–1 classify OK/saturated/below-SNR/ADC-error; bits 2–4 identify
sweep/probe/reserved/reference/dark/revisit. Bit 5 marks early, bit 6 second late,
bit 7 other output; neither bit 5 nor 6 means first late. Probes are early-only.
Pair 255 means unassigned (probe, dark or other output).
DAC mV is `code*full_scale_mv/4096`. Download checks uptime and frozen acquisition
identity around retrieval; no lab archive name is sent to the firmware.

The laser policy record includes independent wavelength coordinates and automatic
current bounds; [settings](settings.md#scoped-laser-calibration-restore) describes
its layout and explicit policy backup/reinstallation around OTA.
