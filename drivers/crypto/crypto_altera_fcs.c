/*
 * SPDX-FileCopyrightText: Copyright (c) 2026, Altera Corporation.
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * AES of the FPGA Crypto Service (FCS) of the Altera SoC FPGA Secure Device
 * Manager (SDM), reached through the SDM mailbox.
 */

#define DT_DRV_COMPAT altr_socfpga_sdm_fcs

#include <errno.h>
#include <string.h>
#include <zephyr/cache.h>
#include <zephyr/crypto/crypto.h>
#include <zephyr/device.h>
#include <zephyr/drivers/crypto/altera_fcs.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/util.h>

#include <altera_sdm.h>

LOG_MODULE_REGISTER(crypto_altera_fcs, CONFIG_CRYPTO_LOG_LEVEL);

BUILD_ASSERT(CONFIG_CRYPTO_INIT_PRIORITY > CONFIG_ALTERA_SDM_INIT_PRIORITY,
	     "FCS crypto must be initialized after the SDM");

/* SDM mailbox commands */
#define FCS_CMD_AES_CRYPT_REQ (0x81U | ALTERA_SDM_MAILBOX_INDIRECT)
#define FCS_CMD_OPEN_SESSION  0xA0U
#define FCS_CMD_IMPORT_KEY    0xA5U
#define FCS_CMD_REMOVE_KEY    0xA7U

/* Crypto service header: request flags and size of the parameters on INIT */
#define FCS_CS_FLAG_INIT     BIT(0)
#define FCS_CS_FLAG_UPDATE   BIT(1)
#define FCS_CS_FLAG_FINALIZE BIT(2)
#define FCS_CS_FLAGS_MASK    GENMASK(31, 24)
#define FCS_CS_SIZE_MASK     GENMASK(23, 0)

/* AES parameters: word 0 holds the block mode and the direction */
#define FCS_AES_BLOCK_MODE_MASK GENMASK(7, 0)
#define FCS_AES_CRYPT_MODE_MASK BIT(8)
#define FCS_AES_MODE_ECB        0U
#define FCS_AES_MODE_CBC        1U
#define FCS_AES_MODE_CTR        2U
#define FCS_AES_ENCRYPT         0U
#define FCS_AES_DECRYPT         1U
#define FCS_AES_PARAM_WORDS     7U
#define FCS_AES_ECB_PARAM_WORDS 3U
#define FCS_AES_IV_WORD         3U

/* Session, context, header, key ID, parameters, source and destination */
#define FCS_AES_REQ_MAX_WORDS (4U + FCS_AES_PARAM_WORDS + 4U)
#define FCS_AES_RESP_WORDS    8U

/* The SDM processes AES data in multiples of 32 bytes */
#define FCS_AES_DATA_ALIGN 32U
#define FCS_AES_BLOCK_SIZE 16U

/* Words reserved as zero after the session ID of the key requests */
#define FCS_KEY_RESERVED_WORDS 2U

#define FCS_BUF_ALIGN 64U

#define FCS_CIPHER_CAPS                                                                            \
	(CAP_OPAQUE_KEY_HNDL | CAP_INPLACE_OPS | CAP_SEPARATE_IO_BUFS | CAP_SYNC_OPS |             \
	 CAP_NO_IV_PREFIX)

BUILD_ASSERT((CONFIG_CRYPTO_ALTERA_FCS_BUF_SIZE % FCS_BUF_ALIGN) == 0U,
	     "FCS buffer size must be a multiple of 64 bytes");

struct fcs_session {
	bool in_use;
	uint32_t key_id;
	uint32_t block_mode;
	uint32_t crypt_mode;
	uint32_t ctr_len;
};

struct fcs_config {
	const struct device *sdm;
};

struct fcs_data {
	struct k_mutex lock;
	uint32_t session_id;
	struct fcs_session sessions[CONFIG_CRYPTO_ALTERA_FCS_MAX_SESSION];
	/* The SDM reads and writes these by physical address */
	uint8_t in[CONFIG_CRYPTO_ALTERA_FCS_BUF_SIZE] __aligned(FCS_BUF_ALIGN);
	uint8_t out[CONFIG_CRYPTO_ALTERA_FCS_BUF_SIZE] __aligned(FCS_BUF_ALIGN);
};

static uint32_t fcs_context_id(const struct fcs_data *data, const struct fcs_session *s)
{
	return (uint32_t)(s - data->sessions) + 1U;
}

/*
 * Run one AES operation from INIT to FINALIZE. The data goes through the
 * bounce buffers in chunks; the last chunk is zero-padded to the SDM
 * alignment and the output of the padding is dropped, which leaves the
 * output of the ECB, CBC and CTR modes unchanged.
 */
