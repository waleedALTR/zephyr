/*
 * Copyright The Zephyr Project Contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/device.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/sys_io.h>

#if defined(CONFIG_DRIVER_MODULE_FS)
#include <zephyr/fs/fs.h>
#include <zephyr/llext/fs_loader.h>

#include <ff.h>
#else
#include <zephyr/llext/buf_loader.h>
#endif

#include "driver_module.h"

LOG_MODULE_REGISTER(app, LOG_LEVEL_INF);

#define LATCH_NODE  DT_NODELABEL(gpio_latch0)
#define MMIO32_NODE DT_NODELABEL(gpio_mmio32_0)
#define USER_NODE   DT_PATH(zephyr_user)

#define RAMDISK_MNT "/RAM:"

BUILD_ASSERT(!IS_ENABLED(CONFIG_GPIO_MMIO_LATCH), "the latch driver must only be in the extension");
BUILD_ASSERT(!IS_ENABLED(CONFIG_GPIO_MMIO32), "the mmio32 driver must only be in the extension");
/* The extension's API struct is not in the image's gpio_driver_api section */
BUILD_ASSERT(!IS_ENABLED(CONFIG_DEVICE_API_ASSERT), "CONFIG_DEVICE_API_ASSERT is not supported");
BUILD_ASSERT(DT_REG_SIZE(LATCH_NODE) == 4, "sample reads the latch as a 32-bit register");
BUILD_ASSERT(DT_REG_SIZE(MMIO32_NODE) == 4, "sample reads the mmio32 as a 32-bit register");

DRIVER_MODULE_DEVICE_DEFINE(LATCH_NODE);
DRIVER_MODULE_DEVICE_DEFINE(MMIO32_NODE);

/* A GPIO controller driven from an extension and a pin to drive */
struct dynamic_gpio {
	struct driver_module mod;
	struct gpio_dt_spec led;
};

#if defined(CONFIG_DRIVER_MODULE_FS)
/* FAT file system written to the RAM disk by QEMU, see CMakeLists.txt */
static int ramdisk_mount(const char *mnt_point)
{
	static FATFS fat_fs;
	static struct fs_mount_t mnt = {
		.type = FS_FATFS,
		.fs_data = &fat_fs,
	};
	static bool mounted;
	int ret;

	if (mounted) {
		return 0;
	}

	mnt.mnt_point = mnt_point;
	ret = fs_mount(&mnt);
	if (ret == 0) {
		mounted = true;
	}

	return ret;
}
#endif

static int led_report(const struct dynamic_gpio *gpio)
{
	const struct gpio_dt_spec *led = &gpio->led;
	int logical = gpio_pin_get_dt(led);
	int raw = gpio_pin_get_raw(led->port, led->pin);

	if ((logical < 0) || (raw < 0)) {
		return -EIO;
	}

	LOG_INF("%s pin %u: logical %d, raw %d, reg 0x%02x", led->port->name, led->pin, logical,
		raw, sys_read32(gpio->mod.reg));

	return 0;
}

static int led_exercise(const struct dynamic_gpio *gpio)
{
	int ret;

	ret = gpio_pin_configure_dt(&gpio->led, GPIO_OUTPUT_INACTIVE);
	if (ret != 0) {
		return ret;
	}

	ret = led_report(gpio);
	if (ret != 0) {
		return ret;
	}

	ret = gpio_pin_set_dt(&gpio->led, 1);
	if (ret != 0) {
		return ret;
	}

	ret = led_report(gpio);
	if (ret != 0) {
		return ret;
	}

	ret = gpio_pin_toggle_dt(&gpio->led);
	if (ret != 0) {
		return ret;
	}

	return led_report(gpio);
}

static int gpio_module_run(struct dynamic_gpio *gpio)
{
	struct driver_module *mod = &gpio->mod;
	const char *name = mod->dev->name;
	int ret;

	LOG_INF("%s ready before load: %s", name, gpio_is_ready_dt(&gpio->led) ? "yes" : "no");

	ret = driver_module_load(mod);
	if (ret != 0) {
		LOG_ERR("Failed to load the %s driver: %d", mod->name, ret);
		return ret;
	}

	LOG_INF("%s ready after bind: %s", name, gpio_is_ready_dt(&gpio->led) ? "yes" : "no");

	ret = led_exercise(gpio);
	if (ret != 0) {
		LOG_ERR("GPIO access failed: %d", ret);
	}

	if (driver_module_unload(mod) == 0) {
		LOG_INF("%s ready after unbind: %s", name,
			gpio_is_ready_dt(&gpio->led) ? "yes" : "no");
	}

	return ret;
}

int main(void)
{
#if defined(CONFIG_DRIVER_MODULE_FS)
	static struct llext_fs_loader latch_ldr = LLEXT_FS_LOADER(RAMDISK_MNT "/latch.llext");
	static struct llext_fs_loader mmio32_ldr = LLEXT_FS_LOADER(RAMDISK_MNT "/mmio32.llext");
#else
	static const uint8_t latch_buf[] = {
#include "latch_ext.inc"
	};
	static const uint8_t mmio32_buf[] = {
#include "mmio32_ext.inc"
	};
	static struct llext_buf_loader latch_ldr = LLEXT_BUF_LOADER(latch_buf, sizeof(latch_buf));
	static struct llext_buf_loader mmio32_ldr =
		LLEXT_BUF_LOADER(mmio32_buf, sizeof(mmio32_buf));
#endif
	static struct dynamic_gpio gpios[] = {
		{
			.mod = DRIVER_MODULE_INIT(LATCH_NODE, "latch", &latch_ldr.loader,
						  "gpio_latch_device"),
			.led = GPIO_DT_SPEC_GET_BY_IDX(USER_NODE, led_gpios, 0),
		},
		{
			.mod = DRIVER_MODULE_INIT(MMIO32_NODE, "mmio32", &mmio32_ldr.loader,
						  "gpio_mmio32_device"),
			.led = GPIO_DT_SPEC_GET_BY_IDX(USER_NODE, led_gpios, 1),
		},
	};
	int ret;

#if defined(CONFIG_DRIVER_MODULE_FS)
	ret = ramdisk_mount(RAMDISK_MNT);
	if (ret != 0) {
		LOG_ERR("Failed to mount the RAM disk: %d", ret);
		return ret;
	}
#endif

	for (size_t i = 0; i < ARRAY_SIZE(gpios); i++) {
		ret = gpio_module_run(&gpios[i]);
		if (ret != 0) {
			return ret;
		}
	}

	LOG_INF("Dynamic device sample done");

	return 0;
}
