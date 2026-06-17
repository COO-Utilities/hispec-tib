# Attenuator Calibration Lab Notes

This note describes the physical behavior of the HiSPEC-FIB FVOA attenuator pairs. It is not a description of a firmware implementation; rather, it preserves lab reasoning, discoveries, and the broader conceptual task. This should help guide data gathering, reasoning about constraints, and understanding of the physical system that informs firmware, AIT, and broader HISPEC maintenance.

## Physical Setup

Each logical attenuator, six total on the TIB PCB, is composed of two physical FVOAs in series. On the TIB PCB, each logical attenuator sits between a laser diode driver, with a lab-installed quantity of static attenuation, and the TIB fiber switchyard, where each route through the switchyard has some additional static attenuation of its own.

The output of the switchyard is looped back to the TIB photodiode via HISPEC's FEI and the fibers to and from it. The losses of this path are variable, span many decades, and their relative measurement through time and FEI configuration state is a principal responsibility of the TIB PCB, whether through low-level manual queries or use of the `measure_throughput` command. On return, the path is routed to a photodiode that registers the optical signal but lacks the range to measure the full dynamic range of the system on its own.

On the CAL PCB variant, there is one logical attenuator used to adjust the laser frequency comb source brightness. That light can be routed anywhere the CAL PCB's output light can go, including the TIB photodiode. While measurement of its attenuation can follow the procedure outlined here, the required cross-board coordination would be handled by and implemented in a higher-level control program in the HISPEC ICS.

The physical attenuators operate by means of a resistive MEMS shutter, where increasing drive voltage pushes the shutter between two SM fiber end faces.

The FVOA datasheet curve is representative of the behavior, but lab testing shows significant variation in turn-on voltage from one FVOA to another. It does, however, validate the model shape adopted here: a linear shutter progressively occluding the mode field.

The digitized nominal FVOA points are:

```text
fvoa_drive_v: 0.00, 1.89, 2.23, 2.32, 2.41, 2.47, 2.55, 2.60, 2.64,
              2.69, 2.74, 2.79, 2.84, 2.89, 2.94, 2.99, 3.04, 3.12, 3.18, 5.0

fvoa_db:      ~0.0,  1.0,  5.0,  8.0, 11.0, 14.0, 17.5, 22.0, 26.0,
             30.0, 33.0, 37.0, 41.0, 44.0, 48.5, 51.0, 55.5, 58.0, 59.0, >65
```

Because of the need to probe the knee well, and because of manufacturing variation between FVOAs, a rigid calibration schedule based on the nominal datasheet curve is unsuitable for blind calibration.

Each FVOA is driven from approximately 0-5 V by an op-amp with a gain of about 1.51x. The op-amp is driven by a precision 12-bit DAC7678 with an output range of 0 to approximately 3.3 V and roughly 4096 codes.

The photodiodes in the system output an approximately 0-10 V signal that is mapped onto a portion of the ADC range: 0-5 V in PCB Rev. 1 lab testing, and 0-2 V in PCB Rev. 2, which has not yet been fabricated. The photodiode is monitored steadily by its own portion of the codebase, which corrects for and maintains a record of the dark level by turning off all light paths to the photodiode and taking a reading. It also maintains the RMS noise of the samples.

Photodiode saturation occurs somewhat above 10 V; lab testing shows saturation around 10.3-11.4 V. At these levels, the ADC readings no longer meaningfully represent the diode voltage other than as a strict lower bound on detected flux. This means values above 5 V in Rev. 1, and above 2 V in Rev. 2, must be treated statistically in fits as lower bounds on the detected flux or on the photodiode millivolt reading that would have been expected in the absence of saturation.

The attenuator is commanded by setting either model linear transmission, model dB attenuation, or DAC output millivolts.

The FVOA sees the drive voltage after the op-amp gain stage, nominally:

```text
fvoa_drive_gain ~= 5.0 / 3.3

fvoa_drive_mv =
    dac_mv * fvoa_drive_gain
```

## FVOA Transmission Model

The model for the FVOA transmission is:

```text
MODEL_ERF_SCALE = 4

erf_scale =
    ERF(MODEL_ERF_SCALE)

fvoa_drive_mv =
    fvoa_drive_gain * dac_mv

erf_delta =
    slope_inv_fvoa_mv
    * (fvoa_drive_mv - fvoa_50pct_mv)

fvoa_tx =
    (erf_scale - ERF(erf_delta))
    / (2.0 * erf_scale)
```

where `MODEL_ERF_SCALE` is a fixed normalization scale, adopted here as 4. The fitted parameter `fvoa_50pct_mv` is the physical FVOA drive voltage, after the op-amp gain stage, corresponding to 50% transmission. With this convention, `erf_delta = 0` at the half-shutter position and therefore `fvoa_tx = 0.5`.

The parameter `slope_inv_fvoa_mv` sets the steepness of the transition in inverse FVOA-drive millivolts. With positive slope, increasing drive voltage increases attenuation and decreases transmission.

## Dynamic Range

The photodiode plus ADC can directly observe only a small fraction of the full system range. The FVOA pair, adjustable laser level, static route losses, and the photodiode measurement path together cover the useful range.

```text
measured_pd_signal_mv =
    min(
        flux_to_mv_gain
        * laser_flux
        * fvoa1_tx
        * fvoa2_tx
        * transmission_loss,
        pd_saturation_mv
    )
    - dark_mv
```

where `pd_saturation_mv` depends on the diode, ADC, temperature, and specific device. It is somewhere slightly above the nominal ADC saturation limit.

As a whole, a single sweep cannot observe both the open plateau and the deep attenuation floor of one FVOA on an absolute scale. The calibration therefore uses overlapping sweep segments, with the companion FVOA adjusted between segments to keep the photodiode within its useful, non-saturated measurement range.

## Calibration Procedure

To calibrate an FVOA, first choose the route, laser, and photodiode to be used for the measurement. The laser is initially set to full output. The FVOA under test, hereafter the DUT FVOA, is set fully open at 0 V. The companion FVOA is initially set to 5 V and then opened using a decreasing binary interval search until the photodiode reaches the highest non-saturated signal level.

The search cannot converge more finely than the DAC quantization step, but this is sufficient for establishing the initial segment normalization. The search terminates when either:

* the photodiode signal is below and within the noise of the highest allowed non-saturated level, or
* the search has exhausted the usable DAC range.

If the photodiode is saturated at the outset, the laser output is decreased first to 50% and then, if necessary, to 5%. The optical configuration should always allow a usable starting condition to be found; failure to do so is treated as a terminal calibration error.

After each FVOA or laser adjustment, the procedure waits for the photodiode measurement to settle. The photodiode signal chain is filtered to about 20 Hz, so a 10 Hz command/update cadence is sufficient.

The DUT FVOA is then swept toward higher attenuation while the companion FVOA is held fixed. The sweep continues until the photodiode signal approaches the lowest statistically useful level, not until the signal is fully lost. At that bridge point, the DUT FVOA is held fixed and the companion FVOA is opened, using the same decreasing binary interval search, until the photodiode again reaches a high, non-saturated signal level.

Because the DUT transmission has not changed during this bridge operation, the ratio of the new and old photodiode readings directly measures the change in companion-FVOA transmission. Subsequent measurements are divided by the cumulative companion-FVOA scale factor so that all sweep segments lie on a common relative-transmission scale.

This process repeats until the DUT sweep reaches 5 V. For the expected HISPEC-FIB dynamic range, this should require at most three segments and will usually require only one or two.

The resulting points share a common normalization, have explicitly propagated uncertainties, and can be converted to dB attenuation for a weighted fit in dB space.

## Normalization Math