static int fcs_aes_run(const struct device *dev, struct fcs_session *s, const uint8_t *iv,
		       const uint8_t *src, uint8_t *dst, size_t len)
{
	const struct fcs_config *config = dev->config;
	struct fcs_data *data = dev->data;
	uint32_t param[FCS_AES_PARAM_WORDS] = {0};
	uint32_t req[FCS_AES_REQ_MAX_WORDS];
	uint32_t resp[FCS_AES_RESP_WORDS];
	size_t param_words;
	size_t padded = ROUND_UP(len, FCS_AES_DATA_ALIGN);
	size_t done = 0U;
	bool first = true;
	int ret = 0;

	param[0] = FIELD_PREP(FCS_AES_BLOCK_MODE_MASK, s->block_mode) |
		   FIELD_PREP(FCS_AES_CRYPT_MODE_MASK, s->crypt_mode);
	if (s->block_mode == FCS_AES_MODE_ECB) {
		param_words = FCS_AES_ECB_PARAM_WORDS;
	} else {
		param_words = FCS_AES_PARAM_WORDS;
		memcpy(&param[FCS_AES_IV_WORD], iv, FCS_AES_BLOCK_SIZE);
	}

	k_mutex_lock(&data->lock, K_FOREVER);

	while (done < padded) {
		size_t chunk = MIN(padded - done, sizeof(data->in));
		size_t copy = MIN(chunk, len - done);
		bool last = (done + chunk) == padded;
		uint32_t flags = (first ? FCS_CS_FLAG_INIT : 0U) |
				 (last ? FCS_CS_FLAG_FINALIZE : FCS_CS_FLAG_UPDATE);
		size_t param_size = first ? (param_words * sizeof(uint32_t)) : 0U;
		size_t resp_len = ARRAY_SIZE(resp);
		size_t n = 0U;

		memcpy(data->in, &src[done], copy);
		memset(&data->in[copy], 0, chunk - copy);
		sys_cache_data_flush_range(data->in, chunk);
		sys_cache_data_invd_range(data->out, chunk);

		req[n++] = data->session_id;
		req[n++] = fcs_context_id(data, s);
		req[n++] = FIELD_PREP(FCS_CS_FLAGS_MASK, flags) |
			   FIELD_PREP(FCS_CS_SIZE_MASK, param_size);
		if (first) {
			req[n++] = s->key_id;
			memcpy(&req[n], param, param_words * sizeof(uint32_t));
			n += param_words;
		}
		req[n++] = (uint32_t)(uintptr_t)data->in;
		req[n++] = chunk;
		req[n++] = (uint32_t)(uintptr_t)data->out;
		req[n++] = chunk;

		ret = altera_sdm_mailbox_send(config->sdm, FCS_CMD_AES_CRYPT_REQ, req, n, resp,
					      &resp_len);
		if (ret != 0) {
			LOG_ERR("AES request failed (%d)", ret);
			break;
		}

		sys_cache_data_invd_range(data->out, chunk);
		memcpy(&dst[done], data->out, copy);

		done += chunk;
		first = false;
	}

	memset(data->in, 0, sizeof(data->in));
	memset(data->out, 0, sizeof(data->out));

	k_mutex_unlock(&data->lock);

	return ret;
}

static int fcs_check_pkt(struct cipher_pkt *pkt, size_t out_len)
{
	if (pkt->in_len <= 0) {
		return -EINVAL;
	}

	if (pkt->out_buf == NULL) {
		pkt->out_buf = pkt->in_buf;
		pkt->out_buf_max = pkt->in_len;
	}

	if ((size_t)pkt->out_buf_max < out_len) {
		return -EINVAL;
	}

	return 0;
}

static int fcs_ecb_op(struct cipher_ctx *ctx, struct cipher_pkt *pkt)
{
	int ret;

	if (((size_t)pkt->in_len % FCS_AES_BLOCK_SIZE) != 0U) {
		return -EINVAL;
	}

	ret = fcs_check_pkt(pkt, pkt->in_len);
	if (ret != 0) {
		return ret;
	}

	ret = fcs_aes_run(ctx->device, ctx->drv_sessn_state, NULL, pkt->in_buf, pkt->out_buf,
			  pkt->in_len);
	if (ret == 0) {
		pkt->out_len = pkt->in_len;
	}

	return ret;
}

