# Human Review Required

This is the central owner-review list for current code-vs-doc mismatches,
source TODOs, and behavior decisions. `commands.md` remains the intended
command/API source of truth; current C source remains the implementation source
of truth.

LLMs Agents: Do NOT change heading names in this file.

## Command/API Mismatches

### Still Open
- `commands.md` documents a larger nested `status` payload. Current
  `status_get()` returns firmware version, boot count, uptime, board/network
  state, MEMS switch count, IP, and laser-bank power only.
- `commands.md` documents optional GET payloads for some commands. Current MQTT
  ingress treats a non-empty payload as SET unless JSON includes
  `msg_type:"get"`.
- `reboot` is SET-only in code. Empty MQTT payloads and bare serial `reboot`
  commands are rejected as unsupported.
- The help response includes aggregate names such as `laserbank` and `power`,
  not the exact currently implemented endpoint list.

### LLM Resolved; Human Review Requested

- `lasersettings` was replaced with `laser/settings`, matching the implemented
  dispatch table.
- Legacy `laserbank/poweron` and `laserbank/poweroff` API documentation was
  replaced with `laserbank/power` and explicit `auto|override_on|override_off`
  modes.
- `temp` is intentionally GET-only and now includes per-laser TEC temperatures
  when the TIB laser bank is powered.
- The documented `laser` command is now a level/status command with
  `laser/status`, `laser/engstatus`, `laser/tune`, and `laser/settings`
  subcommands. The old raw-register laser handlers remain in source for now but
  are no longer in the dispatch table.
- `laserbank/clearfaults` now only power-cycles an already-powered bank when at
  least one driver reports an overcurrent fault.

## Hardware/Profile Decisions

- CAL switch and route names remain provisional in `devices.c` pending final
  fiber-path names.

## Decisions To Make

- laser output state and Maiman TEC/current setpoints are not restored after reboot.
- Verify DS2408 relay polarity on first PCB bring-up.
- Decide intended persistence for MEMS switch state, AS split requested/actual
  state, laser output/tuning state, and last-command metadata.

## Source TODOs Preserved

- `app/src/photodiode.h`: ADC resolution should come from devicetree.
- `app/src/mems_switching.h`: MEMS switch pin fields may want stronger
  constness.
- `app/src/mems_switching.c`: review static/non-static helper nesting.
- `app/src/mems_switching.c`: verify first-boot unknown-state behavior.
- `app/src/mems_switching.c`: review whether router switch lookup should be a
  non-static helper instead of a nested local helper.
- `app/src/command.c`: internal split-route error is marked as theoretically
  impossible if compiled route tables are correct.
- `app/src/devices.c`: final CAL switch/route names need an owner decision.
- `app/src/maiman.h`: compare Maiman behavior against the referenced
  validation/test scripts.
- 
## Deferred Owner-Specified Capabilities

Do not design or implement these without a detailed owner specification:

- Attenuator autocalibration and final default coefficient selection.
- Higher-level laser/laser-bank command interface expansion.
- Broad `command.c` refactoring into domain-owned command helpers.
- Replacing the terse `help` response with a maintained command summary.


## Open items

- Add test coverage around `json_utils.c`, command normalization, and network
  profile selection.
- Add test coverage for the attenuator model and inverse once calibration
  expectations are owner-approved.
- Add automated tests for command parsing and non-hardware domain logic.
