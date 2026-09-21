# FIB PCB hardware

FIB uses this hardware in four locations:
1. The Trunk Interface Box (TIB), which has
    2. 2 MMF Switches (FFLS)
    3. 6 SMF Switches (FFSW)
    4. 12 SMF variable attenuators (2x per laser channel) (FVOA)
    4. 1 off-board 6 laser diode bank controlled via Modbus (Maiman electronics SF8250-ZIF14 + NH8 hub)
    5. 1 off-board power switch to toggle PD power
    6. one board power switch to toggle LD bank power
2. The Achromatic Splitter
    3. 6 SMF switches (FFSW) for muxing lign in the YJ (3x) and HK (3x) channels
2. The YJ Calibration switch
    3. 2 (smaller core) MMF switches (FFSW)
    4. 5 SMF switches (FFSW)
    5. 2 SMF variable attenuators (FVOA)
6. The HK calibration switch, same as the YJ switch but with a slightly different, but electrically identical
   model of fiber components (FFSW & FVOA).

All have an on-board temperature sensor and are interacted with via ethernet when deployed. USB-C is available

See status.md for software details

## Microcontroller
- ST Nucleo H563ZI on a STM32H5 Nucleo-144 board (MB1404)
- STM32H563ZIT6 microcontroller based on the Arm Cortex®-M33 core.
- 2 MB of flash memory and 640 KB of SRAM
- See user manual um3115-stm32h5-nucleo144-board-mb1404-stmicroelectronics.pdf
- Microcontroller reference manual STM32H563ZI.pdf
- https://docs.zephyrproject.org/latest/boards/st/nucleo_h563zi/doc/index.html

Must edit default solder bridges to use i2c2:
• HSE not used: PF0/PH0 and PF1/PH1 are used as GPIOs instead of clocks. The configuration must be:
– SB48 and SB50 ON
– SB49 OFF


## MEMS Switches
Controlled via 3V3 to 5V 16x GPIO expander (PCAL6416AHF,128)
- 3.3V i2c, 5V gpio, 25mA max drive
- Address 0x21 (ADDR high) or 0x20 (ADDR low), using 0x21 (addr is tied to +5V (VDD(P))) 0b0100001
- FFSW lines have 4.7k external resistors. in open drain each switch channel flows 2mA through PCAL
  - FFLS lines do not have a pullup but one may be added at site of unpopulated FFSW drive MOSFET to allow operation in same manner
- Placing ports in push-pull with pull-ups enabled and then idling the external MEMS control lines low should work for all switches.
- The port with FFLS switches also has FFSW switches so I am going with a common selection for both ports for simplicity.
- Initial testing will be with push-pull approach and full drive strength
- Requires 2 pins per FFSW or FFLS
- The Nucleo devicetree configures all 16 PCAL MEMS outputs with GPIO hogs:
  push-pull, pull-ups enabled, active-low at the PCAL pin, and logical
  output-low at boot so the external switch-control lines idle low. Firmware
  pulses the logical line active, which is a high pulse at the switch-control
  line. Zephyr's mainline `nxp,pcal6416a` driver resets the PCAL drive strength
  registers to full drive and leaves both ports push-pull; firmware no longer
  writes the PCAL port-drive register directly.

- FFLS 1 & 2 (sw channels 7 & 8) have their status pins connected to D62 and D63 (PF7 & PF9) on the PCB with on-PCB pullups.
  - The FFLS pulls the sense line low for position B; high means position A.
  - The Nucleo overlay includes `mems-ffls1-sense-gpios` and `mems-ffls2-sense-gpios` stubs with no internal pull because the PCB provides the pullups.
  - Firmware does not presently consume these sense lines.
  - Do not appear to need any solder bridges set.

For board files:
- Nucleo:
    - CN9 19 D69 I2C_B_SCL PF1 I2C2_SCL
    - CN9 21 D68 I2C_B_SDA PF0 I2C2_SDA

