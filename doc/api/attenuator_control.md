# Attenuator Control

`attenuator_estimate_transmission(drv, out)` reads the DAC state and uses each
physical model's stored `rms_db`. The pair's dB uncertainty is
`hypot(rms1, rms2)`, and linear transmission uncertainty is
`T * ln(10)/10 * sigma_db`. It changes no hardware or persistence state.
Accepted automatic fits install their final residual RMS with the coefficients;
see [calibration flow](../attenuator_calibration.md).

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
