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
uses four. A usable primary mean is 65–115% of `pd_target_mv`, or lower at zero
attenuation, with peak below 1850 mV. A primary block ends when the mean falls below
target/6. Its companion attenuation is +6.02 dB (one quarter signal); the upper
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
| Binary metadata, including 30-byte support mask | 614 bytes |
| Laser data / numerical scratch members | 6632 / 3296 bytes |
| Existing shared data / scratch unions | 7680 / 3584 bytes, unchanged |

The worst-case first-output budget is 138 positive-current records, nine dark
records, 18 revisit records, 15 middle-level records and 36 ranging probes.
A full buffer reports incomplete acquisition and never overwrites evidence.
Partial ADC failures use surviving samples; zero valid samples or a window timeout
is an acquisition failure. Fit rejection does not change acquisition completion.
Raw data survive rejection and STOP until either optical calibration starts again
or the controller reboots.

## Raw-PD fit and uncertainty

At current $I_i$, elapsed time $t_i$ and exact DAC pair $j$, fit

```{math}
V_i=b_0+b_1t_i+T_j Q(I_i)+\epsilon_i,\qquad
T_j=10^{-A_j/10},\qquad Q(I)=K P(I).
```

$A_j$ comes from the frozen calibrated FVOA models and recorded DAC codes. The
baseline intercept and slope are shared nuisance parameters constrained by the
same-run darks. $K$ combines PD conversion and unknown fixed path transmission.
The measured `ref_mw` at `ref_ma,ref_temp_c` fixes the power scale. PD conversion
is needed to infer static loss, but not to infer the reference-relative curve.
Absolute power remains conditional on the vendor reference; this experiment cannot
separate an overall current calibration error from optical efficiency.

Scale current to $z\in[-1,1]$. The curve is linear or uses the degree-two Chebyshev
basis $T_1(z)=z$, $T_2(z)=2z^2-1$. When the reference lies in the candidate interval,
use $Q=R+c_1(z-z_r)+2c_2(z^2-z_r^2)$ and $K=R/P_{ref}$. When the reference lies
outside, its measurements determine $R$ separately from the free polynomial
$Q=c_0+c_1z+c_2(2z^2-1)$. A rejected high-current region is not forced to lie on an
extrapolation of the accepted curve. No more than six scalar unknowns are needed.
A degree-two Chebyshev model and a quadratic span the same functions; independent
knee and high-end shapes require higher order, which remains an offline sensitivity
check rather than another embedded model.

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

Enumerate contiguous intervals with at least six usable currents. Every usable
late condition inside the interval participates; no PD band or individual outlier
is dropped to improve acceptance. Acceptance requires:

- nonnegative threshold at least 0.1 mA below the interval, and positive derivative
  through its upper endpoint;
- two distinct attenuation conditions at every fitted current, with measured
  signals separated by at least a factor of two;
- mean residuals, normalized same-current PD-level differences and resolvable
  identical-condition revisit differences within `max_fit_residual_pct` (default 5%);
- each fitted mean's three-standard-error precision within that tolerance;
- common gain shifts within three calibration RMS uncertainties;
- dark means consistent with the fitted time slope, within three mean errors or
  five ADC quantization standard deviations, whichever is larger.

Revisit consistency is checked across the acquisition, even if a candidate interval
would exclude the revisit current. Near-dark ratios cannot establish fractional
repeatability. Dark, reference and attenuation uncertainties enter the fitted
covariance; `threshold_se_ma` is the implicit-root sensitivity projected through
that covariance. It still excludes unknown systematic effects and vendor reference
uncertainty, and is not an absolute-accuracy claim.

Select the widest passing interval, then the lower floor. Prefer a line when both
orders pass the same interval. Keep a diagnostic candidate and explicit text reason
when none passes. The floor becomes `min_autolevel_current_ma`; the upper endpoint
becomes `max_current_ma`, and nominal becomes `min(previous_nominal,new_max)`.
Maximum is never increased. A later calibration whose reference exceeds that maximum
fails its reachability precondition; it never silently exceeds the installed limit.

## Noise, static loss and application

On accepted support, fit temporal variance as
$s_i^2=d^2+G_j^2 C^2+(G_jP_i)^2f^2$. The existing scaled, nonnegative two-term solve
checks the interior, axes and origin, with two variance-weighted refinements.
Negative excess variance remains signed to avoid positive bias. These are effective
source-referred noise estimates: illuminated PD and FVOA noise can remain included.
Static attenuation-calibration uncertainty is not temporal noise.
`update_noise=false` leaves estimates diagnostic; `true` requests both terms.

