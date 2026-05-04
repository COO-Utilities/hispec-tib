# Queues and Work Items

## `inbound_queue`

Defined in `command.c` as a `k_msgq` of `struct Command` with depth
`MAX_PENDING_COMMANDS` (2). MQTT and serial ingress use non-blocking puts. When
the queue is full, MQTT and serial receive a busy/error response path instead of
executing work in the ingress callback.

## `outbound_queue`

Defined in `command.c` as a `k_msgq` of `struct OutMsg` with depth 8. It carries
command responses, warnings, and telemetry to the main loop. The main loop is
the only path that calls `mqtt_publish()`.

Non-best-effort MQTT messages are requeued when MQTT is unavailable or publish
fails. Best-effort warnings are dropped when unavailable or failed.

## `photodiode_queue`

Defined in `photodiode.c` as a `k_msgq` of `struct OutMsg` with depth 4.
Photodiode sampling enqueues telemetry here with `K_NO_WAIT`. If full, the
queue is purged and the current telemetry sample is retried.

`main.c` owns `photodiode_publish_work`, which periodically drains this queue
into `outbound_queue`. That keeps ADC sampling decoupled from MQTT availability.

## Named Scheduled Actions

`app_scheduled_actions.c` wraps a small fixed table of `k_work_delayable`
objects:

- `serial_guard_expire`: clears serial override and re-enables MQTT command
  execution.
- `reboot`: calls `sys_reboot(SYS_REBOOT_COLD)` after a short delay so the
  response can be queued first.

This is not a general scheduler. New delayed actions should be named firmware
behaviors with fixed enum entries.

## MEMS Router Work

Each initialized `mems_router` owns one `k_work_delayable` tick. Every tick:

1. Locks the router.
2. Clears all MEMS pulse pins.
3. Applies any target-state pulses.
4. Advances duty-cycle counters and stop-after counters.
5. Reschedules itself for `MEMS_SWITCH_ELECTRICAL_PULSE_MS`.

The tick uses raw GPIO pin APIs because board profiles store expander pin
numbers rather than `gpio_dt_spec` objects.

## Network Reconnect Work

`lib/coo_commons/network.c` schedules reconnect work when Zephyr reports L4
disconnect. The handler calls `conn_mgr_all_if_connect(true)`.

## SNTP Work

`sntp_sync.c` uses one delayable work item for initial sync, manual reschedule
on network connect, retry after failure, and hourly resync after success.

## Work/Queue Human Review

- MEMS toggler work currently performs repeated GPIO bus activity at the tick
  rate. Source TODO notes this may be more I/O than necessary.
- SNTP work can block in the system workqueue while waiting for an SNTP reply.
- Photodiode telemetry is purged on queue-full; this is acceptable for
  telemetry but should be visible in testing.
