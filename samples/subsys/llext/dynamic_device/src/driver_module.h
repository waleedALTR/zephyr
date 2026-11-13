/*
 * Copyright The Zephyr Project Contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef DRIVER_MODULE_H_
#define DRIVER_MODULE_H_

#include <zephyr/device.h>
#include <zephyr/llext/llext.h>
#include <zephyr/llext/loader.h>
#include <zephyr/llext/symbol.h>
#include <zephyr/sys/sys_io.h>

/**
 * @brief Export the device a driver defines, from the extension it is built into
 *
 * Used in a source file built into the extension together with the unmodified
 * driver. The exported device is the template driver_module_load() looks up
 * by @p sym.
 *
 * @param node_id Devicetree node identifier of the device the driver defines
 * @param sym Name to export the device as
 */
#define DRIVER_MODULE_EXPORT(node_id, sym) EXPORT_SYMBOL_NAMED(DEVICE_DT_NAME_GET(node_id), sym)

/**
 * @brief Define a device whose driver is loaded from an extension
 *
 * Defines the device for @p node_id with no init function, API, config or
 * data; driver_module_load() fills them in. The node must have
 * zephyr,mutable, so the struct device is in RAM, and zephyr,deferred-init, so
 * the kernel does not initialize it at boot.
 *
 * @param node_id Devicetree node identifier
 */
#define DRIVER_MODULE_DEVICE_DEFINE(node_id)                                                       \
	BUILD_ASSERT(DT_PROP(node_id, zephyr_mutable),                                             \
		     DT_NODE_FULL_NAME(node_id) " must be zephyr,mutable");                        \
	BUILD_ASSERT(DT_PROP(node_id, zephyr_deferred_init),                                       \
		     DT_NODE_FULL_NAME(node_id) " must be zephyr,deferred-init");                  \
	DEVICE_DT_DEFINE(node_id, NULL, NULL, NULL, NULL, POST_KERNEL,                             \
			 CONFIG_KERNEL_INIT_PRIORITY_DEVICE, NULL)

/** A driver extension and the device its driver is bound to */
struct driver_module {
	/** Extension name */
	const char *name;
	/** Loader to read the extension ELF image from */
	struct llext_loader *ldr;
	/** Name of the struct device exported by the extension */
	const char *sym;
	/** Device defined with DRIVER_MODULE_DEVICE_DEFINE() to bind the driver to */
	struct device *dev;
	/** Register base address of the device, 0 if it has none; not used by the driver */
	mem_addr_t reg;
	/** Loaded extension, NULL when not loaded */
	struct llext *ext;
};

/* Address of the first register block of a node, 0 if it has none */
#define DRIVER_MODULE_REG_ADDR(node_id)                                                            \
	COND_CODE_1(DT_REG_HAS_IDX(node_id, 0), (DT_REG_ADDR(node_id)), (0))

/**
 * @brief Initializer for a struct driver_module
 *
 * @param node_id Devicetree node of a device defined with
 *                DRIVER_MODULE_DEVICE_DEFINE()
 * @param _name Extension name
 * @param _ldr Loader to read the extension ELF image from
 * @param _sym Name of the struct device exported by the extension
 */
#define DRIVER_MODULE_INIT(node_id, _name, _ldr, _sym)                                             \
	{                                                                                          \
		.name = (_name),                                                                   \
		.ldr = (_ldr),                                                                     \
		.sym = (_sym),                                                                     \
		.dev = DEVICE_DT_GET(node_id),                                                     \
		.reg = DRIVER_MODULE_REG_ADDR(node_id),                                            \
	}

/**
 * @brief Load a driver extension and bind its driver to a device
 *
 * Loads the extension through @p mod->ldr, looks up the struct device
 * @p mod->sym it exports and copies its ops, API, config and data into
 * @p mod->dev, then initializes the device with device_init().
 *
 * @param mod Driver module
 *
 * @retval 0 Success
 * @retval -EBUSY @p mod->dev is already initialized
 * @retval -ENOENT @p mod->sym is not exported by the extension
 * @retval -errno Other error from llext_load() or the driver init function
 */
int driver_module_load(struct driver_module *mod);

/**
 * @brief Unbind a driver from its device and unload its extension
 *
 * Clears the fields taken from the extension and marks the device as not
 * initialized, then unloads the extension. The driver's deinit function is not
 * called, so this is only safe for drivers that register nothing outside their
 * own data, such as interrupts, threads or callbacks.
 *
 * @param mod Module state from driver_module_load()
 *
 * @retval 0 Success
 */
int driver_module_unload(struct driver_module *mod);

#endif /* DRIVER_MODULE_H_ */
