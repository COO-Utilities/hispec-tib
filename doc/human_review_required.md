# Human Review Required

This is the central owner-review list for current code-vs-doc mismatches,
source TODOs, and behavior decisions. `commands.md` remains the intended
command/API source of truth; current C source remains the implementation source
of truth.

LLMs Agents: Do NOT change heading names in this file.

## Command/API Mismatches

### Still Open
- `help_get()` returns a static command-name summary. `commands.md` describes a
  command summary with device info, and the current string is not a complete
  endpoint list with suffix forms such as `laserbank/power`,
  `laserbank/heater`, `laser/status`, `pdsettings/<yj|hk>`, or `split/<yj|hk>`.
- Serial guard documentation says safe read-only MQTT requests are allowed while
  the guard is active. Current `mqtt_get_allowed_during_serial_guard()` blocks
  all `laserbank/*` and `laser` read-like requests, including
  `laserbank/power` and `laserbank/heater` queries, because this class of
  handlers historically had side effects. Decide whether the stricter behavior
  is intended or whether individual read-only endpoints should be allowed.

### LLM Resolved; Human Review Requested

## Hardware/Profile Decisions

## Decisions To Make

- Verify DS2408 relay polarity on first PCB bring-up.
- Decide intended persistence for MEMS switch state, AS split requested/actual
  state and last-command metadata.

## Source TODOs Preserved

- `app/src/photodiode.h`: ADC resolution should come from devicetree.
- `app/src/maiman.h`: compare Maiman behavior against the referenced
  validation/test scripts.

## Deferred Owner-Specified Capabilities

Do not design or implement these without a detailed owner specification:

- Attenuator autocalibration and final default coefficient selection.
- Broad `command.c` refactoring into domain-owned command helpers.
- Verify boot switch state behavior.

## Open items

- Add test coverage around `json_utils.c`, command normalization, and network
  profile selection.
- Add test coverage for the attenuator model and inverse once calibration
  expectations are owner-approved.
- Add automated tests for command parsing and non-hardware domain logic.
- Add an optional maximum flux level to `measure_throughput`.
