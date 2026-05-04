# Nuisances

This page is informal development context, not firmware source of truth.

## Open items

- OpenOCD on macOS/Apple Silicon may need a Raspberry Pi sourced build for
  RP2350-era debugging.
- CLion users should enable RTOS integration and point GDB at the Zephyr SDK
  toolchain.
- STLink firmware and runner selection still depend on local workstation setup.

## LLM-resolved items requiring human review

- WIZnet-PICO iolibrary notes are retained as historical context. The current
  authoritative app target in this audit is the Nucleo H563ZI overlay.

## Local Setup Notes

Use the workspace virtual environment:

```bash
python3 -m venv .venv
source .venv/bin/activate
pip install west
west init -l hispec-tib
west update
west zephyr-export
west packages pip --install
```
