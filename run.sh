#!/bin/bash

# Colors
GREEN='\033[0;32m'
RED='\033[0;31m'
NC='\033[0m'  # No Color

# Check ZEPHYR_BIN
if [ -z "$ZEPHYR_BIN" ]; then
    echo -e "${RED}Error: ZEPHYR_BIN is not set!${NC}"
    echo -e "${GREEN}Example: export ZEPHYR_BIN=~/zephyrproject/zephyr${NC}"
    exit 1
fi

if [ ! -d "$ZEPHYR_BIN" ]; then
    echo -e "${RED}Error: ZEPHYR_BIN is not a directory: $ZEPHYR_BIN${NC}"
    exit 1
fi

# If everything is set
echo -e "${GREEN}Environment variables are set correctly:${NC}"
echo "ZEPHYR_BIN = $ZEPHYR_BIN"

if [ "$(basename "$PWD")" != "build" ]; then
  echo -e "${RED}Error: This script must be run from the 'build' directory.${NC}" >&2
  exit 1
fi

if [[ "$1" == "-d" || "$1" == "--deploy" ]]; then
  cd ../ || { echo "Failed to cd ../"; exit 1; }
  simics_intelfpga_cli --deploy agilex5e-universal
  make
  cd build || { echo "Failed to cd build"; exit 1; }
  exit 0
fi

TOP_FOLDER=$PWD/..
REPO_DIR="$TOP_FOLDER/arm-trusted-firmware-qspi"

# Clone only if not present
if [ ! -d "$REPO_DIR" ]; then
    echo "Cloning repository..."
    git clone -b QPDS24.3_REL_GSRD_PR \
        https://github.com/altera-opensource/arm-trusted-firmware "$REPO_DIR"
    git -C "$REPO_DIR" switch -c test 2>/dev/null || git -C "$REPO_DIR" switch test
    make -C "$REPO_DIR" realclean
else
    echo "Repository already exists. Skipping ATF clonning."
fi

if [ "$1" = "-b" ] || [ "$1" = "--build" ]; then
    ARCH=arm64 CROSS_COMPILE=aarch64-none-linux-gnu- \
    make -C "$REPO_DIR" PLAT=agilex5 SOCFPGA_BOOT_SOURCE_QSPI=1 bl2 bl31 PRELOADED_BL33_BASE=0x80100000 -j$(nproc)
else
    echo "Skipping ATF build"
fi

make -C "$REPO_DIR" fiptool
cp $REPO_DIR/tools/fiptool/fiptool $TOP_FOLDER/.

rm -rf zephyr.bin zephyr.elf
cp $ZEPHYR_BIN/build/zephyr/zephyr.bin .
cp $ZEPHYR_BIN/build/zephyr/zephyr.elf .
cp $REPO_DIR/build/agilex5/release/bl2.bin .
cp $REPO_DIR/build/agilex5/release/bl31.bin .
cp $REPO_DIR/build/agilex5/release/bl2/bl2.elf .
cp $REPO_DIR/build/agilex5/release/bl31/bl31.elf .

aarch64-none-linux-gnu-objcopy -v -I binary -O ihex --change-addresses 0x00000000 bl2.bin bl2.hex

$TOP_FOLDER/fiptool create --soc-fw bl31.bin --nt-fw zephyr.bin fip.bin

FILE="agilex5_factory.sof"
URL="https://releases.rocketboards.org/2024.11/gsrd/agilex5_dk_a5e065bb32aes1_gsrd/ghrd_a5ed065bb32ae6sr0_hps.sof"
TMP="ghrd_a5ed065bb32ae6sr0_hps.sof"

if [ ! -f "$FILE" ]; then
    echo "File not found. Downloading..."
    rm -f "$TMP"
    wget "$URL"
    mv "$TMP" "$FILE"
else
    echo "File already exists. Skipping download."
fi

# QSPI layout, matching the partitions in the Zephyr board devicetree
FIP_OFFSET=0x03C00000
BITSTREAM_OFFSET=0x04400000
BITSTREAM_SLOT_SIZE=0x800000
BITSTREAM_RBF=ghrd.core.rbf
RPD=flash_image_jic.rpd

if [ "$1" = "-b" ] || [ "$1" = "--build" ]; then
    echo "Building QSPI binary using quartus_pfg..."
    # SDM boot info and bitstream with bl2 only: quartus_pfg cannot place
    # raw files without a .pfg file, so they are added to the RPD below.
    quartus_pfg -c -o device=MT25QU02G -o flash_loader=A5ED065BB32AE6SR0 \
        -o mode=ASX4 -o hps_path=bl2.hex -o bitswap=ON \
        agilex5_factory.sof flash_image.jic flash_image_jic.map "$RPD" || exit 1
else
    echo "Skipping quartus_pfg"
fi

# Wrap the core bitstream in an MCUboot image for the fpga_bitstream_storage sample
# Set IMGTOOL to override; otherwise imgtool from PATH or from the west workspace
if [ -z "$IMGTOOL" ]; then
    if command -v imgtool > /dev/null; then
        IMGTOOL=imgtool
    elif [ -f "$ZEPHYR_BIN/../bootloader/mcuboot/scripts/imgtool.py" ]; then
        IMGTOOL="python3 $ZEPHYR_BIN/../bootloader/mcuboot/scripts/imgtool.py"
    else
        echo -e "${RED}Error: imgtool not found.${NC}" >&2
        echo "Install it (pip install imgtool) or set IMGTOOL, for example:" >&2
        echo "  export IMGTOOL=\"python3 <west workspace>/bootloader/mcuboot/scripts/imgtool.py\"" >&2
        exit 1
    fi
fi

rm -f bitstream.img
$IMGTOOL sign --header-size 32 --pad-header --align 4 --version 1.0.0 \
    --slot-size "$BITSTREAM_SLOT_SIZE" "$BITSTREAM_RBF" bitstream.img || exit 1

# Write a raw file into the RPD at a flash offset, filling any gap with 0xff
place_in_rpd() {
    local file=$1
    local offset=$(($2))
    local size

    size=$(stat -c %s "$RPD") || exit 1
    if [ "$size" -lt "$offset" ]; then
        head -c $((offset - size)) /dev/zero | tr '\0' '\377' >> "$RPD"
    fi

    dd if="$file" of="$RPD" bs=4096 seek=$((offset / 4096)) conv=notrunc status=none || exit 1
    echo "Placed $file at $2 in $RPD"
}

place_in_rpd fip.bin "$FIP_OFFSET"
place_in_rpd bitstream.img "$BITSTREAM_OFFSET"

pkill -9 -f simics

../simics ./zephyr_qspi.simics
