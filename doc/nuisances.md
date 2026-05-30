# Nuisances

This page is informal development context, not firmware source of truth.

## Local Setup Notes

- CLion users should enable RTOS integration and point GDB at the Zephyr SDK
  toolchain.
- STLink firmware and runner selection still depend on local workstation setup.

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
