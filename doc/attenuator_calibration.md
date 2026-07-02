# Attenuator Calibration Flow

This page describes the current firmware implementation of automatic TIB FVOA
attenuator calibration. The lab rationale and model notes live in
`attenuator_calibration_lab_notes.md`; this page documents the firmware command,
state, telemetry, and retained-record flow.

Calibration is intentionally measurement driven. It does not use datasheet
voltage schedules, target photodiode voltages, fixed optical volumes, or other
preselected optical limits. The firmware finds usable regions from observed
photodiode saturation and SNR, retains every acquisition record, and fits only
records that are valid for the current attenuator model.

## Ownership

```mermaid
flowchart TD
  Cmd[atten/calibrate command] --> Start[attenuator_calibration_start_auto]
  StatusReq[atten/calibrate query] --> Status[attenuator_calibration_format_status]
  RecordsMetaReq[atten/calibrate/records/<physical>] --> Meta[attenuator_calibration_write_data_metadata]
  RecordsChunkReq[atten/calibrate/records/<physical>/<chunk>] --> Chunk[attenuator_calibration_write_record_chunk]

  PDThread[photodiode sampler thread] --> PDStatus[photodiode_status snapshot]
  PDThread --> ConfigWindow[channel internal configurable window]
  PDThread --> FixedWindow[channel fixed monitoring window]

  TP[throughput monitor thread] --> Tick[attenuator_calibration_tick]
  PDStatus --> Tick
  Tick --> CalState[static calibration state]
  Tick --> Atten[physical FVOA DAC writes]
  Tick --> Laser[laser output percent]

  CalState --> Telemetry[dt/device/atten best-effort telemetry]
  CalState --> Status
  CalState --> Meta
  CalState --> Chunk
  Fit[accepted fit] --> Runtime[attenuator runtime coefficients]
  Fit --> Persist{persist requested}
  Persist -- yes --> NVS[app settings NVS]
  Persist -- no --> Runtime
```

The photodiode module owns ADC reads, current samples, moving windows, and dark
snapshots. The calibration module owns sequencing, retained records, bridge
normalization, fitting, and coefficient application. The throughput monitor
thread advances calibration so no second photodiode worker or calibration
worker exists.

## Start Sequence

```mermaid
flowchart TD
  Request[laser output fiber dwell_ms persist] --> Board{TIB board}
  Board -- no --> ENODEV[return ENODEV]
  Board -- yes --> LaserMap[map laser to logical attenuator]
  LaserMap --> PDRoutes[map laser and fiber to PD route]
  PDRoutes --> Dark{configured dark valid}
  Dark -- no --> EINVAL[return EINVAL]
  Dark -- yes --> PDOn{selected PD already powered}
  PDOn -- no --> EACCES[return EACCES]
  PDOn -- yes --> PDValid{latest PD sample valid}
  PDValid -- no --> ENODATA[return ENODATA]
  PDValid -- yes --> StopTP[stop throughput monitor]
  StopTP --> Routes[apply laser-output and fiber-PD routes]
  Routes --> ConfigWindow[set internal configurable window to dwell_ms]
  ConfigWindow --> MaxAtten[set both FVOAs to max DAC drive]
  MaxAtten --> LaserOff[stop laser output]
  LaserOff --> Init[reset calibration state]
  Init --> First[start dac1 acquisition]
```

Automatic calibration does not power the photodiode or wait for a private
photodiode settle phase. The selected photodiode must already be on and already
producing valid sampler data. The command sets the photodiode internal
configurable-window duration to the calibration dwell and every subsequent
point waits that dwell after changing an attenuator.

Dark handling is separate from attenuator calibration. The calibration reads
the configured dark-subtracted photodiode configurable window; it does not
measure, update, or infer a private dark.

## Per-Point Measurement

```mermaid
sequenceDiagram
  participant Cal as Calibration
  participant Att as FVOA DACs
  participant PD as Photodiode sampler
  participant Rec as Retained records

  Cal->>Att: set swept and companion DAC voltages
  Cal->>Cal: wait dwell_ms
  PD-->>Cal: current configurable window
  Cal->>Cal: classify saturation and SNR
  Cal->>Rec: append point/probe/bridge record
  Cal-->>Cal: schedule next point or fit
```

