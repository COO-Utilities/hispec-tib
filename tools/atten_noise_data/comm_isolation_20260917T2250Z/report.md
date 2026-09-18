# PCB communication isolation — 17 September 2026

**Confirmed on the running PCB: both relay polling and temperature polling independently cause USART2 overruns during Modbus replies.** No reflash, reboot, laser start, or laser-property change was required.

## Controlled results

Each entry counts failed `laser/status` engineering queries for `1028y`, not individual Modbus transactions. All queries were issued with laser and TEC off, zero measured current, and photodiodes off.

| Polling condition | Block failures | Combined failures | Rate |
|---|---:|---:|---:|
| Both enabled | 21/250, 23/250, 23/250 | 67/750 | 8.93% |
| Relay only | 12/250, 11/250 | 23/500 | 4.60% |
| Temperature only | 21/250, 16/250 | 37/500 | 7.40% |
| Neither | 0/250, 0/250 | 0/500 | 0% observed |

An initial baseline before attaching the probe failed 26/250 queries. Attaching the probe did not create the problem. Every block retained boot counter 37 and firmware identity `620d711a0244-dirty`.

Order was detached baseline, both, relay, neither, temperature, both, temperature, neither, relay, both. Queries used identical seeded 0–50 ms jitter. Existing RAM gates were changed only at the housekeeping function boundary; five seconds of settling followed each transition. No breakpoints or debugger halts occurred during these quantitative blocks.

Caveat: disabling temperature removes its 750 ms conversion sleep, increasing relay-only cadence from roughly 1.75 s to 1 s. These rates show that either path is sufficient to produce failures; they do not measure each path's contribution under normal scheduling. Zero observed failures does not establish zero long-term risk.

## Direct hardware evidence

Separate diagnostic runs installed a hardware breakpoint at `0x0804a50a`, the instruction that clears USART overrun **after the branch has already established that ORE is set**. No breakpoint fired during the three-second idle interval before either query burst. The first overrun therefore existed before the diagnostic halt. These runs are excluded from the quantitative table.

Both captures show USART2 (`r3=0x40004400`), ORE clear mask `r1=8`, and an active node-5/function-3 Modbus read. The interrupted thread is `app_blocking`.

Relay capture:

    housekeeping temperature_work_handler
      gpio_port_get_raw / ds2408_port_get_raw
        ds2408_read_reg / w1_write_read
          w1_gpio_reset_bus — returning immediately after irq_unlock
            USART2 interrupt — ORE already set

Temperature capture:

    housekeeping temperature_work_handler
      temperature_sample_once / sensor_sample_fetch
        ds18b20_temperature_convert / w1_reset_select
          w1_gpio_reset_bus — returning immediately after irq_unlock
            USART2 interrupt — ORE already set

See [relay stack and registers](relay_ore_ore.log) and [temperature stack and registers](temperature_ore_ore.log). The requested registers were 0x0041 and 0x008a respectively, both on node 5. Relay-only had temperature_dev=NULL; temperature-only had relay_gpio_online=false.

The 1-Wire reset masks interrupts for 480 + 70 + 410 = **960 microseconds**, plus execution overhead. At 115200 baud / 8N1, each UART byte takes **86.8 microseconds**. USART2 CR1 readback was 0x0000002d: FIFO disabled. UART overrun during this blackout loses response bytes. The driver clears ORE without propagating that error through uart_fifo_read. Modbus later sees short frames (-122), bad CRC (-5), or missing responses (-116); all three occurred in ordinary unhalted blocks.

Relevant source: `zephyr/drivers/w1/w1_zephyr_gpio.c:133`, `zephyr/drivers/serial/uart_stm32.c:974`, `zephyr/subsys/modbus/modbus_serial.c:284`, and `hispec-tib/app/src/housekeeping.c:510`.

## Independent -5 warning after STOP

With both polls suppressed, three consecutive STOP/status pairs produced the same result:

- STOP acknowledged success.
- Following engineering status returned read_rc=0, matching serial number, operation stopped, TEC stopped, zero set/measured current, raw/blocking lock=2, blocked_reason=tec_not_started.
- That successful status query emitted laser_output_fault rc=-5.

See [STOP/status responses](stop_probe.jsonl). Serial warning uptimes were 1118132, 1119853 and 1121586 ms. There were no transport failures in those pairs.

