/*
 * SPDX-FileCopyrightText: Copyright (c) 2026, Altera Corporation.
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Mailbox of the Altera SoC FPGA Secure Device Manager (SDM), reached
 * through the ARM SiP service.
 */

#define DT_DRV_COMPAT altr_socfpga_sdm

#include <errno.h>
#include <inttypes.h>
#include <string.h>
#include <zephyr/device.h>
#include <zephyr/drivers/sip_svc/sip_svc_agilex_mailbox.h>
#include <zephyr/drivers/sip_svc/sip_svc_agilex_smc.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sip_svc/sip_svc.h>
#include <zephyr/sys/util.h>

#include <altera_sdm.h>

LOG_MODULE_REGISTER(altera_sdm, CONFIG_LOG_DEFAULT_LEVEL);

BUILD_ASSERT(CONFIG_ALTERA_SDM_INIT_PRIORITY > CONFIG_ARM_SIP_SVC_SUBSYS_INIT_PRIORITY,
	     "SDM must be initialized after the SiP service");

/* Mailbox command and response header fields */
#define SDM_MB_CMD_ID_MASK     GENMASK(10, 0)
#define SDM_MB_RESP_ERROR_MASK GENMASK(10, 0)
#define SDM_MB_LENGTH_MASK     GENMASK(22, 12)

#define SDM_MB_HEADER_WORDS 1U

#define SDM_MB_CMD_CANCEL 0x03U

/* SDM response error codes */
#define SDM_MB_ERR_AUTH_FAIL    0x0AU
#define SDM_MB_ERR_NOT_ALLOWED  0x85U
#define SDM_MB_ERR_INVALID_CERT 0x91U

BUILD_ASSERT(ALTERA_SDM_SMC_MAX_ARGS == 7U, "SMC arguments map onto a1..a7");
BUILD_ASSERT(ALTERA_SDM_MAILBOX_MAX_ARGS <= (SIP_SVP_MB_MAX_WORD_SIZE - SDM_MB_HEADER_WORDS),
	     "Mailbox arguments exceed the SiP service mailbox size");
BUILD_ASSERT(ALTERA_SDM_MAILBOX_MAX_ARGS <= FIELD_GET(SDM_MB_LENGTH_MASK, SDM_MB_LENGTH_MASK),
	     "Mailbox arguments exceed the header length field");

struct altera_sdm_config {
	const char *method;
};

struct altera_sdm_data {
	void *ctrl;
	uint32_t token;
	struct k_mutex lock;
};

/*
 * One transaction, shared with the SiP service callback. The callback only runs
 * while the client is open, so xfer may live on the caller's stack.
 */
struct altera_sdm_xfer {
	struct k_sem done;
	uint32_t smc_status;
	struct altera_sdm_smc_res regs;
	uint32_t header;
	uint32_t *resp;
	size_t resp_cap;
	size_t resp_len;
};

static int altera_sdm_err_to_errno(uint32_t err)
{
	switch (err) {
	case 0U:
		return 0;
	case SDM_MB_ERR_AUTH_FAIL:
	case SDM_MB_ERR_INVALID_CERT:
		return -EBADMSG;
	case SDM_MB_ERR_NOT_ALLOWED:
		return -EPERM;
	default:
		return -EIO;
	}
}

