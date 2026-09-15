# Maiman and Laser Driver Interface

`laser_estimate_flux(id, out)` uses the laser module's existing cached settings
and current/TEC state. Optical-power uncertainty is
`hypot(power_mw * fractional_noise, constant_noise_mw)`; this operation performs
no Modbus I/O. Its nonblocking state read returns `-EBUSY` during another
laser operation and `-EIO` for an invalid owner estimate. Nonfinite calibration
values are rejected. See [settings](../settings.md) for compiled-table defaults.

```{eval-rst}
.. doxygenfile:: app/src/maiman.h
   :project: hispec_tib

.. doxygenfile:: app/src/lasers.h
   :project: hispec_tib

```

## Emission updates and failures

A laser that is already emitting with valid preparation accepts a current change
with one Modbus write. Increasing and decreasing current use the same path.
Startup still checks identity, applies the runtime profile and controls, starts
TEC operation if needed, and enables emission. Tuned level changes reuse this
qualification, updating the TEC setpoint only when it changes.

At 115200 baud, the former ordinary path's six reads and sixteen writes require
about 34.2 ms of wire time plus 22.0 ms of configured RTU receive framing. One
current write requires about 1.65 + 1.00 = 2.65 ms. These are transport estimates,
not measured end-to-end latency: controller turnaround, thread scheduling, I/O
contention, and optical response are additional. A cold bank also has its boot
wait. No settling delay or polling of optical power is added.

The laser owner keeps preparation and estimate validity. Configuration/power
changes revoke preparation; detected I/O failures or controller faults invalidate
the estimate. Maiman operations remember a failure across subsequent successful
register reads, including positive Modbus exception responses. A later successful
temperature poll does not requalify a failed current operation. Successful current
control establishes the estimate again. The runtime emission flag remains set
after an unsuccessful stop because physical emission is uncertain; a confirmed
stop or bank GPIO off establishes zero output. No automatic retry is performed.
