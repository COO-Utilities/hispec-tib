# Electronics and repair reference

This page owns physical mappings and assumptions. [Fiber paths](hardware_profiles.md)
connect those mappings to API names. A firmware profile describes what is used;
a BOM describes assembly population. Neither alone certifies an installed board.

## Source drawings and revision boundaries

The repository contains the following evidence, added in commit `2fd8e87`:

- {download}`Schematic print <../pcb/FIB_Switcher TIB Schematic Print.PDF>`:
  52 PDF pages, with some sheets dated 21 October 2026. PDF page numbers below
  are viewer page numbers, since repeated Altium sheet numbers are ambiguous.
- {download}`PCB print <../pcb/FIB_Switcher PCB Print.PDF>`: 23 pages of layers
  and assembly views; use component designators to locate the electrical stage.
- {download}`TIB BOM <../pcb/FIB_Switcher_TIB_BOM.xlsx>` and
  {download}`NoTIB BOM <../pcb/FIB_Switcher_NoTIB_BOM.xlsx>`: `Manufacturing`
  sheets dated 4 September 2026, 12:18 and 12:19 respectively.
- {download}`TIB placement <../pcb/FIB_Switcher_TIB.txt>` and
  {download}`NoTIB placement <../pcb/FIB_Switcher_NoTIB.txt>`: 16 July 2026,
  design revision `59e398672786f7c0d6aec813e7c4099687d5be41`.
  NoTIB contains optical patch comments for CAL and AS.

| Drawing pages | Use |
|---|---|
| 1, 10, 52 | AS, CAL and TIB hierarchy/component names |
| 9 | DACs U8/U9, references U12/U13 and TP2 |
| 39–44 | Repeated OPA2991 FVOA-drive stages |
| 45–48 | Controller hierarchy and Nucleo connector nets/straps |
| 49 | ADS1115 U4 and PD input networks |
| 50 | Power circuitry |
| 51 | PCAL6416 U7, MEMS drive mapping and TP1 |

