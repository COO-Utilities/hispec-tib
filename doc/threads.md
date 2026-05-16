# Threads and Blocking Behavior

## Main Thread

`main()` is the network and MQTT pump. It feeds the watchdog, reconnects MQTT
when the network is ready, resubscribes after reconnect, drains `outbound_queue`,
and calls `coo_mqtt_process()`. It can block in MQTT connect/process and sleeps
20 ms when MQTT is disconnected. MQTT connect waits are bounded below the
watchdog interval so a dead broker does not starve the main loop long enough to
reset the device.

`CONFIG_MAIN_THREAD_PRIORITY` is set below MEMS and photodiode timing work but
above command execution so outbound command responses and watchdog feeding are
not delayed by command handlers.

## Command Executor Thread

`command_executor_thread()` blocks on `k_msgq_get(&inbound_queue, K_FOREVER)`.
It dispatches one command and tries one non-blocking enqueue to
`outbound_queue`. Handler blocking varies by command:

- MEMS commands can sleep on router mutexes but do not perform bus I/O directly.
- Attenuator commands can block on DAC I2C.
- Laser/Maiman commands can block on Modbus and laser-bank boot/off sleeps.
- TIB laser-bank heater auto mode runs in its own low-priority thread and can
  block on Modbus temperature polling and heater GPIO writes.
- Settings commands can block on Zephyr settings backend writes.
- Reboot and serial guard commands schedule delayable work.

## Serial Thread

`command_serial_thread()` calls `console_getline()` and blocks until a complete
line is available. Non-empty lines refresh serial guard and enqueue a normalized
command. Serial output is not printed from this thread; it is printed when the
main loop drains `outbound_queue`.

## Photodiode Thread

`photodiode_thread()` is active only for the TIB profile. It waits for board
strap detection and ADS1115 readiness. A `k_timer` provides the 20 ms sampling
cadence and the timer callback only wakes the photodiode thread; ADS1115 bus
I/O remains in thread context. The thread samples YJ and HK channels and updates
dark/noise/window state.

If the timer reports missed periods, the thread logs the missed count and takes
one current sample. Missed ADC sampling periods are not replayed.

## Temperature Thread

`tempsensor_thread()` finds the DS18B20 sensor, then once per second calls
`sensor_sample_fetch()` and `sensor_channel_get()` for ambient temperature. It
updates a mutex-protected cache used by the `temp` command.

## Throughput Monitor Thread

`throughput_monitor_thread()` owns `measure_throughput` stream publication and
optional autolevel control. It reads photodiode snapshots, route-loss settings,
attenuator state, and laser estimates, then enqueues best-effort telemetry to
`outbound_queue`.

## Laser-Bank Control Thread

`laserbank_tempcontrol_thread()` is created only for the TIB profile. It owns
heater auto/override policy, polls Maiman TEC temperature/state at a fixed
interval, reads the cached ambient temperature, and drives the auxiliary
laser-bank heater GPIO. It does not publish MQTT directly; override warnings
are queued through `app_warning_emit()`.

## MEMS Router Thread

`mems_router_thread()` is released by a periodic `k_timer` at
`MEMS_SWITCH_ROUTER_TICK_MS`. The timer expiry runs in interrupt context and
only gives a semaphore. The MEMS thread owns GPIO-expander writes, pulse cleanup,
duty-cycle counter advancement, and stop-after countdowns.

MEMS toggling is not catch-up work. A missed high-to-low cleanup is still
performed because leaving a pulse pin asserted is the worse behavior. A missed
low-to-high pulse is emitted late only when the requested high window still has
enough remaining service time; a fully stale high pulse is skipped and logged.

## SNTP Thread

`sntp_sync_thread()` runs only when `CONFIG_SNTP` is enabled. It handles initial
sync, network-connect wakeups, NTP setting changes, failure retry, and hourly
resync. `sntp_simple()` can block up to the SNTP timeout, but that wait happens
only in the low-priority SNTP thread.

## Zephyr System Workqueue Users

The following delayable work items run in Zephyr system workqueue context:

- Network reconnect work.
- Serial guard expiration.
- Delayed reboot.

Work handlers should remain short. Physical timing loops and SNTP do not run on
the system workqueue.

## Thread Priorities

Current configured priorities:

- MEMS router thread: 2.
- Photodiode thread: 3.
- Main MQTT/outbound/watchdog thread: 4.
- Laser-bank control thread: 5.
- Command executor: 6.
- Serial thread: 6.
- Throughput monitor thread: 7.
- SNTP thread: 10.
- Temperature thread: 11.

Lower numeric values are higher priority in Zephyr preemptive priorities. The
priority order keeps physical MEMS and ADC cadence ahead of network, command,
and telemetry work; command ingress over serial and MQTT is treated as
equivalent at the command-executor layer.
