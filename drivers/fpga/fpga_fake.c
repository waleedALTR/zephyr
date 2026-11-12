/*
 * Copyright (c) 2026, Altera Corporation.
 * SPDX-License-Identifier: Apache-2.0
 */

#define DT_DRV_COMPAT zephyr_fake_fpga

#include <stdbool.h>
#include <stdint.h>

#include <zephyr/device.h>
#include <zephyr/drivers/fpga.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/crc.h>

LOG_MODULE_REGISTER(fpga_fake, CONFIG_FPGA_LOG_LEVEL);

struct fpga_fake_data {
	bool loaded;
};

static enum FPGA_status fpga_fake_get_status(const struct device *dev)
{
	const struct fpga_fake_data *data = dev->data;

	return data->loaded ? FPGA_STATUS_ACTIVE : FPGA_STATUS_INACTIVE;
}

static int fpga_fake_reset(const struct device *dev)
{
	struct fpga_fake_data *data = dev->data;

	data->loaded = false;

	return 0;
}

static int fpga_fake_load(const struct device *dev, uint32_t *image_ptr, uint32_t img_size)
{
	struct fpga_fake_data *data = dev->data;

	LOG_INF("%s: %u bytes, CRC-32 0x%08x", dev->name, img_size,
		crc32_ieee((const uint8_t *)image_ptr, img_size));

	data->loaded = true;

	return 0;
}

static DEVICE_API(fpga, fpga_fake_api) = {
	.get_status = fpga_fake_get_status,
	.reset = fpga_fake_reset,
	.load = fpga_fake_load,
};

#define FPGA_FAKE_DEFINE(inst)                                                                     \
	static struct fpga_fake_data fpga_fake_data_##inst;                                        \
                                                                                                   \
	DEVICE_DT_INST_DEFINE(inst, NULL, NULL, &fpga_fake_data_##inst, NULL, POST_KERNEL,         \
			      CONFIG_FPGA_INIT_PRIORITY, &fpga_fake_api);

DT_INST_FOREACH_STATUS_OKAY(FPGA_FAKE_DEFINE)
