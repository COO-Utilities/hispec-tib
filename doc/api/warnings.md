# Warnings

App producers call `app_output_emit(&args)` with `COO_CMD_RUNTIME_EMIT_WARNING`
or `COO_CMD_RUNTIME_EMIT_DATA`. The app queue owner delegates to the common
command-dispatch emitter, preserving local logging, payloads, and delivery policy.
See [warnings and telemetry](../warnings_and_telemetry.md) for queue behavior.

```{eval-rst}
.. doxygenfile:: app/src/app_output.h
   :project: hispec_tib
```
