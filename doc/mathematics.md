# Mathematics and reported uncertainty

Start with the operation you are using. Equations live with their assumptions,
units and failure behavior, rather than in the command field descriptions.

| Operation | Explanation |
|---|---|
| Three-way time division | [MEMS splitting: loss correction and tick quantization](mems_splitting.md) |
| PD voltage, power and dark subtraction | [Photodiode sampling and uncertainty](photodiode_notes.md) |
| Throughput and feedback | [Throughput measurement and autolevel](throughput.md) |
| Attenuation dB ↔ transmission ↔ drive | [Attenuator model and control](attenuator.md) |
| Bridging, weighted fitting and leakage floor | [Attenuator calibration](attenuator_calibration.md) |
| Laser power, photons and wavelength | [Laser operation and tuning](laser_operation.md) |
| Reference-anchored efficiency, noise and route loss | [Laser calibration](laser_calibration.md) |
| Shared numerical optimizer | [Calibration fitting](calibration_fitting.md) |

## Conventions

Optical **power** attenuation uses $A=-10\log_{10}T$, with transmission $T$ a
unitless fraction, so $T=10^{-A/10}$. Positive dB means loss. Two series devices
have $A=A_1+A_2$ and $T=T_1T_2$. A 3.0103 dB loss transmits one half of the power;
20 dB transmits 0.01. Voltage ratios use this same power convention only when
voltage is proportional to optical power, as for these photodiode outputs.

`level` is a linear fraction for attenuators; laser command `value` is a fraction
of modeled nominal emission (the C tuner takes percent). The calibrated inverse
converts that emission request to current; only the linear model reduces to
threshold-to-nominal current interpolation. Read the command's units:
these are different quantities. DAC millivolts are measured before the FVOA
amplifier; FVOA-drive millivolts are after it. Model power is an estimate from
confirmed settings, not an independent optical measurement.

## What each error means

| Reported quantity | Meaning | May it be divided by $\sqrt{N}$? |
|---|---|---|
| PD `rms_mv` | Population scatter of valid readings about their window mean | Only to estimate mean error under an independence assumption |
| PD dark/mean error | Estimated uncertainty of the offset or window mean | A reused dark offset is common to all subsequent readings |
| FVOA `rms_db` | Unweighted residual scatter of supported calibration points about the accepted curve | No: this is curve accuracy, not fresh measurement noise |
| Laser `fractional_noise`, `constant_noise_mw` | Configured or fitted effective power-noise terms | Their bandwidth/temporal independence must first be established |
| Throughput `tp_pd_err` | Detector contribution to throughput error | Contains common dark-offset uncertainty |
| Throughput `tp_err` | Detector plus modeled source/attenuation uncertainty | No automatic averaging reduction |
| Tune `wavelength_error_nm`, `power_error_mw` | Signed requested-minus-achievable model residuals | These are not statistical uncertainties |
| Laser fit `threshold_se_ma` | Local threshold standard error from the inverse normal matrix | No: already a fit-parameter estimate, conditional on its weights/model |
| Laser fit `rms_fraction` | Unweighted fractional mean-curve residual RMS | No: distinct from within-window temporal noise |
| Fit maximum error / correlation | Model agreement diagnostics | Neither is an uncertainty of a parameter |

Errors are approximately one-sigma estimates where the underlying model supports
that interpretation. They are not confidence intervals, worst-case bounds, or a
complete absolute calibration budget. Static route loss, responsivity, gain,
wavelength response, drift and settling have no separate uncertainty terms in
current firmware. Unknown/nonfinite quantities are NaN in binary data and `null`
in JSON. An overrange detector reading is a nominal lower bound with unavailable
detector/throughput error; a finite numeric value does not make it a valid fit point.

For independent inputs $x_i$, firmware uses first-order propagation:

```{math}
\sigma_f^2 \simeq \sum_i \left(\frac{\partial f}{\partial x_i}\sigma_i\right)^2.
```

Correlated inputs would also require covariance terms
$2\sum_{i<j}(\partial_i f)(\partial_j f)\operatorname{Cov}(x_i,x_j)$.
The present firmware does not carry a covariance matrix. Reusing a dark,
reference window, bridge normalization or model makes errors correlated across
records even where one record's calculation uses quadrature. Keep that distinction
when comparing a reported uncertainty with a measured time-series RMS.

For a window with $N$ valid readings, population RMS is
$\sqrt{\sum_i(x_i-\bar x)^2/N}$. Laser noise fitting converts it to sample variance
$N\,\mathrm{RMS}^2/(N-1)$ when $N>1$. Missing samples reduce $N$; they are not zeros.

## Temporal-noise calculations in the host tool

The [Python guide](python_helpers.md#noise-calculations) describes the pure
`attenuator_noise` and `throughput_noise` helpers. Their explicit bandwidth and
scope RMS inputs support lab comparisons; they do not alter firmware uncertainty
or prove the physical origin of observed fluctuations. Equivalent noise bandwidth
(ENBW) is the integral of the normalized power response, not automatically half
the stream rate or the analog RC cutoff. A 20 Hz RC filter is not a brick-wall
anti-alias filter for a 20 Hz stream.
