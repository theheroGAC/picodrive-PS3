#!/bin/bash
set -e
export PSL1GHT=${PSL1GHT:-/mnt/d/ps3dev-sdk}
export PS3DEV=${PS3DEV:-/mnt/d/ps3dev-sdk}
export PATH=$PSL1GHT/bin:$PSL1GHT/ppu/bin:$PATH

make CC=$PSL1GHT/ppu/bin/ppu-gcc CXX=$PSL1GHT/ppu/bin/ppu-g++ STRIP=$PSL1GHT/ppu/bin/ppu-strip -j4 platform/ps3/plat.o platform/ps3/emu.o platform/common/menu_pico.o pico/32x/pwm.o
make CC=$PSL1GHT/ppu/bin/ppu-gcc CXX=$PSL1GHT/ppu/bin/ppu-g++ STRIP=$PSL1GHT/ppu/bin/ppu-strip PicoDrive.pkg
pkg --contentid=UP0001-PICO00001_00-0000000000000000 pkg/ PicoDrive_CEX.pkg
echo "Build complete: PicoDrive.pkg, picodrive_PS3.gnpdrm.pkg, PicoDrive_CEX.pkg"