The PD scaling follows the resistor network on page 49; its voltage annotation
is obsolete. The [input-chain description](#tib-photodiode-monitoring-adc) below
uses the component values and connections.

**Differences requiring physical review:** FVOA stages show 4.7 kΩ/2.43 kΩ feedback, giving
nominal gain $1+2.43/4.7=1.5170$, while firmware defaults to 1.533. NoTIB placement
comments reverse the red AS first-switch branch destinations relative to the
firmware route interpretation. These are [owner-review items](human_review_required.md);
the installed configuration matters when interpreting those drawings.

## Controller, buses and connectors

The controller is a Nucleo H563ZI (MB1404), STM32H563ZIT6 Cortex-M33, with 2 MB
flash and 640 KB SRAM. See the bundled
{download}`Nucleo manual <um3115-stm32h5-nucleo144-board-mb1404-stmicroelectronics.pdf>`
and {download}`MCU reference <stm32h563zi.pdf>`.

| Function | Nucleo signal / connector | Peripheral / destination |
|---|---|---|
| PD ADC SCL/SDA | PB8/PB9, D15/D14, CN7 2/4 | I2C1, 400 kHz; U4 ADS1115 at 0x48 |
| DAC/MEMS SCL/SDA | PF1/PF0, D69/D68, CN9 19/21 | I2C2, 400 kHz; DAC 0x48/0x4A, PCAL 0x21 |
| Laser Modbus RX/TX | PD6/PD5, D52/D53, CN9 4/6 | USART2, THVD1429 RS-485 |
| RS-485 enable | PD4, D54, CN9 8 | GPIO active high |
| Laser-bank enable | PB2, D72, CN9 13 | GPIO active low, default off |
| Relay 1-Wire | PE9, D71, CN9 15 | UART12 RX, TX/RX swap, single-wire open drain |
| Ambient 1-Wire | PG1, D64, CN9 30 | UART9 TX, single-wire open drain, DS18B20 |
| FFLS status 1/2 | PF7/PF9, D62/D63 | PCB pull-ups; overlay inputs, unused by firmware |

The two devices at address 0x48 are on different buses. Check bus identity before
interpreting an address scan. `app/boards/nucleo_h563zi.overlay` supplies the exact
pin configuration; `devices.c` maps profile names and logical channels.

Required MB1404 solder bridges: PB2 uses SB61 OFF/SB66 ON; PE9 uses SB35 OFF/SB67
ON. The installed-clock assumption is “HSE not used”: SB48/SB50 ON, SB49 OFF,
SB3/SB4 OFF. HSE pins PH0/PH1 are distinct from I2C2 PF0/PF1. Firmware therefore
uses HSI/PLL at 240 MHz, not an HSE source. LSE 32768 Hz clocks RTC and periodic
LPTIM4 uptime at 4096 ticks/s. LSI clocks the independent watchdog. No automatic
STOP states or RTC retention across complete power removal are assumed.

## MEMS drive and switch identity

U7 is a PCAL6416AHF,128: 3.3 V I2C logic, 5 V port supply, ADDR tied high for
0x21. Its outputs drive external transistor stages. The overlay uses push-pull,
pull-ups and **active-low PCAL logic**: logical inactive gives an external low
control line, and logical active creates a high pulse at the switch. A GPIO
`output-low` flag describes logical state, not necessarily low voltage at the pin.

| Stage | Parts verified in schematic/BOM | Function |
|---|---|---|
| FFSW | Two NTGD4167CT1G complementary MOSFET packages per switch, Q6/Q7; 4.7 kΩ resistors | Coil drive for 20 ms firmware pulses |
| FFLS (TIB only) | DMP3085LSD-13 dual P-channel packages Q4/Q5 | Large-core switch drive for 50 ms pulses |

TIB BOM rows 32–33 and NoTIB row 26 agree with the schematic's stage names. Both switch types latch;
firmware limits actuation to five pulses/s. FFLS status low indicates position B,
high A, but firmware does not consume this feedback. See the bundled
{download}`FFSW datasheet <datasheets/FFSW 1x2 Switch.pdf>` and
{download}`FFLS datasheet <datasheets/FFLS 1x2 Switch.pdf>`.

| PCB channel | PCAL A/B GPIO indices | Port bits |
|---|---|---|
| sw1 | 6 / 7 | P0.6 / P0.7 |
| sw2 | 8 / 9 | P1.0 / P1.1 |
| sw3 | 4 / 5 | P0.4 / P0.5 |
| sw4 | 10 / 11 | P1.2 / P1.3 |
| sw5 | 12 / 13 | P1.4 / P1.5 |
| sw6 | 14 / 15 | P1.6 / P1.7 |
| sw7 | 2 / 3 | P0.2 / P0.3 |
| sw8 | 0 / 1 | P0.0 / P0.1 |

TP1 on schematic page 51 exposes drive-side nets. Its physical header pin numbers
are not PCAL bit numbers. Use [profile assignments](hardware_profiles.md) to map
sw1…sw8 to installed switch designators before probing.

## TIB and CAL attenuator drive

TIB populates DAC7678 U8/U9, REF3333 U12/U13 and six dual OPA2991 drive stages;
NoTIB's BOM populates U9/U13 and only FVOADrive5. The latter is consistent with
CAL's one logical pair, not a separately verified AS population manifest.

DAC AVDD/reference are 3.3 V; LDAC is grounded. Ideal 12-bit output is
$V=3.3n/4096$ volts for code 0…4095. Amplifiers drive the FVOAs toward 5 V.
Firmware's gain is a model coefficient; distinguish it from the resistor-derived
gain and the measured post-amplifier voltage. The FVOA datasheet allows 6 V;
lab resistance observations were 530–616 Ω. Verify actual supply/headroom and
loading before interpreting a saturated drive as extra attenuation.

| API channel | PCB attenuator label | DAC/address | DAC1 / DAC2 outputs |
|---|---|---|---|
| `1028y` | Y / pair 1 | U8 / 0x4A | A / C |
| `1270j` | J / pair 2 | U8 / 0x4A | E / G |
| `1430yj` | YJATC / pair 3 | U8 / 0x4A | D / F |
| `1430hk` | HKATC / pair 4 | U9 / 0x48 | A / C |
| `1510h` or CAL `lfc` | H / CAL / pair 5 | U9 / 0x48 | E / G |
| `2330k` | K / pair 6 | U9 / 0x48 | D / F |

The firmware CAL index is **4, zero-based**, meaning physical pair 5. TP2 exposes
pre/post-amplifier nets; consult page 9 for its pin mapping. Two successful DAC
writes are consecutive I2C operations, not simultaneous latching. Refer to the
[attenuator model](attenuator.md), [calibration](attenuator_calibration.md), and
{download}`FVOA datasheet <datasheets/FVOA_atten.pdf>` for optical interpretation.

(photodiode-monitoring)=
## TIB photodiode monitoring ADC

U4 ADS1115 channel A0 samples YJ (J13); A2 samples HK (J12), single-ended. Nominal
detectors are Femto FWPR-20-IN and Thorlabs PDA10DT. Firmware uses ±2.048 V range,
62.5 µV/code and 64 SPS converter rate, producing a fresh sample on each channel
at 20 Hz. It treats 2000 mV as the usable upper limit, distinct from the ADC rail.

Each input uses a precision **0–10 V → 0–2 V divider**. R7 (YJ) and R8 (HK)
are NOMCT16032002AT1 networks containing eight isolated 20 kΩ resistors; four
form the 80 kΩ upper leg and one the 20 kΩ lower leg. The remaining three are
tied to ground at both ends. This presents a nominal 100 kΩ load to the detector.
See page 49 and the [Vishay network specification](https://www.vishay.com/docs/60007/nomc.pdf).

C17/C18 are 0.47 µF capacitors across the lower legs. The divider's Thevenin
resistance is $80\,\mathrm{k\Omega}\parallel20\,\mathrm{k\Omega}=16\,\mathrm{k\Omega}$,
giving $f_c=1/(2\pi\,16\,000\times0.47\,\mu\mathrm F)\simeq21.2$ Hz.
This PCB filter follows the detector's own response; it is separate from the
ADS1115's digital filtering and 20 Hz per-channel sample cadence.

The HK PDA10DT uses **70 dB gain and the 500 Hz bandwidth selector**. Its
high-impedance gain is $4.75\times10^6$ V/A, making the effective ADC gain
$0.2\times4.75\times10^6=9.5\times10^5$ V/A. The 100 kΩ divider is not a
50 Ω termination, so the detector's 50 Ω-load half-gain does not apply.
The [Thorlabs gain table](https://www.thorlabs.com/catalogpages/V21/1576.PDF)
states gain in **kV/A**. Firmware effective transimpedance includes the divider;
applying it twice would corrupt power.
[Sampling and uncertainty](photodiode_notes.md) explains clipping and dark capture.

### TIB route-loss defaults


The compiled YJ values in `devices.c` are provisional **whole-route** losses,
shared by AO and FEI. They include the internal static loss; external test-path
attenuation, dynamic FVOA attenuation, and the separate return are excluded.

| Source | Outbound route input | Total loss (AO and FEI) | Total transmission |
|---|---|---|---|
| 1028y | yj_laser | 34.5 dB | 3.54813389234e-4 |
| 1270j | yj_laser | 41 dB | 7.94328234724e-5 |
| 1430yj | yj_1430 | 62 dB | 6.30957344480e-7 |
| generic | yj_cal | 1 dB | 0.794328234724 |

The saved October 3 power captures, after removing the external 3 dB and MM
return, give recorded-model median losses of about 34.45, 41.03, and 60.69 dB.
For 1430yj, the current 0.24 mW/mA efficiency versus the older 0.192 adds about
0.97 dB; mid/high-current data and the entered source-meter attenuation support
rounding to 62 dB. Do not subtract the external 3 dB again. Current dependence
and source-model uncertainty remain; these are not accepted optical calibrations.
FEI equality is an assumption: the saved FEI captures did not establish an
independent loss. The 1 dB CAL value is a component estimate.

HK retains its nominal 0.83 transmission per red FFSW and existing static losses.
AO and FEI use the same switch count. HK CAL has no measured default and uses
unity. These defaults do not certify the installed attenuation or fiber patch.

| Laser | Outbound route input | Switch product | Static loss | Total transmission |
|---|---|---|---|---|
| 1430hk | hk_1430 | R1 × R2 × R3 = 0.83³ | 50 dB | 5.71787e-6 |
| 1510h | hk_laser | R2 × R3 = 0.83² | 33 dB | 3.45267885246e-4 |
| 2330k | hk_laser | R2 × R3 = 0.83² | 3 dB | 0.345267885246 |

For HK, `total_tx = switch_product * 10^(-static_loss_db / 10)`. Complete FFLS
return transmission is 0.98 for MM and 0.60 for SM on both channels: these are
the compiled `generic` defaults, separate from launch losses. Explicit
`mems/route/loss` records replace the whole route. Named-source overrides are
independent of stored generic overrides. Passive capture selects `generic`.
Dynamic FVOA attenuation is applied separately; static loss must not also be
folded into its fit.


## Laser control and switched power

The off-board bank contains six Maiman controllers and an NH8 hub. USART2 uses
115200 baud, 8N1, native FIFO and interrupt-driven Modbus. The THVD1429 drives
RS-485 with a documented 50 Ω board termination for this hub connection.
Power/ground to the hub come from the bank. See the bundled
{download}`Maiman manual <datasheets/maiman sf8025.pdf>` and
[laser operation](laser_operation.md) for transaction timing and faults.

PB2 drives a BSS138 stage controlling the bank power inhibit. The active-low
logical enable defaults off; heater auto policy can subsequently power the bank.
Off-board DS2408 relays switch PD YJ, PD HK and the auxiliary heater:

| Relay label | Zero-based DS2408 pin | Logical function |
|---|---|---|
| P1 | 0 | YJ PD power |
| P2 | 1 | HK PD power |
| P3 | 2 | Laser-bank auxiliary heater |

All are active-low, with inactive/off startup intent. The custom DS2408 driver
applies `init-gpios` defaults when available; application setup enforces its own
startup state. This allowed missing-at-boot peripheral is not a generic GPIO hog:
a hog would abort on a not-ready controller. Offline commands report errors;
warnings are best-effort, not guaranteed delivery. Successful DS2408 communication
is the current proxy for load-power availability; there is no downstream power-good
measurement. Status reports confirmed logical relay state.

Both 1-Wire buses have external 3.3 V pull-ups and use stock UART timing:
115200 baud data, 9600 baud reset (about 521 µs low). The accepted DS2408 reset
timing is outside its documented 660–720 µs range at this pull-up voltage; earlier
GPIO timing also fell outside it. This is a known board assumption, not a claim
of datasheet compliance. Revisit it if failures warrant. The pinned serial-driver
mutex limitation is described in [architecture](architecture.md#hardware-control).
Ambient DS18B20 conversion can take 750 ms on the app blocking queue.

## Tracing a fault

Start with the detected profile and commanded path, then follow its physical
channel through the tables above. For switching, distinguish requested state,
PCAL output, transistor-side pulse and actual optical continuity. For attenuation,
compare DAC code, pre-amplifier voltage and FVOA drive before judging the optical
model. For a PD fault, check relay state, input voltage, ADC raw code and configured
dark before interpreting computed nW. For a laser fault, preserve controller
readbacks and communication errors separately from cached current/power estimates.
The [review ledger](human_review_required.md) records unresolved drawing/firmware
questions; a drawing export date alone does not resolve installed-board evidence.
