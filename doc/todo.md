# TODO Review Follow-Up

This page records owner-review items that remain after the documentation TODO
review. It is not a command specification; `commands.md` remains authoritative
for intended command behavior.

## Addressed in This Pass

- `commands.md` documents photodiode dark-measurement and dark-settings
  commands, fixes the `status` response topic spelling, removes stale MEMS
  response TODO text, and fixes the AS split route wording.

## LLM Resolved; Human Review Requested

- Hand-built command responses now have a final `_msg_builder()` size guard, and
  `memsroute` uses checked append logic. Some older fixed-shape responses still
  build into local buffers before that guard, so future edits should continue
  converting risky builders to checked append helpers when payload shape expands.
- TIB MEMS channels 7 and 8 are initialized as FFLS switches and use
  `MEMS_SWITCH_ELECTRICAL_PULSE_FFLS_MS` for pulse hold time, toggle-rate
  quantization, and stop-after accounting.


## Deferred Owner-Specified Capabilities

Do not design or implement these without a detailed owner specification:

- `measure_tput`.
- `laserbank/autowarm` and bank temperature management.
- Attenuator autocalibration and final default coefficient selection.
- Laser/laser-bank command interface expansion.
- Broad `command.c` refactoring into domain-owned command helpers.

## Remaining Implementation Items

- Temperature sensing currently exposes only ambient DS18B20 data. Decide
  whether inactive TEC temperatures belong in the temperature thread or in a
  future box-heater/bank-temperature loop.
- Thread priorities still need hardware-timing review.
- Consider whether `photodiode_publish_work` should be removed and photodiode
  telemetry should push directly to `outbound_queue`.
- Replace the terse `help` response with a maintained command summary
- Decide whether the all-switch `mems` query response is acceptable for the
  fixed MQTT payload budget or should be narrowed further.

## Remaining Documentation/Human-Review Items

- SNTP sync runs in system workqueue context and can block up to the SNTP
  timeout. Decide whether that is acceptable for this firmware.
- CAL switch and route names are still provisional.
- Reconcile MEMS electrical mode against hardware notes.
