# Threads and Blocking Behavior

## Main Thread

`main()` is the network and MQTT pump. It feeds the watchdog, reconnects MQTT
when the network is ready, resubscribes after reconnect, drains `outbound_queue`,
and calls `coo_mqtt_process()`. It can block in MQTT connect/process and sleeps
20 ms when MQTT is disconnected. MQTT connect waits are bounded below the
watchdog interval so a dead broker does not starve the main loop long enough to
reset the device.

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
strap detection and ADS1115 readiness, samples YJ and HK channels, updates dark
and noise/window state, and sleeps to target the 20 ms sampling period.

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

`laserbank_control_thread()` is created only for the TIB profile. It owns
heater auto/override policy, polls Maiman TEC temperature/state at a fixed
interval, reads the cached ambient temperature, and drives the auxiliary
laser-bank heater GPIO. It does not publish MQTT directly; override warnings
are queued through `app_warning_emit()`.

## Zephyr System Workqueue Users

The following delayable work items run in Zephyr system workqueue context:

- MEMS router toggler work.
- SNTP sync/retry/resync work.
- Network reconnect work.
- Serial guard expiration.
- Delayed reboot.

Work handlers should remain short. The current SNTP handler may block up to the
SNTP timeout, and the MEMS toggler performs GPIO expander writes on each tick.
That SNTP wait runs in the Zephyr system workqueue, not in the photodiode
thread, command executor, or main MQTT/outbound loop.

## Thread Priorities

Current configured priorities:

- Command executor: 5.
- Photodiode thread: 5.
- Laser-bank control thread: 7.
- Temperature thread: 5, with a source TODO that it should be lowest.
- Serial thread: 6.

These priorities need human review after hardware timing tests.
