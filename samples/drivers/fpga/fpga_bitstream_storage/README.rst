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
   * - ``CONFIG_SAMPLE_FPGA_BITSTREAM_STORAGE_FLASH`` (default)
     - MCUboot image in the ``slot1_partition`` flash partition
   * - ``CONFIG_SAMPLE_FPGA_BITSTREAM_STORAGE_SDCARD``
     - File on the FAT volume of the disk named ``SD``

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
``slot1_partition``.

Upload with MCUmgr
==================

On the Agilex 5 SoC development kit, the sample reads the bitstream from the 8 MiB
``slot1_partition`` at offset ``0xf800000`` of the QSPI flash, and the board configuration
enables :ref:`MCUmgr <mcu_mgr>` over the shell UART. A new bitstream can then be written to the
partition from the host, without reprogramming the flash with a JTAG cable, and is loaded at the
next boot.

How it is set up
----------------

The board configuration enables the MCUmgr image management group, the OS management group for
``reset``, and the shell transport, which carries MCUmgr frames over the console UART next to the
shell.

MCUmgr only writes to partitions labeled ``slotN_partition``, and uploads go to the secondary
slot. The board devicetree therefore labels the bitstream partition ``slot1_partition`` and the
FIP partition ``slot0_partition``, and makes ``slot0_partition`` the ``zephyr,code-partition``.
MCUmgr takes the code partition as the running image and never writes to it, so the FIP is not
overwritten. Zephyr still runs from DRAM; the code partition does not move the image.

The QSPI flash layout is:

.. list-table::
   :header-rows: 1

   * - Range
     - Contents
   * - ``0x0000000`` - ``0x41fffff``
     - SDM boot data, first-stage boot loader and the FIP at ``0x3c00000``
   * - ``0x4200000`` - ``0xf7fffff``
     - Free; the Linux UBI root partition when U-Boot and Linux share the flash
   * - ``0xf800000`` - ``0xfffffff``
     - Bitstream partition (MCUmgr slot 1)

Host tools
----------

* ``imgtool`` from the MCUboot repository, to wrap the bitstream:

  .. code-block:: console

     pip3 install --user imgtool

* An SMP client. The examples use the ``mcumgr`` command-line tool:

  .. code-block:: console

     go install github.com/apache/mynewt-mcumgr-cli/mcumgr@latest

  ``mcumgr`` is a single static binary, so on a host without Go or network access it can be built
  elsewhere with ``CGO_ENABLED=0`` and copied over.

Connect to the board
--------------------

MCUmgr uses the board's console UART. The USB serial converter on the kit exposes several ports;
``ls -l /dev/serial/by-id/`` lists them, and the console is the one that prints the Zephyr boot
banner. Use the stable ``/dev/serial/by-id/`` name in the connection, since ``ttyUSB`` numbers can
change:

.. code-block:: console

   mcumgr conn add agx5 type="serial" \
           connstring="dev=/dev/serial/by-id/<console port>,baud=115200,mtu=4096"
   mcumgr -c agx5 echo hello

``echo`` returns ``hello`` when the connection works. The ``mtu`` must not be larger than
``CONFIG_MCUMGR_TRANSPORT_SHELL_MTU`` (4096); a smaller value such as 1024 also works.

MCUmgr frames and the shell share the UART, so close any terminal program or console server that
has the port open before running ``mcumgr``. Two readers on the port each receive part of the
replies, and transfers then stop with a timeout.

Upload a bitstream
------------------

#. Wrap the bitstream in an image for the 8 MiB partition. Give each new bitstream a higher
   version so that ``image list`` tells them apart:

   .. code-block:: console

      imgtool sign --header-size 32 --pad-header --align 4 --version 1.0.0 \
              --slot-size 0x800000 bitstream.rbf bitstream.img
      imgtool verify bitstream.img

#. Upload the image. MCUmgr erases and writes the partition as the data arrives:

   .. code-block:: console

      mcumgr -c agx5 -t 10 image upload bitstream.img

#. Check that slot 1 holds the new image. Its hash is the image digest ``imgtool verify`` printed:

   .. code-block:: console

      mcumgr -c agx5 image list

   .. code-block:: console

      Images:
       image=0 slot=1
          version: 1.0.0
          bootable: true
          flags:
          hash: ec3cf0414dcc50a45ac195f660583097390b6173c186150d9b299beff2425f7d

   Slot 0, the FIP, has no image header and is not listed.

#. Reset the board. The sample reads the new bitstream at boot and loads it:

   .. code-block:: console

      mcumgr -c agx5 reset

Only ``image upload``, ``image list`` and ``reset`` apply here. The image is not booted by
MCUboot, so ``image test`` and ``image confirm`` have no use.

The new bitstream must contain the same soft IP at the same addresses as the devicetree, since the
soft-IP drivers under ``fpga0`` are initialized again after every load.

Troubleshooting
---------------

.. list-table::
   :header-rows: 1

   * - Symptom
     - Cause
   * - ``Error: NMP timeout`` on every command
     - Wrong serial port, another program holds the port, or the image on the board was built
       without the MCUmgr configuration
   * - ``echo`` works but ``image upload`` stops
     - Another program reads the port during the transfer; close it and run the upload again,
       which resumes at the offset the board reports
   * - ``image upload`` is rejected right away
     - The image is larger than the 8 MiB partition; check the bitstream size and ``--slot-size``
   * - The sample logs ``Failed to read the bitstream ... (-8)``
     - The partition holds no image, or a raw bitstream without the ``imgtool`` header
   * - The sample logs ``Failed to read the bitstream ... (-27)``
     - The bitstream is larger than the sample's RAM buffer
   * - The sample logs ``Failed to load the bitstream``
     - The SDM rejected the bitstream; the ``fpga_altera`` debug messages before it show the stage

``mcumgr -l debug`` prints the frames sent and received.

Building and Running
********************

For the Agilex 5 SoC development kit, with the bitstream image in the QSPI bitstream partition:

.. zephyr-app-commands::
   :zephyr-app: samples/drivers/fpga/fpga_bitstream_storage
   :board: intel_socfpga_agilex5_socdk
   :goals: build

To read ``bitstream.bin`` from the SD card instead:

.. zephyr-app-commands::
   :zephyr-app: samples/drivers/fpga/fpga_bitstream_storage
   :board: intel_socfpga_agilex5_socdk
   :gen-args: -DCONFIG_SAMPLE_FPGA_BITSTREAM_STORAGE_SDCARD=y
   :goals: build

Sample Output
=============

From the QSPI bitstream partition:

.. code-block:: console

   I: Read 2260992 bytes from the bitstream partition
   I: Bitstream loaded, FPGA is active

From the SD card:

.. code-block:: console

   I: /SD::
   I:   F 1884160 bitstream.bin
   I: Read 1884160 bytes from /SD:/bitstream.bin
   I: Bitstream loaded, FPGA is active
