# Human Review Required

This page is the central list of audit decisions, mismatches, and stale content
that should be reviewed by a project owner.

## Command Spec vs Code

- `commands.md` documents `measure_tput`, `lasersettings`, and
  `laserbank/autowarm`; none are implemented.
- `commands.md` documents `temp` alarm set behavior; code implements GET only.
- `commands.md` documents `status` response topic `stauts`; code uses
  `status`.
- `commands.md` documents optional GET payloads. Code treats non-empty MQTT
  payloads as SET unless `msg_type:"get"` is present.
- `status_get()` ignores optional `ip`, `lasers`, and `attens` request fields
  and returns a compact payload.
- `mqtt` docs and source TODOs disagree about whether broker and port should be
  combined as one field.
- `laser` command key parsing appears internally inconsistent and likely does
  not reach real Maiman registers through the documented topic shape.
- `reboot` is SET-only in code even though the intended interface reads like a
  no-payload action.
- `power` and `sleep` are implemented but need intended-spec review.

## Hardware vs Code

- Hardware docs describe two DAC7578 devices and twelve physical TIB FVOA
  channels. Code initializes six logical attenuator channels through one
  `dac7578` device handle.
- Hardware docs distinguish FFSW open-drain and FFLS push-pull MEMS drive.
  Current code uses raw GPIO expander pins and does not apply per-switch
  electrical mode.
- `devices.h` defines Modbus stop bits as two, while runtime setup configures
  one stop bit.
- CAL switch names and route names are explicitly provisional in source.

## Behavior That May Not Match Intent

- `laserbank/poweron`, `laserbank/poweroff`, and `laserbank/clearfaults` are
  registered as both GET and SET handlers, so bare queries perform actions.
- `sleep` parses a bool and returns OK but does nothing.
- Local `laser_t` values map `LASER_1028_Y` and `LASER_1270_J` to the same
  value.
- Photodiode dark calibration ownership is split: settings owns persisted
  values, while the sampler thread completes and optionally stores dark
  measurements.
- Warning publication can be dropped and is not visible in status fields.

## Stale Docs Removed or Rewritten

- `status.md` was rewritten from older platform notes into current Zephyr
  firmware status.
- `runtime_architecture.md` was rewritten to remove stale split-route wording
  and reflect current thread/queue/work structure.
- `libraries.md` was rewritten around current local wrappers and app modules.
- `nuisances.md` was kept informal and narrowed to current known annoyances.
- The root `README.md` was rewritten from an older W5500/Pico template into a
  current Nucleo-oriented overview with links to the audit pages.

## LLM-resolved items requiring human review

- Stale Pico/C++/Zyre status content was removed from the authoritative status
  page because current code is a Zephyr C firmware app.
- Stale splitter wording was replaced with current route names:
  `yj_calin -> yj_split` and `hk_calin -> hk_split`.
- Old library notes were consolidated into a current module/wrapper inventory.
- Old README build and command examples were replaced by the current Nucleo
  build command and links to implementation-derived command docs.

## Codex Judgment Calls

- Informal scratch content stayed labeled as informal instead of becoming an
  authoritative source.
- `hardware.md` was migrated but not rewritten from code assumptions.
- `commands.md` was migrated and lightly modernized where already safe, but
  implementation mismatches were documented in comparison pages instead of
  silently changing the intended spec.
- Source edits were limited to comments/Doxygen and did not intentionally
  change firmware behavior.
