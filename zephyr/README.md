# Required Zephyr patches

`patches.yml` records checksummed patches for the Zephyr revision pinned in
`west.yml`. The application checks/applies them at CMake configure and before
compilation on every build, including incremental builds after `west update`.
They are maintained here rather than committed to the Zephyr checkout.

From the workspace root, the equivalent manual command is:

```sh
./.venv/bin/west patch --src-module hispec-tib --dst-module zephyr apply
```

The small CMake apply command accepts either a clean applicable patch or a patch
already fully applied. A conflict or partially applied patch stops the build
without discarding local changes. Checksum mismatches also stop the build. Clean
and checkout commands are disabled in the metadata; do not use rollback to
resolve a conflict. Reconcile the checkout/patch explicitly when updating Zephyr,
then update the patch checksum and run the regressions and firmware build.

- Modbus: freeze interrupt-driven RTU client frames at timer handoff, then quiesce
  RX/TX, stop the framing timer, and synchronize parser cancellation at request
  entry and completion/timeout. Wait with interrupts enabled and preserve the
  parsed ADU and original result. The caller must hold `iface_lock` and must not
  run on the RX parser's workqueue (the application uses its blocking queue).
  Submission uses upstream's `modbus_work_submit()`; the optional dedicated
  Modbus workqueue remains disabled in this application.
  The short IRQ critical sections target this UP Cortex-M board. Async, ASCII,
  raw and server paths retain their existing behavior; this is not an SMP fix.

Relay and temperature 1-Wire now use the unmodified UART-backed driver, removing
the GPIO interrupt blackout and Maiman's cross-bus locks. The DS18B20 presence
probe and GPIO mutex-init patches are retired. RTU receive-work lifetime remains
an independent issue, so the Modbus patch above stays in place.

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

Host checks: `python tests/transport/check.py` and
`python tests/throughput/check.py` from this repository using the workspace venv.
After the next flash, verify cold DS2408 discovery/startup outputs and the first
DS18B20 acquisition, then repeat concurrent 1028y status reads, relay commands
and temperature polling. Check presence failures, corrupted replies, USART2
overruns and faults; laser emission is unnecessary.
