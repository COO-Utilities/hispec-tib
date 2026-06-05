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

- FFLS 1 & 2 ( sw channels 7 & 8) have their status pins connected to D62 and D63 (PF7 & 9) on the PCB with on pcb pullups.  FFLS will pull low to indicate position but codebase does not presently use them.
  - Do not appear to need any Solder bridges set. 
  - TODO need to update overlay to include these

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
- TIB:
    - sw1: FFSW, SW1_TIBR1 HKATC laser retro or forward
    - sw2: FFSW, SW1_TIBR2 HK CAL/Laser selector
    - sw3: FFSW, SW1_TIBR3 HK FEI/AO selector
    - sw4: FFSW, SW1_TIBB1 YJATC laser retro or forward
    - sw5: FFSW, SW1_TIBB2 YJ CAL/Laser selector
    - sw6: FFSW, SW1_TIBB3 YJ FEI/AO selector
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
- 0 - 4095 count (Vref, default=Vcc via internal reference source)
- Must not exceed Vmax of attenuator (6V for FVOA, so safe). Imax is 36.66 mA
- Use Vcc=3.3 with 1.51x opamp gain to avoid LL shifting
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

Breadboard considerations:
- Kit board has 10K pullups, do we need to get rid of as have pullups on 3.3v side of LL translation?
- FVOA (resistive, typ~3.5V 80mA @ full atten)
- MSOA (resistive, typ~0.5-3.25V, DNE 4.5V, something like 24-38 mA, datasheet unclear)

## TIB Photodiode Monitoring ADC
Uses an ADS1115 16 bit 4 channel muxed ADC
- Use channels A0 and A2
- Run device at 250 SPS, ±6.144 range, 187.5 uV LSB
- Sample each at 50 Hz muxing between the two. The faster ADS1115 data rate
  preserves timing margin for the two-channel 20 ms sampler, at the cost of
  less converter-side averaging than 128 SPS.
- PD coax terminated with 50 Ohm and fed to ADC as singled-ended input (gives 0-5V range from 0-10V PDs)
- I2C addr: 0x48 (0x48 ADDR=gnd, 0x49 ADDR=Vcc)
- Uses 2-channels of LL shifting for i2c 3.3-5V
- Photodiodes are Femto FWPR-20-IN (YJ) and Thorlabs PDA10DT (HK)
- See photodiode_notes.md for additional details

For board files:
- Nucleo:
    - CN7 2 D15 I2C_A_SCL PB8 I2C1_SCL
    - CN7 4 D14 I2C_A_SDA PB9 I2C1_SDA

Breadboard Considerations:
- AF prototype board has 10K pullups, may need to get rid of as LL shifter boards also have pullups

## Laser Diode Control
MODBUS
- Use a UART with 485 driver chip (THVD1429DT)
- 50Ohm termination resistor on PCB per NH8 hub documentation
- 5V and ground to the NH8 from the LD bank

For board files:
- Nucleo:
    - USART2 (valid are UART4/5/7/8/9/12 or USART1/2/3/6/10/11 not !! LPUART1)
    - CN9 4 D52 USART_B_RX PD6 USART2
    - CN9 6 D53 USART_B_TX PD5 USART2
    - CN9 8 D54 USART_B_RTS PD4 USART2

## Laser Bank Power Enable
- 3.3V, GPIO to enable of power driver,
- pull into 1-5v range against a 10k pulldown to ground to enable
- Firmware policy is off after reboot. The Nucleo devicetree hog drives the
  on-board laser-bank power enable low before app setup, and app setup repeats
  the inactive configuration.

For board files:
- Nucleo: CN9 13 D72 IO PB2 -
- MB1404 solder bridges SB61, SB66 must be changed to OFF, ON for PB2 to connect to CN9 pin 13 as GPIO.

## Off-board power switch for photodiodes and laser bank aux heater
Uses a 1-Wire DS2408 GPIO chip controlling relays on P1-P3
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
- Nucleo: CN9 15 D71 IO PE9
- MB1404 solder bridges for PE9 must select GPIO on Zio/ST morpho:
  SB35 OFF, SB67 ON.

## DS18B20 1Wire Temperature Sensor
- 3.3v digital temp sensor for good measure

For board files:
- Nucleo: CN9 30 D64 IO PG1 - (can be configured as UART9_TX)

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
