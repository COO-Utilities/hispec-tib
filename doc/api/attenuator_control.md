# Attenuator Control

`attenuator_estimate_transmission(drv, out)` snapshots confirmed DAC state and
combines curve accuracy (each model's stored `rms_db`) with electrical variation.
`ATTENUATOR_FVOA_NOISE_RMS_MV` defaults to 10 mV RMS at each FVOA control line,
after the amplifier; zero disables the electrical contribution. For each device,
`electrical_sigma_db = abs(d_db_d_voltage_mv) * noise_rms_mv / gain`, using the
full calibrated model's local derivative. Model and electrical contributions
combine independently in quadrature, as do the two series devices. Linear
transmission uncertainty is `T * ln(10)/10 * total_sigma_db`.

The combined error is uncertainty in transmission, not temporal RMS variation.
Calibration errors remain correlated across records; electrical independence
between devices does not imply temporal independence. The calculation assumes
the FVOA follows the voltage fluctuations and applies no bandwidth or averaging
correction. It changes no hardware or persistence state and performs no I/O.
Accepted automatic fits install their final residual RMS with the coefficients;
see [calibration flow](../attenuator_calibration.md).

## Scope noise in Python

Query the coefficient pair once, then supply separate post-amplifier scope
means and RMS readings in millivolts, ordered DAC1 and DAC2:

```python
noise = hspcb.attenuator_noise(
    pcb.atten_coeff("1028y"),
    mean_fvoa_mv=(3824.0, 3942.0),
    noise_rms_mv=(7.2, 9.3),
)
display(pd.DataFrame.from_records(noise))  # pandas, if desired
```

The helper itself is a pure calculation returning a NumPy record array with
`component` rows `dac1`, `dac2`, and `pair`. `db` and `tx` are nominal model
values. `model_sigma_db` / `model_sigma_tx` describe curve accuracy;
`electrical_rms_db` / `electrical_rms_tx` describe voltage-induced variation;
`total_sigma_db` / `total_sigma_tx` combine them. Transmission errors are absolute
linear fractions; `electrical_rms_pct` expresses electrical RMS relative to each
row's nominal transmission. Series transmissions multiply, while independent
dB-error variances add. The Python derivative uses a 0.1 FVOA-side mV step,
including the same leakage floor and empirical correction as firmware.

```{eval-rst}
.. doxygenfile:: app/src/attenuator.h
   :project: hispec_tib
```

## Total attenuation allocation

Total commands move both devices only in the requested direction. Adding dB
uses the less-attenuated device until balanced, then shares the remaining change;
removing dB starts with the more-attenuated device. Each device's modeled maximum
bounds the allocation. For example, `(36, 0)` to 41 dB gives `(36, 5)`; `(39, 35)`
to 40 dB gives `(20, 20)`. An unchanged total makes no allocation change.

This avoids forcing DAC1 to its high-voltage plateau as soon as a total crosses
its fitted maximum: that region has the weakest model accuracy in the lab
captures. Physical voltage/dB commands used by calibration retain explicit
control. Allocation uses the DAC owner's last confirmed voltages and active
coefficients, including during coefficient replacement. Partial writes report
failure and retain successful hardware changes; they are not retried or rolled
back automatically.
