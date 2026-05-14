# Human Review Required

This is the central owner-review list for current code-vs-doc mismatches,
source TODOs, and behavior decisions. `commands.md` remains the intended
command/API source of truth; current C source remains the implementation source
of truth.

LLMs Agents: Do NOT change headings in this file.

## Command/API Mismatches

- `commands.md` documents `measure_tput`, but `command.c` has no dispatch
  entry for it.
- `commands.md` documents `lasersettings`, but `command.c` has no dispatch
  entry for it.
- `commands.md` documents `temp` set/alarm behavior. Current code registers
  `temp` as GET-only and returns cached ambient temperature plus
  `laserbankavg_c:null`.
- `commands.md` documents a larger nested `status` payload. Current
  `status_get()` returns firmware version, boot count, uptime, board/network
  state, MEMS switch count, IP, and laser-bank power only.
- `commands.md` documents optional GET payloads for some commands. Current MQTT
  ingress treats a non-empty payload as SET unless JSON includes
  `msg_type:"get"`.
- The raw `laser` command dispatch accepts `laser/...`, but the handler parses
  the first path segment and then calls `get_laser_channel(laser_name + 5)`.
  A documented key such as `laser/1028y/current` therefore does not appear to
  address a real Maiman register.
- `reboot` is SET-only in code. Empty MQTT payloads and bare serial `reboot`
  commands are rejected as unsupported.
- `laserbank/poweron`, `laserbank/poweroff`, and `laserbank/clearfaults` are
  registered as both GET and SET handlers, so exact bare queries perform power
  actions.
- The help response includes aggregate names such as `laserbank` and `power`,
  not the exact currently implemented endpoint list.

## Hardware/Profile Decisions

- CAL switch and route names remain provisional in `devices.c` pending final
  fiber-path names.

## Decisions To Make

- laser output state and Maiman TEC/current setpoints are not restored after reboot.
- DS2408 relay output state is intentionally not restored after reboot; the
  local driver defaults the expander outputs low/off. Verify DS2408 relay polarity on
  first PCB bring-up.
- Decide whether `temp` should expose the laser-bank heater control loop's TEC
  cache or remain ambient-only.
- Decide whether the SNTP work item may continue blocking in the Zephyr system
  workqueue while waiting for an SNTP reply.
- Review thread priorities after hardware timing tests. `main.c` still tags the
  temperature thread priority with a source TODO.

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
- `app/src/main.c`: temperature thread priority should probably be lowest.
- `app/src/maiman.h`: compare Maiman behavior against the referenced
  validation/test scripts.
- `lib/coo_commons/network.c`: review whether small network parsing wrappers
  are still useful or should be removed.

  
## Deferred Owner-Specified Capabilities

Do not design or implement these without a detailed owner specification:

- `measure_tput`.
- Attenuator autocalibration and final default coefficient selection.
- Higher-level laser/laser-bank command interface expansion.
- Broad `command.c` refactoring into domain-owned command helpers.
- Replacing the terse `help` response with a maintained command summary.

