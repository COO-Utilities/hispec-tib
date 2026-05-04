# Runtime Architecture

This page is a compact runtime note. The fuller Sphinx architecture pages are
`architecture.md`, `threads.md`, and `queues_and_work.md`.

## Threads and Work

- `main.c` owns boot ordering, watchdog setup/feed, network bring-up, MQTT
  connection maintenance, outbound MQTT publishing, and periodic MQTT processing.
- `command_executor_thread()` consumes `inbound_queue`, dispatches one command,
  and enqueues one response to `outbound_queue`.
- `command_serial_thread()` blocks on Zephyr line console input and sends each
  line through the same command path as MQTT.
- `photodiode_thread()` samples the ADS1115, updates dark/noise state, and
  enqueues telemetry to `photodiode_queue`.
- `tempsensor_thread()` polls the DS18B20 ambient sensor once per second.
- MEMS switch timing runs in a router-owned `k_work_delayable` tick.
- SNTP sync, network reconnect, serial guard expiration, delayed reboot, and
  photodiode telemetry transfer are delayable or system work items.

## Command Flow

MQTT request topics under `cmd/hsfib-tib/req/#` and serial command lines both
become `struct Command`. Empty MQTT payloads are GET. Non-empty MQTT payloads
default to SET unless JSON `msg_type:"get"` is present. Serial has no `get` or
`set` keywords: empty payload means GET and non-empty payload means SET after
normalization to JSON.

## Serial Override

Any non-empty serial command refreshes the serial guard. While active, MQTT
commands remain connected but are rejected before executor dispatch; the device
attempts an explicit error response and emits a best-effort warning.

## Warnings and Telemetry

Warnings use `app_warning_emit()` and are non-blocking best-effort MQTT
publications on `dt/hsfib-tib/warning`. Photodiode telemetry is generated at the
sampler cadence and bridged to the outbound MQTT queue by delayable work.

## Current Boundaries

- Command-specific parsing stays in `command.c`.
- Hardware sequencing belongs in the relevant domain module when the behavior is
  reusable outside one command handler.
- Timing-sensitive work does not publish MQTT directly.
- Settings own app-level persistent calibration and operator configuration.
