.. cli:

Intel SoCFPGA Command Line Interface
####################################

Overview
********

A simple program that provide the command line interface
for the Intel SoCFPGA Agilex5 platforms.

FPGA fabric configuration (Agilex5)
***********************************

Staging region is DT ``fpga_config`` at ``0x82000000`` (16 MiB; same as
U-Boot Agilex5 ``CONFIG_SYS_LOAD_ADDR`` / ATF ``BL31_LIMIT``). Mailbox
buffers live in ``mbox_ddr`` at ``0x83000000``.

QSPI path (preferred; verify read before reconfig)
==================================================

Program the raw ``core.rbf`` into the QSPI ``bitstream`` partition
(default offset ``0x0E000000``, up to 32 MiB on the Agilex5 CLI overlay).

Verify QSPI → DDR first (compare CRC to a known-good SD load)::

   fs mount fat /SD:
   rbf load /SD:/baseline.core.rbf
   rbf save_qspi
   rbf load_qspi 2256896
   rbf verify /SD:/baseline.core.rbf

Then start reconfig from QSPI (or from DDR already filled)::

   fpga_manager load_qspi 2256896 0
   # or: fpga_manager load_mem 2256896 0

SD path (slower; kept for reference)
====================================

Cadence SDHC needs **512-byte** chunked reads into ``FPGA_CONFIG``::

   fs mount fat /SD:
   rbf load /SD:/baseline.core.rbf
   fpga_manager load_mem 2256896 0

Or one-shot through the manager (slow on shell thread)::

   fpga_manager load /SD:/baseline.core.rbf 0

Use type ``0`` for full configuration and ``1`` for partial (freeze controller).

Note: HPS must own the QSPI controller (ATF / SDM mailbox grant) before
``load_qspi`` / ``rbf *qspi`` / flash shell access will work.
