/*
 * SPDX-FileCopyrightText: Copyright (c) 2026, Altera Corporation.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file
 * @brief Key management of the Altera SoC FPGA FCS crypto driver
 *
 * The FCS crypto service of the Secure Device Manager (SDM) only uses keys
 * imported into its crypto service session. A key is imported as an SDM key
 * object, which carries the key ID. The key ID is then passed as the opaque
 * key handle of a cipher session (CAP_OPAQUE_KEY_HNDL):
 *
 * @code{.c}
 * ctx.key.handle = (void *)(uintptr_t)key_id;
 * @endcode
 */

#ifndef ZEPHYR_INCLUDE_DRIVERS_CRYPTO_ALTERA_FCS_H_
#define ZEPHYR_INCLUDE_DRIVERS_CRYPTO_ALTERA_FCS_H_

#include <stddef.h>
#include <stdint.h>

#include <zephyr/device.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @defgroup altera_fcs Altera FCS crypto driver
 * @ingroup crypto
 * @{
 */

/** @brief Largest SDM key object, in 32-bit words */
#define ALTERA_FCS_KEY_OBJ_MAX_WORDS 85U

/**
 * @brief Import a key object into the crypto service session.
 *
 * @param dev     FCS crypto device.
 * @param key_obj SDM key object.
 * @param words   Number of words in @p key_obj, at most
 *                ALTERA_FCS_KEY_OBJ_MAX_WORDS.
 * @param status  Key import status word returned by the SDM. May be NULL.
 *
 * @retval 0       The SDM processed the request; @p status holds its result.
 * @retval -EINVAL Invalid argument.
 * @retval -ENODEV Device not ready.
 * @retval -EIO    The SDM rejected the request.
 * @retval -errno  Other negative errno code from the SDM mailbox.
 */
int altera_fcs_import_key(const struct device *dev, const uint32_t *key_obj, size_t words,
			  uint32_t *status);

/**
 * @brief Remove a key from the crypto service session.
 *
 * @param dev    FCS crypto device.
 * @param key_id ID of the key to remove.
 *
 * @retval 0       Success.
 * @retval -ENODEV Device not ready.
 * @retval -EIO    The SDM rejected the request.
 * @retval -errno  Other negative errno code from the SDM mailbox.
 */
int altera_fcs_remove_key(const struct device *dev, uint32_t key_id);

/** @} */

#ifdef __cplusplus
}
#endif

#endif /* ZEPHYR_INCLUDE_DRIVERS_CRYPTO_ALTERA_FCS_H_ */