Let the dark-subtracted, non-saturated photodiode measurement be:

```text
pd_signal_mv =
    pd_mv - dark_mv
```

For sweep segment `k`, with the companion FVOA fixed:

```text
pd_signal_mv =
    measurement_scale
    * fvoa_dut_tx
    * fvoa_companion_tx[k]
```

where `measurement_scale` includes laser flux, fixed route throughput, photodiode responsivity, ADC scaling, and any other factors that are constant during the segment.

The first segment is normalized using the DUT fully open:

```text
relative_fvoa_tx =
    pd_signal_mv / pd_signal_open_mv
```

At each bridge point, the DUT is held fixed while the companion FVOA is opened. The bridge ratio is:

```text
bridge_ratio[k] =
    pd_signal_new_mv / pd_signal_old_mv
```

Because the DUT transmission is unchanged during the bridge operation:

```text
bridge_ratio[k] =
    fvoa_companion_tx[k + 1]
    / fvoa_companion_tx[k]
```

Define the cumulative segment scale factor:

```text
segment_scale[0] = 1

segment_scale[k + 1] =
    segment_scale[k] * bridge_ratio[k]
```

Any measurement from segment `k` is then normalized as:

```text
relative_fvoa_tx =
    pd_signal_mv
    / (pd_signal_open_mv * segment_scale[k])
```

Uncertainty propagation for the bridge ratio is:

```text
sigma_bridge_ratio / bridge_ratio =
    sqrt(
        (sigma_pd_signal_new_mv / pd_signal_new_mv)^2
      + (sigma_pd_signal_old_mv / pd_signal_old_mv)^2
    )
```

The cumulative segment-scale uncertainty is:

```text
(sigma_segment_scale[k] / segment_scale[k])^2 =
    sum_j<k (
        sigma_bridge_ratio[j]
        / bridge_ratio[j]
    )^2
```

The uncertainty in a normalized transmission point is:

```text
sigma_relative_fvoa_tx / relative_fvoa_tx =
    sqrt(
        (sigma_pd_signal_mv / pd_signal_mv)^2
      + (sigma_pd_signal_open_mv / pd_signal_open_mv)^2
      + (sigma_segment_scale[k] / segment_scale[k])^2
    )
```

The corresponding attenuation in dB is:

```text
atten_db =
    -10 * log10(relative_fvoa_tx)
```

with uncertainty:

```text
sigma_atten_db =
    (10 / ln(10))
    * (sigma_relative_fvoa_tx / relative_fvoa_tx)
```

The normalization procedure is designed to avoid saturated photodiode measurements. During each bridge operation, the companion FVOA is adjusted only until the photodiode reaches a high, non-saturated signal level. Measurements above the calibrated linear detector range are excluded from ordinary least-squares fitting or handled explicitly as censored bounds.

## Fitting

The normalized transmission points can be converted to dB attenuation and fit with weighted residuals in dB space.

The fitted model parameters for each physical FVOA are:

```text
fvoa_50pct_mv
slope_inv_fvoa_mv
```

where `fvoa_50pct_mv` captures the FVOA-to-FVOA variation in turn-on voltage and `slope_inv_fvoa_mv` captures the steepness of the shutter transition.

The procedure is then repeated for the second FVOA in the logical attenuator pair.

## Lab Notes

For calibration, every photodiode average should use the photodiode thread's configured dark-subtracted result. If the configured dark is invalid, the calibration should fail plainly. A user-provided dark is acceptable. The calibration routine should not invent a local dark from its own data.

Presently, `pd_0p5s_rms_mv` is the reliable RMS error in current telemetry. It may not always be the final preferred error estimate. In current lab data, physically representative uncertainties are closer to:

```text
low signal region:
    about +/-5 mV near and below 100 mV,
    with usable readings above about 10 mV

upper signal region:
    about +/-200 mV in the multi-volt PCB region;
    anything above 5 V should be considered saturated
```
