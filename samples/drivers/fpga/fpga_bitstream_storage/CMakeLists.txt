# Copyright (c) 2026, Altera Corporation.
# SPDX-License-Identifier: Apache-2.0

cmake_minimum_required(VERSION 3.20.0)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(fpga_bitstream_storage)

target_sources(app PRIVATE src/main.c)

if(CONFIG_SAMPLE_FPGA_BITSTREAM_STORAGE_FLASH)
  # Only the image header layout is taken from the MCUboot module
  target_include_directories(app PRIVATE
    ${ZEPHYR_MCUBOOT_MODULE_DIR}/boot/bootutil/include
    ${ZEPHYR_MCUBOOT_MODULE_DIR}/boot/zephyr/include
  )
endif()
