# Command Interface Internals

The shared command specification includes separate `ota_query_allowed` and
`ota_effect_allowed` fields, both false by default. Admission is checked at
execution time; see [OTA integration](ota.md) and the
[application allowlist](../commands.md#commands-while-ota-is-active).

```{eval-rst}
.. doxygenfile:: app/src/command.h
   :project: hispec_tib

.. doxygenfile:: app/src/attenuator_command.h
   :project: hispec_tib

.. doxygenfile:: app/src/laser_command.h
   :project: hispec_tib

.. doxygenfile:: app/src/photodiode_command.h
   :project: hispec_tib

.. doxygenfile:: app/src/throughput_command.h
   :project: hispec_tib
```
