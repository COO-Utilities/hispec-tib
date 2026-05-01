# Runtime Architecture

This note records the current firmware shape. `status.md`, `commands.md`, and
`hardware.md` remain the source of truth for intent, command behavior, and
hardware mapping.

## Threads and Work

- `main.c` owns boot ordering, watchdog setup, network bring-up, MQTT connection
  maintenance, outbound MQTT publishing, and periodic MQTT processing.
- `command_executor_thread` is the common command executor. Serial and MQTT
  ingress both queue `struct Command` items into `inbound_queue`; the executor
  dispatches through the command table in `command.c`.
- `command_serial_thread` uses Zephyr's line console so typed serial commands
  are one line at a time. `command_parse_serial_line()` treats the first token
  as the command key and the rest as optional payload; serial has no `get` or
  `set` keywords. `normalize_serial_payload()` accepts raw JSON unchanged or
  converts key/value and shorthand payloads into the same JSON shape used by
  MQTT before dispatch.
- `photodiode_thread` samples the ADC and queues telemetry. A delayable work
  item drains photodiode telemetry into the normal outbound queue.
- `tempsensor_thread` owns ambient temperature polling.
- MEMS switch timing is handled by a router-owned `k_work_delayable` tick in
  `mems_switching.c`; command parsing and MQTT work stay out of that timing
  path.

## Named Scheduled Actions

`app_scheduled_actions.c` is the shared pattern for "do this later unless it is
rescheduled or canceled" behavior. It is intentionally small and explicit:

- `serial_guard_expire`: clears the serial override after the configured quiet
  interval.
- `reboot`: delays reboot long enough for the command response to be queued.

The module wraps Zephyr `k_work_delayable` objects behind
`enum app_scheduled_action_id`. New uses should add a named enum entry only when
the action is a real firmware behavior, not a user-created automation job.

## Serial Override

Any accepted serial line calls `command_serial_note_activity()`. If
`serialguard_s` is nonzero, that marks network command execution as disabled and
reschedules `serial_guard_expire` for `serialguard_s` seconds after the most
recent serial command.

MQTT remains connected while the network is ready. When serial override is
active, `command_handle_mqtt_publish()` rejects MQTT commands before they enter
the executor queue, logs the rejection, and attempts to publish an error
response on the request response topic. This keeps local serial control
predictable without making remote clients diagnose a silent disconnect.

Serial responses are not separately generated. `_msg_builder()` creates the
same `OutMsg` payload used for MQTT responses, and `print_serial_response()`
prints the response topic followed by the payload with 80-column wrapping and
tab indentation at print time.

## Warnings

`app_warning_emit()` is the shared warning helper. It logs locally with
`LOG_WRN()` and tries one non-blocking enqueue to `dt/<device>/warning` using
`OUT_TARGET_MQTT_BEST_EFFORT`. Best-effort outbound messages are dropped when
MQTT is unavailable or publish fails; they are not retried because warning
publication must not block command responses or hardware timing paths.

## Current Boundaries

- Commands should keep parsing/validation in `command.c` and call device/domain
  modules for hardware behavior.
- Device timing loops should not publish MQTT directly when a bounded queue or
  response helper can decouple timing-sensitive work from network availability.
- Persistent settings currently cover IP, MQTT, boot count, and serial guard
  timeout. Calibration and operating-state persistence are still future work.
- Warning publication is available for lightweight suspicious/degraded
  conditions. Warning coverage still needs to be added to more hardware and
  calibration paths as those subsystems mature.
