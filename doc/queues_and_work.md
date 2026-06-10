# Queues, Timers, and Work Items

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
fails. Best-effort messages (i.e. warnings and periodic telemetry) are dropped
when unavailable or failed. Boot watchdog telemetry uses the non-best-effort
target so it is retried until MQTT is available. If the main loop observes
`outbound_queue` at capacity while draining, it logs one local warning while the
queue remains full; it does not enqueue or publish another warning about the
full queue.

Throughput monitoring enqueues photodiode stream telemetry
to `outbound_queue` with `K_NO_WAIT`; if the queue is full, the current sample
is dropped.

Photodiode sampling is released by a `k_timer`; ADC I/O runs in the photodiode
thread, not in the timer ISR.

## Command Dispatcher Delayable Work

When `CONFIG_COO_CMD_SERIAL_GUARD` is enabled, `command_dispatch.c` owns one
`k_work_delayable` item for serial guard expiration. It clears the runtime-only
guard flag after the configured holdoff; no serial guard state is persisted.

## Reboot Delayable Work

`command.c` owns the non-cancelable reboot work item used by the `reboot`
command. The command schedules `sys_reboot(SYS_REBOOT_COLD)` after a short
response window and rejects later app commands while reboot is pending.

## App Blocking Workqueue

`main.c` starts one app-owned workqueue named `app_blocking`. It is for
background work that can block on hardware I/O and therefore must not run on
Zephyr's system workqueue. Zephyr Modbus client RX completion also runs on the
system workqueue; moving blocking app work away from it prevents physically
received Modbus frames from timing out before the RX parser work item can run.

Current users:

- `housekeeping.c` ambient-temperature refresh, which can block on DS18B20 I/O.
- `laserbank_tempcontrol.c` heater policy, which can block on Maiman Modbus
  polling and relay GPIO.
- `lasers.c` auto-off expiration, which can block on Maiman Modbus while
  stopping output.

## Generic Scheduled Action Helper

`lib/coo_commons/scheduled_action.c` provides an optional fixed-table wrapper
around Zephyr `k_work_delayable` for future shared firmware actions. It does not
allocate, create threads, or implement user-programmable scheduling. Callbacks
run in Zephyr's system workqueue and must stay short.

## MEMS Router Work

The active `mems_router` is driven by a periodic `k_timer` and a dedicated MEMS
thread. The timer callback only gives a semaphore; GPIO-expander writes happen
in the MEMS thread. Every MEMS thread tick:

1. Locks the router.
2. Clears pulse pins whose per-switch electrical pulse window has expired.
3. Applies any target-state pulses for switches whose service interval is due.
4. Advances duty-cycle counters and stop-after counters on each switch's
   FFSW/FFLS pulse-width cadence.

The tick uses raw GPIO pin APIs because board profiles store expander pin
numbers rather than `gpio_dt_spec` objects.

MEMS timing variation is aggregated. Warnings are reserved for active toggles
where the rising pulse is late or skipped; base tick variation, static-switch
delay, and high-to-low cleanup lateness are informational. High-to-low pulse
cleanup is still applied, late low-to-high pulses are applied only when their
requested high window has not fully elapsed, and fully stale high pulses are
skipped. Static state changes may remain pending across multiple MEMS ticks
when needed to satisfy the datasheet minimum interval between actuation pulses.

## Network Reconnect and DHCP Fallback Work

`lib/coo_commons/network.c` schedules reconnect work when Zephyr connection
manager reports L4 disconnect. It also schedules DHCP fallback work when
DHCP-first mode is active and Zephyr reports the interface operationally up. If
no lease arrives before `CONFIG_NETWORK_HELPER_DHCP_TIMEOUT_MS`, the fallback
work item applies the configured static profile as a Zephyr-overridable IPv4
address so DHCP can still replace it later. Runtime IP changes from the `ip`
command call
`network_reconfigure()` directly from the command executor; it starts DHCP or
applies static policy but does not block for the DHCP wait and does not require
reboot.

## SNTP Work

`sntp_sync.c` uses one low-priority thread for initial sync, network-connect
wakeups, retry after failure, and hourly resync after success. Manual `time`
commands do not alter SNTP status; SNTP will update the clock on the next
successful sync. The blocking `sntp_simple()` wait does not run on the system
workqueue.
