### Approach
- Low Complexity: KISS, minimal `tasks`, minimal inter-task coordination complexity.
- Devices (photodiodes, attenuators, switches) are instantiated once, lifetime-managed explicitly.
- Clear concurrency model: Only asynchronous hardware polling (photodiodes) is task-separated.
- Memory efficiency: Avoid dynamic allocation; careful use of `string_view`, fixed buffers, and compile-time constants.
- Structured around extensibility (e.g., laser diode driver support, additional sensors).
- Zephyr: Use Zephyr's tooling wherever possible vs rolling our own
- C++ exceptions are disabled: error handling is done via return codes and checking object states.
- Think Zephyr like arduino