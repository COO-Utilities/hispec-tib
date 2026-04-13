## Approach
- Low Complexity: "Keep it simple" principle", structure to minimize inter-task coordination complexity.
- Follow design patterns for Zephyr 4.3 
- Assume code developed is to be maintained by non-expert embedded developers unfamiliar with either FreeRTOS or Zephyr so readability is important
- Memory efficiency: Avoid dynamic allocation; use `string_view`, fixed buffers, and compile-time constants as appropriate for embedded systems.
- Structure modularly, support graceful degradation of functionality when hardware is not available if possible.
- Zephyr: Use Zephyr's tooling/drivers/libraries where possible
- C++ exceptions are disabled: error handling is done via return codes and checking object states.
- Build command discipline: use `~/zephyrproject/.venv/bin/west` explicitly for builds in this workspace, and match the CLion command form with `--build-dir /Users/jibailey/src/hispec-zephyr-mlang/hispec-tib/app/build` plus the explicit Ninja path when validating changes.
- Build sequencing discipline: when running both `--cmake-only` and full build checks, run them sequentially (never in parallel) if they share the same `--build-dir`.

## Notable files:
- Hardware context at hispec-tib/app/doc/hardware.md
- Command interface specification at: hispec-tib/app/doc/commands.md
- Known/planned libraries at: hispec-tib/app/doc/libraries.md
- Development status at: hispec-tib/app/doc/status.md
- breadboard.md and nuisances.md should be ignored.
