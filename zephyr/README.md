# Native Zephyr transports

The application uses the unmodified Zephyr revision pinned in `west.yml`.
There is no patch manifest, application helper, or configure/build patch hook.

USART2 uses the STM32 driver's native `fifo-enable` setting with interrupt-driven
Modbus RTU and GPIO driver enable. Relay and temperature 1-Wire use separate
UARTs through the stock serial driver, removing the GPIO interrupt blackout and
Maiman's cross-bus locks. The dedicated Modbus workqueue remains disabled.

Maiman owns the Modbus client configuration and uses public lifecycle APIs.
Only a read/write returning `-ETIMEDOUT` calls `modbus_disable()`, while holding
the existing laser I/O mutex outside the parser's workqueue. This disables RX/TX,
stops the framing timer, and synchronizes cancellation of the shared parser work
item. The original timeout is returned without retry. The interface stays
disabled until the next requested register transaction (including a health poll)
calls `modbus_init_client()`. Initialization configures local state and sends
nothing; communication health changes only after an actual controller response.
Other transaction errors do not disable or reinitialize the client.

This replaces timeout cleanup with public APIs; it does not reproduce the former
patch's frame freeze or cleanup on every successful transaction. RTU still has
no transaction ID, so a sufficiently late on-wire reply remains ambiguous. See
[Maiman behavior](../doc/api/maiman_laser.md) for error and busy-wait details.

The serial 1-Wire driver still uses a zero-initialized native bus mutex without
initializing its wait queue. This is an upstream defect, left unpatched for the
current ownership: housekeeping is the sole DS18B20 caller, and all runtime
relay operations pass through the DS2408 driver's initialized mutex (as well as
housekeeping's I/O lock). Each bus has one slave, no other raw bus caller, and no
shell access. The pinned kernel's uncontended lock/unlock path works; the crash
previously exposed by Maiman required contention on the native bus mutex.
Revisit initialization if this ownership changes. Sensor conversion still
sleeps outside the bus lock. Stock reset timing and the accepted DS2408 timing
exception at 3.3 V are documented in [hardware.md](../doc/hardware.md).

After the next flash, verify cold DS2408 discovery/startup outputs and the first
DS18B20 acquisition, then repeat concurrent 1028y status reads, relay commands
and temperature polling. Check presence failures, corrupted replies, USART2
overruns and faults; laser emission is unnecessary for these communication checks.
Also check a response-loss timeout followed by a later request, and repeat the
original dark-acquisition/calibration sequence under the agreed laser limits.