The DAC write is followed by a dwell equal to the configured photodiode
configurable window. After the dwell, calibration reads the current
configurable window, not the last closed window. The window supplies:

- raw mean millivolts,
- dark-subtracted mean millivolts,
- raw RMS,
- propagated net-mean error,
- failed sample count,
- min/max and max raw code.

The sampler's step detection snapshots the current configurable window into the
last configurable window without resetting the current rolling window.
Calibration normally ignores the last window because it represents a prior
optical level.

## Usable-Band Model

The acquisition logic treats photodiode readings as a band:

- `saturated`: the photodiode mean is pinned at the ADC rail, so the optical
  signal is too bright;
- `ok`: the dark-subtracted mean is positive and has enough SNR;
- `below_snr`: the optical signal is too dim for a useful fitted point.

These are not interchangeable failure modes. A saturated DUT sweep sample is
retained as a diagnostic record and the sweep continues toward more DUT
attenuation. A below-SNR DUT sweep sample marks the dim edge of the current
segment and starts bridge normalization from the latest retained usable anchor.

For the companion FVOA the DAC direction must be read carefully: lower companion
DAC opens the companion and raises photodiode signal; higher companion DAC
attenuates more. Companion searches maintain a low-DAC saturated side, a
more-attenuated high-DAC side, and the lowest usable companion DAC candidate.
That candidate is the highest non-saturated photodiode signal found by the
bounded search.

## Per-Physical Acquisition

Each logical attenuator has two physical FVOAs. Calibration runs the same
sequence for `dac1` and then `dac2`.

```mermaid
flowchart TD
  StartPhysical[start physical FVOA] --> Initial[initial probe: DUT open, companion max]
  Initial --> SaturatedAtMax{still saturated at companion max}
  SaturatedAtMax -- yes --> LowerLaser{next laser level available}
  LowerLaser -- yes --> Initial
  LowerLaser -- no --> Error[calibration error]
  SaturatedAtMax -- no --> CompanionSearch[binary search companion FVOA]
  CompanionSearch --> Reference[select usable initial-probe record as open reference]
  Reference --> Sweep[linear DUT sweep from 0 to max]
  Sweep --> Band{photodiode band}
  Band -- saturated --> Bright[retain diagnostic; sweep toward more DUT attenuation]
  Bright --> MoreRange{DUT near max drive}
  Band -- ok --> Usable[retain fit candidate; update latest anchor]
  Usable --> MoreRange
  MoreRange -- yes --> FinishPhysical
  MoreRange -- no --> Sweep
  Band -- below_snr --> Bracket{dim edge bracketed}
  Bracket -- no --> FinishPhysical
  Bracket -- yes --> Bridge[bridge normalize]
  Bridge --> Sweep
  FinishPhysical --> Next{dac1 complete}
  Next -- yes --> StartDac2[start dac2]
  Next -- no --> Fit[fit both physical FVOAs]
```

The initial probe protects the photodiode by starting with the companion FVOA
at maximum attenuation. If even that clips the ADC, firmware retries with lower
laser output levels. The companion binary search then finds the most open
companion setting that is still non-saturated.

The selected open reference is a measured `initial_probe` record named by
metadata. Firmware does not append a separate reference copy and does not
repeat the measurement solely to confirm it. If the initial companion search
cannot find a usable candidate, the acquisition errors because no valid
normalization point exists for that physical FVOA.

The DUT sweep is linear and classification-aware. It starts at 0 mV and advances
by `ATTEN_CAL_SWEEP_STEP_MV` until full DAC drive. A usable point updates the
latest bridge anchor. A saturated point is too bright, so it also advances to
the next linear DUT step but is not a fit candidate and does not become a bridge
trigger. A below-SNR point marks the dim edge of the current segment. When that
dim edge appears before the DUT reaches full drive, firmware performs bridge
normalization instead of discarding the remaining dynamic range. The similarly
named `ATTEN_CAL_SEARCH_MIN_STEP_MV` is only the minimum bracket width for
companion-FVOA binary searches.

