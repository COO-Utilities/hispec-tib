# Warnings and telemetry

Command replies answer a request; telemetry reports asynchronous measurements,
progress or warnings. They have different delivery guarantees. The
[control specification](commands.md) owns payload layouts and response metadata;
[runtime architecture](architecture.md#queue-and-delayed-work-contracts) owns queueing.

## Topics and interpretation

`<device>` is the formal name selected by the board strap: `hsfib-tib`,
`hsfib-bcal`, `hsfib-rcal` or `hsfib-as`.

| Topic suffix under `dt/<device>/` | Contents | Interpretation |
|---|---|---|
| `warning` | JSON severity, code, message, context and uptime | An event, not a complete state snapshot |
| `yj_tput`, `hk_tput` | JSON or binary, selected by `measure_throughput` | Best-effort fresh-conversion records; gaps are possible |
| `atten` | JSON attenuator-calibration progress/fit events | Retained binary records remain the detailed acquisition evidence |
| `laser/calibrate` | JSON laser-calibration status | Query status/data for current results and retained records |
| `boot` | Reset/watchdog evidence | Required queued output; still subject to bounded resources |

Warnings have `severity`, `code`, `msg`, `context` and `uptime_ms` fields.
Uptime is not UTC. The Python helper retains a bounded warning history and logs
other telemetry; [collectors](python_helpers.md#throughput-collection-and-plotting)
decode throughput outside the MQTT callback.

## Warning families

| Codes | Meaning / operation reference |
|---|---|
| `serial_guard_active` | A serial override blocked an MQTT effect request; see [serial guard](commands.md#serialguard) |
| `mqtt_broker_revert` | A new broker connection failed and prior configuration was restored |
| `attenuator_clamped`, `mems_timing_quantized`, `split_ratio_quantized` | Hardware/model/tick limits changed the representable result; read confirmed state |
| `photodiode_adc_error`, `photodiode_dark_failed`, `photodiode_noise` | Acquisition failure, unsuccessful dark or elevated window RMS; see [sampling](photodiode_notes.md) |
| `laser_communication`, `laser_communication_fault`, `laser_communication_recovered`, `laser_output_fault` | Transaction failure, sustained loss, recovery or observed output fault; see [laser health](laser_operation.md#communication-and-power-lifetime) |
| `relay_gpio_offline`, `relay_communication`, `relay_communication_fault`, `relay_communication_recovered` | Shared relay availability; affected measurements stop on owner fault |
| `throughput_stopped` | A consumer stopped or was displaced; inspect message/context and current status |
| `atten_calibration_restart`, `atten_correction_rejected` | Calibration replacement or numerical correction fallback; see [calibration](attenuator_calibration.md) |
| `laserbank_heater_override` | Forced heater mode; see [heater control](commands.md#laserbankheater) |

ADC warnings are rate-limited per channel to one per ten seconds; failed samples
are still counted. PD noise assessment waits through reported input changes and
five seconds afterward, then needs a fresh 500 ms window with at least two valid
samples; its warning interval is 60 seconds per channel. Laser/relay first
communication failures warn immediately, repeats at most once per five seconds;
fault/recovery transitions remain immediate. Recovery does not restart acquisition.

## Delivery and state

`app_output_emit()` is the application producer boundary; it delegates to
`coo_cmd_runtime_emit()`, which logs warnings locally and attempts a bounded JSON enqueue
without waiting. Scratch contention, a full queue, disconnection or failed publish
can lose best-effort telemetry. Required output is retried only after successful
enqueue and can still be lost if requeueing fails. Warnings do not create a generic
sticky-warning register; domain health/fault state has its own meaning.

Output-pressure diagnostics remain local to avoid adding traffic to a full queue.
A missing warning is not evidence of healthy hardware, and an old warning is not
proof of a current fault. Query the owning command and preserve raw readings when
investigating. Command replies are `struct coo_cmd_response` objects drained by
main, with the request's accepted MQTT response-topic/correlation properties; they
are separate from the asynchronous topics above.
