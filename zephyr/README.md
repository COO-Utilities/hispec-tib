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

- GPIO 1-Wire: initialize each device's native bus mutex in the driver init.
  A zeroed mutex can appear to work until contention appends through its null
  wait-queue tail. Maiman's bus exclusion exposed this upstream driver defect.
- DS18B20: acquire the native 1-Wire bus lock for the lazy initial presence probe.
  Conversion command and readout already lock; the conversion wait stays unlocked.
- Modbus: freeze interrupt-driven RTU client frames at timer handoff, then quiesce
  RX/TX, stop the framing timer, and synchronize parser cancellation at request
  entry and completion/timeout. Wait with interrupts enabled and preserve the
  parsed ADU and original result. The caller must hold `iface_lock` and must not
  run on the RX parser's workqueue (the application uses its blocking queue).
  Submission uses upstream's `modbus_work_submit()`; the optional dedicated
  Modbus workqueue remains disabled in this application.
  The short IRQ critical sections target this UP Cortex-M board. Async, ASCII,
  raw and server paths retain their existing behavior; this is not an SMP fix.

Maiman holds both native 1-Wire bus locks through this cleanup. This is essential:
releasing them at the ACK deadline while old RX work remains active reopens the
UART/1-Wire overlap. No polling, command format, stored laser property, bus
configuration, thread priority or response deadline is changed.

Host checks: `python tests/transport/check.py` and
`python tests/throughput/check.py` from this repository using the workspace venv.
The mutex regression uses the real STM32 GPIO driver, GPIO 1-Wire driver and
Zephyr kernel on the Nucleo. It checks recursive locking, blocked waiters, handoff
and reuse for both bus instances without slave transactions. There is no fake
GPIO configuration callback. QEMU's GPIO emulator rejects the open-drain mode
this driver requires, so this is a target test. Build from the workspace root:

```sh
./.venv/bin/west patch --src-module hispec-tib --dst-module zephyr apply
./.venv/bin/west build -b nucleo_h563zi/stm32h563xx -d /tmp/hispec-w1-mutex hispec-tib/tests/transport/w1_mutex
```

Hardware validation after flashing must include cold sensor initialization,
concurrent relay/temperature polling, and timeout recovery; host tests cannot
establish absence of physical UART overruns.
