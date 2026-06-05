# Threads and Blocking Behavior

## Main Thread

`main()` is the network and MQTT pump. It feeds the watchdog, reconnects MQTT
when the network is ready, resubscribes after reconnect, drains `outbound_queue`,
and calls `coo_mqtt_process()`. It can block in MQTT connect/process and sleeps
20 ms when MQTT is disconnected. MQTT connect waits are bounded below the
watchdog interval so a dead broker does not starve the main loop long enough to
reset the device.

`CONFIG_MAIN_THREAD_PRIORITY` is set below app timing work but above system
workqueue and command execution so outbound command responses and watchdog
feeding are not delayed by command handlers.

## Command Executor Thread

`coo_cmd_runtime_executor_thread()` blocks on `k_msgq_get(&inbound_queue, K_FOREVER)`.
It dispatches one command and tries one non-blocking enqueue to
`outbound_queue`. Handler blocking varies by command:

- MEMS commands can sleep on router mutexes but do not perform bus I/O directly.
- Attenuator commands can block on DAC I2C.
- Laser/Maiman commands can block on Modbus and laser-bank boot/off sleeps.
- TIB laser-bank heater auto mode runs from laser-bank temperature-control
  delayable work and can block on Modbus temperature polling and heater GPIO
  writes.
- Persistent settings commands can block on Zephyr NVS writes.
- `reboot` and `serialguard` use command-dispatch-owned delayable work.
- Command-dispatch lastcommand persistence can block on Zephyr NVS writes before
  an effect handler runs.

## Serial Thread

`coo_cmd_runtime_serial_thread()` calls `console_getline()` and blocks until a complete
line is available. Non-empty lines refresh serial guard and enqueue a normalized
command. Serial output is not printed from this thread; it is printed when the
main loop drains `outbound_queue`.

## Photodiode Thread

`main()` starts `photodiode_thread()` only for the TIB profile after board
detection and device setup. The thread waits for ADS1115 readiness. A `k_timer`
provides the 20 ms sampling cadence and the timer callback only wakes the
photodiode thread; ADS1115 bus I/O remains in thread context. The thread samples
YJ and HK channels and updates dark/noise/window state.

If the timer reports missed periods, the thread logs the missed count and takes
one current sample. Missed ADC sampling periods are not replayed.

## Throughput Monitor Thread

`main()` starts `throughput_monitor_thread()` only for the TIB profile. It owns
`measure_throughput` stream publication and optional autolevel control. It reads
photodiode snapshots, route-loss settings, attenuator state, and laser
estimates, then enqueues best-effort telemetry to `outbound_queue`.

The throughput monitor runs promptly when active because autolevel decisions
should react on the same general timescale as photodiode sampling. It remains
below MEMS and photodiode sampling because it can write attenuator DACs and
notify laser state.

## MEMS Router Thread

`mems_router_thread()` is released by a periodic `k_timer` at
`MEMS_SWITCH_ROUTER_TICK_MS`. The timer expiry runs in interrupt context and
only gives a semaphore. The MEMS thread owns GPIO-expander writes, pulse cleanup,
duty-cycle counter advancement, and stop-after countdowns.

MEMS toggling is not catch-up work. A missed high-to-low cleanup is still
performed because leaving a pulse pin asserted is the worse behavior. During
active toggling, a missed low-to-high pulse is emitted late only when the
requested high window still has enough remaining service time; a fully stale
high pulse is skipped and logged as a warning. Static state changes obey the
same minimum pulse spacing as toggling, so a command received immediately after
the previous opposite pulse may be delayed until the switch can be pulsed
safely.

## SNTP Thread

`sntp_sync_thread()` runs only when `CONFIG_SNTP` is enabled. It handles initial
sync, network-connect wakeups, NTP setting changes, failure retry, and hourly
resync. `sntp_simple()` can block up to the SNTP timeout, but that wait happens
only in the low-priority SNTP thread.

## Zephyr System Workqueue Users

The following delayable work items run in Zephyr system workqueue context:

- Network reconnect and DHCP fallback work.
- Serial guard expiration.
- Delayed reboot.
- Laser auto-off expiration. This work is owned by `lasers.c`; it is scheduled
  only when a laser command arms a timeout and may block briefly on Modbus while
  stopping an expired output.
- Ambient temperature sampling. This work is owned by `housekeeping.c` and may
  block briefly on DS18B20 sensor I/O while refreshing the `temp` cache.
- Laser-bank heater policy. This work is owned by `laserbank_tempcontrol.c` and
  may block on Maiman Modbus polling and slow relay GPIO I/O.

Physical MEMS, photodiode sampling, throughput monitoring, and SNTP do not run
on the system workqueue. The system workqueue is configured as a preemptible
thread below app timing work because several current work items can block on
network, Modbus, 1-Wire, GPIO, or settings I/O.

## Thread Priorities

Current configured priorities:

- ADS1X1X acquisition thread: 0.
- MEMS router thread: 1.
- Photodiode thread: 2.
- Throughput monitor thread: 3.
- Main MQTT/outbound/watchdog thread: 4.
- Zephyr system workqueue: 5.
- Command executor: 6.
- Serial thread: 6.
- Zephyr logging thread: 13.
- SNTP thread: 14.

Lower numeric values are higher priority within Zephyr preemptive priorities.
Negative priorities are cooperative and remain above these app threads; Zephyr
network cooperative threads therefore must stay bounded and are not used for
app hardware timing loops. The configured app order keeps ADS completion, MEMS,
photodiode, and active throughput work ahead of main, system work, and command
handlers. Command ingress over serial and MQTT is treated as equivalent at the
command-executor layer. SNTP is intentionally lower than deferred logging.
