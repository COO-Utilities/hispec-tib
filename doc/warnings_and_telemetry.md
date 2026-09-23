# Warnings and Telemetry

## Warning Flow

App warnings are emitted with `app_output_emit(&args)` using
`COO_CMD_RUNTIME_EMIT_WARNING`. The existing app queue owner supplies this
interface through `app_output.h`, delegating to `coo_cmd_runtime_emit()`.
Domain producers do not obtain the command runtime. Dispatcher-owned warnings
use the common emitter directly.

Behavior:

- Logs locally with `LOG_WRN`.
- Uses the command-dispatch runtime emit helper to build JSON with severity,
  code, message, context, and uptime.
- Enqueues either a best-effort or required MQTT message to `outbound_queue`
  with `K_NO_WAIT`, according to the caller's explicit delivery argument.
- Drops best-effort MQTT warnings if the queue is full, MQTT is unavailable, or
  publish fails. Required warnings are retried by the outbound drain after
  successful enqueue, but enqueue can still fail if the bounded queue is full.
- If a producer does not supply a response buffer, warning construction uses
  the existing guarded scratch buffer. Contention returns `-EAGAIN`; the local
  log is retained. Queue exhaustion returns `-ENOSPC` without waiting.
- Warnings are intentionally not mirrored into sticky status fields. Operators
  can inspect logs or retry/query state after a warning.

Warning topic:

```text
dt/<device>/warning
```

The `<device>` component follows the selected board strap: `hsfib-tib`,
`hsfib-rcal`, `hsfib-bcal`, or `hsfib-as`.

Examples of warning codes:

- `serial_guard_active`
- `attenuator_clamped`
- `photodiode_adc_error` (at most once per channel every 10 seconds; every
  failed sample is still counted in photodiode windows)
- `photodiode_noise`
- `mems_timing_quantized`
- `split_ratio_quantized`
- `laserbank_heater_override`

`outbound_queue_full` is logged locally by the outbound drain when the queue is
already at capacity. It is intentionally not emitted as MQTT/serial warning
telemetry because doing so would add more output pressure during overload.

## Throughput Telemetry

Throughput telemetry is produced by `throughput_monitor_thread()` when
`measure_throughput` is active. It is published on:

```text
dt/<device>/yj_tput
dt/<device>/hk_tput
```

Payload format is selected by the command request and is specified in
`commands.md`.

Throughput telemetry is best-effort. It is queued through `app_output_emit()`
with `K_NO_WAIT`. If the outbound queue is full, the current sample
is dropped. If MQTT is unavailable or publish fails after transfer, the sample
is dropped.

## Command Responses

Command responses are `struct coo_cmd_response` records built by handlers and drained by
the main loop. MQTT response topic selection is:

1. MQTT 5 `response_topic` property when present and fitting the fixed buffer.
2. Default `cmd/<device>/resp/<key>`.

MQTT 5 correlation data is opaque requester state. Accepted command requests
copy it into a fixed 16-byte static buffer, and command responses echo those
bytes exactly.
