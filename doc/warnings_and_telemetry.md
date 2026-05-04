# Warnings and Telemetry

## Warning Flow

Warnings are emitted with `app_warning_emit(code, msg, context)`.

Behavior:

- Logs locally with `LOG_WRN`.
- Builds JSON with severity, code, message, context, and uptime.
- Enqueues one `OUT_TARGET_MQTT_BEST_EFFORT` message to `outbound_queue` with
  `K_NO_WAIT`.
- Drops the MQTT warning if the queue is full, MQTT is unavailable, or publish
  fails.

Warning topic:

```text
dt/hsfib-tib/warning
```

Current warning codes seen in code:

- `serial_guard_active`
- `attenuator_clamped`
- `photodiode_noise`
- `mems_rate_quantized`
- `split_ratio_quantized`

## Photodiode Telemetry

Photodiode telemetry is produced by `photodiode_thread()` on:

```text
dt/hsfib-tib/photodiode
```

Payload includes per-channel validity, raw counts, mV, dark-subtracted mV,
estimated power, residual RMS noise, dark settings, dark measurement state,
age, sample count, and uptime.

Telemetry is queued through `photodiode_queue` first, then transferred to the
normal outbound queue by `photodiode_publish_work`.

## Command Responses

Command responses are `struct OutMsg` records built by handlers and drained by
the main loop. MQTT response topic selection is:

1. MQTT 5 `response_topic` property when present and fitting the fixed buffer.
2. Default `cmd/hsfib-tib/resp/<key>`.

Correlation data is echoed when it fits the fixed response buffer.
