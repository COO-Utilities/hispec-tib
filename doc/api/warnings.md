# Warnings

Warnings are emitted by `app_warning_emit()`, which is documented with the
command interface internals because it is now owned by `command.c`. Reusable
warning JSON construction and non-blocking queue emission are documented under
the common command-dispatch helpers.
