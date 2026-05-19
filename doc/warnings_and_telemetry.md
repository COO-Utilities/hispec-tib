# Warnings and Telemetry

## Warning Flow

Warnings are emitted with `coo_cmd_runtime_warning_emit(command_runtime_get(), code, msg, context)`.

Behavior:

- Logs locally with `LOG_WRN`.
- Uses the command-dispatch warning helper to build JSON with severity, code,
  message, context, and uptime.
- Enqueues one `OUT_TARGET_MQTT_BEST_EFFORT` message to `outbound_queue` with
  `K_NO_WAIT`.
- Drops the MQTT warning if the queue is full, MQTT is unavailable, or publish
  fails.
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
- `mems_rate_quantized`
- `split_ratio_quantized`
- `outbound_queue_full`
- `laserbank_heater_override`

## Throughput Telemetry

Throughput telemetry is produced by `throughput_monitor_run_once()` when
`measure_throughput` is active. Housekeeping calls that service pass every
100 ms on TIB. Telemetry is published on:

```text
dt/<device>/yj_tput
dt/<device>/hk_tput
```

Payload format is selected by the command request and is specified in
`commands.md`.

Telemetry is best-effort. It is queued directly to `outbound_queue` with
`K_NO_WAIT`. If the outbound queue is full, the current sample is dropped. If
MQTT is unavailable or publish fails after transfer, the sample is dropped.

## Command Responses

Command responses are `struct OutMsg` records built by handlers and drained by
the main loop. MQTT response topic selection is:

1. MQTT 5 `response_topic` property when present and fitting the fixed buffer.
2. Default `cmd/<device>/resp/<key>`.

MQTT 5 correlation data is opaque requester state. Accepted command requests
copy it into a fixed 16-byte static buffer, and command responses echo those
bytes exactly.
