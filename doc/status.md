# HISPEC-TIB Firmware Status

This page is the maintained current-status note for the Zephyr firmware. It
summarizes current implementation state; `hardware.md` remains the hardware
source of truth, `commands.md` remains the intended command/API source of truth,
and current C source remains the source of truth for what is implemented today.

## Current Shape

- Target application: `hispec-tib/app`, built as a Zephyr C application.
- Primary board currently described by overlay: `nucleo_h563zi/stm32h563xx`.
- Boot path: `main.c` initializes watchdog, settings, board strap detection,
  profile-specific devices, command runtime, serial/MQTT command paths,
  photodiode telemetry bridge, SNTP, network, and MQTT.
- Command ingress: MQTT and serial both produce `struct Command` records and
  enqueue them to `inbound_queue`.
- Command execution: `command_executor_thread()` dispatches commands through the
  static table in `command.c`.
- Outbound flow: command responses, warnings, and telemetry are placed on
  `outbound_queue`; only the main loop publishes MQTT.
- Hardware profiles: TIB, CAL YJ, CAL HK, AS, and unknown are selected from
  active-low board strap GPIOs.
- Persistence: Zephyr settings under the `tib` subtree store board type, boot
  count, serial guard, IP, MQTT, attenuator calibration coefficients, and
  photodiode calibration/noise/gain settings.

## Implemented Areas

- Board strap detection with exactly-one-active validation.
- TIB/CAL/AS MEMS switch profile selection and route tables.
- MEMS static switching, duty-cycle toggling, exact tick duty control, active
  route readback, and quantization warnings.
- AS `split` command using fixed YJ/HK AS routes and exact MEMS tick ratios.
- TIB laser-bank GPIO power on/off and clear-faults-by-power-cycle commands.
- Raw Maiman Modbus register get/set helpers and higher-level laser helper APIs.
- Logical attenuator value, dB value, and coefficient command support.
- Photodiode sampling, telemetry, explicit dark measurement, stored dark update,
  lowest-dark tracking, and noise warnings.
- DS18B20 ambient temperature sampling and `temp` query.
- IPv4 network helper with DHCP/static/fallback behavior and link monitoring.
- MQTT broker settings with runtime reconnect trigger.
- Serial command guard with scheduled expiration and MQTT command rejection.
- SNTP sync from manual or DHCP NTP server.
- Watchdog setup/feed in the main loop.

## Partially Implemented

- Laser command coverage exists only as raw register get/set plumbing, and the
  current command key parsing appears inconsistent with the dispatch prefix.
- Higher-level laser tuning/status helper APIs exist in `lasers.c`, but the
  public command table does not expose most of them.
- Attenuator support covers six logical channels and one DAC device label; the
  hardware document describes two DAC7578 devices and twelve physical FVOAs.
- Runtime network reconfiguration is available in the library helper, but the
  `ip` command currently reports reboot-required for network-affecting changes.
- SNTP status is exposed through `time` and `ip`; manual time set does not mark
  SNTP status as manual.

## Open items

- Reconcile implemented command behavior against `commands.md`; see
  `command_implementation_audit.md` and `implemented_commands.md`.
- Reconcile hardware/code mismatches for attenuator DAC coverage, MEMS GPIO
  electrical mode, and Modbus stop-bit constants; see
  `human_review_required.md`.
- Decide intended persistence for MEMS state, split state, laser output state,
  relay outputs, and laser-bank power state.
- Decide whether `power` and `sleep` remain supported commands, and document or
  remove the no-op `sleep` behavior.
- Add automated tests for command parsing and non-hardware domain logic.
- Decide whether the COO commons network/MQTT helpers should remain app-local
  wrappers or become shared library APIs with stricter contracts.

## LLM-resolved items requiring human review

- Old status notes said settings persistence was unimplemented. Current code now
  persists IP, MQTT, serial guard, boot count, attenuator coefficients, and
  photodiode calibration settings. Human review is still needed for missing
  operating-state persistence.
- Old status notes said SNTP was unimplemented. Current code has `sntp_sync.c`
  and exposes status through `time`; review is still needed for manual/DHCP
  policy and failure reporting.
- Old status notes described `main.cpp`, `executor_task.cpp`, and Zyre/Pico
  modules from a previous architecture. Those notes are stale for this Zephyr C
  app and are replaced by `architecture.md`, `threads.md`, and
  `queues_and_work.md`.
- Old notes said MQTT handling lived in `main.c`; current MQTT ingress is in
  `command_handle_mqtt_publish()` and MQTT connection/publish pumping remains
  in `main.c`.
