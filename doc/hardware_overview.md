# Hardware

The same controller design serves three assemblies: the Trunk Interface Box
(TIB), Achromatic Splitter (AS), and calibration switchyard (CAL, populated for
YJ or HK). Firmware selects one of four profiles using a board-identity strap.

TIB launches controlled laser light and measures returned light; AS divides light
among outputs by time switching; CAL selects calibration sources and destinations.
The optical switches latch their path, while variable attenuators continuously
set transmission from analog drive voltage. Ethernet/MQTT and the serial console
control the electronics; neither a command ACK nor a saved switch state proves
optical continuity.

Read [fiber paths and profile names](hardware_profiles.md) to follow the light.
Use [electronics and repair reference](hardware.md) to trace a command to its
bus, component and connector. The latter identifies source-document revisions
and unresolved differences with the installed system.

```{toctree}
:maxdepth: 2

hardware_profiles.md
hardware.md
```
