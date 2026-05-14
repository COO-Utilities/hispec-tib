# Firmware Architecture

## Authority

This architecture audit is derived from current code in `hispec-tib/app`.
`hardware.md` remains authoritative for wiring and physical assumptions.
`commands.md` remains authoritative for intended command/API behavior. When code
and intended docs disagree, the mismatch is listed in
`human_review_required.md` and `command_implementation_audit.md`.

## System Overview

The firmware is a Zephyr C application with static queues, fixed command
handlers, board-profile selection, and direct domain modules for each hardware
area. The application avoids dynamic allocation and user-programmable runtime
scheduling.

Runtime ownership is:

- `main.c`: boot order, watchdog, network/MQTT loop, outbound queue draining.
- `command.c`: MQTT/serial command normalization, dispatch, response builders.
- `devices.c`: board strap detection, profile setup, shared device objects.
- `mems_switching.c`: MEMS switch state, route matching, toggler work.
- `attenuator.c`: DAC channel setup/read/write and coefficient application.
- `maiman.c`: raw/scaled Modbus register transactions.
- `lasers.c`: laser-bank power sequencing, driver verification, estimates, and
  higher-level Maiman helper APIs.
- `photodiode.c`: ADC sampling, dark calibration, noise tracking, and rolling
  sample windows.
- `throughput_monitor.c`: measure-throughput streaming, route-loss application,
  and optional autolevel control.
- `tempsense.c`: DS18B20 polling and cache.
- `sntp_sync.c`: SNTP delayable-work sync and status.
- `app_settings.c`: Zephyr settings-backed app configuration and calibration.
- `app_warning.c`: local warning log plus best-effort MQTT warning publication.

## Boot Sequence

1. `main()` starts and initializes the watchdog.
2. `app_settings_init()` loads defaults and then stored app settings.
3. `devices_detect_board_type()` reads four active-low strap GPIOs.
4. `app_settings_note_board_type()` persists board type and clears other app
   settings if a different valid board type is detected after a prior boot.
5. `devices_ready()` checks/profile-configures required devices.
6. `setup_mems_switches_and_routes()` builds the active MEMS router.
7. `setup_attenuators()` initializes profile-available logical attenuators and
   loads persisted coefficients into runtime attenuator objects.
8. Command runtime registers named scheduled actions.
9. Executor and serial threads are created. Photodiode and temperature threads
   were defined statically and self-gate on board/device availability.
10. SNTP, network, MQTT client, broker settings, and command subscription are
    initialized.
11. The main loop feeds the watchdog, keeps MQTT connected when network is
    ready, drains outbound messages, and processes MQTT events.

## Board Profiles

Board identity comes from exactly one active strap:

- `tib`: 8 MEMS switches, TIB routes, six logical attenuator channels, laser
  bank GPIOs, DS2408 relay outputs, Modbus, ADS1115 photodiodes.
- `cal_yj`: 7 MEMS switches, CAL routes, one logical CAL attenuator channel (TIB's H channel).
- `cal_hk`: same firmware profile shape as CAL YJ.
- `as`: 6 MEMS switches, AS routes, no attenuators.
- `unknown`: no board-specific hardware setup is allowed.

## Command Model

MQTT subscribes to `cmd/hsfib-tib/req/#`. The suffix after that prefix is the
command key. An MQTT response topic property is used when present; otherwise
the default response topic is `cmd/hsfib-tib/resp/<key>`.

Empty MQTT payloads are GET. Non-empty MQTT payloads default to SET unless the
JSON payload has `msg_type:"get"`. Serial commands have no get/set words:
`<key>` is GET and `<key> <payload>` is SET after serial shorthand/key-value
normalization.

The command executor runs exactly one command at a time from `inbound_queue`.
Handlers may block on I/O, sleep, enqueue warnings, update settings, and return
one response.

## Hardware Control

Commands do not directly publish. For example, MEMS commands update
router-owned switch state that is applied by the MEMS delayable-work tick.
Photodiode sampling does not publish directly. Throughput monitoring owns
photodiode stream publication through `outbound_queue`. Warning publication is
non-blocking and best-effort.

Maiman register calls are blocking Modbus RTU transactions. Laser-bank power
commands can sleep while waiting for the Maiman modules to boot or for a
fault-clear power-cycle interval.

## Implemented vs Intended

Implemented behavior is detailed in `implemented_commands.md`. Intended command
behavior remains in `commands.md`. Current code-vs-doc gaps and owner-review
items are centralized in `human_review_required.md`.

## Design Constraints

- Static memory and bounded queues are preferred.
- Domain modules own hardware sequencing; command handlers own command schema.
- Timing-sensitive paths should not perform MQTT publish calls.
- Warnings and telemetry are best-effort.
- Broad schedulers, plugin systems, and dynamic command registries are out of
  scope for current firmware.
