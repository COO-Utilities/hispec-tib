### openocd build issue

When I first got build/flash/debug working on my mac M3 i found openocd needs to be built from raspberry pi source as rp2350 not yet mainlined

use edited openocd file
brew install --build-from-source ./openocd.rb --head 
expect `Error: /opt/homebrew/Cellar/open-ocd/HEAD-cf9c0b4_1 is not a directory`, its seems fine

### WIZnet-PICO iolibrary_driver issues

The WIZnet-PICO-LWIP-C example uses the iolibrary_driver at ce4a7b6  (Feb 20 2022)
The WIZnet-PICO-C example uses the iolibrary_driver at c86300e  (Apr 13 2025) but there is a bug in a `#if 1`
that prevents their example from building for the 5500 pico 2 that needs fixing


### CLION
- Make sure to select Enable the RTOS integration in Settings | Build, Execution, Deployment | Embedded Development | RTOS Integration.
- make sure to find and set debugger zephyr-sdk-0.17.0/arm-zephyr-eabi/bin/arm-zephyr-eabi-gdb

### updating zephyr/python

#at top level
python3 -m venv .venv
source .venv/bin/activate
pip install west
west init -l hispec-tib
west update
west zephyr-export
west packages pip --install
cd zephyr
west sdk install



## STLink
run /opt/ST/STM32CubeCLT_1.21.0/STLinkUpgrade.sh

use --runner stlink_gdbserver --port-number 3333

