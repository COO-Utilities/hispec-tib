# Human Review Required

This is the central owner-review list for current code-vs-doc mismatches,
source TODOs, and behavior decisions.

LLMs Agents: Do NOT change heading names in this file.

## PCB Validation
- [ ] Verify boot MEMS switch state behavior.
- [ ] Verify ADC levels with scope
- [ ] Verify DAC levels with scope pre/post opamp
- [ ] Sort out MEMS loop and ADC loop timing overruns
- [ ] Validate MODBUS comms with NMH & a spare driver

## Command/API Mismatches

### LLM Resolved; Human Review Requested

- [ ] Confirm serial response pretty-printing is readable in CoolTerm and CLion.
- [ ] Confirm serial `help` content is useful enough for bring-up and matches
  expected operator wording.
- [ ] Confirm command options in serial help are complete enough for current
  workflow.

## Decisions To Make

- Decide intended persistence for MEMS switch state, AS split requested/actual state and last-command metadata.

## TODOs

- `app/src/maiman.h`: compare Maiman behavior against the referenced validation/test scripts.

## Deferred Owner-Specified Capabilities

Do not design or implement these without a detailed owner specification:

- Verify boot MEMS switch state behavior.

## Test items

- Add test coverage around `json_utils.c`, command normalization, and network profile selection.
- Add test coverage for the attenuator model and inverse once calibration expectations are owner-approved.
- Add automated tests for command parsing and non-hardware domain logic.
