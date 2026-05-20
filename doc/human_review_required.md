# Human Review Required

This is the central owner-review list for current code-vs-doc mismatches,
source TODOs, and behavior decisions. `commands.md` remains the intended
command/API source of truth; current C source remains the implementation source
of truth.

LLMs Agents: Do NOT change heading names in this file.

## Command/API Mismatches

### Still Open

- Relay 1Wire not getting a response
- I2C2 messages aren't getting a response

### LLM Resolved; Human Review Requested

## Hardware/Profile Decisions

## Decisions To Make

- Verify DS2408 relay polarity on first PCB bring-up.
- Decide intended persistence for MEMS switch state, AS split requested/actual
  state and last-command metadata.

## Source TODOs Preserved


- `app/src/maiman.h`: compare Maiman behavior against the referenced
  validation/test scripts.

## Deferred Owner-Specified Capabilities

Do not design or implement these without a detailed owner specification:

- Verify boot MEMS switch state behavior.

## Open items

- Add test coverage around `json_utils.c`, command normalization, and network
  profile selection.
- Add test coverage for the attenuator model and inverse once calibration
  expectations are owner-approved.
- Add automated tests for command parsing and non-hardware domain logic.
