# Attenuator C contracts

See [the model and inverse](../attenuator.md) for the equations, units,
allocation and uncertainty, and [calibration](../attenuator_calibration.md) for
acquisition and acceptance.

```{eval-rst}
.. doxygenfile:: app/src/attenuator.h
   :project: hispec_tib
   :sections: briefdescription detaileddescription define enum typedef func var

.. doxygenstruct:: attenuator_model_coeffs
   :members:
.. doxygenstruct:: attenuator_dac_cfg
   :members:
.. doxygenstruct:: attenuator_status
   :members:
.. doxygenstruct:: attenuator_transmission_estimate
   :members:
.. doxygenstruct:: attenuator
   :members:
.. doxygenstruct:: attenuator_target
   :members:
```

## Forward-model result

`atten_model_eval()` fills the following double-precision values and derivatives.
Drive derivatives use DAC-output mV; coefficient derivatives use their named units.

```{literalinclude} ../../app/src/attenuator.h
:language: c
:start-at: struct atten_model_eval {
:end-at: };
```
