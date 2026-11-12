.. zephyr:code-sample:: fpga-bitstream-storage
   :name: FPGA bitstream from storage
   :relevant-api: fpga_interface

   Read an FPGA bitstream from an SD card or from flash and load it.

Overview
********

This sample reads a bitstream into a static buffer and loads it into the FPGA with
:c:func:`fpga_load`. The FPGA is the device the ``fpga0`` devicetree alias points to. The
``SAMPLE_FPGA_BITSTREAM_STORAGE`` choice selects where the bitstream is read from:

.. list-table::
   :header-rows: 1

   * - Option
     - Storage
   * - ``CONFIG_SAMPLE_FPGA_BITSTREAM_STORAGE_SDCARD`` (default)
     - File on the FAT volume of the disk named ``SD``
   * - ``CONFIG_SAMPLE_FPGA_BITSTREAM_STORAGE_FLASH``
     - MCUboot image in the ``bitstream_partition`` flash partition

On the SD card, the file is ``CONFIG_SAMPLE_FPGA_BITSTREAM_FILE_NAME`` in the root directory of the
volume, and its size is the bitstream size. A bitstream larger than
``CONFIG_SAMPLE_FPGA_BITSTREAM_MAX_SIZE`` is rejected.

Flash partition
===============

A flash partition does not record how long the bitstream is, so the bitstream is wrapped in an
MCUboot image with ``imgtool`` from the MCUboot repository:

.. code-block:: console

   imgtool sign --header-size 32 --pad-header --align 4 --version 1.0.0 \
           --slot-size <partition size> bitstream.rbf bitstream.img

``imgtool`` puts a 32-byte header in front of the unchanged bitstream and a TLV area with the image
hash behind it. Write the image to the start of the partition. The sample takes the header size
and the bitstream size from the header and rejects a wrong magic number, an image larger than the
partition or than the buffer, and an encrypted or compressed image. It does not verify the image
hash. Only the header layout is taken from the MCUboot module; MCUboot does not have to be the
bootloader.

The board devicetree or an overlay must provide a flash partition with the node label
``bitstream_partition``.

Building and Running
********************

For the Agilex 5 SoC development kit, with the bitstream in ``bitstream.bin`` on the SD card:

.. zephyr-app-commands::
   :zephyr-app: samples/drivers/fpga/fpga_bitstream_storage
   :board: intel_socfpga_agilex5_socdk
   :goals: build

For ``native_sim``, which has no FPGA, the sample uses the flash partition and the fake FPGA
driver (``zephyr,fake-fpga``), which logs the size and the CRC-32 of the bitstream it is given:

.. zephyr-app-commands::
   :zephyr-app: samples/drivers/fpga/fpga_bitstream_storage
   :board: native_sim
   :goals: build

The bitstream partition is the 120 KiB ``scratch_partition`` at offset ``0xde000`` of the simulated
flash. Wrap any file of up to 122000 bytes, put the image at that offset of a flash file and run
the sample with it:

.. code-block:: console

   head -c 100000 /dev/urandom > bitstream.rbf
   imgtool sign --header-size 32 --pad-header --align 4 --version 1.0.0 \
           --slot-size 0x1e000 bitstream.rbf bitstream.img
   dd if=bitstream.img of=flash.bin bs=1 seek=$((0xde000)) conv=notrunc
   build/zephyr/zephyr.exe -flash=flash.bin

.. code-block:: console

   I: Read 100000 bytes from the bitstream partition
   I: fake-fpga: 100000 bytes, CRC-32 0x8448580c
   I: Bitstream loaded, FPGA is active

The CRC-32 is the one ``crc32 bitstream.rbf`` prints on the host.

Sample Output
=============

.. code-block:: console

   I: /SD::
   I:   F 1884160 bitstream.bin
   I: Read 1884160 bytes from /SD:/bitstream.bin
   I: Bitstream loaded, FPGA is active
