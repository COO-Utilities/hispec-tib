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

The maintained Rev. 2 model assumes 20 Hz low-pass filters after 0–10 V to
0–2 V dividers; the [as-built transfer needs confirmation](hardware.md#source-drawings-and-revision-boundaries). A single pole has $\tau=1/(2\pi20)=7.96$ ms and about 55 ms to
0.1% settling. This is a circuit approximation, not a measured settling guarantee.
Ideal filtered white noise has $e^{-2\pi20(0.05)}\simeq0.0019$ instantaneous
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

Raw voltage at or above **2000 mV** is overrange, before the numerical ADC rail
of **2047.9375 mV**. Such optical power/throughput is only a nominal lower bound;
detector and throughput errors become NaN/JSON null and S/N is suppressed.
Calibration examines the raw **maximum** of each window, since a single clipped
sample can bias an otherwise plausible mean. The source-error estimate may still
be finite: it describes a different part of the measurement.

Responsivity/gain drift, illuminated shot noise and physical settling have no
separate measured terms in this firmware error budget. The host
[temporal-noise helpers](python_helpers.md#noise-calculations) support explicit
comparisons without conflating those terms with static model accuracy.
