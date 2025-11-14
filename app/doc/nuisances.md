### openocd build issue

When I first got build/flash/debug working on my mac M3 i found openocd needs to be built from raspberry pi source as rp2350 not yet mainlined

use edited openocd file
brew install --build-from-source ./openocd.rb --head 
expect `Error: /opt/homebrew/Cellar/open-ocd/HEAD-cf9c0b4_1 is not a directory`, its seems fine

### WIZnet-PICO iolibrary_driver issues

The WIZnet-PICO-LWIP-C example uses the iolibrary_driver at ce4a7b6  (Feb 20 2022)
The WIZnet-PICO-C example uses the iolibrary_driver at c86300e  (Apr 13 2025) but there is a bug in a `#if 1`
that prevents their example from building for the 5500 pico 2 that needs fixing
