# Calibration C contracts

Acquisition, numerical availability, physical acceptance, RAM application and
persistence are separate outcomes. See [attenuator calibration](../attenuator_calibration.md),
[laser calibration](../laser_calibration.md) and [solver mathematics](../calibration_fitting.md).
The storage header is internal shared ownership, not a public wire format.

```{eval-rst}
.. doxygenfile:: app/src/attenuator_calibration.h
   :project: hispec_tib

.. doxygenfile:: app/src/laser_calibration.h
   :project: hispec_tib

.. doxygenfile:: app/src/calibration_fit.h
   :project: hispec_tib

.. doxygenfile:: app/src/calibration_storage.h
   :project: hispec_tib
   :sections: define enum

.. doxygenstruct:: atten_cal_record
   :members:
.. doxygenstruct:: atten_cal_bridge
   :members:
.. doxygenstruct:: atten_cal_fit_point
   :members:
.. doxygenstruct:: laser_cal_record
   :members:
.. doxygenstruct:: laser_cal_metadata
   :members:
```

## Shared storage ownership

Attenuator and laser calibration share the following unions because only one
calibration may own acquisition or fitting at a time. The shared lock protects
reset and record copies; the fitter retains immutable data until cancellation
joins it. These are in-memory layouts; binary telemetry explicitly encodes
fields without transmitting C padding.

```{literalinclude} ../../app/src/calibration_storage.h
:language: c
:start-at: union calibration_data {
:end-at: extern atomic_t calibration_data_kind;
```
