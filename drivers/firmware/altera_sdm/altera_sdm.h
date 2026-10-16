/*
 * SPDX-FileCopyrightText: Copyright (c) 2026, Altera Corporation.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file
 * @brief Altera SoC FPGA Secure Device Manager (SDM) mailbox API
 */

#ifndef ZEPHYR_DRIVERS_FIRMWARE_ALTERA_SDM_ALTERA_SDM_H_
#define ZEPHYR_DRIVERS_FIRMWARE_ALTERA_SDM_ALTERA_SDM_H_

#include <stddef.h>
#include <stdint.h>

#include <zephyr/device.h>
#include <zephyr/sys/util_macro.h>

/**
 * @defgroup altera_sdm Altera SDM mailbox
 * @{
 */

/** @brief Largest number of argument words in one mailbox command */
#define ALTERA_SDM_MAILBOX_MAX_ARGS 1023U

/**
 * @brief Flag for altera_sdm_mailbox_send() marking an indirect command.
 *
 * The arguments of an indirect command carry the addresses and sizes of
 * buffers the SDM reads and writes, rather than the data itself.
 */
#define ALTERA_SDM_MAILBOX_INDIRECT BIT(11)

/**
 * @brief Send a command to the SDM mailbox and wait for its response.
 *
 * Blocks until the SDM answers or CONFIG_ALTERA_SDM_TIMEOUT_MS elapses.
 * Commands from several threads are serialized.
 *
 * @param dev      SDM device, the parent node of the calling device.
 * @param cmd      Mailbox command code, optionally ORed with
 *                 ALTERA_SDM_MAILBOX_INDIRECT.
 * @param args     Command argument words. May be NULL when @p nargs is 0.
 * @param nargs    Number of words in @p args.
 * @param resp     Buffer for the response data words, without the response
 *                 header. May be NULL when @p resp_len is NULL.
 * @param resp_len On entry, the capacity of @p resp in words. On return, the
 *                 number of response data words. May be NULL when the
 *                 response carries no data.
 *
 * @retval 0           Success.
 * @retval -EINVAL     Invalid argument.
 * @retval -ENODEV     SDM device not ready.
 * @retval -ENOMEM     Out of heap memory for the mailbox buffers.
 * @retval -ETIMEDOUT  No response within CONFIG_ALTERA_SDM_TIMEOUT_MS.
 * @retval -EMSGSIZE   Response larger than @p resp.
 * @retval -EIO        The SMC call failed or the SDM returned an error.
 * @retval -errno      Other negative errno code from the SiP service.
 */
int altera_sdm_mailbox_send(const struct device *dev, uint16_t cmd, const uint32_t *args,
			    size_t nargs, uint32_t *resp, size_t *resp_len);

/** @brief Largest number of argument registers of a SiP service call */
#define ALTERA_SDM_SMC_MAX_ARGS 7U

/** @brief Result registers of a SiP service call */
struct altera_sdm_smc_res {
	/** Status, SMC_STATUS_OKAY on success */
	uint64_t a0;
	/** First result register */
	uint64_t a1;
	/** Second result register */
	uint64_t a2;
	/** Third result register */
	uint64_t a3;
};

/**
 * @brief Make a synchronous SiP service call to the EL3 firmware.
 *
 * Serves the SiP services that are not SDM mailbox commands, such as
 * SMC_FUNC_ID_SET_HPS_BRIDGES. Blocks until the call returns or
 * CONFIG_ALTERA_SDM_TIMEOUT_MS elapses. Calls from several threads are
 * serialized with the mailbox commands.
 *
 * @param dev     SDM device.
 * @param func_id SiP service function ID.
 * @param args    Argument registers a1 to a7, in order. May be NULL when
 *                @p nargs is 0.
 * @param nargs   Number of words in @p args, at most ALTERA_SDM_SMC_MAX_ARGS.
 *                The remaining argument registers are 0.
 * @param res     Result registers, also filled when the call returns an
 *                error status. May be NULL.
 *
 * @retval 0          Success.
 * @retval -EINVAL    Invalid argument.
 * @retval -ENODEV    SDM device not ready.
 * @retval -ETIMEDOUT No result within CONFIG_ALTERA_SDM_TIMEOUT_MS.
 * @retval -EIO       The call returned an error status.
 * @retval -errno     Other negative errno code from the SiP service.
 */
int altera_sdm_smc_call(const struct device *dev, uint32_t func_id, const uint64_t *args,
			size_t nargs, struct altera_sdm_smc_res *res);

/** @} */

#endif /* ZEPHYR_DRIVERS_FIRMWARE_ALTERA_SDM_ALTERA_SDM_H_ */