## Bridge Normalization

```mermaid
sequenceDiagram
  participant DUT as DUT FVOA
  participant Other as Companion FVOA
  participant PD as Photodiode configurable window
  participant Cal as Calibration
  participant Rec as Retained records

  Cal->>Rec: find latest usable DUT point in current segment
  Cal->>DUT: hold that DUT drive
  Cal->>Other: search lower companion DAC for lowest usable point
  PD-->>Cal: bridge_probe records
  Cal->>Rec: store before/after record indices in bridge table
  Cal->>Cal: ratio = accepted bridge_probe / retained DUT anchor
  Cal->>Cal: segment_scale *= ratio
  Cal->>Cal: add bridge variance to scale variance
  Cal->>DUT: resume DUT sweep in new segment
```

Because the DUT FVOA does not move during the bridge, the before/after
photodiode ratio measures only the change in companion transmission. The
before side is the latest usable retained `point` in the segment being closed.
The after side is the accepted retained `bridge_probe` in the new segment.
Those record indices are stored in the bridge table rather than copied into
synthetic records. Later DUT measurements are divided by the cumulative segment
scale so all segments share the open-reference normalization.

If a bridge search cannot find a usable companion point, firmware either
tightens the saturated-side search floor and keeps probing or finishes the
physical when the search demonstrates that the current bridge would add no more
useful attenuation range. A missing bridge anchor means the current segment has
no usable sweep point; after a bridge, a single immediately below-SNR point is
treated as the natural end of the physical sweep rather than an acquisition
error.

## Tick State

```mermaid
stateDiagram-v2
  [*] --> Inactive
  Inactive --> Running: atten/calibrate start
  Running --> WaitWindow: DAC pair set
  WaitWindow --> WaitWindow: dwell not elapsed
  WaitWindow --> Running: measurement handled, next DAC pair set
  WaitWindow --> Complete: both physical fits complete
  WaitWindow --> Error: sequencing or apply error
  Running --> Inactive: stop=true
  Complete --> Inactive: stop=true
  Error --> Inactive: stop=true
```

There is no separate photodiode-settle, DAC-settle, or photodiode-average
phase. The only active wait is the configured dwell for the current internal
photodiode configurable window.

## Records, Telemetry, and Fit

```mermaid
flowchart TD
  Window[photodiode configurable window] --> Measurement[classify measurement]
  Measurement --> Record[append retained record]
  Record --> Telemetry[emit best-effort telemetry]
  Record --> Derive[derive bridge scales, scaled signal, tx, dB]
  Derive --> FitEligible{classification ok and tx in fit domain}
  FitEligible -- yes --> FitInput[fit candidate]
  FitEligible -- no --> RetainedOnly[diagnostic record only]

  FitInput --> Optimize[weighted dB-space model fit]
  Optimize --> Metrics[residuals, correlation, span]
  Metrics --> Accepted{both physical fits accepted}
  Accepted -- no --> CompleteFailed[complete with fit failed]
  Accepted -- yes --> Apply[apply runtime coefficients]
  Apply --> Persist{persist requested}
  Persist -- yes --> Store[NVS settings]
  Persist -- no --> CompleteOK[complete with runtime-only coeffs]
  Store --> CompleteOK
```

Every retained dataset is available through
`atten/calibrate/records/<dac1|dac2>` followed by numbered
`atten/calibrate/records/<dac1|dac2>/<chunk>` record chunks. The first response
contains HAC4 metadata: state, fit flags, record size, records per chunk,
record count, record chunk count, selected open-reference record, and bridge
before/after record indices. Each numbered chunk contains only raw records.
Telemetry on `dt/<device>/atten` is useful for live monitoring but is not the
authoritative dataset.

