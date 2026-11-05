# Laser calibration

`laser_calibration.c` owns acquisition, fitting and optional application after
FVOA calibration, on the existing throughput thread. There is no new thread or
heap allocation. See [commands](commands.md#lasercalibrate) for prerequisites and
payloads. A field call can acquire, accept and install a calibration without Python.

## Compiled defaults adopted November 4, 2026

The default power models in `laser_properties.h` use only the six digitized
vendor traces in `doc/datasheets/*_power_vs_current.txt`, not the current in-situ
calibration curves. For each diode, fit the extrapolated zero $t$ and quadratic
coefficient $q$ to all digitized points at or below the unchanged vendor reference
current. Trapezoidal current-spacing weights prevent dense digitization from
dominating the least-squares fit. Fix $I_e=I_{ref}$ and
$\eta=P_{ref}/(I_{ref}-t)$ so the curve passes through the vendor reference.
The commissioning notebook's offline `vendor_defaults` cell reproduces the fit.

| Laser / report | $t$, mA | $\eta$, mW/mA | $q$, mW/mA² | $I_e$, mA | Weighted RMS / maximum residual, mW |
|---|---:|---:|---:|---:|---:|
| 1028y / BF69016 | 16.6083487737 | 0.1876673812875 | −6.06411690252e−5 | 250 | 0.20630 / 0.60604 |
| 1270j / 8881AFR | 9.784031814486 | 0.2368771919681 | −4.188985980908e−5 | 52 | 0.02144 / 0.07699 |
| 1430yj / 8897ACR | 3.592432600386 | 0.2202276090237 | −7.876910936396e−5 | 49 | 0.01965 / 0.05804 |
| 1430hk / 8895ACR | 3.43502055322 | 0.2194667949248 | −2.581978567103e−4 | 49 | 0.02062 / 0.06180 |
| 1510h / 8882ACT | 4.095546033222 | 0.233076034664 | −2.424936267747e−4 | 47 | 0.03396 / 0.10645 |
| 2330k / D4520009 | 20.60959567841 | 0.03199504038348 | −8.911153979743e−5 | 120 | 0.02688 / 0.08343 |

All six stored curves have positive slope from their fitted zero through the
unchanged engineering maximum. The zeros are model coordinates, not new
turn-on measurements. Plot digitization and the imposed reference limit their
physical accuracy; the extra digits preserve the calculation. The vendor
reference powers remain 43.8, 10, 10, 10, 10 and 3.18 mW, respectively. Safety
limits, nominal currents, diode identities, temperature and wavelength references
are unchanged. Startup automatic floors still use threshold plus 0.1 mA rounded
up to the driver grid, and ceilings remain nominal current.

Noise defaults are independent of these vendor fits. The active calibration
readback frozen at 22:41:58 UTC on November 4 supplies **fractional noise only**:

| Laser | Fractional RMS default | Source capture in `tools/commissioning_data/` |
|---|---:|---|
| 1028y | 0.00898166511 | `laser_cal_1028y_20261104T160820_958998Z.pkl` |
| 1270j | 0.0424917488 | `laser_cal_1270j_20261104T161251_981668Z.pkl` |
| 1430yj | 0.0139060581 | `laser_cal_1430yj_20261104T161726_641001Z.pkl` |
| 1430hk | 0.0151791845 | `laser_cal_1430hk_20261104T162132_374295Z.pkl` |
| 1510h | 0.0577663351 | `laser_cal_1510h_20261104T162544_320364Z.pkl` |
| 2330k | 0.00234274023 | `laser_cal_2330k_20261104T163052_064646Z.pkl` |

These are effective illuminated-path variability estimates, not intrinsic diode
noise. The associated apply/readback captures agree with the frozen values.
By owner choice, **`constant_noise_mw=0` for every compiled default**; fitted
constant terms are not adopted. Startup and explicit calibration restore share
the fractional-noise table. Restore copies all four compiled curve coefficients,
including nonzero curvature. Valid saved overrides still take precedence on
boot; changing compiled defaults does not rewrite the live board or its NVS.

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
| Binary metadata, including 30-byte support mask | 588 bytes |
| Laser data / numerical scratch members | 6616 / 2888 bytes |
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

## Central response and observed variability

The two settled late windows at one commanded current and exact DAC pair form
one condition. Their mean is corrected using linear interpolation between the
three measured same-run dark means, at the midpoint of the two timestamps.
Light observations must be bracketed by the measured darks. This correction
cannot identify unobserved baseline changes between anchors.

The source model combines the locally measured threshold with vendor-supported
curvature and the configured reference power. This is intended to improve
relative throughput when current changes without teaching the source model a
current-correlated FVOA response. A constant gain per exact DAC pair cannot
separate those two effects over a broad sweep; more precise PD windows do not
resolve that ambiguity.

For corrected signal $S_i$, source power $P(I_i)$ and pair gain $G_j$, the local
threshold fit minimizes the mean absolute logarithmic residual
$|\log S_i-\log G_j-\log P(I_i)|$. Each condition has equal weight, and the
optimal log gain is the median for that pair. The same positive pair gains are
then estimated over the full retained support for diagnostics and noise. They
are shared across visits, so no fitted time-dependent gain removes an observed
excursion. This does not require the individual FVOA models to add accurately in
dB. A pair measured at only one distinct current retains a display gain but
cannot identify source shape or enter central-quality statistics.

`median_fraction` is `exp(median(abs(log(S/predicted))))-1`: a descriptive,
symmetric multiplicative central residual. RMS and maximum retain ordinary
`S/predicted-1` fractional residuals. These are not Gaussian sigmas, confidence
limits, or bounds on possible variability. Fixed pair gains cannot disentangle
current-dependent coupling from intrinsic laser response. Absolute source power
remains conditional on the existing vendor reference current/power; the captured
calibrations alone do not establish an independent absolute optical scale.

### Local threshold and broad diagnostic support

Sparse ranging islands with fewer than four distinct currents cannot identify
a lasing knee. For the two lowest-current informative DAC pairs, use the first six
distinct currents (or all four/five available); median repeated visits and scale
each pair by its largest local signal. This local window avoids interpreting
high-current rolloff as a lasing knee.

The small diagnostic model is `a + b*max(0, I-k)`, with nonnegative baseline and
positive slope. Search the knee on a bounded grid, retaining at least three
currents above it. A knee qualifies when its baseline is at most the configured
fractional tolerance, its squared residual cost is at most half that of a
straight line.
These are engineering selection criteria, not confidence tests. If a knee does
not qualify, the local line's nonnegative extrapolated zero sets the lower
eligibility boundary. Both cases retain the existing 0.1 mA headroom before
selecting the first current measured by both pairs above both independently
estimated boundaries. A dim companion need not detect below-lasing light, and
a one-grid-step difference does not invalidate their common usable support.
If their local windows have no usable common current, report unavailable support;
do not start a search for a passing interval.
The local knee is a support diagnostic, distinct from the installed model's
extrapolated zero. Only positive paired late observations from those same local
windows, at or above the selected floor, determine the installed threshold.
High-current path curvature cannot move that threshold. The complete broad
capture remains available for model diagnostics, noise and review.

At least six distinct usable currents are required above the selected floor.
The broad candidate extends to the unchanged requested maximum. No upper-end
rescue or repeated floor adjustment is performed. An isolated maximum-current
point with its own DAC pair does not validate the continuation of the response
model to that current and requires review.

### Model and vendor constraint

Write the reference-normalized response as

```{math}
\frac{P(I)}{P_{ref}}=\frac{I-t}{I_{ref}-t}
\left[1+c\frac{I-I_{ref}}{I_{ref}-t}\right].
```

The threshold search is bounded by zero and the selected floor minus 0.1 mA.
At each trial threshold, the reference fixes the linear efficiency. Use zero
curvature when that line agrees with the vendor trace within the existing
characterization allowance. Otherwise fit one quadratic coefficient to the
vendor trace, weighting by current spacing so dense digitization does not
dominate it, and retain a physical increasing response. Broad in-situ residuals
do not choose curvature or refit the threshold. Numerical work is cancellable,
uses the existing shared static scratch, and runs at the existing lowest
application priority.

Immutable tables in `laser_calibration.c` come from the existing report
extractions in `doc/datasheets/`. They are approximate digitizations, not raw
vendor measurements. At each retained knot inside support require
`abs(Pfit-Pvendor) <= tolerance * vendor_reference_power`; the default tolerance
is 5%. The trace is not extrapolated. A physical candidate outside that allowance
requires review. The reference guides the proposed shape; this is not evidence
that the in-situ optical path independently measured its curvature.

| Laser | Report serial | Trace range (mA) | Retained knots | Reference power (mW) |
|---|---|---:|---:|---:|
| 1028y | BF69016 | 0.33–249.01 | 60 | 43.8 |
| 1270j | 8881AFR | 10.04–60.41 | 22 | 10 |
| 1430yj | 8897ACR | 3.99–55.53 | 27 | 10 |
| 1430hk | 8895ACR | 3.99–55.53 | 25 | 10 |
| 1510h | 8882ACT | 5.03–55.53 | 23 | 10 |
| 2330k | D4520009 | 0.96–119.52 | 68 | 3.18 |

Automatic acceptance retains the existing central median-residual tolerance and
requires compatible measured high/low PD-band shapes. Select the widest pair of
bands with at least six common measured currents and a factor-two signal
separation. At each current, take the median across repeated conditions, then
compare the bands' logarithmic signal ratios after removing their constant gain
ratio. `band_fraction` is the median absolute log-ratio deviation, converted to a
fraction with `expm1`. This is a raw shape diagnostic, not agreement between two
curves both pulled toward the same vendor trace. It uses the same default 5%
tolerance; absent coverage is unavailable, not evidence of agreement. Raw
acquisition and every excursion remain available regardless of acceptance.

The installed representation is unchanged:

```{math}
P(I)=(I-t)[\eta+q(I-I_e)].
```

`threshold_current_ma` is the curve's extrapolated zero.
`efficiency_mw_per_ma` is $P_{ref}/(I_{ref}-t)$.
`efficiency_mw_per_ma2` is the vendor-supported quadratic correction and
`efficiency_ref_ma` equals the configured reference current. The curve therefore
passes through the reference power without carrying a discarded broad-fit slope
in its representation. Supply all four coefficients when adopting a model. Engineering
maximum/nominal currents remain unchanged; the automatic current bounds express
adopted support and do not limit engineering `value_ma` commands.

### Repeat summaries

Repeated current and exact DAC codes characterize the system's observed
variability. They never veto a central model. `repeat.matched` counts all matched
paired conditions whose two late windows each passed acquisition validity,
including its signal-to-noise classification. Below-SNR windows remain in the raw
records but do not form counted pairs; an acquisition-valid below-lasing pair can
still count as matched. `repeat.usable` counts comparisons at or above the
independently selected lasing floor with both signals
above ten measured condition standard errors. `repeat.in_range` counts usable
comparisons inside the selected model support. The latter standard-error check
is only an illumination eligibility check, not a claim about slow variability.

The worst eligible excursion is `abs(after/before-1)`, with commanded current,
exact DAC codes, both dark-corrected signals, elapsed time and support membership.
It is neither a Gaussian sigma nor a maximum possible excursion. Current-range
membership alone does not say whether its attenuator setting is representative:
review the DAC voltages and that pair's measured usable response. If no eligible
comparison exists, the worst value is unavailable; all original records remain
visible. There is no misleading percentage from below-lasing spontaneous light.
Review-needed calibration automatically changes no curve, noise or route settings.

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
percentage of vendor reference power, with the fixed characterization allowance explained.

Restore returns to compiled curve, automatic floor/ceiling, maximum **and nominal**, power reference,
noise and selected outbound-route defaults. It preserves current calibration,
identity, independent `wave_ref_*`, unrelated TEC policy, return and other-output
losses. This is a return to datasheet defaults, not an undo to a previous run.

## Status and retained data

Status exposes `state`, `laser`, `output`, `records`, `dwell_ms`, `settle_ms`,
`current_ma`, `acquisition_complete`, `overflow`, `shutdown_pending`, and `error`.
States are inactive/running/fitting/awaiting_output/complete/stopped/error.
`fit` contains numerical availability, acceptance/application/persistence
flags, text errors, exact supporting window count `n`, the four curve coefficients,
floor/maximum, `top_supported`, first measured dark value (`baseline_mv`), and central median,
RMS and maximum residuals. The measured dark anchors are in binary metadata.
The top-level `repeat` object provides the counts and worst eligible comparison
described above; it is available independently of automatic acceptance.
`noise` reports constant/fractional estimates and independent flags/error.
`routes` contains first and other-output loss outcomes. All errors are strings or
null; unexpected errno uses `unexpected error (<ENAME>)`. Unavailable numbers are null.

`laser/calibrate/records` returns **588 bytes**, little endian, magic `HLC1`,
version **6**. No old wire layout is accepted by the live decoder.

| Offset | Contents |
|---:|---|
| 5–15 | State, laser, PD channel, fiber ASCII, first-FEI, options (loss=1/noise=2/other=4/persist=8/apply=16), record size=24, count, chunk capacity, DAC-pair count, reserved |
| 16 | `uint32 dwell_ms`, `uint64 started_ms` (uptime) |
| 28 | 15 doubles: power-reference triplet, maximum, nominal, seed threshold, current-cal %, PD gain, configured dark mean/RMS, return transmission, first/other loopback dB, compiled threshold/efficiency |
| 148 | Six floats: DAC full scales, limits, observed temperature min/max |
| 172, 244 | Each FVOA: six doubles (f50, slope, max attenuation, calibrated max, gain, RMS) and six float corrections |
| 316 | Two doubles: residual tolerance %, PD target mV |
| 332 | 16 doubles: fitted free DAC-pair gain mV/mW |
| 460 | Double pooled dark variance |
| 468 | 30-byte exact fit-support mask, low bit first; both late windows per fitted condition |
| 498 | `uint32 settle_ms` |
| 502 | Three `uint32 baseline_knots_ms` values, in elapsed-time order |
| 514 | Three double measured `baseline_knots_mv` values |
| 538 | Three doubles: median residual fraction, raw PD-band log-ratio shape deviation as a fraction, worst eligible repeat fraction |
| 562 | Three uint16 counts: matched repeats, usable repeats, usable repeats in support |
| 568 | Three uint16 values: worst-repeat current in 0.1 mA, DAC1 code, DAC2 code |
| 574 | uint32 elapsed time between worst-repeat conditions, ms |
| 578 | Two floats: before/after corrected signals, mV |
| 586 | uint8 worst-repeat support membership; byte 587: source-curve maximum supported by a multi-current DAC pair |

Unavailable dark anchors have NaN values and zero times. A reader must not
interpolate them; retained measured dark windows still support raw diagnostic
plots after an incomplete or invalid fit. Conditions belonging to a singleton-current DAC pair have a display gain but
are excluded from the reported curve-support mask and `n`.

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
