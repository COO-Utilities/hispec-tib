## Power:

Adafruit BFF 5-20Vin 5V@1.2A out



## Controller
[W5500-EVB-Pico2](https://docs.wiznet.io/Product/iEthernet/W5500/w5500-evb-pico2)




### Power on/off
- GP6 (check 120V relay for current draw of opto as unspecified)

# To Breadboard

### Solder up
- 4 SI3552DV
- 1 DMP3085LSD
- 1 PCAL6416AHF
- 3 OPA2991

### Pins to
- Pico
  - Use I2C1 on pins GP2 (SDA) and GP3 (SCL)  (gpio & dac w/o)
  - Use UART1 GP8 (TX) & GP9 (RX) 3.3v (w/ll)
  - Use I2C0 on pins GP4 (SDA) and GP5 (SCL) (adc w/ ll)
  - Power enable on pin GP6
- DAC
- ADC
- 2x LL shifter 

### Find
- 100k x6
- 28k x6
- 52k x6
- 5V & 3.3V supply
- 4.7k  x9


## Device pin/port notes for W5500-EVB-pico2

W5500 uses SPI0 on pins GP16-21

GP25 is user LED
GP29 is 3v3 ADC

Free:
I2C0 & I2C1
UART0
UART1
