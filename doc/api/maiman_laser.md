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
Current, percent, power, and tuning paths all round to the 0.1 mA register grid
within their current bounds. Cached estimates and tune results use the applied
value. Healthy repeated register values skip the write without refreshing
communication health; an explicit auto-off request still rearms its deadline.
Startup/recovery and TEC-only tuning changes still perform the required I/O.
Requests that round to zero have the same accounting as explicit zero.
Zero current pauses emission-time accounting but preserves started state, tuning,
and the existing auto-off deadline. Explicit `laser stop=true`, auto-off, and
measurement-owned expiry use zero-current plus STOP and their existing TEC policy.

Identity is verified once per driver per bank-power interval. Configuration is
applied once and retained across STOP and communication errors; explicit settings
changes/reset and bank power cycling invalidate affected configuration. Operational
faults remain separate from known identity/configuration. An unsuccessful current
or STOP write is still reported; acknowledged zero current is retained even if STOP
fails. A confirmed repeated stop avoids redundant writes.

`hispec_laser_set_output_percent_autooff(..., apply_tune)` makes stored tuning
explicit: manual levels and calibration apply it; throughput skips it and keeps
the live TEC target. Preparation still installs the configured default target.
Profile programming uses writable TEC bounds 0x0072 (minimum) and 0x0071
(maximum), signed values scaled by 100. It expands the existing envelope before
moving the default target and narrowing bounds. Absolute limits are read-only.
A programming failure leaves preparation false and prevents a subsequent START
until preparation succeeds. Range-only settings changes stop emission and defer
programming until next preparation; combined driver-backed edits program now.

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

Relay and temperature 1-Wire transfers use separate UART peripherals with
interrupts enabled. Maiman does not acquire their bus locks; its owner retains
the existing I/O serialization through the response and 350 ms busy interval.
The remaining Zephyr patch freezes completed client frames before parsing and
drains old parser work before reusing the receive buffer, including after a
timeout; [build integration](../../zephyr/README.md) checks it automatically.
Late on-wire replies still have no transaction ID; cleanup removes stale
software work, not that protocol limitation.

The laser owner keeps preparation and confirmed setpoints separately from
operational communication health. Failed control operations or confirmed
controller faults invalidate operational readiness without erasing confirmed configuration. Diagnostic failures warn and invalidate
only that observation; five seconds without a response while started (including
zero current) faults operation. Successful communication restores availability, but measurements
remain stopped and a control fault requires a successful control operation.
When both the cached command state and observed controller state are stopped,
the physical-interlock bit alone does not invalidate the acknowledged zero/STOP
state. Status still reports the raw interlock, readiness and blocked reason.
Identity mismatches, hard controller faults, and unexpected loss of operation or
TEC while started retain their fault behavior. A diagnostic read never clears
an existing control fault.
Failed shutdown preserves its emission/shutdown obligation. See
[communication flow](../photodiode_notes.md#communication-and-power-lifetime).

## Bench transaction timing diagnostics

Normal firmware uses Maiman and laser INFO levels, Modbus WARNING, and a shared
2048-byte deferred-log buffer. Transaction/quiet and preparation details use DEBUG.
These application logs work with unmodified Zephyr and use standard module log
levels, with no per-operation verbosity argument or separate trace switch.
Communication-health timestamps and busy waits are maintained even when logging is off.

For a diagnostic capture, build separately with these Kconfig overrides:

```sh
./.venv/bin/west build --board=nucleo_h563zi/stm32h563xx \
  --build-dir ./hispec-tib/app/build-maiman-log ./hispec-tib/app -- \
  -DCONFIG_MAIMAN_LOG_LEVEL_DBG=y -DCONFIG_LASERS_LOG_LEVEL_DBG=y
```

Optionally add `-DCONFIG_MODBUS_LOG_LEVEL_DBG=y` for Zephyr's stock Modbus debug
messages and packet dumps. This increases log volume and does not provide separate
frame-arrival or RX-worker timing probes. The separate `debug.conf` profiling
fragment overrides the buffer to 16 KiB and is not needed for this capture. The
2 KiB buffer is allocated once across modules; its increase from 1 KiB adds 1 KiB
of RAM but does not guarantee lossless diagnostic bursts.

Capture the serial console continuously from before the first command through at
least one second after the final response. Use firmware monotonic timestamps for
analysis; terminal wall-clock timestamps include buffering. Also retain any dropped
log-message warnings: missing records leave gaps in the transaction history.

| Record | Interpretation |
|---|---|
| `MB seq=... node=... op=... reg=... value=... start_ms=... elapsed_ms=... gap_ms=... rc=...` | One read/write, including failures. Elapsed time ends when the Modbus API returns, before the quiet wait. Gap is since the preceding API completion; first gap is -1. |
| `MB quiet ... start_ms=... wait_ms=350` / `release_ms=...` | Busy operation and enforced release time. The next request must start at or after release. |
| `Laser ... prepare configuration_needed=...` / `applying configuration` | Separates ordinary restart from actual configuration writes. |

Use `start_ms + elapsed_ms` to locate API completion, including timeouts, and the
quiet-wait records to check the busy guard. Application elapsed times cannot
distinguish late device responses from delayed RX processing; missing transaction
records cannot establish that no frame arrived. Separating those causes requires
additional evidence, such as a UART capture or existing Zephyr tracing/debugger
facilities. RTU has no transaction ID: transaction `seq` is firmware bookkeeping,
not an on-wire identifier, and ambiguous late frames cannot be assigned to a
request with certainty.

Successful response timestamps remain the actual API completion time, never the end
of the quiet interval. Neither elapsed time nor the guard turns a timeout into a
successful acknowledgement. Use these records to assess the 75 ms ACK deadline
within the measurement limits above.