static void altera_sdm_callback(uint32_t c_token, struct sip_svc_response *response)
{
	ARG_UNUSED(c_token);

	if (response == NULL) {
		return;
	}

	struct altera_sdm_xfer *xfer = response->priv_data;
	uint32_t *buf = (uint32_t *)(uintptr_t)response->resp_data_addr;
	size_t words = response->resp_data_size / sizeof(uint32_t);

	/* Low 16 bits of the EL3 a0 */
	xfer->smc_status = SIP_SVC_PROTO_HEADER_GET_CODE(response->header);
	xfer->regs.a0 = response->a0;
	xfer->regs.a1 = response->a1;
	xfer->regs.a2 = response->a2;
	xfer->regs.a3 = response->a3;

	if ((xfer->smc_status == SMC_STATUS_OKAY) && (buf != NULL) &&
	    (words >= SDM_MB_HEADER_WORDS)) {
		size_t len = FIELD_GET(SDM_MB_LENGTH_MASK, buf[0]);

		xfer->header = buf[0];
		xfer->resp_len = MIN(len, MIN(words - SDM_MB_HEADER_WORDS, xfer->resp_cap));
		if (xfer->resp_len > 0U) {
			memcpy(xfer->resp, &buf[SDM_MB_HEADER_WORDS],
			       xfer->resp_len * sizeof(uint32_t));
		}
	}

	/* The client owns the response buffer, the SiP service frees the command buffer */
	k_free(buf);

	k_sem_give(&xfer->done);
}

/* Close the client, cancelling a timed out mailbox command first */
static int altera_sdm_close(struct altera_sdm_data *data, bool cancel)
{
	struct sip_svc_request request = {0};
	uint32_t *cmd_buf = cancel ? k_malloc(sizeof(uint32_t)) : NULL;
	int ret;

	if (cmd_buf == NULL) {
		return sip_svc_close(data->ctrl, data->token, NULL);
	}

	cmd_buf[0] = FIELD_PREP(SDM_MB_CMD_ID_MASK, SDM_MB_CMD_CANCEL);
	request.header = SIP_SVC_PROTO_HEADER(SIP_SVC_PROTO_CMD_ASYNC, 0);
	request.a0 = SMC_FUNC_ID_MAILBOX_SEND_COMMAND;
	request.a2 = (unsigned long)(uintptr_t)cmd_buf;
	request.a3 = sizeof(uint32_t);

	ret = sip_svc_close(data->ctrl, data->token, &request);
	if (ret == -ENOTSUP) {
		/* Cancel not queued, the client is still open */
		k_free(cmd_buf);
		ret = sip_svc_close(data->ctrl, data->token, NULL);
	}

	return ret;
}

/*
 * Run one request on the SiP service client and wait for its callback. For an
 * asynchronous request, the command and response buffers are freed here when
 * the request cannot be queued; otherwise their ownership has passed on.
 */
static int altera_sdm_submit(const struct device *dev, struct sip_svc_request *request,
			     struct altera_sdm_xfer *xfer)
{
	struct altera_sdm_data *data = dev->data;
	bool async = SIP_SVC_PROTO_HEADER_GET_CODE(request->header) == SIP_SVC_PROTO_CMD_ASYNC;
	bool own_bufs = async;
	bool timed_out = false;
	int ret;
	int err;

	if (!device_is_ready(dev)) {
		ret = -ENODEV;
		goto out;
	}

	k_sem_init(&xfer->done, 0, 1);
	xfer->smc_status = SMC_STATUS_INVALID;
	request->priv_data = xfer;

	k_mutex_lock(&data->lock, K_FOREVER);

	/* Also waits for a cancelled command to drain */
	ret = sip_svc_open(data->ctrl, data->token, K_MSEC(CONFIG_ALTERA_SDM_TIMEOUT_MS));
	if (ret != 0) {
		LOG_ERR("Failed to open SiP service client (%d)", ret);
		goto unlock;
	}

	ret = sip_svc_send(data->ctrl, data->token, request, altera_sdm_callback);
	if (ret < 0) {
		LOG_ERR("Failed to send SiP service request 0x%lx (%d)", request->a0, ret);
		goto close;
	}

