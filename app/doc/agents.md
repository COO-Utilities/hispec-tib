## Approach
- Low Complexity: "Keep it simple" principle", structure to minimize inter-task coordination complexity.
- Follow design patterns for Zephyr 4.4
- Assume code developed is to be maintained by non-expert embedded developers unfamiliar with either FreeRTOS or Zephyr so readability is important
- Use high-level functionality offered by Zephyr and avoid low-level APIs unless necessary. For example use console vs low-level characters on UART.
- Memory efficiency: Avoid dynamic allocation; use `string_view`, fixed buffers, and compile-time constants as appropriate for embedded systems.
- Structure modularly, support graceful degradation of functionality when hardware is not available if possible.
- Zephyr: Use Zephyr's tooling/drivers/libraries where possible
- C++ exceptions are disabled: error handling is done via return codes and checking object states.
- Build command discipline: use west in the `<thisdir>/.venv` for builds, and match the CLion command form with `--build-dir <thisdir>/hispec-tib/app/build` plus the explicit Ninja path when validating changes.
- Build sequencing discipline: when running both `--cmake-only` and full build checks, run them sequentially (never in parallel) if they share the same `--build-dir`.

## Notable files:
- Hardware context at hispec-tib/app/doc/hardware.md
- Command interface specification at: hispec-tib/app/doc/commands.md
- Known/planned libraries at: hispec-tib/app/doc/libraries.md
- Development status at: hispec-tib/app/doc/status.md
- Ignore breadboard.md
- nuisances.md is a set of reminders to humans about development nuisances.
