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
- `command.c`: app command queues, static command behavior table, serial guard
  policy, request classification, and app/cross-domain command handlers.
- `devices.c`: board strap detection, profile setup, shared device objects.
- `mems_switching.c`: MEMS switch state, route matching, timer-driven router thread.
- `attenuator.c`: DAC channel setup/read/write and coefficient application.
- `attenuator_command.c`: command-schema validation for `atten` value and
  coefficient requests.
- `maiman.c`: raw/scaled Modbus register transactions.
- `lasers.c`: laser-bank power sequencing, driver verification, laser settings
  validation, output estimates, and higher-level Maiman helper APIs.
- `laser_command.c`: command-schema validation and response shaping for laser
  and laser-bank requests.
- `photodiode.c`: ADC sampling, dark calibration, noise tracking, and rolling
  sample windows.
- `photodiode_command.c`: command-schema validation for `pd` and `pdsettings`.
- `throughput_command.c`: command-schema validation for `measure_throughput`.
- `throughput_monitor.c`: measure-throughput streaming, route-loss application,
  and optional autolevel control.
- `housekeeping.c`: slow ambient-temperature sampling, laser-bank heater
  policy cadence, and slow power timeout service.
- `sntp_sync.c`: low-priority SNTP sync thread and status.
- `app_settings.c`: direct-NVS app configuration and calibration.
- `app_identity.c`: selected board-profile MQTT device ID.

Project-local wrappers under `lib/coo_commons` are intentionally small:

- `network.c`: Ethernet IPv4 bootstrap and runtime reconfiguration with
  DHCP/static/fallback profile selection.
- `mqtt_client.c`: MQTT 5 broker parsing, broker resolution, connect/process,
  and subscription helpers around Zephyr MQTT.
- `command_dispatch.c`: fixed-buffer MQTT/serial command request, static
  longest-prefix dispatch, topic formatting, response metadata, warning JSON,
  serial payload normalization, and serial response printing helpers.
- `json_utils.c`: constrained keyed JSON extraction and fixed-buffer append
  helpers used by command code.

The application otherwise uses Zephyr GPIO, I2C, ADC, DAC, UART, Modbus,
settings/NVS, watchdog, console, networking, MQTT, SNTP, sensor, and 1-Wire
APIs directly.

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
9. Executor, serial, and housekeeping threads are created. Photodiode and
   throughput monitor threads were defined statically and self-gate on
   board/device availability.
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

MQTT subscribes to `cmd/<device>/req/#`. The suffix after that prefix is the
command key. The shared command-dispatch helper copies the MQTT request into a
fixed request object. An MQTT response topic property is used when present;
otherwise the default response topic is `cmd/<device>/resp/<key>`.
The `<device>` namespace is selected from the detected board strap: `tib` maps
to `hsfib-tib`, `cal_hk` to `hsfib-rcal`, `cal_yj` to `hsfib-bcal`, and `as` to
`hsfib-as`. The same name is used for telemetry under `dt/<device>/...` and as
the MQTT client ID.

Requests are classified by schema and topic shape, not by user-visible method
verbs. The app's static command behavior table owns the default query/effect
policy and names the special payload rules used by `command_infer_msg_type()`.

Empty or no-payload requests are queries except for no-payload actions such as
`reboot`, `laserbank/clearfaults`, and laser-bank topic suffixes such as
`laserbank/power/override_on`. Non-empty payloads normally mean an effect
request, but documented query payloads remain queries: `status`, laser status
endpoints, laser name-only queries, laser tune/settings readbacks, and
`memsroute/route_loss` payloads that contain only `route`.

Serial commands use the same classification after line normalization by the
shared command-dispatch helper:
`<key>` becomes an empty JSON payload, raw JSON is copied, `key=value` tokens
are wrapped as JSON fields, and command-table-selected human shorthands are
translated into the same payload shapes as MQTT. The old payload `msg_type`
convention is not part of current ingress classification.

The command executor runs exactly one request at a time from `inbound_queue`.
Handlers may block on I/O, sleep, enqueue warnings, update settings, and return
one response. Pure queries are not recorded as `lastcommand`; known
effect-capable requests are recorded before handler execution.

## Network And MQTT Runtime

Network capability presence follows Zephyr Kconfig: DHCP uses
`CONFIG_NET_DHCPV4`, DNS uses `CONFIG_DNS_RESOLVER`, and SNTP uses
`CONFIG_SNTP`. App code should not add duplicate capability flags.

IPv4 configuration precedence is:

1. Runtime settings from the `ip` command.
2. Compile-time static IPv4 defaults.
3. Fallback service profile for direct laptop recovery.

At apply time the helper tries DHCP first when configured, otherwise static,
then fallback, then DHCP as the last attempt for static-first mode. The `ip`
command applies network-affecting changes at runtime through
`network_reconfigure()`; it no longer requires reboot for ordinary IPv4 profile
changes. Failed runtime reconfigure attempts restore the prior active profile.

DNS and NTP addresses are profile/settings data. Unsupported DNS/NTP fields are
reported by command code. Manual DNS is applied to Zephyr's resolver when DNS is
compiled in and a nonzero DNS server is configured; DHCP DNS is used when DHCP
provides it and `preferdhcpdns` is true.

MQTT broker hostnames are accepted only when they resolve before settings are
updated. Numeric IPv4 brokers do not require DNS. After a broker setting change,
the main loop disconnects and tries the new broker once; if that connection
fails, firmware restores the prior broker setting and emits a best-effort
`mqtt_broker_revert` warning.

SNTP is independent of manual `time` commands. Manual time setting updates
Zephyr's realtime clock; it does not mark SNTP state as manual. If SNTP is configured
and later succeeds, it will update the clock again, and failures remain visible
through `time`, `ip`, and status paths that report SNTP state.
SNTP network waits run in a low-priority SNTP thread, not on the system
workqueue and not in the command, MQTT, MEMS, or ADC timing paths.

## Hardware Control

Commands do not directly publish. For example, MEMS commands update
router-owned switch state that is applied by the timer-driven MEMS router
thread.
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
