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
  Sweep --> PointOK{point usable}
  PointOK -- yes --> MoreRange{DUT near max drive}
  MoreRange -- yes --> FinishPhysical
  MoreRange -- no --> Sweep
  PointOK -- no --> Bracket{transition bracketed}
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

The DUT sweep is binary. A usable point extends the low side of the sweep; an
unusable point narrows the high side. When the sweep brackets the useful region
before the DUT reaches full drive, firmware performs bridge normalization
instead of discarding the remaining dynamic range.

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
  Cal->>Other: binary-open companion until non-saturated
  PD-->>Cal: bridge_probe records
  PD-->>Cal: bridge_after signal
  Cal->>Cal: ratio = after / before
  Cal->>Cal: segment_scale *= ratio
  Cal->>Cal: add bridge variance to scale variance
  Cal->>DUT: resume DUT sweep in new segment
```

Because the DUT FVOA does not move during the bridge, the before/after
photodiode ratio measures only the change in companion transmission. Later DUT
measurements are divided by the cumulative segment scale so all segments share
the open-reference normalization.

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
  Record --> FitEligible{usable and tx in fit domain}
  FitEligible -- yes --> FitInput[fit candidate]
  FitEligible -- no --> RetainedOnly[diagnostic record only]

  FitInput --> Convert[tx to erf-delta]
  Convert --> WLS[weighted linear fit]
  WLS --> Metrics[residuals, correlation, span]
  Metrics --> Accepted{both physical fits accepted}
  Accepted -- no --> CompleteFailed[complete with fit failed]
  Accepted -- yes --> Apply[apply runtime coefficients]
  Apply --> Persist{persist requested}
  Persist -- yes --> Store[NVS settings]
  Persist -- no --> CompleteOK[complete with runtime-only coeffs]
  Store --> CompleteOK
```

Every retained record is available through
`atten/calibrate/records/<dac1|dac2>[/<start>]` as a bounded binary HAC3 chunk.
Telemetry on `dt/<device>/atten` is useful for live monitoring but is not the
authoritative dataset.

The retained record events are:

| Event | Meaning |
| --- | --- |
| `point` | open reference or ordinary DUT sweep point |
| `initial_probe` | companion-search point before the open reference |
| `bridge_before` | low-SNR edge before opening the companion |
| `bridge_probe` | companion binary-search point during bridge |
| `bridge_after` | accepted post-bridge normalization point |

The fit converts normalized transmission to the attenuator model coordinate:

```text
delta = erfinv(erf(4) - 2 * erf(4) * tx)
```

and fits:

```text
delta = slope_inv_fvoa_mv * (gain * dac_mv - fvoa_50pct_mv)
```

An accepted coefficient object contains:

```json
{
  "fvoa_50pct_mv": 3144.95,
  "slope_inv_fvoa_mv": 0.00303104,
  "gain": 1.533
}
```