static int fcs_cbc_op(struct cipher_ctx *ctx, struct cipher_pkt *pkt, uint8_t *iv)
{
	struct fcs_session *s = ctx->drv_sessn_state;
	bool prefix = (ctx->flags & CAP_NO_IV_PREFIX) == 0U;
	bool encrypt = s->crypt_mode == FCS_AES_ENCRYPT;
	size_t skip = (prefix && !encrypt) ? FCS_AES_BLOCK_SIZE : 0U;
	size_t pos = (prefix && encrypt) ? FCS_AES_BLOCK_SIZE : 0U;
	size_t len;
	int ret;

	if (((size_t)pkt->in_len <= skip) || (((size_t)pkt->in_len % FCS_AES_BLOCK_SIZE) != 0U)) {
		return -EINVAL;
	}

	len = pkt->in_len - skip;

	ret = fcs_check_pkt(pkt, pos + len);
	if (ret != 0) {
		return ret;
	}

	ret = fcs_aes_run(ctx->device, s, iv, &pkt->in_buf[skip], &pkt->out_buf[pos], len);
	if (ret != 0) {
		return ret;
	}

	if (pos > 0U) {
		memcpy(pkt->out_buf, iv, FCS_AES_BLOCK_SIZE);
	}

	pkt->out_len = pos + len;

	return 0;
}

static int fcs_ctr_op(struct cipher_ctx *ctx, struct cipher_pkt *pkt, uint8_t *iv)
{
	struct fcs_session *s = ctx->drv_sessn_state;
	size_t iv_len = FCS_AES_BLOCK_SIZE - (s->ctr_len / BITS_PER_BYTE);
	uint8_t counter[FCS_AES_BLOCK_SIZE] = {0};
	int ret;

	ret = fcs_check_pkt(pkt, pkt->in_len);
	if (ret != 0) {
		return ret;
	}

	/* The counter part of the first counter block starts at zero */
	memcpy(counter, iv, iv_len);

	ret = fcs_aes_run(ctx->device, s, counter, pkt->in_buf, pkt->out_buf, pkt->in_len);
	if (ret == 0) {
		pkt->out_len = pkt->in_len;
	}

	return ret;
}

static int fcs_query_hw_caps(const struct device *dev)
{
	ARG_UNUSED(dev);

	return FCS_CIPHER_CAPS;
}

static int fcs_cipher_begin_session(const struct device *dev, struct cipher_ctx *ctx,
				    enum cipher_algo algo, enum cipher_mode mode,
				    enum cipher_op op_type)
{
	struct fcs_data *data = dev->data;
	struct fcs_session *s = NULL;

	if (algo != CRYPTO_CIPHER_ALGO_AES) {
		LOG_ERR("Unsupported algorithm");
		return -ENOTSUP;
	}

	if (((ctx->flags & ~FCS_CIPHER_CAPS) != 0U) || ((ctx->flags & CAP_OPAQUE_KEY_HNDL) == 0U)) {
		LOG_ERR("Unsupported flags 0x%x", ctx->flags);
		return -ENOTSUP;
	}

	if ((mode == CRYPTO_CIPHER_MODE_CTR) &&
	    ((ctx->mode_params.ctr_info.ctr_len == 0U) ||
	     ((ctx->mode_params.ctr_info.ctr_len % BITS_PER_BYTE) != 0U) ||
	     (ctx->mode_params.ctr_info.ctr_len > (FCS_AES_BLOCK_SIZE * BITS_PER_BYTE)))) {
		LOG_ERR("Invalid CTR counter length %u", ctx->mode_params.ctr_info.ctr_len);
		return -EINVAL;
	}

	k_mutex_lock(&data->lock, K_FOREVER);
	for (size_t i = 0U; i < ARRAY_SIZE(data->sessions); i++) {
		if (!data->sessions[i].in_use) {
			s = &data->sessions[i];
			s->in_use = true;
			break;
		}
	}
	k_mutex_unlock(&data->lock);

	if (s == NULL) {
		LOG_ERR("No free session");
		return -ENOSPC;
	}

	s->key_id = (uint32_t)(uintptr_t)ctx->key.handle;
	s->crypt_mode = (op_type == CRYPTO_CIPHER_OP_ENCRYPT) ? FCS_AES_ENCRYPT : FCS_AES_DECRYPT;
	s->ctr_len = ctx->mode_params.ctr_info.ctr_len;

	switch (mode) {
	case CRYPTO_CIPHER_MODE_ECB:
		s->block_mode = FCS_AES_MODE_ECB;
		ctx->ops.block_crypt_hndlr = fcs_ecb_op;
		break;
	case CRYPTO_CIPHER_MODE_CBC:
		s->block_mode = FCS_AES_MODE_CBC;
		ctx->ops.cbc_crypt_hndlr = fcs_cbc_op;
		break;
	case CRYPTO_CIPHER_MODE_CTR:
		s->block_mode = FCS_AES_MODE_CTR;
		ctx->ops.ctr_crypt_hndlr = fcs_ctr_op;
		break;
	default:
		LOG_ERR("Unsupported mode %d", mode);
		s->in_use = false;
		return -ENOTSUP;
	}

