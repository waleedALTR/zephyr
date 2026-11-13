.. zephyr:code-sample:: llext-dynamic-device
   :name: Linkable loadable extensions dynamic device
   :relevant-api: llext_apis gpio_interface

   Run in-tree drivers from extensions behind devices defined in the image.

Overview
********

This sample loads unmodified GPIO drivers with the :ref:`llext` subsystem and
binds them at runtime to devicetree devices defined in the image, which keeps
using the regular :ref:`GPIO API <gpio_api>`.

The board overlays describe a ``gpio-mmio-latch`` and an ``arm,mmio32-gpio``
controller, with registers in memory mapped 1:1. Both nodes are
``zephyr,mutable`` and ``zephyr,deferred-init``. The image defines them with
``DRIVER_MODULE_DEVICE_DEFINE()`` and contains no driver code.

Each driver is built into its own extension with ``src/driver_ext.c``, which
exports the driver's ``struct device`` with ``DRIVER_MODULE_EXPORT()``. The
extensions are written to a FAT image that QEMU loads into a RAM disk; with
:kconfig:option:`CONFIG_DRIVER_MODULE_FS` disabled they are embedded in the
image instead.

For each controller, ``driver_module_load()`` loads the extension, copies the
exported device's ops, API, config and data into the image's device and calls
:c:func:`device_init`. The application then drives a pin, and
``driver_module_unload()`` clears the device and unloads the extension without
calling the driver's deinit function.

Limitations
***********

- :kconfig:option:`CONFIG_DEVICE_MUTABLE` is experimental, and the node's
  binding must include ``mutable.yaml``.
- :kconfig:option:`CONFIG_DEVICE_API_ASSERT` is not supported, as the API
  struct lives in the extension.
- The extension must be built with the same configuration and devicetree as
  the image.
- Unbinding is only safe for drivers that register nothing outside their own
  data, such as interrupts or threads.

Requirements
************

A board overlay in :zephyr_file:`samples/subsys/llext/dynamic_device/boards`,
and mtools on the build host for the RAM disk image. The sample runs in QEMU on
:zephyr:board:`qemu_cortex_a9 <qemu_cortex_a9>` and
:zephyr:board:`qemu_cortex_a53 <qemu_cortex_a53>`, and builds for
:zephyr:board:`intel_socfpga_agilex5_socdk`.

Building and running
********************

.. zephyr-app-commands::
   :zephyr-app: samples/subsys/llext/dynamic_device
   :board: qemu_cortex_a9
   :goals: build run
   :compact:

Sample output
*************

.. code-block:: console

   <inf> app: gpio@2f000 ready before load: no
   <inf> llext: Loaded extension latch
   <inf> app: gpio@2f000 ready after bind: yes
   <inf> app: gpio@2f000 pin 3: logical 0, raw 1, reg 0x5a
   <inf> app: gpio@2f000 pin 3: logical 1, raw 0, reg 0x52
   <inf> app: gpio@2f000 pin 3: logical 0, raw 1, reg 0x5a
   <inf> app: gpio@2f000 ready after unbind: no
   <inf> app: gpio@2f004 ready before load: no
   ...
   <inf> app: Dynamic device sample done