Saturation classification is based on the photodiode window mean reaching the
ADC rail. Window extrema are diagnostic only, since electrical and optical noise
can produce isolated rail excursions without pinning the diode. Saturated sweep
records are retained but are not fit candidates and do not trigger bridge
normalization. Below-SNR sweep records trigger a bridge unless the DUT is
already at the end of the firmware drive range.

Retained records store raw acquisition facts only: `sweep_mv`, `other_mv`,
`laser_pct`, `signal_mv`, `signal_err_mv`, `max_mv`, `event`,
`classification`, and `segment`. The Python helper keeps those firmware names
and adds `fvoa_mv` and `other_fvoa_mv` as host-side plotting coordinates
derived from DAC millivolts and the default FVOA drive gain. Bridge scale,
scaled signal, relative transmission, dB attenuation, fit inclusion, and
residuals are computed from the raw records and accepted bridge boundaries
after acquisition.

The retained record events are:

| Event | Meaning |
| --- | --- |
| `point` | ordinary DUT sweep point |
| `initial_probe` | companion-search point before the open reference |
| `bridge_probe` | companion-search point during bridge normalization |

The open reference, bridge-before, and bridge-after roles are metadata indices
into these retained records. They are plotted as roles by host tools but are
not separate firmware event values.

The derived relative transmission for a fit point is:

```text
tx = signal_mv / (reference_signal_mv * segment_scale[segment])
```

with relative variance:

```text
(sigma_tx / tx)^2 =
    (signal_err_mv / signal_mv)^2
  + (reference_signal_err_mv / reference_signal_mv)^2
  + (sigma_segment_scale / segment_scale)^2
```

The fit minimizes uncertainty-weighted residuals in attenuator model dB output
space:

```text
measured_db = -10 * log10(tx)
max_atten_db = mean(measured_db for final three fit points)
floor_tx = 10^(-max_atten_db / 10)
model_tx = floor_tx + (1 - floor_tx) * ideal_model_tx
residual = model_db(dac_mv, fvoa_50pct_mv, slope_inv_fvoa_mv,
                    max_atten_db, gain)
         - measured_db
```

`max_atten_db` is the physical FVOA leakage floor, not the sum available from a
logical two-FVOA attenuator. Firmware estimates it from the final three usable
fit points, propagates that uncertainty into the weighted dB residuals, and
then optimizes only `fvoa_50pct_mv` and `slope_inv_fvoa_mv`.

After the base fit, firmware fits an optional four-term Chebyshev correction to
the remaining dB residuals:

```text
start_db = -10 * log10(0.99)
t = clamp((base_db - start_db) / (max_atten_db - start_db), 0, 1)
x = 2 * t - 1
correction_db = t * (1 - t) * sum(c_i * T_i(x))
model_db = base_db + correction_db
```

This correction is intentionally ringfenced from the three physical
coefficients. It is zero near open transmission and at the modeled leakage
floor, and it is accepted only if the corrected model remains monotonic on the
actual sweep-point grid. If the residual solve is ill-conditioned or fails that
monotonicity check, firmware leaves `correction_coeff` as all zeros and keeps
the base fit.

## Notebook Inspection

`tools/attenuator_calibration_lab.ipynb` is the lab-side inspection script for
this flow. It has two intentionally separate paths:

- the embedded path runs `atten_calibrate_auto`, retrieves
  `atten_calibration_data`, and plots retained records, bridge events,
  residuals, and the coefficient-derived 2D attenuation surface;
- the manual exploration path directly calls `atten()`, sleeps for the
  configured dwell, and reads `pd()` so SNR, saturation, bridge, and fit
  thresholds can be changed quickly.

The manual path supports both the firmware-style weighted fit and a SciPy
least-squares exploratory fit. Its plots show propagated photodiode and
normalization uncertainty; coefficients should be reviewed before any
`set_atten_coeff(..., persist=True)` command is used.

An accepted coefficient object contains:

```json
{
  "fvoa_50pct_mv": 3144.95,
  "slope_inv_fvoa_mv": 0.00303104,
  "max_atten_db": 48.36,
  "gain": 1.533,
  "correction_coeff": [0.12, -0.03, 0.01, 0.0]
}
```
