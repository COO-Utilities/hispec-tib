# Temperature Sensing

```{eval-rst}
.. doxygenfile:: app/src/housekeeping.h
   :project: hispec_tib
```

The `temps` command reads an already-powered laser bank in the foreground.
Its `laserbank_c` averages valid probes with stopped TECs; it is unavailable
when every TEC is active, the bank is off or no idle probe is readable.
Individual diode temperatures remain present whether idle or regulated.
The command does not enable power or substitute the heater's aged cache.
