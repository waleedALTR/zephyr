/*
 * Copyright The Zephyr Project Contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <errno.h>
#include <zephyr/device.h>
#include <zephyr/llext/llext.h>

#include "driver_module.h"

static void dev_clear(struct device *dev)
{
	dev->config = NULL;
	dev->api = NULL;
	dev->data = NULL;
	dev->ops = (struct device_ops){0};
}

static int dev_bind(struct device *dev, const struct device *drv)
{
	int ret;

	if (dev->state->initialized) {
		return -EBUSY;
	}

	dev->config = drv->config;
	dev->api = drv->api;
	dev->data = drv->data;
	dev->ops = drv->ops;

	ret = device_init(dev);
	if (ret != 0) {
		dev_clear(dev);
	}

	return ret;
}

/*
 * Detach the driver without calling its deinit function. Only safe for drivers
 * that register nothing outside their own data (interrupts, threads, callbacks),
 * as nothing is released before the extension is unloaded.
 */
static int dev_unbind(struct device *dev)
{
	dev_clear(dev);
	dev->state->initialized = false;
	dev->state->init_res = 0;

	return 0;
}

int driver_module_load(struct driver_module *mod)
{
	struct llext_load_param ldr_parm = LLEXT_LOAD_PARAM_DEFAULT;
	const struct device *drv;
	struct llext *ext = NULL;
	int ret;

	ret = llext_load(mod->ldr, mod->name, &ext, &ldr_parm);
	if (ret != 0) {
		return ret;
	}

	drv = llext_find_sym(&ext->exp_tab, mod->sym);
	if (drv == NULL) {
		ret = -ENOENT;
		goto unload;
	}

	ret = dev_bind(mod->dev, drv);
	if (ret != 0) {
		goto unload;
	}

	mod->ext = ext;

	return 0;

unload:
	(void)llext_unload(&ext);

	return ret;
}

int driver_module_unload(struct driver_module *mod)
{
	int ret;

	/* the device points into the extension until it is unbound */
	ret = dev_unbind(mod->dev);
	if (ret != 0) {
		return ret;
	}

	(void)llext_unload(&mod->ext);

	return 0;
}
