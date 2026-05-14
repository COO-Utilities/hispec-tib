# TODO Review Follow-Up

This page records owner-review items that remain after the documentation TODO
review. It is not a command specification; `commands.md` remains authoritative
for intended command behavior.

## Addressed in This Pass


## LLM Resolved; Human Review Requested

- Hand-built command responses now have a final `_msg_builder()` size guard, and
  `memsroute` uses checked append logic. Some older fixed-shape responses still
  build into local buffers before that guard, so future edits should continue
  converting risky builders to checked append helpers when payload shape expands.

## Deferred Owner-Specified Capabilities

Do not design or implement these without a detailed owner specification:

- `measure_tput`.
- Attenuator autocalibration and final default coefficient selection.
- Laser/laser-bank command interface expansion.
- Broad `command.c` refactoring into domain-owned command helpers.
- Replace the terse `help` response with a maintained command summary

## Remaining Implementation Items

- Temperature sensing currently exposes only ambient DS18B20 data. Decide
  whether the `temp` command should include the laser-bank control loop's TEC
  temperature cache.
- Thread priorities still need hardware-timing review.

## Remaining Documentation/Human-Review Items

- SNTP sync runs in system workqueue context and can block up to the SNTP
  timeout. Decide whether that is acceptable for this firmware.
- CAL switch and route names are still provisional.
- Reconcile MEMS electrical mode against hardware notes.