	ctx->ops.cipher_mode = mode;
	ctx->drv_sessn_state = s;

	return 0;
}

static int fcs_cipher_free_session(const struct device *dev, struct cipher_ctx *ctx)
{
	struct fcs_data *data = dev->data;
	struct fcs_session *s = ctx->drv_sessn_state;

	k_mutex_lock(&data->lock, K_FOREVER);
	memset(s, 0, sizeof(*s));
	k_mutex_unlock(&data->lock);

	ctx->drv_sessn_state = NULL;

	return 0;
}

int altera_fcs_import_key(const struct device *dev, const uint32_t *key_obj, size_t words,
			  uint32_t *status)
{
	const struct fcs_config *config;
	struct fcs_data *data;
	uint32_t req[1U + FCS_KEY_RESERVED_WORDS + ALTERA_FCS_KEY_OBJ_MAX_WORDS] = {0};
	uint32_t resp;
	size_t resp_len = 1U;
	int ret;

	if ((dev == NULL) || (key_obj == NULL) || (words == 0U) ||
	    (words > ALTERA_FCS_KEY_OBJ_MAX_WORDS)) {
		return -EINVAL;
	}

	if (!device_is_ready(dev)) {
		return -ENODEV;
	}

	config = dev->config;
	data = dev->data;

	req[0] = data->session_id;
	memcpy(&req[1U + FCS_KEY_RESERVED_WORDS], key_obj, words * sizeof(uint32_t));

	ret = altera_sdm_mailbox_send(config->sdm, FCS_CMD_IMPORT_KEY, req,
				      1U + FCS_KEY_RESERVED_WORDS + words, &resp, &resp_len);
	if (ret != 0) {
		return ret;
	}

	if (status != NULL) {
		*status = (resp_len > 0U) ? resp : 0U;
	}

	return 0;
}

int altera_fcs_remove_key(const struct device *dev, uint32_t key_id)
{
	const struct fcs_config *config;
	struct fcs_data *data;
	uint32_t req[1U + FCS_KEY_RESERVED_WORDS + 1U] = {0};

	if (dev == NULL) {
		return -EINVAL;
	}

	if (!device_is_ready(dev)) {
		return -ENODEV;
	}

	config = dev->config;
	data = dev->data;

	req[0] = data->session_id;
	req[ARRAY_SIZE(req) - 1U] = key_id;

	return altera_sdm_mailbox_send(config->sdm, FCS_CMD_REMOVE_KEY, req, ARRAY_SIZE(req), NULL,
				       NULL);
}

static DEVICE_API(crypto, fcs_api) = {
	.query_hw_caps = fcs_query_hw_caps,
	.cipher_begin_session = fcs_cipher_begin_session,
	.cipher_free_session = fcs_cipher_free_session,
};

static int fcs_init(const struct device *dev)
{
	const struct fcs_config *config = dev->config;
	struct fcs_data *data = dev->data;
	size_t resp_len = 1U;
	int ret;

	k_mutex_init(&data->lock);

	if (!device_is_ready(config->sdm)) {
		LOG_ERR_DEVICE_NOT_READY(config->sdm);
		return -ENODEV;
	}

	/* The mailbox carries 32-bit buffer addresses */
	if (((uintptr_t)data->out + sizeof(data->out)) > UINT32_MAX) {
		LOG_ERR("Buffers not addressable by the SDM");
		return -EINVAL;
	}

	ret = altera_sdm_mailbox_send(config->sdm, FCS_CMD_OPEN_SESSION, NULL, 0U,
				      &data->session_id, &resp_len);
	if ((ret == 0) && (resp_len != 1U)) {
		ret = -EIO;
	}

	if (ret != 0) {
		LOG_ERR("Failed to open crypto service session (%d)", ret);
		return ret;
	}

	return 0;
}

#define FCS_DEFINE(inst)                                                                           \
	static const struct fcs_config fcs_config_##inst = {                                       \
		.sdm = DEVICE_DT_GET(DT_INST_PARENT(inst)),                                        \
	};                                                                                         \
                                                                                                   \
	static struct fcs_data fcs_data_##inst;                                                    \
                                                                                                   \
	DEVICE_DT_INST_DEFINE(inst, fcs_init, NULL, &fcs_data_##inst, &fcs_config_##inst,          \
			      POST_KERNEL, CONFIG_CRYPTO_INIT_PRIORITY, &fcs_api);

DT_INST_FOREACH_STATUS_OKAY(FCS_DEFINE)
