/*
 * SPDX-FileCopyrightText: Copyright (c) 2026, Altera Corporation.
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Hardware monitor of the Altera SoC FPGA Secure Device Manager (SDM): voltage
 * and temperature sensors read with the SDM mailbox GET_VOLTAGE and
 * GET_TEMPERATURE commands.
 */

#define DT_DRV_COMPAT altr_socfpga_sdm_hwmon

#include <errno.h>
#include <zephyr/device.h>
#include <zephyr/drivers/sensor.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/util.h>

#include <altera_sdm.h>

LOG_MODULE_REGISTER(altera_sdm_hwmon, CONFIG_SENSOR_LOG_LEVEL);

#define SDM_CMD_GET_VOLTAGE     0x18U
#define SDM_CMD_GET_TEMPERATURE 0x19U

/* The voltage is an unsigned fixed point number with 16 fractional bits */
#define SDM_VOLTAGE_FRAC_BITS 16U

/* The temperature is a signed fixed point number in degrees Celsius */
#define SDM_TEMP_FRAC_DIV 256

/* GET_TEMPERATURE argument: channel mask in bits 31:16, sensor location in 15:0 */
#define SDM_TEMP_CHANNEL_SHIFT 16U
#define SDM_TEMP_LOCATION_MASK BIT_MASK(16)

/* GET_TEMPERATURE error codes returned in place of a temperature */
#define SDM_TEMP_ERR_FIRST 0x80000000U
#define SDM_TEMP_ERR_LAST  0x80000006U

#define SDM_MAX_CHANNEL 15U

#define SDM_CHANNEL_NONE UINT32_MAX

struct sdm_hwmon_config {
	const struct device *sdm;
	uint32_t volt_channel;
	uint32_t temp_arg;
};

struct sdm_hwmon_data {
	uint32_t volt_raw;
	int32_t temp_raw;
};

static int sdm_hwmon_read(const struct device *dev, uint32_t cmd, uint32_t arg, uint32_t *resp)
{
	const struct sdm_hwmon_config *config = dev->config;
	size_t resp_len = 1U;
	int ret;

	ret = altera_sdm_mailbox_send(config->sdm, cmd, &arg, 1U, resp, &resp_len);
	if (ret != 0) {
		return ret;
	}

	if (resp_len != 1U) {
		LOG_ERR("Unexpected response length %zu", resp_len);
		return -EIO;
	}

	return 0;
}

static int sdm_hwmon_fetch_voltage(const struct device *dev)
{
	const struct sdm_hwmon_config *config = dev->config;
	struct sdm_hwmon_data *data = dev->data;

	return sdm_hwmon_read(dev, SDM_CMD_GET_VOLTAGE, BIT(config->volt_channel),
			      &data->volt_raw);
}

static int sdm_hwmon_fetch_temp(const struct device *dev)
{
	const struct sdm_hwmon_config *config = dev->config;
	struct sdm_hwmon_data *data = dev->data;
	uint32_t resp;
	int ret;

	ret = sdm_hwmon_read(dev, SDM_CMD_GET_TEMPERATURE, config->temp_arg, &resp);
	if (ret != 0) {
		return ret;
	}

	if ((resp >= SDM_TEMP_ERR_FIRST) && (resp <= SDM_TEMP_ERR_LAST)) {
		LOG_ERR("Temperature read failed: 0x%08x", resp);
		return -EIO;
	}

	data->temp_raw = (int32_t)resp;

	return 0;
}

static int sdm_hwmon_sample_fetch(const struct device *dev, enum sensor_channel chan)
{
	const struct sdm_hwmon_config *config = dev->config;
	bool has_volt = config->volt_channel != SDM_CHANNEL_NONE;
	int ret;

	switch (chan) {
	case SENSOR_CHAN_ALL:
		if (has_volt) {
			ret = sdm_hwmon_fetch_voltage(dev);
			if (ret != 0) {
				return ret;
			}
		}
		return sdm_hwmon_fetch_temp(dev);
	case SENSOR_CHAN_VOLTAGE:
		if (!has_volt) {
			return -ENOTSUP;
		}
		return sdm_hwmon_fetch_voltage(dev);
	case SENSOR_CHAN_DIE_TEMP:
		return sdm_hwmon_fetch_temp(dev);
	default:
		return -ENOTSUP;
	}
}

