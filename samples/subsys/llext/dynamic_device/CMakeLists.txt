# Copyright The Zephyr Project Contributors
# SPDX-License-Identifier: Apache-2.0

cmake_minimum_required(VERSION 3.28.0)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(llext_dynamic_device)

target_sources(app PRIVATE src/main.c src/driver_module.c)

# For each extension: the driver built into it, the node label of the device
# that driver defines and the name src/driver_ext.c exports that device as
set(ext_latch_driver gpio_mmio_latch.c)
set(ext_latch_node gpio_latch0)
set(ext_latch_sym gpio_latch_device)
set(ext_mmio32_driver gpio_mmio32.c)
set(ext_mmio32_node gpio_mmio32_0)
set(ext_mmio32_sym gpio_mmio32_device)
set(ext_names latch mmio32)

set(ext_bins)
foreach(ext_name ${ext_names})
  set(ext_bin ${PROJECT_BINARY_DIR}/llext/${ext_name}.llext)
  add_llext_target(${ext_name}_ext
    OUTPUT  ${ext_bin}
    SOURCES
      src/driver_ext.c
      ${ZEPHYR_BASE}/drivers/gpio/${ext_${ext_name}_driver}
  )
  llext_compile_definitions(${ext_name}_ext
    DRIVER_EXT_NODE=${ext_${ext_name}_node}
    DRIVER_EXT_SYM=${ext_${ext_name}_sym}
  )
  list(APPEND ext_bins ${ext_bin})

  if(NOT CONFIG_DRIVER_MODULE_FS)
    # Embed the extension in the image
    generate_inc_file_for_target(app ${ext_bin}
      ${ZEPHYR_BINARY_DIR}/include/generated/${ext_name}_ext.inc)
  endif()
endforeach()

if(NOT CONFIG_DRIVER_MODULE_FS)
  return()
endif()

# FAT image holding the extensions, sized like the RAM disk, which QEMU loads at
# the address given in CONFIG_QEMU_EXTRA_FLAGS
dt_nodelabel(ramdisk_mem NODELABEL ramdisk_mem REQUIRED)
dt_reg_addr(ramdisk_addr PATH ${ramdisk_mem})
dt_reg_size(ramdisk_size PATH ${ramdisk_mem})
dt_comp_path(ramdisk_node COMPATIBLE "zephyr,ram-disk")
dt_prop(sector_size PATH ${ramdisk_node} PROPERTY "sector-size")
dt_prop(sector_count PATH ${ramdisk_node} PROPERTY "sector-count")

math(EXPR ramdisk_addr "${ramdisk_addr}" OUTPUT_FORMAT HEXADECIMAL)
math(EXPR disk_size "${sector_size} * ${sector_count}")
if(NOT CONFIG_QEMU_EXTRA_FLAGS MATCHES "file=ramdisk.img,addr=${ramdisk_addr},")
  message(FATAL_ERROR "CONFIG_QEMU_EXTRA_FLAGS must load ramdisk.img at ${ramdisk_addr}")
endif()
if(NOT disk_size EQUAL ramdisk_size)
  message(FATAL_ERROR "RAM disk size does not match the ramdisk_mem region")
endif()

set(ramdisk_img ${APPLICATION_BINARY_DIR}/ramdisk.img)
find_program(MFORMAT mformat REQUIRED)
find_program(MCOPY mcopy REQUIRED)
add_custom_command(
  OUTPUT ${ramdisk_img}
  COMMAND ${CMAKE_COMMAND} -E rm -f ${ramdisk_img}
  COMMAND ${MFORMAT} -i ${ramdisk_img} -C -T ${sector_count} -h 1 -s ${sector_count}
          -M ${sector_size} ::
  COMMAND ${MCOPY} -i ${ramdisk_img} ${ext_bins} ::
  DEPENDS ${ext_bins}
  COMMENT "Generating RAM disk image with ${ext_names}"
)
add_custom_target(ramdisk_img ALL DEPENDS ${ramdisk_img})

# Also when only a run target is built, e.g. west build -t run
foreach(target run_qemu debugserver_qemu)
  if(TARGET ${target})
    add_dependencies(${target} ramdisk_img)
  endif()
endforeach()
