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
  RecordsReq[atten/calibrate/records query] --> Chunk[attenuator_calibration_write_data_chunk]

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
  CompanionSearch --> Reference[record open reference]
  Reference --> ReferenceOK{reference usable}
  ReferenceOK -- no --> Error
  ReferenceOK -- yes --> Sweep[binary sweep DUT toward attenuation]
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

The DUT sweep is binary and classification-aware. A usable point updates the latest
bridge anchor and extends the lower side of the useful sweep. A saturated point
is too bright, so it also advances the search toward more DUT attenuation but is
not a fit candidate and does not become a bridge trigger. A below-SNR point narrows
the dim side of the sweep. When that dim edge is bracketed before the DUT
reaches full drive, firmware performs bridge normalization instead of discarding
the remaining dynamic range.

## Bridge Normalization

```mermaid
sequenceDiagram
  participant DUT as DUT FVOA
  participant Other as Companion FVOA
  participant PD as Photodiode configurable window
  participant Cal as Calibration

  Cal->>DUT: hold last usable DUT drive
  Cal->>Other: keep current companion drive
  PD-->>Cal: bridge_before signal
  Cal->>Other: search lower companion DAC for lowest usable point
  PD-->>Cal: bridge_probe records
  Cal->>Other: confirm usable companion candidate
  PD-->>Cal: bridge_after signal if accepted
  Cal->>Cal: ratio = after / before
  Cal->>Cal: segment_scale *= ratio
  Cal->>Cal: add bridge variance to scale variance
  Cal->>DUT: resume DUT sweep in new segment
```

Because the DUT FVOA does not move during the bridge, the before/after
photodiode ratio measures only the change in companion transmission. Later DUT
measurements are divided by the cumulative segment scale so all segments share
the open-reference normalization.

If a bridge confirmation is saturated or below-SNR, that result is folded back
into the companion bracket and retained as a `bridge_probe`; it is not by itself
evidence that the swept-DUT anchor is bad. Swept-DUT backoff is reserved for
cases where `bridge_before` is unusable, the companion search cannot find a
useful candidate with real headroom, or bridge ratio validation remains
impossible after bounded probing.

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

Every retained record is available through
`atten/calibrate/records/<dac1|dac2>[/<start>]` as a bounded binary HAC4 chunk.
Telemetry on `dt/<device>/atten` is useful for live monitoring but is not the
authoritative dataset.

Saturation classification is based on the photodiode window mean reaching the
ADC rail. Window extrema are diagnostic only, since electrical and optical noise
can produce isolated rail excursions without pinning the diode. If a bridge
cannot be completed from the held DUT anchor after bounded companion probing,
the acquisition backs off to an earlier usable sweep point in the current
segment and overwrites the marginal retained records. The backoff is bounded by
the firmware constants for ADC-range fraction and by at most half of the current
segment's usable sweep points; each backoff emits live telemetry/logging so the
operator can see the discarded boundary attempts.

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
| `reference` | open reference that normalizes relative transmission |
| `bridge_before` | boundary repeat before opening the companion |
| `bridge_probe` | companion search point or failed bridge confirmation |
| `bridge_after` | accepted post-bridge normalization point |

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
residual = model_db(dac_mv, fvoa_50pct_mv, slope_inv_fvoa_mv, gain)
         - measured_db
```

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
  "gain": 1.533
}
```