static int sdm_hwmon_channel_get(const struct device *dev, enum sensor_channel chan,
				 struct sensor_value *val)
{
	const struct sdm_hwmon_config *config = dev->config;
	struct sdm_hwmon_data *data = dev->data;
	uint32_t frac;

	switch (chan) {
	case SENSOR_CHAN_VOLTAGE:
		if (config->volt_channel == SDM_CHANNEL_NONE) {
			return -ENOTSUP;
		}
		frac = data->volt_raw & BIT_MASK(SDM_VOLTAGE_FRAC_BITS);
		val->val1 = (int32_t)(data->volt_raw >> SDM_VOLTAGE_FRAC_BITS);
		val->val2 = (int32_t)(((uint64_t)frac * 1000000U) >> SDM_VOLTAGE_FRAC_BITS);
		return 0;
	case SENSOR_CHAN_DIE_TEMP:
		val->val1 = data->temp_raw / SDM_TEMP_FRAC_DIV;
		val->val2 = (data->temp_raw % SDM_TEMP_FRAC_DIV) * 1000000 / SDM_TEMP_FRAC_DIV;
		return 0;
	default:
		return -ENOTSUP;
	}
}

static DEVICE_API(sensor, sdm_hwmon_api) = {
	.sample_fetch = sdm_hwmon_sample_fetch,
	.channel_get = sdm_hwmon_channel_get,
};

static int sdm_hwmon_init(const struct device *dev)
{
	const struct sdm_hwmon_config *config = dev->config;

	if (!device_is_ready(config->sdm)) {
		LOG_ERR_DEVICE_NOT_READY(config->sdm);
		return -ENODEV;
	}

	return 0;
}

#define SDM_HWMON_TEMP_ARG(inst)                                                                   \
	((DT_INST_PROP(inst, temperature_location) & SDM_TEMP_LOCATION_MASK) |                     \
	 COND_CODE_1(DT_INST_NODE_HAS_PROP(inst, temperature_channel),                             \
		     (BIT(DT_INST_PROP(inst, temperature_channel) + SDM_TEMP_CHANNEL_SHIFT)), (0)))

#define SDM_HWMON_DEFINE(inst)                                                                     \
	BUILD_ASSERT(DT_INST_PROP_OR(inst, voltage_channel, 0) <= SDM_MAX_CHANNEL,                 \
		     "SDM voltage channel out of range");                                          \
	BUILD_ASSERT(DT_INST_PROP_OR(inst, temperature_channel, 0) <= SDM_MAX_CHANNEL,             \
		     "SDM temperature channel out of range");                                      \
	BUILD_ASSERT(DT_INST_PROP(inst, temperature_location) <= SDM_TEMP_LOCATION_MASK,           \
		     "SDM temperature sensor location out of range");                              \
                                                                                                   \
	static const struct sdm_hwmon_config sdm_hwmon_config_##inst = {                           \
		.sdm = DEVICE_DT_GET(DT_INST_PARENT(inst)),                                        \
		.volt_channel = DT_INST_PROP_OR(inst, voltage_channel, SDM_CHANNEL_NONE),          \
		.temp_arg = SDM_HWMON_TEMP_ARG(inst),                                              \
	};                                                                                         \
                                                                                                   \
	static struct sdm_hwmon_data sdm_hwmon_data_##inst;                                        \
                                                                                                   \
	SENSOR_DEVICE_DT_INST_DEFINE(inst, sdm_hwmon_init, NULL, &sdm_hwmon_data_##inst,           \
				     &sdm_hwmon_config_##inst, POST_KERNEL,                        \
				     CONFIG_SENSOR_INIT_PRIORITY, &sdm_hwmon_api);

DT_INST_FOREACH_STATUS_OKAY(SDM_HWMON_DEFINE)
