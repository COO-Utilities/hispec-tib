# Implementation Gaps

This page collects gaps found during the documentation audit. It is not a
firmware roadmap by itself; items need owner review before being treated as
requirements.

## Command/API Gaps

- `measure_tput` is documented but has no dispatch entry.
- `lasersettings` is documented but has no dispatch entry.
- `laserbank/autowarm` is documented but has no dispatch entry.
- `temp` set/alarm behavior is documented but not implemented.
- `status` returns a compact payload and does not implement the larger nested
  payload described in `commands.md`.
- `laser` command parsing appears unable to address a real laser with the
  currently dispatched key shape.
- `reboot` requires SET semantics in implementation even though docs imply a
  no-payload action.
- `power` and `sleep` exist in code but need spec review.

## Hardware/Profile Gaps

- TIB hardware uses two DAC7578 devices and twelve physical FVOA channels.
  Firmware currently exposes six logical attenuator channels and initializes
  one `dac7578` device.
- MEMS electrical mode is not represented per switch in firmware. Hardware
  notes distinguish FFSW open-drain and FFLS push-pull.
- Modbus stop-bit constants and runtime configuration disagree.
- CAL switch names have an explicit source TODO pending final fiber path names.
- Temperature sensing only exposes ambient DS18B20 data; laser-bank average and
  alarm lockout behavior are not implemented.

## Persistence Gaps

The following runtime state is not persisted:

- MEMS switch state.
- Active routes.
- Split requested/actual state.
- Laser-bank power state.
- DS2408 relay output state.
- Laser current, pulse, temperature, and tuning state.
- Last-command status.

## Telemetry and Warning Gaps

- Warning delivery is lossy by design and is not mirrored into sticky status.
- Photodiode telemetry has a hardcoded device id/topic.
- Command response JSON is hand-built in fixed buffers; some overflow paths are
  handled better than others.
- Photodiode queue-full policy purges telemetry and retries the current sample.

## Source TODOs Preserved

- `app/src/photodiode.h`: ADC resolution should come from devicetree.
- `app/src/mems_switching.c`: MEMS tick may perform excess I2C bus activity.
- `app/src/mems_switching.c`: verify first-boot unknown-state behavior.
- `app/src/main.c`: temperature thread priority should probably be lowest.
- `app/src/devices.c`: final CAL route/switch names need owner decision.
- `app/src/command.c`: MQTT host/port schema TODO tagged `-JIB`.
- `app/src/command.c`: internal route-error TODO.
- `app/src/command.c`: `sleep` TODO.
- `app/src/attenuator.c`: attenuator calibration/nonlinearity TODO.
- `lib/coo_commons/network.c`: DHCP/network helper necessity TODOs.
- `hardware.md`: photodiode ADC clamp TODO.