	own_bufs = false;
	ret = k_sem_take(&xfer->done, K_MSEC(CONFIG_ALTERA_SDM_TIMEOUT_MS));
	if (ret != 0) {
		LOG_ERR("SiP service request 0x%lx timed out", request->a0);
		timed_out = true;
		ret = -ETIMEDOUT;
	}

close:
	err = altera_sdm_close(data, async && timed_out);
	if (err != 0) {
		LOG_ERR("Failed to close SiP service client (%d)", err);
		ret = (ret != 0) ? ret : err;
	}

unlock:
	k_mutex_unlock(&data->lock);

out:
	if (own_bufs) {
		/* Not queued: both buffers are still owned here */
		k_free((void *)(uintptr_t)request->a2);
		k_free((void *)(uintptr_t)request->resp_data_addr);
	}

	return ret;
}

int altera_sdm_mailbox_send(const struct device *dev, uint16_t cmd, const uint32_t *args,
			    size_t nargs, uint32_t *resp, size_t *resp_len)
{
	struct altera_sdm_xfer xfer = {
		.resp = resp,
		.resp_cap = (resp_len != NULL) ? *resp_len : 0U,
	};
	struct sip_svc_request request = {0};
	size_t cmd_size = (SDM_MB_HEADER_WORDS + nargs) * sizeof(uint32_t);
	size_t resp_size = (SDM_MB_HEADER_WORDS + xfer.resp_cap) * sizeof(uint32_t);
	uint32_t *cmd_buf = NULL;
	uint32_t *resp_buf = NULL;
	uint32_t sdm_err;
	size_t len;
	int ret;

	if ((dev == NULL) || ((cmd & ~(SDM_MB_CMD_ID_MASK | ALTERA_SDM_MAILBOX_INDIRECT)) != 0U) ||
	    (nargs > ALTERA_SDM_MAILBOX_MAX_ARGS) || ((nargs > 0U) && (args == NULL)) ||
	    ((xfer.resp_cap > 0U) && (resp == NULL)) ||
	    (xfer.resp_cap > (SIP_SVP_MB_MAX_WORD_SIZE - SDM_MB_HEADER_WORDS))) {
		ret = -EINVAL;
		goto out;
	}

	cmd_buf = k_malloc(cmd_size);
	resp_buf = k_malloc(resp_size);
	if ((cmd_buf == NULL) || (resp_buf == NULL)) {
		k_free(cmd_buf);
		k_free(resp_buf);
		ret = -ENOMEM;
		goto out;
	}

	cmd_buf[0] = FIELD_PREP(SDM_MB_LENGTH_MASK, nargs) | (cmd & ALTERA_SDM_MAILBOX_INDIRECT) |
		     FIELD_PREP(SDM_MB_CMD_ID_MASK, cmd);
	if (nargs > 0U) {
		memcpy(&cmd_buf[SDM_MB_HEADER_WORDS], args, nargs * sizeof(uint32_t));
	}

	request.header = SIP_SVC_PROTO_HEADER(SIP_SVC_PROTO_CMD_ASYNC, 0);
	request.a0 = SMC_FUNC_ID_MAILBOX_SEND_COMMAND;
	request.a2 = (unsigned long)(uintptr_t)cmd_buf;
	request.a3 = cmd_size;
	request.resp_data_addr = (uint64_t)(uintptr_t)resp_buf;
	request.resp_data_size = resp_size;

	/* altera_sdm_submit() takes ownership of the buffers */
	ret = altera_sdm_submit(dev, &request, &xfer);
	if (ret != 0) {
		goto out;
	}

	if (xfer.smc_status == SMC_STATUS_OKAY) {
		sdm_err = (uint32_t)FIELD_GET(SDM_MB_RESP_ERROR_MASK, xfer.header);
	} else if ((int16_t)xfer.smc_status < 0) {
		/* EL3 returns the SDM error code negated */
		sdm_err = (uint32_t)(-(int16_t)xfer.smc_status);
	} else {
		LOG_ERR("Mailbox command 0x%x: SMC call failed (0x%x)", cmd, xfer.smc_status);
		ret = -EIO;
		goto out;
	}

	ret = altera_sdm_err_to_errno(sdm_err);
	if (ret != 0) {
		LOG_ERR("Mailbox command 0x%x: SDM error 0x%x", cmd, sdm_err);
		goto out;
	}

	len = FIELD_GET(SDM_MB_LENGTH_MASK, xfer.header);
	if (len > xfer.resp_len) {
		LOG_ERR("Mailbox command 0x%x: response of %zu words exceeds %zu", cmd, len,
			xfer.resp_cap);
		ret = -EMSGSIZE;
		goto out;
	}

	if (resp_len != NULL) {
		*resp_len = len;
	}

out:
	return ret;
}

