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

MQTT 5 `response_topic` is honored when it fits the fixed topic buffer.
`correlation_data` is echoed when it is 16 bytes or less.

Serial commands share the same normalized command path. A bare serial key is a
GET; a key with payload is a SET. See:

- `doc/commands.md` for intended command/API behavior.
- `doc/implemented_commands.md` for the implementation-derived command list.
- `doc/command_implementation_audit.md` for mismatches and stale behavior.

## Runtime Shape

- `main.c`: boot order, watchdog feed, network/MQTT loop, outbound publish.
- `command.c`: app command queues, serial guard policy, command table, and
  command handlers, using `lib/coo_commons/command_dispatch.c` for reusable
  MQTT/serial request and response mechanics.
- `devices.c`: board strap detection and board-profile setup.
- `mems_switching.c`: MEMS switch state, routes, and toggler work.
- `attenuator.c`: DAC-backed logical attenuator control and calibration.
- `maiman.c` and `lasers.c`: Maiman Modbus and laser-bank helpers.
- `photodiode.c`: ADS1115 sampling, dark calibration, noise, telemetry.
- `tempsense.c`: DS18B20 ambient temperature cache.
- `sntp_sync.c`: SNTP sync and time status.
- `app_settings.c`: app-owned runtime settings and direct Zephyr NVS persistence.
- `app_identity.c`: selected board-profile MQTT device ID.

Architecture pages live in `doc/architecture.md`, `doc/threads.md`, and `doc/queues_and_work.md`.

## Python Quick Start

For local Python testing, install Python 3.11 or newer with `venv` support.
On macOS/Linux, run these commands from the cloned repository directory
(the directory containing this README):

```bash
python3 -m venv .venv
source .venv/bin/activate
python -m pip install -r tools/requirements.txt
cd tools
python -m notebook
```

Open a lab notebook or create a Python notebook and import the helper:

```python
import hispec_fibpcb as hspcb
```

For a terminal IPython session, run this from `tools` with the environment active:

```bash
ipython -i -c "import hispec_fibpcb as hspcb"
```

Each clone gets its own `.venv`; it is ignored by Git and should not be copied
between machines. For later sessions, activate it from the repository directory
with `source .venv/bin/activate`, then `cd tools` and launch Jupyter or IPython.
The notebook kernel uses this environment without global kernel registration.

This minimal host-tool setup includes analysis, plotting, and interactive widgets.
Optional oscilloscope integration requires additional instrument-specific packages.
Connecting to a board requires a reachable MQTT broker and the device settings
in the notebook. Importing the helper alone does not connect to hardware.

The firmware build commands elsewhere in this README run from the parent Zephyr
workspace and use its separate `.venv`. That existing environment can also install
these packages with `./.venv/bin/python -m pip install -r hispec-tib/tools/requirements.txt`
from the workspace directory. The clone-local setup above does not install Zephyr
build tools.

## Throughput Lab Notebook

Open `tools/throuput_monitor_lab.ipynb` using the environment from the Python
quick start above, which includes the `ipympl` interactive Matplotlib backend.

The throughput section provides a fixed receive-log widget (500 records,
updated at most twice per second), binary acquisition, a nonblocking six-panel
dashboard, snapshot export, and explicit
selected-laser shutdown. Use `%matplotlib widget`; keep the returned animation
referenced. Pause/close affects display only. `monitor.stop()` stops the firmware
measurement and the laser used by its autolevel operation; purely passive
monitoring leaves manual laser output alone. The notebook also explicitly sets
the selected laser level to zero. Bank power and TECs remain available. This
shutdown behavior requires firmware built with the corresponding
throughput-monitor change.

Throughput packets feed collection directly and are excluded from logging.
The dashboard shows logarithmic throughput with a linked dB-loss axis, PD input
with the 400–1600 mV control band, detector and total-throughput S/N, source
current/attenuation, estimated laser output (µW), and route-corrected
delivered/detected power (nW). Firmware sends one fresh conversion per channel
every 50 ms; the dashboard refreshes at most 4 Hz. Overrange points are nominal
throughput lower bounds, with no PD/throughput error band or S/N. Calibration uncertainty can
limit total S/N even when detector S/N is high. Only the displayed record tail
is converted per frame. Display gaps for nonpositive log values or undefined
S/N do not change the underlying records or CSV exports.

For externally supplied light, use
`pcb.measure_throughput("none", channel="hk", fiber="S", autolevel=False, collect=True)`.
It selects and corrects the SM return while leaving launch switching untouched.
An optional `input`/`output` pair selects a calibration launch. PD power/error
and detector S/N remain available; source and throughput fields are undefined.

Both photodiodes can stream; firmware permits only one autolevel owner because
the instrument light paths overlap. See [sampling and uncertainty](doc/photodiode_notes.md)
for the error budget, missing-sample behavior, and 64 SPS converter option.

Saved outputs are historical observations. Run cells individually; dark,
calibration, and manual laser tests are separate lab operations.

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

- Resolve command/spec mismatches listed in `doc/human_review_required.md`.


## License

SPDX-License-Identifier: Apache-2.0

Copyright (c) 2025 Caltech Optical Observatories
