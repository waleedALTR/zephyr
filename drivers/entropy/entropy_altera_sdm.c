/*
 * SPDX-FileCopyrightText: Copyright (c) 2026, Altera Corporation.
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Random number generator of the Altera SoC FPGA Secure Device Manager
 * (SDM), read with the SDM mailbox FCS RANDOM_GEN command in a crypto service
 * session.
 */

#define DT_DRV_COMPAT altr_socfpga_sdm_rng

#include <errno.h>
#include <string.h>
#include <zephyr/device.h>
#include <zephyr/drivers/entropy.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/util.h>

#include <altera_sdm.h>

LOG_MODULE_REGISTER(altera_sdm_rng, CONFIG_ENTROPY_LOG_LEVEL);

BUILD_ASSERT(CONFIG_ENTROPY_ALTERA_SDM_INIT_PRIORITY > CONFIG_ALTERA_SDM_INIT_PRIORITY,
	     "SDM RNG must be initialized after the SDM");

#define ALTERA_SDM_CMD_FCS_RANDOM_GEN       0x80U
#define ALTERA_SDM_CMD_FCS_OPEN_CS_SESSION  0xA0U
#define ALTERA_SDM_CMD_FCS_CLOSE_CS_SESSION 0xA1U

/* RANDOM_GEN arguments: session ID, context ID, crypto header, size in bytes */
#define ALTERA_SDM_RNG_CONTEXT_ID 1U
#define ALTERA_SDM_RNG_CS_FLAGS   ((BIT(0) | BIT(2)) << 24) /* INIT | FINALIZE */

/* Random words requested per RANDOM_GEN command */
#define ALTERA_SDM_RNG_WORDS 8U

/* The RANDOM_GEN response echoes three words before the random data */
#define ALTERA_SDM_RNG_RESP_OFFSET 3U

struct altera_sdm_rng_config {
	const struct device *sdm;
};

static int altera_sdm_rng_fill(const struct device *sdm, uint32_t session, uint8_t *buffer,
			       uint16_t length)
{
	const uint32_t args[] = {session, ALTERA_SDM_RNG_CONTEXT_ID, ALTERA_SDM_RNG_CS_FLAGS,
				 ALTERA_SDM_RNG_WORDS * sizeof(uint32_t)};
	uint32_t resp[ALTERA_SDM_RNG_RESP_OFFSET + ALTERA_SDM_RNG_WORDS];
	size_t resp_len;
	size_t chunk;
	int ret = 0;

	while (length > 0U) {
		resp_len = ARRAY_SIZE(resp);

		ret = altera_sdm_mailbox_send(sdm, ALTERA_SDM_CMD_FCS_RANDOM_GEN, args,
					      ARRAY_SIZE(args), resp, &resp_len);
		if (ret != 0) {
			break;
		}

		if (resp_len != ARRAY_SIZE(resp)) {
			LOG_ERR("Unexpected response length %zu", resp_len);
			ret = -EIO;
			break;
		}

		chunk = MIN((size_t)length, ALTERA_SDM_RNG_WORDS * sizeof(uint32_t));
		memcpy(buffer, &resp[ALTERA_SDM_RNG_RESP_OFFSET], chunk);
		buffer += chunk;
		length -= chunk;
	}

	memset(resp, 0, sizeof(resp));

	return ret;
}

static int altera_sdm_rng_get_entropy(const struct device *dev, uint8_t *buffer, uint16_t length)
{
	const struct altera_sdm_rng_config *config = dev->config;
	uint32_t session;
	size_t resp_len = 1U;
	int ret;
	int err;

	ret = altera_sdm_mailbox_send(config->sdm, ALTERA_SDM_CMD_FCS_OPEN_CS_SESSION, NULL, 0U,
				      &session, &resp_len);
	if ((ret == 0) && (resp_len != 1U)) {
		LOG_ERR("Unexpected session response length %zu", resp_len);
		ret = -EIO;
	}
	if (ret == -EPERM) {
		LOG_ERR("Crypto services are not allowed by the device security settings");
	}
	if (ret != 0) {
		return ret;
	}

	ret = altera_sdm_rng_fill(config->sdm, session, buffer, length);

	err = altera_sdm_mailbox_send(config->sdm, ALTERA_SDM_CMD_FCS_CLOSE_CS_SESSION, &session,
				      1U, NULL, NULL);
	if (err != 0) {
		LOG_ERR("Failed to close session 0x%x (%d)", session, err);
	}

	return (ret != 0) ? ret : err;
}

static DEVICE_API(entropy, altera_sdm_rng_api) = {
	.get_entropy = altera_sdm_rng_get_entropy,
};

static int altera_sdm_rng_init(const struct device *dev)
{
	const struct altera_sdm_rng_config *config = dev->config;

	if (!device_is_ready(config->sdm)) {
		LOG_ERR_DEVICE_NOT_READY(config->sdm);
		return -ENODEV;
	}

	return 0;
}

#define ALTERA_SDM_RNG_DEFINE(inst)                                                                \
	static const struct altera_sdm_rng_config altera_sdm_rng_config_##inst = {                 \
		.sdm = DEVICE_DT_GET(DT_INST_PARENT(inst)),                                        \
	};                                                                                         \
                                                                                                   \
	DEVICE_DT_INST_DEFINE(inst, altera_sdm_rng_init, NULL, NULL,                               \
			      &altera_sdm_rng_config_##inst, POST_KERNEL,                          \
			      CONFIG_ENTROPY_ALTERA_SDM_INIT_PRIORITY, &altera_sdm_rng_api);

DT_INST_FOREACH_STATUS_OKAY(ALTERA_SDM_RNG_DEFINE)