At the drive level transistors are
- NTGD4167 (Agiltron's recommended SI3552DV is obsolete) (for FFLS)
    - N & P MOSFET
    - 2x each for FFSW
    - 2x 4.7k resistors for pullups
- DMP3085LSD (for FFSW)
    - 2x P MOSFET
    - 1x each for FFLS
    - 2x 10k resistor for status line (pullup and current limiting)

### Switch assignments
PCAL assignments:
  - sw1 A B: P0 6, 7 / gpio 6, 7 
  - sw2 A B: P1 0, 1 / gpio 8, 9 
  - sw3 A B: P0 4, 5 / gpio 4, 5 
  - sw4 A B: P1 2, 3 / gpio 10, 11 
  - sw5 A B: P1 4, 5 / gpio 12, 13 
  - sw6 A B: P1 6, 7 / gpio 14, 15 
  - sw7 A B: P0 2, 3 / gpio 2, 3 
  - sw8 A B: P0 0, 1 / gpio 0, 1 

- TIB:
  - sw1: FFSW, SW1_TIBB1 YJATC laser forward/retro selector
  - sw2: FFSW, SW1_TIBB2 YJ Laser/CAL selector
  - sw3: FFSW, SW1_TIBB3 YJ AO/FEI selector
  - sw4: FFSW, SW1_TIBR1 HKATC laser forward/retro selector
  - sw5: FFSW, SW1_TIBR2 HK Laser/CAL selector
  - sw6: FFSW, SW1_TIBR3 HK AO/FEI selector
  - sw7: FFLS, SW2_FFLS1 YJ MM/SM PD Selector
  - sw8: FFLS, SW2_FFLS2 HK MM/SM PD Selector
- AS
  - sw1: FFSW, SW1_ASR1 HK Splitter in
  - sw2: FFSW, SW1_ASR2 HK Splitter out 1 (Cal/Split1)
  - sw3: FFSW, SW1_ASR3 HK Splitter out 2 (Split2/Split3)
  - sw4: FFSW, SW1_ASB1 YJ Splitter in
  - sw5: FFSW, SW1_ASB2 YJ Splitter out 1 (Cal/Split1)
  - sw6: FFSW, SW1_ASB3 YJ Splitter out 2 (Split2/Split3) 
  - sw7: NC
  - sw8: NC
  
- CAL 
  - sw1: FFSW, SW1_CAL1 LFC/Etalon Selector
  - sw2: FFSW, SW1_CAL2 Lamp/BB Selector
  - sw3: FFSW, SW1_CAL3 BB IS/Cal Selector
  - sw4: FFSW, SW1_CAL4 Lamp+BB vs LFC+Eta Selector
  - sw5: FFSW, SW1_CAL5 Cal/Dark Selector
  - sw6: FFSW, SW1_CAL6 IS BB/Dark Selector
  - sw7: FFSW, SW1_CAL7 MSR/TIB Selector
  - sw8: NC

## TIB & CAL Attenuator Drive
A pair of DAC7678 8 chan DAC driving OPA2991 2 channel OpAmps
- DAC codes are 0 - 4095; ideal output transfer uses `code / 4096`.
- Both DAC7678 devices use an external 3.3 V REF3333AIDBZR reference, so
  `Vout = code / 4096 * VREFIN`, clipped by AVDD.
- Current board DAC AVDD is 3.3V. The OPA2991 scales the DAC 0 - 3.3V output
  toward the FVOA 0 - 5V command range; op-amp gain is firmware-calibrated.
- Must not exceed Vmax of attenuator (6V for FVOA, so safe). Imax is 36.66 mA
- OpAmp supplies required current to attenuator.
- Each laser channel uses a pair of physical attenuators:
  - CAL: 2 DAC channels in use (1 channel x 2 attenuators)
  - TIB: 12 DAC channels in use (6 channels x 2 attenuators)
- I2C addr: 0x48 and 0x4A  (DS says: 0x4C floating pin, 0x48 GND, 0x4A VCC)
- LDAC is tied to ground.
- Channels:
  - 0x4A
    - Y Attens: A=1, C=2
    - J Attens: E=1, G=2 
    - YJATC Attens: D=1 & F=2
  - 0x48
    - HKATC Attens: A=1, C=2
    - H/CAL Attens: E=1, G=2
    - K Attens: D=1, F=2

For board files:
- Nucleo:
    - CN9 19 D69 I2C_B_SCL PF1 I2C2_SCL
    - CN9 21 D68 I2C_B_SDA PF0 I2C2_SDA

## TIB Photodiode Monitoring ADC
Uses an ADS1115 16 bit 4 channel muxed ADC
- Use channels A0 and A2
- Run device at 250 SPS, ±2.048 V range, 62.5 uV LSB. The intended 0-2 V
  input range leaves 48 mV of headroom below the ADC's numerical rail.
- PD 50 Ohm coax is fed to the ADC as a single-ended input.
- Input circuitry uses filtering and a precision divider to map 0-10 V PD
  output to 0-2 V with 20 Hz bandwidth.
- Sample each at 20 Hz, muxing between the two within a 50 ms period. Each
  throughput record uses one fresh conversion. The overlay retains 250 SPS;
  selecting 64 SPS permits two conversions in about 31.3 ms before I2C and
  scheduling overhead. Confirm runtime margin and noise on hardware before
  changing that default. Timing allowances derive from the selected rate.
- I2C addr: 0x48 (0x48 ADDR=gnd, 0x49 ADDR=Vcc)
- ADC runs at 3.3v
- Photodiodes are Femto FWPR-20-IN (YJ) and Thorlabs PDA10DT (HK)
- See photodiode_notes.md for additional details

For board files:
- Nucleo:
    - CN7 2 D15 I2C_A_SCL PB8 I2C1_SCL
    - CN7 4 D14 I2C_A_SDA PB9 I2C1_SDA

Static attenuation anticipated required:
  - 1028: -90.0 to -73.0 dB, range 17.0 dB, **static -73.0 dB**
  - 1270:  -70.0 to -40.0 dB, range 30.0 dB, **static -40.0 dB**
  - 1430: range 0.0 dB, **static -100.0 dB**
  - 1510: -80.0 to -33.0 dB, range 47.0 dB, **static -33.0 dB**
  - 2330: -73.0 to -3.0 dB, range 70.0 dB, **static -3.0 dB**

### TIB route-loss defaults

Nominal transmission is 0.88 per blue (YJ B1/B2/B3) FFSW and 0.83 per red
(HK R1/R2/R3) FFSW. Complete FFLS return-path transmission is 0.98 for MM to
PD and 0.60 for SM to PD on both channels. The return factors are separate
from the outbound switch losses.

The compiled route defaults in `devices.c` combine static laser attenuation
with the switches traversed below. Current lab defaults use 50 dB for 1028y and
1430hk; the component planning table above retains its original target values. AO and FEI use the same switch count.

| Laser | Outbound route input | Switch product | Static loss | Total transmission |
|---|---|---|---|---|
| 1028y | yj_laser | B2 × B3 = 0.88² | 50 dB | 7.744e-6 |
| 1270j | yj_laser | B2 × B3 = 0.88² | 40 dB | 7.744e-5 |
| 1430yj | yj_1430 | B1 × B2 × B3 = 0.88³ | 100 dB | 6.81472e-11 |
| 1430hk | hk_1430 | R1 × R2 × R3 = 0.83³ | 50 dB | 5.71787e-6 |
| 1510h | hk_laser | R2 × R3 = 0.83² | 33 dB | 3.45267885246e-4 |
| 2330k | hk_laser | R2 × R3 = 0.83² | 3 dB | 0.345267885246 |

`total_tx = switch_product * 10^(-static_loss_db / 10)`. These are nominal
assembly defaults, not measurements of the installed path. Explicit
`mems/route/loss` records replace the whole total. Dynamic FVOA attenuation is
applied separately, so static attenuation must not also be folded into its fit.
MM/SM return defaults are four generic channel/fiber entries, including unknown
or astrophysical light. Existing per-laser return overrides apply when that
laser is selected; unknown-source captures use the generic return value.

## Laser Diode Control
MODBUS
- Use a UART with 485 driver chip (THVD1429DT)
- 50 Ohm termination resistor on PCB per NH8 hub documentation
- 5V and ground to the NH8 from the LD bank

For board files:
- Nucleo:
    - USART2 (valid are UART4/5/7/8/9/12 or USART1/2/3/6/10/11 not !! LPUART1)
    - CN9 4 D52 USART_B_RX PD6 USART2
    - CN9 6 D53 USART_B_TX PD5 USART2
    - CN9 8 D54 USART_B_RTS PD4 USART2

The Nucleo overlay enables USART2's native eight-byte hardware FIFO with
`fifo-enable`. Modbus uses the stock interrupt-driven UART API at 115200 baud,
8N1, with PD4 controlled as GPIO driver enable. UART9 and UART12 separately
provide the 1-Wire waveforms; no Zephyr source patches are required.

## Laser Bank Power Enable
- 3.3V, GPIO to enable power driver
- Switches gate of a BSS138 that connects the not inhibit of the power IC to ground
- Default is to be off after reboot. The Nucleo devicetree hog drives to this state.
- At the nucleo it is ACTIVE_LOW with a pull up GPIO_PULL_UP

For board files:
- Nucleo: CN9 13 D72 IO PB2 -
- MB1404 solder bridges SB61, SB66 must be changed to OFF, ON for PB2 to connect to CN9 pin 13 as GPIO.

## Off-board power switch for photodiodes and laser bank aux heater
Uses a 1-Wire DS2408 GPIO chip controlling relays on P1-P3
- The data line has an external 3.3 V pull-up and connects directly to the Nucleo
  without level shifting.
- Zephyr's stock UART-backed 1-Wire driver uses 115200 baud for data and 9600
  baud for reset, producing an approximately 521 us reset-low pulse. The
  [DS2408 datasheet](https://www.analog.com/media/en/technical-documentation/data-sheets/ds2408.pdf)
  specifies 660–720 us at this pull-up voltage (page 3). The owner accepts the
  stock timing for this board: the previous 480 us GPIO reset also operated
  outside that specification. Revisit reset timing only if observed failures
  justify it; no reset-timing extension is applied.
- Hardware assumption: successful communication with the DS2408 establishes that
  power is available for its PD/heater loads. Logical relay states establish which
  loads are powered; no additional downstream power-good feedback is required.
- Housekeeping checks the port approximately once per second. Five seconds without
  a response is an operational fault; a single failed transaction warns. The
  throughput loop consumes confirmed state without 1-Wire I/O.
- P1 is the power switch for the YJ photodiode
- P2 is the power switch for the HK photodiode
- P3 is the power switch for the laser bank aux heater
- The DS2408 driver should set all expander outputs to their overlay-configured defaults during driver init in the same 
  manner as any system GPIOs when the chip is present. Absent configuration, driver should not configure the chip 
  (allowing default power-on or current config to persist). Application device startup code will enforce startup logic 
  state for the relays. 
- If the off-board relay expander is missing at boot, firmware emits a (non-droppable) warning,
  reports the relay GPIO expander offline in `status`, and ignores relay power commands with a warning.
- The DS2408 is intentionally not configured through a generic GPIO hog because
  Zephyr's hog init aborts on a not-ready GPIO controller. The relay board is an
  allowed missing-at-boot fault (the mems' PCAL being unavaialble would indicate a much larger, PCB, problem).

For board files:
- Nucleo: CN9 15 D71 IO PE9, UART12_RX (AF6) with TX/RX swap and single-wire
  mode. Pinctrl configures open-drain drive with the external pull-up.
- MB1404 solder bridges for PE9 must route it to Zio/ST morpho:
  SB35 OFF, SB67 ON.

## DS18B20 1Wire Temperature Sensor
- 3.3v digital temp sensor for good measure

For board files:
- Nucleo: CN9 30 D64 IO PG1, UART9_TX (AF11) in single-wire mode, open-drain
  with the existing external pull-up. Uses the same stock serial 1-Wire timings.

Nucleo board pins in use:
- USB
    - PB13 USB PD controller side for the CC1 pin
    - PA12 USB differential pair P
    - PA11 USB differential pair M
    - PB14 USB PD controller side for the CC2 pin
- STLINK-V3EC
    - PC3 USB PD controller side for the CC1 pin
    - PC4 USB PD controller side for the CC2 pin
    - PB15 USB differential pair P
    - PB14 USB differential pair M
- RMII interface for ETH
