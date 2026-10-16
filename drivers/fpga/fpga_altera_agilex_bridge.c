/*
 * Copyright (c) 2024, Intel Corporation.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#define DT_DRV_COMPAT altr_socfpga_agilex_bridge

#include <errno.h>
#include <zephyr/device.h>
#include <zephyr/drivers/sip_svc/sip_svc_agilex_smc.h>
#include <zephyr/drivers/fpga.h>
#include <zephyr/logging/log.h>
#include <altera_sdm.h>
#include "fpga_altera_agilex_bridge.h"

LOG_MODULE_REGISTER(fpga_altera, CONFIG_FPGA_LOG_LEVEL);

struct fpga_bridge_config {
	/* Secure Device Manager */
	const struct device *sdm;
};

/* Validate the Reconfig status response */
static int32_t fpga_reconfig_status_validate(struct fpga_config_status *reconfig_status_resp)
{
	uint32_t ret = 0;

	/* Check for any error */
	ret = reconfig_status_resp->state;
	if (ret == MBOX_CFGSTAT_VAB_BS_PREAUTH) {
		return MBOX_CONFIG_STATUS_STATE_CONFIG;
	}

	if (ret && ret != MBOX_CONFIG_STATUS_STATE_CONFIG) {
		return ret;
	}

	/* Make sure nStatus is not 0 */
	ret = reconfig_status_resp->pin_status.pin_status;
	if (!(ret & RECONFIG_PIN_STATUS_NSTATUS)) {
		return MBOX_CFGSTAT_STATE_ERROR_HARDWARE;
	}

	ret = reconfig_status_resp->soft_function_status;
	if ((ret & RECONFIG_SOFTFUNC_STATUS_CONF_DONE) &&
	    (ret & RECONFIG_SOFTFUNC_STATUS_INIT_DONE) && !reconfig_status_resp->state) {
		return 0; /* Configuration success */
	}

	return MBOX_CONFIG_STATUS_STATE_CONFIG;
}

/* Will send the mailbox command to check the status of the FPGA */
static int32_t fpga_config_ready_check(const struct device *dev)
{
	const struct fpga_bridge_config *config = dev->config;
	/* Response header slot followed by the response data */
	uint32_t resp[FPGA_CONFIG_STATUS_RESPONSE_MAX_LEN] = {0};
	size_t resp_len = ARRAY_SIZE(resp) - 1U;
	int32_t ret;

	/* Sending the FPGA config status mailbox command */
	ret = altera_sdm_mailbox_send(config->sdm, FPGA_CONFIG_STATUS, NULL, 0, &resp[1],
				      &resp_len);
	if (ret != 0) {
		LOG_ERR("Failed to Send the Mailbox Command (%d)", ret);
		return -ECANCELED;
	}

	/* Verify the mailbox response holds the fields up to the soft function status */
	if (resp_len < MBOX_RECONFIG_STATUS_SOFT_FUNCTION) {
		return -EINVAL;
	}

	/* Verify the FPGA config status response */
	return fpga_reconfig_status_validate((struct fpga_config_status *)resp);
}

static int32_t socfpga_bridges_reset(const struct device *dev, uint32_t enable)
{
	const struct fpga_bridge_config *config = dev->config;
	uint64_t args[3] = {0};
	int ret;

	args[1] = FIELD_GET(BIT(0), enable) | BIT(1);
	args[2] = BRIDGE_MASK;

	ret = altera_sdm_smc_call(config->sdm, SMC_FUNC_ID_SET_HPS_BRIDGES, args,
				  ARRAY_SIZE(args), NULL);
	if (ret != 0) {
		LOG_ERR("Failed to send the smc Command (%d)", ret);
	}

	return ret;
}

static int altera_fpga_on(const struct device *dev)
{
	int32_t ret = 0;

	if (!dev) {
		LOG_ERR("No such device found");
		return -ENODEV;
	}

	/* Check FPGA status before bridge enable/disable */
	ret = fpga_config_ready_check(dev);
	if (ret) {
		LOG_ERR("FPGA not ready. Bridge reset aborted!");
		return -EIO;
	}

	/* Bridge reset */
	ret = socfpga_bridges_reset(dev, 0x01);
	if (ret) {
		LOG_ERR("Bridge reset failed");
	}

	return ret;
}

static int altera_fpga_off(const struct device *dev)
{
	int32_t ret = 0;

	if (!dev) {
		LOG_ERR("No such device found");
		return -ENODEV;
	}

	/* Check FPGA status before bridge enable/disable */
	ret = fpga_config_ready_check(dev);
	if (ret) {
		LOG_ERR("FPGA not ready. Bridge reset aborted!");
		return -EIO;
	}

	/* Bridge reset */
	ret = socfpga_bridges_reset(dev, 0x00);
	if (ret) {
		LOG_ERR("Bridge reset failed");
	}

	return ret;
}

static int altera_fpga_init(const struct device *dev)
{
	const struct fpga_bridge_config *config = dev->config;

	if (!device_is_ready(config->sdm)) {
		LOG_ERR_DEVICE_NOT_READY(config->sdm);
		return -ENODEV;
	}

	return 0;
}

static DEVICE_API(fpga, altera_fpga_api) = {
	.on = altera_fpga_on,
	.off = altera_fpga_off,
};

#define CREATE_ALTERA_FPGA_BRIDGE_DEV(inst)						\
	static const struct fpga_bridge_config fpga_bridge_config_##inst = {		\
		.sdm = DEVICE_DT_GET_ONE(altr_socfpga_sdm),				\
	};										\
	DEVICE_DT_INST_DEFINE(inst,						\
			altera_fpga_init,					\
			NULL, NULL,						\
			&fpga_bridge_config_##inst, POST_KERNEL,		\
			CONFIG_FPGA_INIT_PRIORITY,				\
			&altera_fpga_api);					\

DT_INST_FOREACH_STATUS_OKAY(CREATE_ALTERA_FPGA_BRIDGE_DEV);
