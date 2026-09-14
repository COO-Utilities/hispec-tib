# Maiman and Laser Driver Interface

`laser_estimate_flux(id, out)` uses the laser module's existing cached settings
and current/TEC state. Optical-power uncertainty is
`hypot(power_mw * fractional_noise, constant_noise_mw)`; this operation performs
no Modbus I/O. See [settings](../settings.md) for compiled-table defaults.

```{eval-rst}
.. doxygenfile:: app/src/maiman.h
   :project: hispec_tib

.. doxygenfile:: app/src/lasers.h
   :project: hispec_tib

```
