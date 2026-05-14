# Development Status

This page is the maintained current-status note for the Zephyr firmware. It
summarizes current implementation state; `hardware.md` remains the hardware
source of truth, `commands.md` remains the intended command/API source of truth,
and current C source remains the source of truth for what is implemented today.

## Current Shape

- Target application: `hispec-tib/app`, built as a Zephyr C application.
- Primary board currently described by overlay: `nucleo_h563zi/stm32h563xx`.
- Boot path: `main.c` initializes watchdog, settings, board strap detection,
  profile-specific devices, command runtime, serial/MQTT command paths,
  laser-bank heater control, SNTP, network, and MQTT. Watchdog or settings
  initialization failure stops boot.
- Command ingress: MQTT and serial both produce `struct Command` records and
  enqueue them to `inbound_queue`.
- Command execution: `command_executor_thread()` dispatches commands through the
  static table in `command.c`.
- Outbound flow: command responses, warnings, and telemetry are placed on
  `outbound_queue`; only the main loop publishes MQTT.
- Hardware profiles: TIB, CAL YJ, CAL HK, AS, and unknown are selected from
  active-low board strap GPIOs.
- Persistence: Zephyr settings under the `tib` subtree store board type, boot
  count, serial guard, IP, MQTT, laser-bank heater mode,
  attenuator calibration coefficients, route-loss records, and photodiode
  calibration/noise/gain settings.


## Partially Implemented

- Laser command coverage exists only as raw register get/set plumbing, and the
  current command key parsing appears inconsistent with the dispatch prefix.
- Higher-level laser tuning/status helper APIs exist in `lasers.c`, but the
  public command table does not expose most of them.
- Runtime network reconfiguration is available in the library helper, but the
  `ip` command currently reports reboot-required for network-affecting changes.
- SNTP status is exposed through `time` and `ip`; manual time set does not mark
  SNTP status as manual. SNTP might also have too much blocking on a main/command thread

## Open items

- Reconcile implemented command behavior against `commands.md`; see
  `command_implementation_audit.md` and `implemented_commands.md`.
- Decide intended persistence for MEMS switch state, AS split requested/actual
  state, laser output/tuning state, and last-command metadata.

- Add automated tests for command parsing and non-hardware domain logic.




