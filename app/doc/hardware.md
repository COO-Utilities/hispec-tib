# FIB MEMS switching PCB hardware

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

### Microcontroller
- ST Nucleo H563ZI on a STM32H5 Nucleo-144 board (MB1404)
- STM32H563ZIT6 microcontroller based on the Arm Cortex®-M33 core.
- 2 MB of flash memory and 640 KB of SRAM
- See user manual um3115-stm32h5-nucleo144-board-mb1404-stmicroelectronics.pdf
- Microcontroller reference manual STM32H563ZI.pdf

### MEMS Switches
Controlled via 3V3 to 5V 16x GPIO expander (PCAL6416AHF)
- 3.3V i2c, 5V gpio, 25mA max drive
- Address 0x33 (ADDR high) or 0x22 (ADDR low), using 0x33
- Configure as open drain for FFSW as they have 5V pullups
- Configure as push-pull for FFLS
- Require 2 pins per FFSW or FFLS

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
    - 2x P  MOSFET
    - 1x each for FFLS
    - 2x 10k resistor for status line (pullup and current limiting)

### TIB & CAL Attenuator Drive
A pair of DAC7578SPW 8 chan DAC driving OPA2991 2 channel OpAmps
- 0 - 4095 (Vref, default=Vcc)
- Must not exceed Vmax of attenuator (6V for FVOA, so safe). Imax is 36.66 mA
- Use Vcc=3.3 with 1.51x opamp gain to avoid LL shifting
- OpAmp supplies required current to attenuator.
- I2C addr: 0x48 (channels 1-3) and 0x4A (chan 4-6)  (0x4C floating pin, 0x48 GND, 0x4A VCC)
- LDAC is tied to ground.
- Channels: 1=Y, 2=J, 3=YJATC, 4=HKATC, 5=H and CALatten, 6=K

For board files:
- Nucleo:
    - CN7 2 D15 I2C_A_SCL PB8 I2C1_SCL
    - CN7 4 D14 I2C_A_SDA PB9 I2C1_SDA

Breadboard considerations:
- Kit board has 10K pullups, do we need to get rid of as have pullups on 3.3v side of LL translation?
- FVOA (resistive, typ~3.5V 80mA @ full atten)
- MSOA (resistive, typ~0.5-3.25V, DNE 4.5V, something like 24-38 mA, datasheet unclear)

### TIB Photodiode Monitoring ADC
Uses an ADS1115 16 bit 4 channel muxed ADC
- Use channels A0 and A2
- Run device at 128 SPS, ±6.144 range, 187.5 uV LSB
- Sample each at 50 Hz muxing between the two
- PD coax terminated with 50 Ohm and fed to ADC as singled-ended input (gives 0-5V range from 0-10V PDs)
- I2C addr: 0x48 (0x48 ADDR=gnd, 0x49 ADDR=Vcc)
- Uses 2-channels of LL shifting for i2c 3.3-5V
- TODO Elec: Consider clamping Ain at MAX Vdd+.3 (say .2V above 5V with a shottkey(?) diode)

For board files:
- Nucleo:
    - CN7 2 D15 I2C_A_SCL PB8 I2C1_SCL
    - CN7 4 D14 I2C_A_SDA PB9 I2C1_SDA

Breadboard Considerations:
- AF prototype board has 10K pullups, may need to get rid of as LL shifter boards also have pullups

### Laser Diode Control
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

### Laser Bank Power Enable
- 3.3V, GPIO to enable of power driver,
- pull into 1-5v range against a 10k pulldown to ground to enable

For board files:
- Nucleo: CN9 13 D72 IO PB2 -

### Off-board power switch for photodiodes and laser bank aux heater
Uses a 1-Wire DS2408 GPIO chip controlling relays on P1-P3
- P1 is the power switch for the YJ photodiode
- P2 is the power switch for the HK photodiode
- P3 is the power switch for the laser bank aux heater

For board files:
- Nucleo: CN9 15 D71 IO PE9

### DS18B20 1Wire Temperature Sensor
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