The code path explains this: `commit_current_runtime_locked()` marks the stopped zero-current estimate valid; `hispec_laser_get_status()` unconditionally treats nonzero blocking_lock_status as invalidating output state, even when intentionally stopped; `invalidate_output_locked()` emits a generic -EIO warning on the transition from valid to invalid. Thus this particular -5 does not mean a Modbus read/write failed. It is separate from the real CRC-derived -5 errors above.

Relevant source: `hispec-tib/app/src/lasers.c:372`, `:1446`, `:203`. A fix must distinguish stopped/not-ready state from a failed control operation or an unexpected active-output fault.

## Timeout followed by recovery four milliseconds later

Restoring relay polling after deliberately suppressing it reproduced the user's exact warning pattern:

    00:10:54.322 relay_communication_fault rc=-116
    00:10:54.326 relay_communication_recovered rc=0

The first relay transaction succeeded. Housekeeping checks its five-second response deadline **before** performing that transaction. A delayed/skipped poll therefore generates this pair without a timed-out relay transaction. The pattern repeated at later restoration boundaries.

Laser background health uses the same check-before-read pattern. Calibration fitting runs synchronously in the priority-3 throughput thread; background health checks run in priority-7 app_blocking. The numerical fitting path contains no sleeps/yields. The supplied DAC1 and DAC2 fit warnings were 5272 ms apart, exceeding the five-second health deadline. Scheduling delay from fitting is therefore a strong explanation for that later warning sequence, but the original fit's runtime was not directly profiled during this investigation: retained calibration was inactive/empty on this boot.

The correction warning itself describes rejection of the full six-term candidate followed by selection of five terms; its reported negative slope belongs to the rejected candidate.

## Recent commits and upstream

`hispec-tib` commit **9cc7a524 (15 September)** added the unconditional periodic relay read to housekeeping. Temperature polling was already present. This explains increased exposure to an existing interrupt blackout, but does not prove which first flash made the problem visible.

Compared pinned Zephyr **e60d3379133c4315504e9113ecdcc05fbe552756** with the proposed **55303e23f59df42d3ba41f0e93ca2192240f804a**. The Modbus changes redirect work submission; the 1-Wire and DS18B20 changes here are logging/include changes. STM32 UART changes concern async/DMA, power management and RTS behavior. None removes this interrupt blackout or fixes the RTU buffer-ownership race in this interrupt-driven configuration.

## What this establishes for the fix

1. Relay-only inhibition is insufficient. Modbus reply reception must be protected from **both** 1-Wire paths. Exclusion should cover physical transactions; the temperature conversion wait need not hold that exclusion.
2. The independent RTU timer/RX/parser ownership race remains a code defect: timer expiry queues parsing without freezing RX or preventing later timer rearming. This experiment does not attribute individual -122 failures to that race; UART truncation alone already explains -122. Application transaction mutexes cannot repair ISR/timer/workqueue ownership.
3. The stopped-state -5 warning and stale-health scheduling/deadline behavior require separate, narrow corrections. They must not be hidden by retrying transport operations.

No production fix was applied or validated in this investigation.

## Integrity and cleanup

- Saved ELF/binary/config/source snapshots before probing; full **398200-byte flash readback matched** the saved binary byte-for-byte. SHA256: a8207361eb9d9db563888065226d0e015ad94f2cdc8f0b369f9f352f8838c0b0.
- Restored original RAM values: relay_gpio_online=true; temperature_dev=0x0804d0a8; temperature_initialized remained true throughout.
- Removed/flushed hardware breakpoints. Final debugger logs show all eight slots available and no breakpoints installed.
- Closed pyOCD/GDB and passive serial capture. A post-disconnect MQTT status query succeeded: same firmware, boot counter 37, relay_err=0, photodiodes off, ambient temperature updating.
- Final engineering read succeeded: 1028y off, TEC off, current set and measured both 0.0 mA. Laser settings, attenuator values and bank-power policy exactly match their initial snapshots. Heater remains auto/off.
- No repository source, firmware, tests, or explanatory comments were edited. No build was run. Only investigation scripts and captured evidence were added.

[Machine-readable matrix](matrix_summary.json). [Full evidence archive](evidence.tar.gz) contains all raw query blocks, serial capture, GDB scripts/logs, initial/final state, frozen host module/source files, ELF/config and flash readback. `analysis_notes.md` in the archive records method details; `query_block.py`, `set_condition.py`, `capture_ore.py`, and `stop_probe.py` preserve exactly what was run. The RAM addresses are specific to this verified build and are not reusable against an arbitrary firmware image.
