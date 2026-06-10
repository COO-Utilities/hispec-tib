# Warnings and Telemetry

## Warning Flow

Warnings are emitted with `coo_cmd_runtime_emit(command_runtime_get(), &args)`
using `COO_CMD_RUNTIME_EMIT_WARNING`.

Behavior:

- Logs locally with `LOG_WRN`.
- Uses the command-dispatch runtime emit helper to build JSON with severity,
  code, message, context, and uptime.
- Enqueues either a best-effort or required MQTT message to `outbound_queue`
  with `K_NO_WAIT`, according to the caller's explicit delivery argument.
- Drops best-effort MQTT warnings if the queue is full, MQTT is unavailable, or
  publish fails. Required warnings are retried by the outbound drain after
  successful enqueue, but enqueue can still fail if the bounded queue is full.
- Warnings are intentionally not mirrored into sticky status fields. Operators
  can inspect logs or retry/query state after a warning.

Warning topic:

```text
dt/<device>/warning
```

The `<device>` component follows the selected board strap: `hsfib-tib`,
`hsfib-rcal`, `hsfib-bcal`, or `hsfib-as`.

Current warning codes seen in code:

- `serial_guard_active`
- `attenuator_clamped`
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

Throughput telemetry is best-effort. It is queued through the command runtime
emit helper with `K_NO_WAIT`. If the outbound queue is full, the current sample
is dropped. If MQTT is unavailable or publish fails after transfer, the sample
is dropped.

## Command Responses

Command responses are `struct OutMsg` records built by handlers and drained by
the main loop. MQTT response topic selection is:

1. MQTT 5 `response_topic` property when present and fitting the fixed buffer.
2. Default `cmd/<device>/resp/<key>`.

MQTT 5 correlation data is opaque requester state. Accepted command requests
copy it into a fixed 16-byte static buffer, and command responses echo those
bytes exactly.
