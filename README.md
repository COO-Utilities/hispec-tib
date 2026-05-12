# HISPEC-FIB Zephyr RTOS Firmware

This repository contains Zephyr RTOS firmware for HISPEC FIB PCB controllers.
The current audited application is `app`, written in C, with MQTT and serial
command interfaces for board-profile-specific optical routing, attenuator,
photodiode, laser-bank, settings, network, SNTP, and telemetry behavior.

## Current Target

The maintained build target for this audit is:

```bash
./.venv/bin/west build \
  --board=nucleo_h563zi/stm32h563xx \
  --build-dir ./hispec-tib/app/build \
  ./hispec-tib/app
```

Use the workspace virtual environment for Python and west commands:

```bash
./.venv/bin/python
./.venv/bin/west
```

## Hardware Profiles

Board identity is selected by active-low strap GPIOs documented in
`doc/hardware.md` and configured in `app/boards/nucleo_h563zi.overlay`.

Implemented firmware profiles:

- `tib`: MEMS routing, six logical attenuator channels, photodiodes, laser-bank
  power, relay GPIOs, Maiman Modbus, network/MQTT, SNTP, settings, watchdog.
- `cal_yj`: calibration MEMS routes and one logical attenuator channel.
- `cal_hk`: same firmware profile shape as `cal_yj`.
- `as`: achromatic splitter MEMS routes and split-ratio command support.
- `unknown`: selected when board straps are missing or conflicting; board
  hardware setup is refused.

`doc/hardware.md` is the hardware source of truth. `doc/hardware_profiles.md`
documents how current code maps those hardware facts into firmware profiles.

## Command Interfaces

MQTT requests use:

```text
cmd/hsfib-tib/req/#
```

Default command responses use:

```text
cmd/hsfib-tib/resp/<key>
```

MQTT 5 `response_topic` and `correlation_data` are honored when they fit the
fixed command buffers.

Serial commands share the same normalized command path. A bare serial key is a
GET; a key with payload is a SET. See:

- `doc/commands.md` for intended command/API behavior.
- `doc/implemented_commands.md` for the implementation-derived command list.
- `doc/command_implementation_audit.md` for mismatches and stale behavior.

## Runtime Shape

- `main.c`: boot order, watchdog feed, network/MQTT loop, outbound publish.
- `command.c`: MQTT/serial ingress, serial guard, dispatch, response queues.
- `devices.c`: board strap detection and board-profile setup.
- `mems_switching.c`: MEMS switch state, routes, and toggler work.
- `attenuator.c`: DAC-backed logical attenuator control and calibration.
- `maiman.c` and `lasers.c`: Maiman Modbus and laser-bank helpers.
- `photodiode.c`: ADS1115 sampling, dark calibration, noise, telemetry.
- `tempsense.c`: DS18B20 ambient temperature cache.
- `sntp_sync.c`: SNTP sync and time status.
- `app_settings.c`: Zephyr settings-backed app state.
- `app_warning.c`: best-effort warning publication.

Architecture pages live in `doc/architecture.md`, `doc/threads.md`, and `doc/queues_and_work.md`.

## Documentation Build

Documentation is Markdown-first Sphinx with Doxygen XML extraction through
Breathe and Mermaid diagram rendering:

```bash
./.venv/bin/python -m sphinx -b html hispec-tib/doc hispec-tib/doc/_build_sphinx/html
```

The docs build requires:

- Sphinx
- alabaster
- myst-parser
- breathe
- sphinxcontrib-mermaid
- Doxygen

Python package requirements are listed in `doc/requirements.txt`. Doxygen runs
from the Sphinx build as a pre-step.

## Repository Layout

```text
hispec-tib/
  app/                 Zephyr application
  app/src/             Firmware C sources and headers
  app/boards/          App board overlays
  doc/                 Sphinx, Markdown, Doxygen, and audit docs
  include/coo_commons/ Shared wrapper headers
  lib/coo_commons/     Local network, MQTT, JSON, and PID helpers
  drivers/gpio/ds2408/ Project-local DS2408 GPIO driver
```

`app/doc` now contains short migration stubs pointing to `doc`.

## Open items

- Review W5500/Pico references and remove
- Reconcile DAC7578/twelve-FVOA hardware description with current six logical
  attenuator channels and single `dac7578` device handle.
- Resolve command/spec mismatches listed in `doc/human_review_required.md`.


## License

SPDX-License-Identifier: Apache-2.0

Copyright (c) 2025 Caltech Optical Observatories