For the reference scale, `combined_loss_db = 10*log10(pd_gain/K)-loopback_loss_db`;
subtract configured return loss to obtain the selected outbound loss. Reference
power, PD gain, return transmission and external cable loss are fixed assumptions.
Negative/nonfinite inferred losses reject. A second output acquires its own dark
and reference after an emission-off recabling pause, without another current sweep.

The field default `apply=true` validates all requested estimates before installing
curve, floor, maximum/nominal and optional noise/loss. `persist=true` additionally
writes NVS; `apply=false,persist=true` is invalid. Numerical acceptance, RAM application
and persistence are independent outcomes. First-stage updates survive a later
second-stage failure. A failed core cannot apply noise or route loss.

Commissioning uses `apply=False,persist=False`, saves raw data first, and reviews
the transmitted support, raw PD/current, attenuation chronology, normalized curves,
local slopes, residuals, settling, revisits, darks and vendor/meter comparisons.
Manual installation supplies all four curve coefficients through `laser/settings`;
no retained dataset or laboratory identifier is required. Archive identity checks
belong to Python only.

Restore returns to compiled curve, floor, maximum **and nominal**, power reference,
noise and selected outbound-route defaults. It preserves current calibration,
identity, independent `wave_ref_*`, unrelated TEC policy, return and other-output
losses. This is a return to datasheet defaults, not an undo to a previous run.

## Status and retained data

Status exposes `state`, `laser`, `output`, `records`, `dwell_ms`, `settle_ms`,
`current_ma`, `acquisition_complete`, `overflow`, `shutdown_pending`, and `error`.
States are inactive/running/fitting/awaiting_output/complete/stopped/error.
`fit` contains numerical availability, acceptance/application/persistence flags,
text errors, exact supporting window count `n`, the four curve coefficients,
floor/maximum, baseline intercept, residual RMS/maximum and threshold SE.
`noise` reports constant/fractional estimates and independent flags/error.
`routes` contains first and other-output loss outcomes. All errors are strings or
null; unexpected errno uses `unexpected error (<ENAME>)`. Unavailable numbers are null.

`laser/calibrate/records` returns **614 bytes**, little endian, magic `HLC1`,
version **4**. No old wire layout is accepted by the live decoder.

| Offset | Contents |
|---:|---|
| 5–15 | State, laser, PD channel, fiber ASCII, first-FEI, options (loss=1/noise=2/other=4/persist=8/apply=16), record size=24, count, chunk capacity, DAC-pair count, reserved |
| 16 | `uint32 dwell_ms`, `uint64 started_ms` (uptime) |
| 28 | 15 doubles: power-reference triplet, maximum, nominal, seed threshold, current-cal %, PD gain, configured dark mean/RMS, return transmission, first/other loopback dB, compiled threshold/efficiency |
| 148 | Six floats: DAC full scales, limits, observed temperature min/max |
| 172, 244 | Each FVOA: six doubles (f50, slope, max attenuation, calibrated max, gain, RMS) and six float corrections |
| 316 | Two doubles: residual tolerance %, PD target mV |
| 332 | 16 doubles: nominal calibrated segment gain mV/mW, before nuisance shifts |
| 460 | Two doubles: residual baseline at time zero, pooled dark variance |
| 476 | 30-byte exact fit-support mask, low bit first; both late windows per fitted condition |
| 506 | `uint32 settle_ms` |
| 510 | Double baseline slope mV/s |
| 518 | 16 float fractional common-gain shifts |
| 582 | Four doubles: maximum revisit fraction, PD-band fraction, attenuation shift in calibration sigma, dark residual mV |

`laser/calibrate/records/<chunk>` returns headerless `<IffHHHhBBBB` records:
elapsed ms, net mV, population RMS, current in 0.1 mA, two physical DAC codes,
signed ADC maximum, good/failed counts, exact-DAC-pair identity and flags.
Bits 0–1 classify OK/saturated/below-SNR/ADC-error; bits 2–4 identify
sweep/probe/reserved/reference/dark/revisit. Bit 5 marks early, bit 6 second late,
bit 7 other output; neither bit 5 nor 6 means first late. Probes are early-only.
Pair 255 means unassigned (probe, dark or other output).
DAC mV is `code*full_scale_mv/4096`. Download checks uptime and frozen acquisition
identity around retrieval; no lab archive name is sent to the firmware.

The laser policy record is now 168 bytes with independent wavelength coordinates;
[settings](settings.md#scoped-laser-calibration-restore) describes exact-length
rejection of older records and explicit policy backup/reinstallation around OTA.
