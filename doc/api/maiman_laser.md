# Maiman and Laser Driver Interface

`laser_estimate_flux(id, out)` uses the laser module's existing cached settings
and current/TEC state. Optical-power uncertainty is
`hypot(power_mw * fractional_noise, constant_noise_mw)`; this operation performs
no Modbus I/O. It waits only for the short state-copy mutex, returns `-EINVAL`
for invalid/uninitialized use, and never reports operational `-EBUSY`/`-EIO`.
Throughput and calibration use `hispec_laser_output_status()` separately to
check confirmed emission, control/controller faults, and communication timeout.
See [settings](../settings.md) for compiled-table defaults.

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

The laser owner keeps preparation and confirmed setpoints separately from
operational communication health. Failed control operations or confirmed
controller faults revoke preparation. Diagnostic failures warn and invalidate
only that observation; five seconds without a response while emitting faults
operation. Successful communication restores availability, but measurements
remain stopped and a control fault requires a successful control operation.
Failed shutdown preserves its emission/shutdown obligation. See
[communication flow](../photodiode_notes.md#communication-and-power-lifetime).