int altera_sdm_smc_call(const struct device *dev, uint32_t func_id, const uint64_t *args,
			size_t nargs, struct altera_sdm_smc_res *res)
{
	struct altera_sdm_xfer xfer = {0};
	struct sip_svc_request request = {0};
	uint64_t a[ALTERA_SDM_SMC_MAX_ARGS] = {0};
	int ret;

	if ((dev == NULL) || (nargs > ALTERA_SDM_SMC_MAX_ARGS) ||
	    ((nargs > 0U) && (args == NULL))) {
		ret = -EINVAL;
		goto out;
	}

	if (nargs > 0U) {
		memcpy(a, args, nargs * sizeof(uint64_t));
	}

	request.header = SIP_SVC_PROTO_HEADER(SIP_SVC_PROTO_CMD_SYNC, 0);
	request.a0 = func_id;
	request.a1 = a[0];
	request.a2 = a[1];
	request.a3 = a[2];
	request.a4 = a[3];
	request.a5 = a[4];
	request.a6 = a[5];
	request.a7 = a[6];

	ret = altera_sdm_submit(dev, &request, &xfer);
	if (ret != 0) {
		goto out;
	}

	if (res != NULL) {
		*res = xfer.regs;
	}

	/* The response header keeps only the low 16 bits of a0 */
	if (xfer.regs.a0 != SMC_STATUS_OKAY) {
		LOG_ERR("SMC call 0x%x failed (0x%" PRIx64 ")", func_id, (uint64_t)xfer.regs.a0);
		ret = -EIO;
	}

out:
	return ret;
}

static int altera_sdm_init(const struct device *dev)
{
	const struct altera_sdm_config *config = dev->config;
	struct altera_sdm_data *data = dev->data;
	int ret = 0;

	k_mutex_init(&data->lock);

	data->ctrl = sip_svc_get_controller((char *)config->method);
	if (data->ctrl == NULL) {
		LOG_ERR("SiP service controller \"%s\" not found", config->method);
		ret = -ENODEV;
		goto out;
	}

	data->token = sip_svc_register(data->ctrl, NULL);
	if (data->token == SIP_SVC_ID_INVALID) {
		LOG_ERR("Failed to register SiP service client");
		ret = -EBUSY;
	}

out:
	return ret;
}

#define ALTERA_SDM_DEFINE(inst)                                                                    \
	BUILD_ASSERT(DT_NODE_HAS_COMPAT(DT_INST_PARENT(inst), intel_socfpga_agilex_sip_smc),       \
		     "SDM node must be a child of the SiP service controller node");               \
                                                                                                   \
	static const struct altera_sdm_config altera_sdm_config_##inst = {                         \
		.method = DT_PROP(DT_INST_PARENT(inst), method),                                   \
	};                                                                                         \
                                                                                                   \
	static struct altera_sdm_data altera_sdm_data_##inst;                                      \
                                                                                                   \
	DEVICE_DT_INST_DEFINE(inst, altera_sdm_init, NULL, &altera_sdm_data_##inst,                \
			      &altera_sdm_config_##inst, POST_KERNEL,                              \
			      CONFIG_ALTERA_SDM_INIT_PRIORITY, NULL);

DT_INST_FOREACH_STATUS_OKAY(ALTERA_SDM_DEFINE)
