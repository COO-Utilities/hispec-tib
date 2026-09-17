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

A started laser accepts current changes, including zero, with one Modbus write.
Zero current pauses emission-time accounting but preserves started state, tuning,
and the existing auto-off deadline. Explicit `laser stop=true`, auto-off, and
measurement-owned expiry use zero-current plus STOP and their existing TEC policy.

Identity is verified once per driver per bank-power interval. Configuration is
applied once and retained across STOP and communication errors; explicit settings
changes/reset and bank power cycling invalidate affected configuration. Operational
faults remain separate from known identity/configuration. An unsuccessful current
or STOP write is still reported; acknowledged zero current is retained even if STOP
fails. A confirmed repeated stop avoids redundant writes.

At 115200 baud, the former ordinary path's six reads and sixteen writes require
about 34.2 ms of wire time plus 22.0 ms of configured RTU receive framing. One
current write requires about 1.65 + 1.00 = 2.65 ms. These are transport estimates,
not measured end-to-end latency: controller turnaround, thread scheduling, I/O
contention, and optical response are additional. A cold bank also has its boot
wait. Ordinary current changes add no settling delay or optical polling. LD START/STOP and
explicit EEPROM SAVE/RESET wait 350 ms after the transaction attempt, including
a failed acknowledgement, before allowing another transaction. This implements the
approximately 300 ms busy interval described on page 22 of the repository SF8025
manual, with 50 ms margin. The response timeout remains 75 ms.

The laser owner keeps preparation and confirmed setpoints separately from
operational communication health. Failed control operations or confirmed
controller faults invalidate operational readiness without erasing confirmed configuration. Diagnostic failures warn and invalidate
only that observation; five seconds without a response while started (including
zero current) faults operation. Successful communication restores availability, but measurements
remain stopped and a control fault requires a successful control operation.
Failed shutdown preserves its emission/shutdown obligation. See
[communication flow](../photodiode_notes.md#communication-and-power-lifetime).

## Temporary bench timing trace

Normal firmware uses Maiman and laser INFO levels, Modbus WARNING, and a shared
2048-byte deferred-log buffer. Transaction/quiet and preparation details use DEBUG;
compact receive probes use Modbus INFO. These are standard module log levels, with
no per-operation verbosity argument or separate trace switch. Communication-health
timestamps and busy waits are maintained even when logging is off.

For a diagnostic capture, build separately with these Kconfig overrides:

```sh
./.venv/bin/west build --board=nucleo_h563zi/stm32h563xx \
  --build-dir ./hispec-tib/app/build-maiman-log ./hispec-tib/app -- \
  -DCONFIG_MAIMAN_LOG_LEVEL_DBG=y -DCONFIG_LASERS_LOG_LEVEL_DBG=y \
  -DCONFIG_MODBUS_LOG_LEVEL_INF=y
```

Keep the workspace Zephyr checkout with the application: the receive probes are
in `subsys/modbus/modbus_serial.c` and `modbus_core.c`. Modbus DEBUG also enables
existing packet dumps, so use INFO for compact timing captures. The separate
`debug.conf` profiling fragment overrides the buffer to 16 KiB and is not needed
for this capture. The 2 KiB buffer is allocated once across modules; its increase
from 1 KiB adds 1 KiB of RAM but does not guarantee lossless diagnostic bursts.

Capture the serial console continuously from before the first command through at
least one second after the final response. Use firmware monotonic timestamps for
analysis; terminal wall-clock timestamps include buffering. Also retain any dropped
log-message warnings: missing records cannot establish that no frame arrived.

| Record | Interpretation |
|---|---|
| `MB seq=... node=... op=... reg=... value=... start_ms=... elapsed_ms=... gap_ms=... rc=...` | One read/write, including failures. Elapsed time ends when the Modbus API returns, before the quiet wait. Gap is since the preceding API completion; first gap is -1. |
| `MB quiet ... start_ms=... wait_ms=350` / `release_ms=...` | Busy operation and enforced release time. The next request must start at or after release. |
| `MB frame boundary_ms=... bytes=...` | RTU framing timer completed and queued RX processing. Includes framing delay; not the exact last-byte arrival time. |
| `MB rx processed_ms=... rc=... node=... fc=... len=... write_reg=...` | RX worker processed a frame. Failed frames omit decoded identity; read responses have no register address, reported as -1. |
| `Laser ... prepare configuration_needed=...` / `applying configuration` | Separates ordinary restart from actual configuration writes. |

For a timeout, compare `start_ms + elapsed_ms` with frame-boundary and processing
records. A boundary before timeout but processing afterward points to RX scheduling;
a boundary after timeout shows late frame completion. No boundary means no completed
frame was observed, provided logging was not dropped. RTU has no transaction ID:
associate responses using ordering, node/function and echoed write register, and do
not assign ambiguous late frames to a request with certainty. Transaction `seq` is
host-side firmware bookkeeping, not an on-wire identifier.

Successful response timestamps remain the actual API completion time, never the end
of the quiet interval. Neither elapsed time nor the guard turns a timeout into a
successful acknowledgement. Use this capture to decide whether 75 ms needs revision.
