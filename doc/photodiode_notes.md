# Photodiode sampling and uncertainty

The photodiode owner acquires detector voltage and timestamps, maintains rolling
windows and dark records, and reports measurement quality. It has no laser,
attenuator or wavelength context. [Throughput](throughput.md) applies that context
after acquisition. See [hardware](hardware.md#tib-photodiode-monitoring-adc) for the
input chain and [commands](commands.md#pd) for readback/control fields.

## Acquisition and windows

Each channel produces a fresh ADS1115 conversion every **50 ms (20 Hz)**.
The configured 64 SPS converter performs YJ/HK sequentially: about 31.3 ms of
conversion time per round, before I2C and scheduling overhead. A conversion is
never reused as a new stream reading. One binary semaphore wakes throughput after
the round; delayed consumers may miss acquisitions, but do not replay them.

The fixed 500 ms window serves diagnostics; a configurable window up to 2000 ms
serves dark capture and calibration. Neither is the input to autolevel. Resetting
the configurable window excludes conversions begun before the reset and counts
subsequent conversion attempts, including failures, until the rounded duration
is filled. Surviving samples determine the mean/RMS. Zero valid samples fails the
average; callers do not retry individual failed ADC conversions.

The sampler stores monotonic acquisition start (`sample_ms`) and estimated UTC
midpoint (`t_ms`). Rolling voltage/net samples and mean/RMS use float; dark
subtraction and final error/power propagation use double. Public fields are double.
A transient ADC error warns, discards that sample and leaves the previous latest
state without advancing its timestamp. This prevents duplicate telemetry/control.

The [80 kΩ / 20 kΩ dividers and 0.47 µF capacitors](hardware.md#tib-photodiode-monitoring-adc)
map 0–10 V to 0–2 V and form nominal 21.2 Hz low-pass filters. Their single-pole
time constant is $\tau=(80\,\mathrm{k\Omega}\parallel20\,\mathrm{k\Omega})\,0.47\,\mu\mathrm F
=7.52$ ms, giving about 52 ms to 0.1% settling. This describes the PCB network;
the detector and ADC also have their own responses.
Ideal white noise filtered by this pole has $e^{-0.05/0.00752}\simeq0.0013$ instantaneous
sample correlation; real detector/ADC response, aliasing and optical motion need
measurement. Firmware applies no inverse filter or illuminated-stream blanking.
Recapture dark after changing the sample cadence: saved records contain duration
and failures, not their original sampling rate.

## Dark acquisition and noise warnings

```{mermaid}
flowchart TD
  Request[pd/dark capture] --> Quiesce[Stop throughput and its owned laser]
  Quiesce --> Eligible{PD on and relevant lasers off continuously for 10 seconds?}
  Eligible -->|no| Eligible
  Eligible -->|yes| Reset[Reset configurable window; exclude old conversion]
  Reset --> Collect[Collect requested post-reset attempts]
  Collect --> Changed{Power or illumination changed?}
  Changed -->|yes| Eligible
  Changed -->|no| Check{Valid samples and no clipping?}
  Check -->|yes| Save[Install dark; optional NVS; measured-only lowest update]
  Check -->|no| Error[Retain prior dark; report error]
```

Already elapsed eligible time counts. Capture does not turn off unrelated manual
lasers or resume monitoring afterward. A forced dark supplies an offset and its
uncertainty without establishing empirical detector noise. Zero-duration forced
values cannot replace the measured lowest-dark record.

Noise warnings have a separate rule. Hardware owners bracket PD power, actual
DAC/laser changes and MEMS pulses with begin/end notifications. Both channels wait
until overlapping operations finish, five seconds pass, and ten fresh attempts
replace the diagnostic window. At least two readings must be valid. Known no-ops
do not restart the grace period; partial/failed changes do. This affects warnings
only, not samples or windows. Continuous control can defer warnings indefinitely.
The per-channel cooldown is 60 seconds; `noisewarn_mv=0` disables it. Window RMS
cannot identify an electrical versus optical origin.

## Voltage and mean uncertainty

Let $q=0.0625/\sqrt{12}=0.01804$ mV be the one-code uniform-rounding RMS floor.
For a measured dark with population RMS $r_D$ and $N_D$ valid readings:

```{math}
\sigma_{\rm read}=\max(q,r_D),\qquad
\sigma_D=\frac{\sigma_{\rm read}}{\sqrt{N_D}}.
```

$N_D$ is the rounded duration/50 ms minus failed readings. Measured dark noise
already contains quantization; adding $q$ again would double-count it. For a
forced dark, $\sigma_{\rm read}=q$ and $\sigma_D$ is the supplied `rms_mv`.
For raw reading $V$ and dark $D$:

```{math}
V_{\rm net}=V-D,\qquad
\sigma_{\rm net}=\sqrt{\sigma_{\rm read}^2+\sigma_D^2}.
```

For a window with valid count $N$ and population RMS $r$:

```{math}
\sigma_{\overline{V}_{\rm net}}=
\sqrt{\frac{\max(r,\sigma_{\rm read})^2}{N}+\sigma_D^2}.
```

The dark-offset term is common to all later samples and never shrinks with this
window. For example, dark RMS 1 mV from 20 samples gives offset uncertainty
0.2236 mV and single-reading error 1.0247 mV. A new ten-sample window of RMS
2 mV has mean error 0.6708 mV. This assumes independent reading noise; use the
[uncertainty guide](mathematics.md) when comparing it with temporal scatter.

## Voltage to optical power

With effective transimpedance $G$ in V/A (including the divider) and nominal
responsivity $R$ in A/W:

```{math}
P_D[\mathrm{nW}]=\frac{10^6\max(V_{\rm net}[\mathrm{mV}],0)}{GR},\qquad
\sigma_{P_D}[\mathrm{nW}]=\frac{10^6\sigma_{\rm net}[\mathrm{mV}]}{GR}.
```

Signed net voltage is retained even when negative inferred power clips to zero.
NaN voltage propagates as unavailable. A known-source wavelength coefficient is
applied by throughput, not by the ADC owner; its current table is unity.

| Channel | Detector gain before divider (V/A) | Effective ADC gain $G$ (V/A) | Default $R$ (A/W) |
|---|---|---|---|
| YJ, Femto FWPR-20-IN | $1.0\times10^{11}$ | $2.0\times10^{10}$ | 0.93 |
| HK, Thorlabs PDA10DT at 70 dB | $4.75\times10^6$ | $9.5\times10^5$ | 0.60971 |

For HK, 1000 mV net gives $1.726\,\mu\mathrm W$ with these defaults. A 10 mV
ADC-input RMS corresponds to 17.3 nW for HK or 0.538 pW for YJ. These are
nominal conversions at the configured responsivity, not a wavelength-independent
detector sensitivity. The HK 500 Hz selector does not set the bandwidth of the
complete detector, divider, ADC and sampling chain.

Raw voltage at or above **2000 mV** is overrange, before the numerical ADC rail
of **2047.9375 mV**. Such optical power/throughput is only a nominal lower bound;
detector and throughput errors become NaN/JSON null and S/N is suppressed.
Calibration examines the raw **maximum** of each window, since a single clipped
sample can bias an otherwise plausible mean. The source-error estimate may still
be finite: it describes a different part of the measurement.

Detector outputs were observed to saturate around **10.3–11.4 V** in lab tests.
The current divider maps that to 2.06–2.28 V, beyond the ADC's approximately
2.048 V rail (10.24 V at the detector output). The ADC therefore clips first
for those observed detector limits. Use the 2000 mV operating limit when
planning measurements; a flat ADC trace cannot locate detector saturation.

Responsivity/gain drift, illuminated shot noise and physical settling have no
separate measured terms in this firmware error budget. The host
[temporal-noise helpers](python_helpers.md#noise-calculations) support explicit
comparisons without conflating those terms with static model accuracy.

## Detector response references

These digitized responsivity curves are inputs for optical calculations, in
**A/W**, rather than measurements of the installed channels. The complete data
are available as {download}`Femto FWPR-20-IN CSV <reference_data/femto_fwpr20in_responsivity.csv>`
(9 points) and {download}`Thorlabs PDA10DT CSV <reference_data/thorlabs_pda10dt_responsivity.csv>`
(96 points, 1400–2350 nm in 10 nm steps).

```{figure} figures/photodiode_responsivity.svg
:alt: Femto responsivity rises from 0.2 A/W at 900 nm to about 0.95 A/W near 1500 nm and falls at 1700 nm; Thorlabs rises from about 0.5 A/W at 1400 nm to about 1.25 A/W near 2200 nm.

Reference responsivity samples. Connecting lines guide interpolation within the
tabulated ranges; they do not establish an installed-channel calibration.
```

| TIB wavelength (nm) | YJ reference (A/W) | HK reference (A/W) |
|---|---|---|
| 1028 | 0.656, linearly interpolated | — |
| 1270 | 0.85 | — |
| 1430 | 0.93 | 0.5264 |
| 1510 | — | 0.60971 |
| 2330 | — | 1.23378 |

The firmware uses the configured nominal responsivity per channel (0.93 for YJ,
0.60971 for HK); it does not interpolate these files. The known-source wavelength
correction table remains unity. Keep that distinction when comparing modeled
optical power at different wavelengths with reported nW.

### Sensitivity and noise bandwidth

The [Femto receiver specification](https://www.femto.de/images/pdf-dokumente/de-fwpr-20-in.pdf)
gives $10^{11}$ V/A at a load of at least 100 kΩ, a 20 Hz −3 dB bandwidth,
110 pW optical saturation for linear amplification at 1550 nm, and NEP
$7.5\ \mathrm{fW}/\sqrt{\mathrm{Hz}}$ at 1550 nm and 1 Hz. Its electrical test
conditions include ±15 V supply, 25 °C and 20 minutes of warm-up. These receiver
figures precede the PCB divider/filter and ADC.

For a white input-referred noise density NEP and a normalized full-chain response
$H(f)$, the corresponding ADC voltage RMS is

```{math}
B_{\rm ENBW}=\int_0^\infty |H(f)|^2\,df,\qquad
\sigma_{V,\rm detector}=G R\,\mathrm{NEP}\sqrt{B_{\rm ENBW}}.
```

$G$ already includes the 0.2 divider; NEP and $R$ must refer to the same
wavelength. A single RC pole alone has
$B_{\rm ENBW}=\pi f_c/2$, about 33.2 Hz for the PCB's 21.2 Hz pole. The full
response also contains the detector and ADC filters. Neither the HK 500 Hz
selector nor the stream's 10 Hz Nyquist frequency is the full-chain ENBW.
For nonwhite noise, integrate its power spectral density through $|H(f)|^2$
instead of treating one quoted NEP as constant across the band.

For collected data, measured dark RMS is the empirical detector/ADC noise term;
do not add a datasheet NEP estimate or quantization to it again. Illuminated
shot noise and FVOA drive noise are separate temporal contributions. Use the
[noise calculation helper](python_helpers.md#noise-calculations) with an explicit
bandwidth and the [attenuator noise assumptions](attenuator.md#transmission-uncertainty)
when comparing a scope measurement with PD scatter.
