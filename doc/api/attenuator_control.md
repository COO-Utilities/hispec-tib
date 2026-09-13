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
