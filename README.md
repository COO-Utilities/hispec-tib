# HISPEC-FIB Zephyr firmware

Firmware for the TIB, calibration and achromatic-splitter PCBs, built on the
STM32H563 Nucleo with Zephyr. The application controls optical routing,
attenuation, photodiode measurement and, on TIB, the laser bank.

Start with the [documentation contents](doc/index.rst):

- [Hardware introduction](doc/hardware_overview.md), [electrical reference](doc/hardware.md)
  and [optical paths by profile](doc/hardware_profiles.md).
- [Mathematics and uncertainty](doc/mathematics.md), with links to each operation.
- [Control interface specification](doc/commands.md): the single command and
  telemetry reference for MQTT and serial.
- [Python command and lab helpers](doc/python_helpers.md): a convenience test
  tool with typed replies, collectors, fitting and plotting. The official Python
  API is maintained outside this repository.
- [Runtime architecture](doc/architecture.md), [settings](doc/settings.md) and
  [C contracts](doc/api/index.md).
- [Human review ledger](doc/human_review_required.md): unresolved intent,
  implementation and hardware questions.

## Firmware setup and build

Run from the parent Zephyr workspace, containing `hispec-tib/` and `.venv/`:

```bash
python3 -m venv .venv
source .venv/bin/activate
python -m pip install west
west init -l hispec-tib
west update
west zephyr-export
west packages pip --install
```

For an already initialized workspace, use its environment and build:

```bash
./.venv/bin/west build \
  --board=nucleo_h563zi/stm32h563xx \
  --build-dir ./hispec-tib/app/build \
  ./hispec-tib/app
```

CLion users should enable RTOS integration and select GDB from the Zephyr SDK.
ST-Link firmware and runner selection depend on the local workstation.
Board identity comes from the active-low straps in the
[hardware reference](doc/hardware.md), not a separate binary per profile.

## Documentation build

Install Doxygen through the workstation package manager and the Python
requirements into the workspace environment. From the parent workspace:

```bash
./.venv/bin/python -m pip install -r hispec-tib/doc/requirements.txt
./.venv/bin/python -m sphinx -b html -n -W --keep-going \
  hispec-tib/doc hispec-tib/doc/_build_sphinx/html
```

Sphinx runs Doxygen, reads Markdown through MyST and renders equations with
MathJax and diagrams with Mermaid. Open the generated `index.html` in a browser;
MathJax and Mermaid need access to their configured JavaScript resources.

Host-only Python setup and notebook use are in the
[Python helper guide](doc/python_helpers.md#setup-and-connection).

## Repository layout

| Path | Purpose |
|---|---|
| `app/src/` | Application firmware and header contracts |
| `app/boards/` | Board overlay |
| `include/coo_commons/`, `lib/coo_commons/` | Shared command, network, MQTT, JSON and control helpers |
| `drivers/gpio/ds2408/` | Local relay GPIO driver |
| `doc/` | Sphinx documentation and supporting datasheets/bench evidence |
| `pcb/` | Committed schematic, assembly, BOM and placement evidence |
| `tools/` | Python convenience helper, notebooks and lab captures |

## License

SPDX-License-Identifier: Apache-2.0

Copyright (c) 2025 Caltech Optical Observatories
