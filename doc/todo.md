- make ch 7/8 on TIB use MEMS_SWITCH_ELECTRICAL_PULSE_FFLS_MS for switch pulse width and disallow splitting 

- `hardware.md` describes two DAC7578 devices and twelve physical FVOA channels
  on TIB. Current code exposes six logical attenuator channels and only uses the
  `dac7578` devicetree label.

- `devices.h` defines `MODBUS_STOPBITS` as two stop bits, while
  `setup_modbus_client()` currently configures one stop bit.

- doumentation needs a true hardware details page. make Hardware have subpages of hardware details and present hardware profiles page

- remove the ADC1115 mutex, it is wholly unnecessary, remove it from documentation
- add grabbing the inactive-tec temps to the tempsense thread and folding them into the available system temp OR make sure that is handled by the box heater loop.


- check that the current SNTP handler's blocking up to the SNTP timeout isn't an issue for the zephyr thead/work system

- decide on thread priorities, lower is more important, and update docs


- interaction of photodiode_publish_work and outbound queue could result in some ADC messages being stale unless adc messages are flagged as best effort so FLAG ADC messages as best effort
  - can we just make photodiode queue push directly to the outbound queue?

- ensure ADC sample drop is noted in publish description

- drop /tib from stored settings key 


- finialize/update settings defaults for e.g. Attenuator coefficients

-
- Photodiode telemetry hardcodes the device id and topic, make this consistent with how device-id/topics are set for other publishes

- review all MQTT response payloads that are hand-built strings; buffer overflow paths are handled inconsistently across commands.

- !!!! URGENT "Correlation data is echoed when it fits the fixed response buffer." this is not acceptable. CORRELATION DATA MUST BE output for system correctness

- fail to boot if watchdog can't be made ready. Update doc/mermaid 

- document what a settings load failure at boot means: Any settings load fail other than firstboot is an failure that requires human intervention, to at a minimum, reinit settings 

- allow mqtt gets while serial is active

- ensure that in photodiode monitoring thread noise above thresh threashold and noise monitoring are common to dark/non-dark measure path, that any above threashold does not stop thread and that mermaid diagram isn't incorrectly implying loop termination after app_warning_emit photodiode_noise (loop should not ever terminate)

- see if tempsense can get rid of mutexes or if needed update mermail to clarify it is for reading not device access

- make the help command

- docs for memsroute suggest the order needs flipping still, figure out and clean up what needs cleanup. the  flip this so it is dest:source and dest that have no source are "no source"


- simplify mems command response to see mems/<switchname> 

- sort out "TODO this response bloated to likely beyond what is reasonable MQTT" for the mems query and update docs

- !!!! MAJOR acutually implement measure_tput¶

- add right side toc overview so at least command endpoints page has a clickable for each command

- fix markdown formatting of | `power` | `cmd/hsfib-tib/req/power` | `cmd/hsfib-tib/resp/power` | `power [on|off]` |

- figure out what "pd dark-measurement actions are implementation-specific." means

- Laserbank power actions are implemented without autowarm or deep driver fault detection. fix as part of bank temp manager

- document photodiode dark management commands in commands.md