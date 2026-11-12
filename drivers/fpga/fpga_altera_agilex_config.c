/*
 * Copyright (c) 2026, Altera Corporation.
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Altera SoC FPGA platform specific functions.
 */

#include <errno.h>
#include <stdio.h>
#include <string.h>

#include <zephyr/device.h>
#include <zephyr/drivers/fpga.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/cache.h>
#include <zephyr/sip_svc/sip_svc.h>
#include <zephyr/drivers/sip_svc/sip_svc_agilex_smc.h>
#include <zephyr/drivers/sip_svc/sip_svc_agilex_mailbox.h>
#include <zephyr/drivers/sip_svc/sip_svc_proto.h>
#include "fpga_altera_agilex_config.h"

#if !DT_NODE_HAS_STATUS(DT_NODELABEL(fpga0), okay)
#error "Altera FPGA config path requires DT nodelabel fpga0 (altr,socfpga-agilex-fpga)"
#endif

#if defined(CONFIG_SOC_AGILEX5)
#include <zephyr/drivers/sip_svc/sip_svc_agilex_mbox_ddr.h>
#endif

LOG_MODULE_REGISTER(fpga_altera_config, CONFIG_FPGA_LOG_LEVEL);

/* Set in STATUS callback on successful full reconfig; bridges re-enabled at EXIT. */
static bool fm_reconfig_succeeded;

#define FPGA_MB_CMD_ADDR_MEM_SIZE 100
#define FPGA_MB_RESPONSE_MEM_SIZE 100
#define FPGA_MB_CMD_WORDS	  DIV_ROUND_UP(FPGA_MB_CMD_ADDR_MEM_SIZE, sizeof(uint32_t))
#define FPGA_MB_RESP_WORDS	  DIV_ROUND_UP(FPGA_MB_RESPONSE_MEM_SIZE, sizeof(uint32_t))
#define FPGA_CONFIG_THREAD_PRIORITY                                                                \
	((CONFIG_ARM_SIP_SVC_SUBSYS_THREAD_PRIORITY + 1) < (CONFIG_NUM_PREEMPT_PRIORITIES - 1))    \
		? (CONFIG_ARM_SIP_SVC_SUBSYS_THREAD_PRIORITY + 1)                                  \
		: CONFIG_ARM_SIP_SVC_SUBSYS_THREAD_PRIORITY

K_KERNEL_STACK_DEFINE(reconfig_thread_stack_area,
		      CONFIG_FPGA_ALTERA_PROGRAM_THREAD_STACK_SIZE);

static uint32_t mb_client_token;
static uint32_t curr_stage;
static uint32_t fpga_reconfig_dma_count;
static uint32_t fpga_reconfig_dma_block_size;
static struct sip_svc_controller *mb_smc_dev;
static struct k_mutex config_state;
static struct k_thread config_thread_data;

#define MGR_SYNC_SMC_TIMEOUT_MS 5000

struct mgr_sync_smc_result {
	struct k_sem done;
	uint64_t a0;
	uint64_t a1;
	uint64_t a2;
	uint64_t a3;
};

static void mgr_sync_smc_cb(uint32_t c_token, struct sip_svc_response *response)
{
	struct mgr_sync_smc_result *r;

	ARG_UNUSED(c_token);
	if (!response || !response->priv_data) {
		return;
	}
	r = response->priv_data;
	r->a0 = response->a0;
	r->a1 = response->a1;
	r->a2 = response->a2;
	r->a3 = response->a3;
	k_sem_give(&r->done);
}

#if defined(CONFIG_SOC_AGILEX5)
/* Match ATF: SDM RECONFIG_DATA addresses are phys - DRAM_BASE. */
#define AGX5_DRAM_BASE 0x80000000ULL
/*
 * TF-A is_address_in_ddr_range() requires mailbox buffers >= BL31_LIMIT
 * (0x82000000). k_malloc lands in Zephyr SRAM (~0x801xxxxx) and ATF rejects
 * with "data buffer not in the DDR range" — RECONFIG then returns empty/zero
 * DMA count/size.
 */
static SOCFPGA_MBOX_DDR uint32_t mbox_cmd_buf[FPGA_MB_CMD_WORDS] __aligned(64);
static SOCFPGA_MBOX_DDR uint32_t mbox_resp_buf[FPGA_MB_RESP_WORDS] __aligned(64);
/* Match ATF WRITE window (FPGA_CONFIG_BUFFER_SIZE = 4). */
#define AGX5_DATA_SLOTS 4
static SOCFPGA_MBOX_DDR uint32_t mbox_data_cmd[AGX5_DATA_SLOTS][FPGA_MB_CMD_WORDS] __aligned(64);
static SOCFPGA_MBOX_DDR uint32_t mbox_data_resp[AGX5_DATA_SLOTS][FPGA_MB_RESP_WORDS] __aligned(64);
static struct fm_private_data agx5_data_priv[AGX5_DATA_SLOTS];
static uint8_t agx5_data_slot_used[AGX5_DATA_SLOTS];
/* Must outlive sip_svc callbacks for inflight DATA chunks. */
static struct fm_lock_data agx5_data_sync_lock;

/*
 * Program SDM BE remapper via INTEL_SIP_SMC_FPGA_CONFIG_START (same as U-Boot).
 * Stock BL31 remapper_init() writes AR/AW=DRAM_BASE here; SDM_REMAPPER_CONFIG
 * enable is a no-op on stock ATF. Zephyr still owns V2 CANCEL/RECONFIG/DATA
 * after this (START's RECONFIG session is cancelled below).
 * a1=1 matches U-Boot intel_sdm_mb_load().
 */
static int agx5_fpga_config_start(void)
{
	struct sip_svc_request request = {0};
	struct mgr_sync_smc_result res;
	int32_t tid;

	if (!mb_smc_dev) {
		return -ENODEV;
	}

	k_sem_init(&res.done, 0, 1);
	res.a0 = SMC_STATUS_ERROR;
	res.a1 = 0;
	res.a2 = 0;
	res.a3 = 0;

	request.header = SIP_SVC_PROTO_HEADER(SIP_SVC_PROTO_CMD_SYNC, 0);
	request.a0 = SMC_FUNC_ID_FPGA_CONFIG_START;
	request.a1 = 1U;
	request.priv_data = &res;

	tid = sip_svc_send(mb_smc_dev, mb_client_token, &request, mgr_sync_smc_cb);
	if (tid == SIP_SVC_ID_INVALID) {
		return -EBUSY;
	}
	if (k_sem_take(&res.done, K_MSEC(MGR_SYNC_SMC_TIMEOUT_MS)) != 0) {
		return -ETIMEDOUT;
	}
	if (res.a0 != SMC_STATUS_OKAY) {
		LOG_ERR("FPGA_CONFIG_START failed a0=0x%llx", res.a0);
		return -EIO;
	}
	LOG_DBG("FPGA_CONFIG_START ok a0=0x%llx", res.a0);
	return 0;
}
static SOCFPGA_MBOX_DDR uint32_t mbox_cancel_cmd __aligned(4);
#endif /* CONFIG_SOC_AGILEX5 */

/**
 * @brief Open session the SiP svc client
 *
 * @return 0 on success or negative value on fail
 */
static int32_t svc_client_open(void)
{
	/* Do not hang forever if mailbox shell still holds SINGLY_OPEN. */
	k_timeout_t timeout = K_MSEC(5000);

	if (!mb_smc_dev) {
		LOG_ERR("Mailbox client is not registered");
		return -ENODEV;
	}

	if (sip_svc_open(mb_smc_dev, mb_client_token, timeout)) {
		LOG_ERR("Mailbox client open fail (SiP held by another client? "
			"try 'mailbox close')");
		return -ENODEV;
	}

	return 0;
}

/**
 * @brief Init the sip_svc client
 *	first it will register the client then it will open the client
 *
 * @return 0 on success or negative value on fail
 */
static int32_t altera_fpga_config_init(void)
{
	if (mb_smc_dev) {
		LOG_DBG("Mailbox client already registered");
		return 0;
	}

	mb_smc_dev = sip_svc_get_controller("smc");
	if (!mb_smc_dev) {
		LOG_ERR("Arm SiP service not found");
		return -ENODEV;
	}

	mb_client_token = sip_svc_register(mb_smc_dev, NULL);
	if (mb_client_token == SIP_SVC_ID_INVALID) {
		mb_smc_dev = NULL;
		LOG_ERR("Mailbox client register fail");
		return -EINVAL;
	}

	curr_stage = FPGA_CONFIG_INIT;
	k_mutex_init(&config_state);

	return 0;
}

/**
 * @brief Close the svc client
 *
 * @return 0 on success or negative value on fail
 */
static int32_t svc_client_close(void)
{
	int32_t err = 0;
	uint32_t cmd_size = sizeof(uint32_t);
	struct sip_svc_request request;
	uint32_t *cmd_addr;

	if (!mb_smc_dev) {
		LOG_ERR("Mailbox client is not registered");
		return 0;
	}

#if defined(CONFIG_SOC_AGILEX5)
	mbox_cancel_cmd = MAILBOX_CANCEL_COMMAND;
	cmd_addr = &mbox_cancel_cmd;
#else
	cmd_addr = k_malloc(cmd_size);

	if (!cmd_addr) {
		k_mutex_unlock(&config_state);
		return -ENOMEM;
	}

	*cmd_addr = MAILBOX_CANCEL_COMMAND;
#endif

	request.header = SIP_SVC_PROTO_HEADER(SIP_SVC_PROTO_CMD_ASYNC, 0);
	request.a0 = SMC_FUNC_ID_MAILBOX_SEND_COMMAND;
	request.a1 = 0;
	request.a2 = (uint64_t)cmd_addr;
	request.a3 = (uint64_t)cmd_size;
	request.a4 = 0;
	request.a5 = 0;
	request.a6 = 0;
	request.a7 = 0;
	request.resp_data_addr = (uint64_t)NULL;
	request.resp_data_size = 0;
	request.priv_data = NULL;

	err = sip_svc_close(mb_smc_dev, mb_client_token, &request);
	if (err) {
#if !defined(CONFIG_SOC_AGILEX5)
		k_free(cmd_addr);
#endif
		LOG_ERR("Mailbox client close fail (%d)", err);
	}

	curr_stage = FPGA_CONFIG_INIT;
	k_mutex_unlock(&config_state);
	return err;
}

/* SDM mailbox response codes (see U-Boot mailbox_s10.h ALT_SDM_MBOX_RESP_CODE) */
static const char *fpga_mbox_err_str(uint32_t mbox_err)
{
	switch (mbox_err) {
	case 0x0U:
		return " (OK)";
	case 0x8U:
		return " (CLIENT_ID_NO_MATCH — no open RECONFIG for this client)";
	case 0x9U:
		return " (INVALID_ADDR)";
	case 0xBU:
		return " (TIMEOUT)";
	case 0xCU:
		return " (HW_NOT_RDY)";
	case 0x3FFU:
		return " (GENERIC_RESPONSE_ERROR)";
	default:
		return "";
	}
}

/**
 * @brief Call back function which we receive when we send the data
 * based on the current stage it will collect the data
 *
 * @param[in] c_token Token id for our svc services
 * @param[in] res Buffer will contain the response
 * @param[in] size Size of the response
 *
 * @return void
 */
static void cmd_send_callback(uint32_t c_token, struct sip_svc_response *response)
{
	if (response == NULL) {
		return;
	}

	uint32_t *resp_data = NULL;
	uint32_t resp_len = 0;
	uint32_t i = 0;
	struct fm_private_data *private_data = (struct fm_private_data *)response->priv_data;
	union mailbox_response_header response_header;

	response_header.header = 0;

	LOG_DBG("sip_svc send command callback\n");
	LOG_DBG("\tresponse data=\n");
	LOG_DBG("\theader=%08x\n", response->header);
	LOG_DBG("\ta0=%016lx\n", response->a0);
	LOG_DBG("\ta1=%016lx\n", response->a1);
	LOG_DBG("\ta2=%016lx\n", response->a2);
	LOG_DBG("\ta3=%016lx\n", response->a3);

	/*
	 * ATF V2 mailbox poll returns:
	 *   a0 == 0            → SMC OK (parse mbox header for SDM err)
	 *   a0 > 0 small       → SMC protocol (REJECT/BUSY/…)
	 *   a0 < 0             → SDM mbox err as -code (e.g. -1023 = 0x3FF)
	 * Do not call negative codes "SMC rejected / MBOX_DDR".
	 */
	if (response->a0 != 0 && response->a0 != SMC_STATUS_OKAY) {
		uint32_t hdr = 0;

		if (response->resp_data_addr && response->resp_data_size >= sizeof(uint32_t)) {
			hdr = *(uint32_t *)response->resp_data_addr;
		}
		if ((long)response->a0 < 0) {
			uint32_t mbox_err = (uint32_t)(-(long)response->a0);

			LOG_ERR("SDM mailbox err=0x%x a0=%ld stage=%u hdr=0x%x "
				"client=%u job=%u len=%u%s",
				mbox_err, (long)response->a0, curr_stage, hdr,
				SIP_SVC_MB_HEADER_GET_CLIENT_ID(hdr),
				(hdr >> SIP_SVP_MB_HEADER_JOB_ID_OFFSET) &
					SIP_SVP_MB_HEADER_JOB_ID_MASK,
				SIP_SVC_MB_HEADER_GET_LENGTH(hdr),
				fpga_mbox_err_str(mbox_err));
		} else {
			LOG_ERR("SMC status a0=%ld stage=%u hdr=0x%x",
				(long)response->a0, curr_stage, hdr);
		}
		if (curr_stage == FPGA_RECONFIG_SEND || curr_stage == FPGA_RECONFIG_DATA_SEND ||
		    curr_stage == FPGA_CANCEL_STAGE) {
			curr_stage = FPGA_RECONFIG_EXIT;
		}
		if (private_data && private_data->private_data_lock) {
			private_data->private_data_lock->response_status = true;
			k_sem_give(&private_data->private_data_lock->reconfig_data_sem);
#if defined(CONFIG_SOC_AGILEX5)
			if (private_data->reconfig_data_send_done) {
				k_sem_give(&private_data->private_data_lock
						    ->reconfig_data_send_done_sem);
			}
			for (i = 0; i < AGX5_DATA_SLOTS; i++) {
				if (&agx5_data_priv[i] == private_data) {
					agx5_data_slot_used[i] = 0;
					break;
				}
			}
#endif
		}
		return;
	}

	/* Condition to check only for the mailbox command not for the non-mailbox command */
	if (response->resp_data_size) {
		resp_data = (uint32_t *)response->resp_data_addr;
		resp_len = response->resp_data_size / 4;
		if (resp_data && resp_len) {
			response_header = (union mailbox_response_header)resp_data[0];
			for (i = 0; i < resp_len; i++) {
				LOG_DBG("\t\t[%4d] %08x\n", i, resp_data[i]);
			}
		} else {
			LOG_ERR("\t\tInvalid addr (%p) or len (%d)", resp_data, resp_len);
		}
	}
	/* Condition for non-mailbox command*/
	else {
		LOG_DBG("Response data size is Zero !!");
	}

	switch (curr_stage) {
	case FPGA_CANCEL_STAGE:
		if (response_header.header_t.error_code == 0x00 ||
		    response_header.header_t.error_code == 0x3FF ||
		    response_header.header_t.error_code == 0x2FF) {
			LOG_DBG("CANCEL ok (mbox err=0x%x) -> RECONFIG",
				response_header.header_t.error_code);
			curr_stage = FPGA_RECONFIG_SEND;
		} else {
			LOG_ERR("CANCEL failed mbox err=0x%x",
				response_header.header_t.error_code);
			curr_stage = FPGA_RECONFIG_EXIT;
		}
		break;

	case FPGA_RECONFIG_SEND:
		if (resp_data && response_header.header_t.error_code == 0x00) {
			LOG_DBG("RECONFIG resp hdr=0x%x data_len=%u resp_words=%u "
				"[%08x %08x %08x]",
				response_header.header,
				response_header.header_t.data_length, resp_len,
				resp_data[0],
				resp_len > 1 ? resp_data[1] : 0U,
				resp_len > 2 ? resp_data[2] : 0U);
			/*
			 * ATF uses response[0]/[1] after stripping the header.
			 * Zephyr keeps the header at [0]; DMA params are [1]/[2].
			 */
			if (response_header.header_t.data_length < 2 || resp_len < 3) {
				LOG_ERR("RECONFIG returned no DMA params "
					"(data_len=%u) — SDM not ready for reconfig",
					response_header.header_t.data_length);
				curr_stage = FPGA_RECONFIG_EXIT;
				break;
			}
			fpga_reconfig_dma_count = resp_data[MBOX_RECONFIG_DMA_COUNT];
			fpga_reconfig_dma_block_size = resp_data[MBOX_RECONFIG_DMA_SIZE];
			LOG_DBG("RECONFIG ok: DMA count=%u block=%u (%u bytes / ~%u KiB)",
				fpga_reconfig_dma_count, fpga_reconfig_dma_block_size,
				fpga_reconfig_dma_block_size,
				fpga_reconfig_dma_block_size / 1024U);
			if (fpga_reconfig_dma_count == 0 || fpga_reconfig_dma_block_size == 0) {
				LOG_ERR("RECONFIG returned invalid DMA params (count=%u size=%u)",
					fpga_reconfig_dma_count, fpga_reconfig_dma_block_size);
				curr_stage = FPGA_RECONFIG_EXIT;
			} else {
				curr_stage = FPGA_RECONFIG_DATA_SEND;
			}
		} else {
			LOG_ERR("RECONFIG failed mbox err=0x%x",
				response_header.header_t.error_code);
			curr_stage = FPGA_RECONFIG_EXIT;
		}
		break;

	case FPGA_RECONFIG_DATA_SEND:
		if (!private_data || !private_data->private_data_lock) {
			LOG_ERR("RECONFIG_DATA callback: null priv_data=%p lock — skip",
				private_data);
			break;
		}
		if (response_header.header_t.error_code == 0x00) {
			LOG_DBG("RECONFIG_DATA chunk reply ok (mbox_err=0 done=%d)",
				private_data->reconfig_data_send_done ? 1 : 0);
			if (private_data->reconfig_data_send_done == true) {
				LOG_DBG("RECONFIG_DATA done");
				curr_stage = FPGA_RECONFIG_CHECK_STATUS;
			}
		} else {
			LOG_ERR("RECONFIG_DATA failed mbox err=0x%x hdr=0x%x%s",
				response_header.header_t.error_code, response_header.header,
				fpga_mbox_err_str(response_header.header_t.error_code));
			private_data->private_data_lock->response_status = true;
			curr_stage = FPGA_RECONFIG_EXIT;
		}

		if (private_data->reconfig_data_send_done == true) {
			k_sem_give(&(private_data->private_data_lock->reconfig_data_send_done_sem));
		}
#if defined(CONFIG_SOC_AGILEX5)
		for (i = 0; i < AGX5_DATA_SLOTS; i++) {
			if (&agx5_data_priv[i] == private_data) {
				agx5_data_slot_used[i] = 0;
				break;
			}
		}
#endif
		break;

	case FPGA_RECONFIG_CHECK_STATUS:
		if (resp_data)
			private_data->config_status.header = (union mailbox_response_header)
				resp_data[MBOX_RECONFIG_STATUS_HEADER];
		if (resp_data && response_header.header_t.error_code == 0x00) {
			if (private_data) {
				private_data->config_status.state =
					resp_data[MBOX_RECONFIG_STATUS_STATE];
				private_data->config_status.version = (union config_status_version)
					resp_data[MBOX_RECONFIG_STATUS_VERSION];
				private_data->config_status.pin_status =
					(union config_status_pin_status)
						resp_data[MBOX_RECONFIG_STATUS_PIN_STATUS];
				private_data->config_status.soft_function_status =
					resp_data[MBOX_RECONFIG_STATUS_SOFT_FUNCTION];
				private_data->config_status.error_location =
					resp_data[MBOX_RECONFIG_STATUS_ERROR_LOCATION];
				private_data->config_status.error_details =
					resp_data[MBOX_RECONFIG_STATUS_ERROR_DETAILS];

				if (!private_data->config_status.state) {
					LOG_DBG("RECONFIG_STATUS done: soft=0x%x pin=0x%x "
						"(need soft CONF_DONE|INIT_DONE=0x3)",
						private_data->config_status.soft_function_status,
						private_data->config_status.pin_status.pin_status);
					/* Full path only; bridges re-enabled at EXIT. */
					fm_reconfig_succeeded = true;
					curr_stage = FPGA_RECONFIG_EXIT;
				} else if (private_data->config_status.state !=
						   MBOX_CONFIG_STATUS_STATE_CONFIG &&
					   private_data->config_status.state !=
						   MBOX_CFGSTAT_VAB_BS_PREAUTH) {
					LOG_ERR("RECONFIG_STATUS bad state=0x%x soft=0x%x "
						"pin=0x%x err_loc=0x%x err_det=0x%x",
						private_data->config_status.state,
						private_data->config_status.soft_function_status,
						private_data->config_status.pin_status.pin_status,
						private_data->config_status.error_location,
						private_data->config_status.error_details);
					curr_stage = FPGA_RECONFIG_EXIT;
				} else {
					LOG_DBG("RECONFIG_STATUS busy state=0x%x soft=0x%x",
						private_data->config_status.state,
						private_data->config_status.soft_function_status);
				}
			}
		} else {
			LOG_ERR("RECONFIG_STATUS mbox failed err=0x%x",
				response_header.header_t.error_code);
		}

		break;

	case FPGA_RECONFIG_EXIT:
		LOG_DBG("Callback at EXIT stage");
		if (private_data && private_data->reconfig_data_send_done == true &&
		    private_data->private_data_lock) {
			k_sem_give(&(private_data->private_data_lock->reconfig_data_send_done_sem));
		}
		break;
	}

	/* Client only responsible to free the response data memory space,
	 * the command data memory space had been freed by sip_svc service.
	 * Agilex5 MBOX_DDR buffers are static and must not be freed.
	 */
	if (response->resp_data_addr) {
#if !defined(CONFIG_SOC_AGILEX5)
		LOG_DBG("\tFree response memory %p\n", (char *)response->resp_data_addr);
		k_free((char *)response->resp_data_addr);
#endif
	}

	if (private_data && private_data->private_data_lock) {
		k_sem_give(&(private_data->private_data_lock->reconfig_data_sem));
	}
	/* Heap priv_data (DATA chunks) is freed by send_reconfig_data after wait */
}

/**
 * @brief Send the reconfig data to SDM
 *
 * @param[in] buf Bitstream data chunk
 * @param[in] size Size of the buf data
 *
 * @return 0 on success or negative value on fail
 */
#if defined(CONFIG_SOC_AGILEX5)
/*
 * Credit-pipelined RECONFIG_DATA: fill the SDM credit window (dma_count,
 * capped at AGX5_DATA_SLOTS), wait only when inflight == credits.
 * Wait-sequence (credits=1) A/B harness archived under
 * patches/snapshots/serialize-diag-wait-sequence.c.snippet.
 */
static int32_t send_reconfig_data(char *buf, size_t size)
{
	struct fm_lock_data *sync_lock = &agx5_data_sync_lock;
	struct sip_svc_request request;
	uintptr_t phys_base = POINTER_TO_UINT(buf);
	uintptr_t sdm_base;
	uint32_t rbf_total = size;
	uint32_t rbf_rem = rbf_total;
	uint32_t rbf_sent = 0;
	uint32_t inflight = 0;
	uint32_t credits;
	uint32_t slot = 0;
	uint32_t chunk_n = 0;
	bool last_submitted = false;
	int32_t tid;
	int32_t err = 0;

	if (fpga_reconfig_dma_count == 0 || fpga_reconfig_dma_block_size == 0) {
		LOG_ERR("Cannot send RECONFIG_DATA: DMA count=%u block=%u",
			fpga_reconfig_dma_count, fpga_reconfig_dma_block_size);
		return -EINVAL;
	}

	if (phys_base < AGX5_DRAM_BASE) {
		LOG_ERR("Bitstream addr 0x%lx below DRAM_BASE", (unsigned long)phys_base);
		return -EINVAL;
	}
	sdm_base = phys_base - (uintptr_t)AGX5_DRAM_BASE;

	credits = fpga_reconfig_dma_count;
	if (credits > AGX5_DATA_SLOTS) {
		credits = AGX5_DATA_SLOTS;
	}

	memset(sync_lock, 0, sizeof(*sync_lock));
	memset(agx5_data_slot_used, 0, sizeof(agx5_data_slot_used));
	k_sem_init(&sync_lock->reconfig_data_sem, 0, credits);
	k_sem_init(&sync_lock->reconfig_data_send_done_sem, 0, 1);

	sys_cache_data_flush_range(buf, size);

	LOG_DBG("RECONFIG_DATA: phys 0x%lx sdm 0x%lx (%u bytes) credits=%u block=%u",
		(unsigned long)phys_base, (unsigned long)sdm_base, rbf_total, credits,
		fpga_reconfig_dma_block_size);

	while ((rbf_rem > 0 || inflight > 0) && !sync_lock->response_status) {
		if (rbf_rem > 0 && inflight < credits) {
			uint32_t chunk = (rbf_rem < fpga_reconfig_dma_block_size)
						 ? rbf_rem
						 : fpga_reconfig_dma_block_size;
			uint32_t *cmd;
			uint32_t *resp;
			struct fm_private_data *priv;
			uint32_t s;
			uint32_t sdm_addr;
			bool found = false;

			for (s = 0; s < credits; s++) {
				slot = (slot + 1) % credits;
				if (!agx5_data_slot_used[slot]) {
					found = true;
					break;
				}
			}
			if (!found) {
				goto wait_one;
			}

			agx5_data_slot_used[slot] = 1;
			cmd = mbox_data_cmd[slot];
			resp = mbox_data_resp[slot];
			priv = &agx5_data_priv[slot];
			memset(cmd, 0, FPGA_MB_CMD_WORDS * sizeof(uint32_t));
			memset(resp, 0, FPGA_MB_RESP_WORDS * sizeof(uint32_t));
			memset(priv, 0, sizeof(*priv));
			priv->private_data_lock = sync_lock;
			priv->heap_allocated = 0;

			sdm_addr = (uint32_t)(sdm_base + rbf_sent);
			cmd[0] = MBOX_REQUEST_HEADER(FPGA_RECONFIG_DATA,
						     RECONFIG_DATA_MB_CMD_INDIRECT_MODE,
						     RECONFIG_DATA_MB_CMD_LENGTH);
			cmd[1] = MBOX_RECONFIG_REQUEST_DATA_FORMAT(
				RECONFIG_DATA_MB_CMD_DIRECT_COUNT,
				RECONFIG_DATA_MB_CMD_INDIRECT_ARG,
				RECONFIG_DATA_MB_CMD_INDIRECT_RESPONSE);
			cmd[2] = sdm_addr;
			cmd[3] = chunk;
			if (chunk >= rbf_rem) {
				priv->reconfig_data_send_done = 1;
				last_submitted = true;
			}

			sys_cache_data_flush_range(cmd, RECONFIG_DATA_MB_CMD_SIZE);

			memset(&request, 0, sizeof(request));
			request.header = SIP_SVC_PROTO_HEADER(SIP_SVC_PROTO_CMD_ASYNC, 0);
			request.a0 = SMC_FUNC_ID_MAILBOX_SEND_COMMAND;
			request.a2 = (uint64_t)cmd;
			request.a3 = (uint64_t)RECONFIG_DATA_MB_CMD_SIZE;
			request.resp_data_addr = (uint64_t)resp;
			request.resp_data_size = FPGA_MB_RESPONSE_MEM_SIZE;
			request.priv_data = priv;

			LOG_DBG("RECONFIG_DATA submit #%u slot=%u sdm=0x%x len=%u "
				"inflight=%u rem=%u",
				chunk_n, slot, sdm_addr, chunk, inflight + 1, rbf_rem);

			tid = sip_svc_send(mb_smc_dev, mb_client_token, &request,
					   cmd_send_callback);
			if (tid == SIP_SVC_ID_INVALID) {
				agx5_data_slot_used[slot] = 0;
				LOG_ERR("RECONFIG_DATA submit failed");
				err = -EBUSY;
				break;
			}
			ARG_UNUSED(tid);

			rbf_sent += chunk;
			rbf_rem = rbf_total - rbf_sent;
			inflight++;
			chunk_n++;
			continue;
		}

wait_one:
		if (k_sem_take(&sync_lock->reconfig_data_sem,
			       K_MSEC(RECONFIG_DATA_CHUNK_TIMEOUT_MS)) != 0) {
			LOG_ERR("RECONFIG_DATA wait timeout (inflight=%u rem=%u)",
				inflight, rbf_rem);
			err = -ETIMEDOUT;
			break;
		}
		if (inflight) {
			inflight--;
		}
	}

	while (inflight > 0) {
		if (k_sem_take(&sync_lock->reconfig_data_sem,
			       K_MSEC(RECONFIG_DATA_CHUNK_TIMEOUT_MS)) != 0) {
			LOG_ERR("RECONFIG_DATA drain timeout (inflight=%u)", inflight);
			err = err ? err : -ETIMEDOUT;
			break;
		}
		inflight--;
	}

	if (!err && last_submitted && !sync_lock->response_status) {
		if (k_sem_take(&sync_lock->reconfig_data_send_done_sem,
			       K_MSEC(RECONFIG_DATA_CHUNK_TIMEOUT_MS)) != 0) {
			LOG_ERR("RECONFIG_DATA last-chunk done timeout");
			return -ETIMEDOUT;
		}
	}
	if (err) {
		return err;
	}
	if (sync_lock->response_status) {
		return -EINVAL;
	}
	LOG_DBG("RECONFIG_DATA done (%u submits, %u bytes)", chunk_n, rbf_sent);
	return 0;
}
#else /* !CONFIG_SOC_AGILEX5 */
static int32_t send_reconfig_data(char *buf, size_t size)
{
	uint32_t *cmd_addr_buffer;
	uint32_t *resp_addr;
	uint32_t svc_buf_idx = 0;
	uint32_t buf_idx = 0;
	int32_t trans_id = 0;
	int32_t err = 0;
	uint32_t rbf_data_total = size;
	uint32_t rbf_data_rem = rbf_data_total;
	uint32_t rbf_data_sent = 0;
	struct fm_private_data *private_data = NULL;
	struct fm_lock_data sync_lock;
	struct sip_svc_request *request_buffer;
	bool last_chunk = false;
	uintptr_t sdm_base = POINTER_TO_UINT(buf);

	if (fpga_reconfig_dma_count == 0 || fpga_reconfig_dma_block_size == 0) {
		LOG_ERR("Cannot send RECONFIG_DATA: DMA count=%u block=%u",
			fpga_reconfig_dma_count, fpga_reconfig_dma_block_size);
		return -EINVAL;
	}

	memset(&sync_lock, 0, sizeof(sync_lock));
	k_sem_init(&sync_lock.reconfig_data_sem, 3, 3);
	k_sem_init(&sync_lock.reconfig_data_send_done_sem, 0, 1);

	LOG_DBG("RECONFIG_DATA async start: %u bytes, DMA count %u, block %u",
		(uint32_t)size, fpga_reconfig_dma_count, fpga_reconfig_dma_block_size);

	request_buffer = (struct sip_svc_request *)k_malloc(fpga_reconfig_dma_count *
							    sizeof(struct sip_svc_request));
	if (!request_buffer) {
		return -ENOMEM;
	}

	while (rbf_data_rem > 0 && (!sync_lock.response_status)) {
		private_data = (struct fm_private_data *)k_malloc(sizeof(struct fm_private_data));
		if (!private_data) {
			k_free(request_buffer);
			return -ENOMEM;
		}
		memset(private_data, 0, sizeof(*private_data));
		private_data->private_data_lock = &sync_lock;
		private_data->heap_allocated = 1;

		cmd_addr_buffer = (uint32_t *)k_malloc(FPGA_MB_CMD_ADDR_MEM_SIZE);
		if (!cmd_addr_buffer) {
			k_free(request_buffer);
			k_free(private_data);
			return -ENOMEM;
		}
		resp_addr = (uint32_t *)k_malloc(FPGA_MB_RESPONSE_MEM_SIZE);
		if (!resp_addr) {
			k_free(request_buffer);
			k_free(private_data);
			k_free(cmd_addr_buffer);
			return -ENOMEM;
		}

		request_buffer[svc_buf_idx].header =
			SIP_SVC_PROTO_HEADER(SIP_SVC_PROTO_CMD_ASYNC, 0);
		request_buffer[svc_buf_idx].a0 = SMC_FUNC_ID_MAILBOX_SEND_COMMAND;
		request_buffer[svc_buf_idx].a1 = 0;
		request_buffer[svc_buf_idx].a2 = (uint64_t)cmd_addr_buffer;
		request_buffer[svc_buf_idx].a3 = (uint64_t)RECONFIG_DATA_MB_CMD_SIZE;
		request_buffer[svc_buf_idx].resp_data_addr = (uint64_t)resp_addr;
		request_buffer[svc_buf_idx].resp_data_size = FPGA_MB_RESPONSE_MEM_SIZE;
		request_buffer[svc_buf_idx].priv_data = (void *)private_data;

		cmd_addr_buffer[buf_idx++] =
			MBOX_REQUEST_HEADER(FPGA_RECONFIG_DATA, RECONFIG_DATA_MB_CMD_INDIRECT_MODE,
					    RECONFIG_DATA_MB_CMD_LENGTH);
		cmd_addr_buffer[buf_idx++] = MBOX_RECONFIG_REQUEST_DATA_FORMAT(
			RECONFIG_DATA_MB_CMD_DIRECT_COUNT, RECONFIG_DATA_MB_CMD_INDIRECT_ARG,
			RECONFIG_DATA_MB_CMD_INDIRECT_RESPONSE);
		cmd_addr_buffer[buf_idx++] = (uint32_t)(sdm_base + rbf_data_sent);

		if (rbf_data_rem < fpga_reconfig_dma_block_size) {
			cmd_addr_buffer[buf_idx++] = rbf_data_rem;
			private_data->reconfig_data_send_done = 1;
			last_chunk = true;
		} else {
			cmd_addr_buffer[buf_idx++] = fpga_reconfig_dma_block_size;
		}

		trans_id = sip_svc_send(mb_smc_dev, mb_client_token, &request_buffer[svc_buf_idx],
					cmd_send_callback);
		if (trans_id == SIP_SVC_ID_INVALID) {
			k_free(cmd_addr_buffer);
			k_free(resp_addr);
			k_free(private_data);
			err = -EBUSY;
			break;
		}

		if (k_sem_take(&sync_lock.reconfig_data_sem,
			       K_MSEC(RECONFIG_DATA_CHUNK_TIMEOUT_MS)) != 0) {
			LOG_ERR("RECONFIG_DATA async chunk timeout");
			err = -ETIMEDOUT;
			break;
		}
		k_free(private_data);
		private_data = NULL;

		if (rbf_data_rem < fpga_reconfig_dma_block_size) {
			rbf_data_sent += rbf_data_rem;
		} else {
			rbf_data_sent += fpga_reconfig_dma_block_size;
		}
		rbf_data_rem = rbf_data_total - rbf_data_sent;
		svc_buf_idx = (svc_buf_idx + 1) % fpga_reconfig_dma_count;
		buf_idx = 0;
		err = 0;
	}

	k_free(request_buffer);
	if (err) {
		return err;
	}
	if ((rbf_data_rem == 0) && last_chunk) {
		if (k_sem_take(&sync_lock.reconfig_data_send_done_sem,
			       K_MSEC(RECONFIG_DATA_CHUNK_TIMEOUT_MS)) != 0) {
			return -ETIMEDOUT;
		}
	}
	if (sync_lock.response_status) {
		return -EINVAL;
	}
	return err;
}
#endif /* CONFIG_SOC_AGILEX5 */

/**
 * @brief Send the data to sip_svc service layer
 *	based on the cmd_type further data will be send to SDM using mailbox
 *
 * @param[in] cmd_type Command type (Mailbox or Non-Mailbox)
 * @param[in] function_identifier Function identifier for each command type
 * @param[in] cmd_id Command id for each mailbox command
 *
 * @return 0 on success or negative value on fail
 */
static int32_t smc_send(uint32_t cmd_type, uint64_t function_identifier, uint32_t cmd_id,
			struct fm_private_data *private_data)
{
	int32_t trans_id = 0;
	uint32_t *cmd_addr = NULL;
	uint32_t *resp_addr = NULL;
	struct sip_svc_request request;

	if (!mb_smc_dev) {
		LOG_ERR("Mailbox client is not registered");
		return 0;
	}
	if (cmd_type == SIP_SVC_PROTO_CMD_ASYNC) {
#if defined(CONFIG_SOC_AGILEX5)
		cmd_addr = mbox_cmd_buf;
		resp_addr = mbox_resp_buf;
		memset(mbox_cmd_buf, 0, sizeof(mbox_cmd_buf));
		memset(mbox_resp_buf, 0, sizeof(mbox_resp_buf));
#else
		cmd_addr = (uint32_t *)k_malloc(FPGA_MB_CMD_ADDR_MEM_SIZE);
		if (!cmd_addr) {
			LOG_ERR("Failed to allocate command memory");
			return -ENOMEM;
		}
		resp_addr = (uint32_t *)k_malloc(FPGA_MB_RESPONSE_MEM_SIZE);
		if (!resp_addr) {
			k_free(cmd_addr);
			return -ENOMEM;
		}
#endif
		cmd_addr[0] = MBOX_REQUEST_HEADER(cmd_id, 0, 0);
	}
	request.header = SIP_SVC_PROTO_HEADER(cmd_type, 0);
	request.a0 = function_identifier;
	request.a1 = 0;
	if (cmd_type == SIP_SVC_PROTO_CMD_ASYNC) {
		request.a2 = (uint64_t)cmd_addr;
		request.a3 = 4;
	} else {
		request.a2 = cmd_id;
		request.a3 = 0;
	}
	request.a4 = 0;
	request.a5 = 0;
	request.a6 = 0;
	request.a7 = 0;

	if (cmd_type == SIP_SVC_PROTO_CMD_ASYNC) {
		request.resp_data_addr = (uint64_t)resp_addr;
		request.resp_data_size = FPGA_MB_RESPONSE_MEM_SIZE;
		if (cmd_addr) {
			for (int32_t mbox_idx = 0; mbox_idx < request.a3 / 4; mbox_idx++) {
				LOG_DBG("\t [%d ] %08x\n", mbox_idx, cmd_addr[mbox_idx]);
			}
		}
	} else {
		request.resp_data_addr = 0;
		request.resp_data_size = 0;
	}
	request.priv_data = (void *)private_data;

	trans_id = sip_svc_send(mb_smc_dev, mb_client_token, &request, cmd_send_callback);

	if (trans_id == SIP_SVC_ID_INVALID) {
		LOG_ERR("Mailbox send fail cmd_id=0x%x (no open or no free trans_id)", cmd_id);
#if !defined(CONFIG_SOC_AGILEX5)
		k_free(cmd_addr);
		k_free(resp_addr);
#endif
		return -EBUSY;
	}

	LOG_DBG("Mailbox send ok: cmd_id=0x%x trans_id=%d", cmd_id, trans_id);
	return 0;
}

/**
 * @brief Full FPGA reconfiguration thread (bridge off → program → bridge on).
 *
 * @param[in] p1 Bitstream location
 * @param[in] p2 Image size
 * @param[in] p3 Unused
 */
static void reconfig_start(void *p1, void *p2, void *p3)
{
	char *image_ptr = (char *)p1;
	uint32_t img_size = POINTER_TO_UINT(p2);
	uint32_t ret = 0;
	uint32_t cmd_type = 0;
	uint64_t function_identifier = 0;
	uint32_t cmd_id = 0;
	uint32_t retry_count = 0;
	struct fm_private_data private_data;

	ARG_UNUSED(p3);

	/*
	 * Must zero heap_allocated: callback k_free()s only when that flag is
	 * set. Uninitialized stack garbage caused k_free(stack) → sip_svc
	 * Data Abort with FAR 0xaaa… right after CANCEL.
	 */
	memset(&private_data, 0, sizeof(private_data));

	private_data.private_data_lock =
		(struct fm_lock_data *)k_malloc(sizeof(struct fm_lock_data));
	if (!private_data.private_data_lock) {
		LOG_ERR("Failed to allocate command memory");
		k_mutex_unlock(&config_state);
		return;
	}
	memset(private_data.private_data_lock, 0, sizeof(*private_data.private_data_lock));

	k_sem_init(&(private_data.private_data_lock->reconfig_data_sem), 0, 1);
	fm_reconfig_succeeded = false;

	LOG_INF("FPGA FULL config start (V2 mailbox): image %p size %u",
		(void *)image_ptr, img_size);

	/*
	 * Full load contract: bridge off → program → bridge on.
	 * SiP sessions are serial (bridge client, then manager client).
	 * -EIO from off means fabric not ready yet (typical HPS-first); continue.
	 */
	{
		const struct device *fpga_dev = DEVICE_DT_GET(DT_NODELABEL(fpga0));
		int32_t br_ret;

		if (!device_is_ready(fpga_dev)) {
			LOG_ERR("fpga0 not ready");
			k_mutex_unlock(&config_state);
			k_free(private_data.private_data_lock);
			return;
		}

		LOG_DBG("-> BRIDGE_OFF (full load)");
		br_ret = fpga_off(fpga_dev);
		if (br_ret && br_ret != -EIO) {
			LOG_ERR("BRIDGE_OFF failed ret=%d", br_ret);
			k_mutex_unlock(&config_state);
			k_free(private_data.private_data_lock);
			return;
		}
		if (br_ret == -EIO) {
			LOG_DBG("BRIDGE_OFF: FPGA not ready, assume already isolated");
		}
	}

	curr_stage = FPGA_CONFIG_INIT;

	while (true) {
		function_identifier = 0;
		cmd_id = 0;
		cmd_type = 0;

		switch (curr_stage) {
		case FPGA_CONFIG_INIT:
			LOG_DBG("-> CONFIG_INIT (SiP open)");
			ret = svc_client_open();
			if (!ret) {
#if defined(CONFIG_SOC_AGILEX5)
				/*
				 * U-Boot path: FPGA_CONFIG_START does isolation
				 * (CONFIG_COMP), remapper, CANCEL, RECONFIG.
				 * Skip Zephyr CONFIG_COMP; CANCEL/RECONFIG below
				 * take the mailbox session for V2 DATA.
				 */
				LOG_DBG("-> FPGA_CONFIG_START");
				ret = agx5_fpga_config_start();
				if (ret) {
					LOG_ERR("FPGA_CONFIG_START failed ret=%d", ret);
					k_free(private_data.private_data_lock);
					(void)svc_client_close();
					return;
				}
#endif
				curr_stage = FPGA_CANCEL_STAGE;
			} else {
				LOG_ERR("CONFIG_INIT (SiP open) failed ret=%d", ret);
				k_mutex_unlock(&config_state);
				k_free(private_data.private_data_lock);
				return;
			}
			break;

		case FPGA_CANCEL_STAGE:
			LOG_DBG("-> CANCEL");
			cmd_type = SIP_SVC_PROTO_CMD_ASYNC;
			function_identifier = SMC_FUNC_ID_MAILBOX_SEND_COMMAND;
			cmd_id = FPGA_CANCEL;
			break;

		case FPGA_RECONFIG_SEND:
			LOG_DBG("-> RECONFIG");
			cmd_type = SIP_SVC_PROTO_CMD_ASYNC;
			function_identifier = SMC_FUNC_ID_MAILBOX_SEND_COMMAND;
			cmd_id = FPGA_RECONFIG;
			break;

		case FPGA_RECONFIG_DATA_SEND:
			LOG_DBG("-> RECONFIG_DATA");
			if (send_reconfig_data(image_ptr, img_size)) {
				LOG_ERR("RECONFIG_DATA send failed");
				curr_stage = FPGA_RECONFIG_EXIT;
			} else {
				LOG_DBG("RECONFIG_DATA done");
				curr_stage = FPGA_RECONFIG_CHECK_STATUS;
			}
			continue;

		case FPGA_RECONFIG_CHECK_STATUS:
			LOG_DBG("-> RECONFIG_STATUS poll %u/%u",
				retry_count + 1, RECONFIG_STATUS_RETRY_COUNT);
			cmd_type = SIP_SVC_PROTO_CMD_ASYNC;
			function_identifier = SMC_FUNC_ID_MAILBOX_SEND_COMMAND;
			cmd_id = FPGA_RECONFIG_STATUS;
			if (retry_count++ == RECONFIG_STATUS_RETRY_COUNT) {
				LOG_ERR("RECONFIG_STATUS timeout after %u polls",
					RECONFIG_STATUS_RETRY_COUNT);
				curr_stage = FPGA_RECONFIG_EXIT;
				retry_count = 0;
				continue;
			}
			k_sleep(K_MSEC(RECONFIG_STATUS_INTERVAL_DELAY_US));
			break;

		case FPGA_RECONFIG_EXIT:
			LOG_INF("FPGA config sequence finished (exit ok=%d)",
				fm_reconfig_succeeded);
			k_free(private_data.private_data_lock);
			(void)svc_client_close();
			/* Manager SiP closed; safe to use fpga0 bridge client. */
			if (fm_reconfig_succeeded) {
				const struct device *fpga_dev =
					DEVICE_DT_GET(DT_NODELABEL(fpga0));
				int32_t br_ret;

				LOG_DBG("-> BRIDGE_ON (full load success)");
				br_ret = fpga_on(fpga_dev);
				if (br_ret) {
					LOG_ERR("BRIDGE_ON failed ret=%d "
						"(fabric programmed, still isolated)",
						br_ret);
				}
			}
			return;
		}

		if (function_identifier) {
			ret = smc_send(cmd_type, function_identifier, cmd_id, &private_data);
			if (ret == -EBUSY) {
				LOG_ERR("Mailbox send failed at stage cmd_id=0x%x", cmd_id);
				k_free(private_data.private_data_lock);
				(void)svc_client_close();
				return;
			}
			/* Wait for callback */
			if (k_sem_take(&(private_data.private_data_lock->reconfig_data_sem),
				       K_FOREVER) != 0) {
				return;
			}
		}
	}
}

/* Validate the Reconfig status response */
static int32_t fpga_reconfig_status_validate(struct fpga_config_status *reconfig_status_resp)
{
	int32_t ret = 0;

	/* Check for any error */
	ret = reconfig_status_resp->state;
	if (ret && ret != MBOX_CONFIG_STATUS_STATE_CONFIG)
		return ret;

	/* Make sure nStatus is not 0 */
	ret = reconfig_status_resp->pin_status.pin_status;
	if (!(ret & RECONFIG_PIN_STATUS_NSTATUS))
		return MBOX_CFGSTAT_STATE_ERROR_HARDWARE;

	ret = reconfig_status_resp->soft_function_status;
	if (ret & RECONFIG_SOFTFUNC_STATUS_SEU_ERROR)
		return MBOX_CFGSTAT_STATE_ERROR_HARDWARE;

	if ((ret & RECONFIG_SOFTFUNC_STATUS_CONF_DONE) &&
	    (ret & RECONFIG_SOFTFUNC_STATUS_INIT_DONE) && !reconfig_status_resp->state)
		return 0; /* Configuration success */

	return MBOX_CONFIG_STATUS_STATE_CONFIG;
}

/* Create and start the reconfiguration thread */
static uint32_t config_thread_start(char *image_ptr, uint32_t img_size)
{
	/* Will start Reconfig state from here only */
	k_tid_t config_tid = k_thread_create(&config_thread_data, reconfig_thread_stack_area,
						K_KERNEL_STACK_SIZEOF(reconfig_thread_stack_area),
						reconfig_start, (void *)image_ptr,
						UINT_TO_POINTER(img_size), NULL,
						FPGA_CONFIG_THREAD_PRIORITY, 0, K_NO_WAIT);

	if (!config_tid) {
		LOG_ERR("Failed to Create Thread !!");
		k_mutex_unlock(&config_state);
		return -EINVAL;
	}

	k_thread_name_set(&config_thread_data, "fpga_config");
	LOG_DBG("Thread created successfully");
	return 0;
}

int32_t altera_fpga_config_load(char *image_ptr, uint32_t img_size)
{
	uintptr_t phys = POINTER_TO_UINT(image_ptr);

	if (image_ptr == NULL) {
		return -EFAULT;
	}

	if (img_size == 0U) {
		return -ENOSR;
	}

	/*
	 * The SDM takes the bitstream address as a 32-bit offset from the start
	 * of DRAM, so the whole buffer must lie in the first 4 GiB of DRAM.
	 */
	if ((phys < AGX5_DRAM_BASE) ||
	    (((uint64_t)phys - AGX5_DRAM_BASE + img_size) > (UINT64_C(1) << 32))) {
		return -EFAULT;
	}

	if (k_mutex_lock(&config_state, K_NO_WAIT)) {
		LOG_ERR("Failed to acquire mutex lock");
		return -EBUSY;
	}

	fm_reconfig_succeeded = false;

	if (config_thread_start(image_ptr, img_size)) {
		return -EINVAL;
	}

	/*
	 * The thread runs the whole sequence: bridges off, reconfiguration,
	 * bridges on. Wait for it, so that the caller gets the result and
	 * cannot use the SiP clients while the sequence needs them.
	 */
	(void)k_thread_join(&config_thread_data, K_FOREVER);

	/* The mutex belongs to this thread, the config thread cannot release it */
	k_mutex_unlock(&config_state);

	return fm_reconfig_succeeded ? 0 : -EIO;
}

/* Store the fpga status in config_status_info buffer*/
static void get_config_status_info(struct fpga_config_status *status, char *config_status_info)
{
	int32_t ret = 0;

	if ((status->header.header_t.data_length == 0x06) &&
	    (status->header.header_t.error_code == 0x00)) {
		sprintf(config_status_info, "\n\tConfig State 0x%08x\n", status->state);
		sprintf(config_status_info + strlen(config_status_info),
			"\tConfig Version 0x%08x\n", status->version.version);
		sprintf(config_status_info + strlen(config_status_info),
			"\tConfig Version update_number 0x%02x\n",
			status->version.response_version_member.update_number);
		sprintf(config_status_info + strlen(config_status_info),
			"\tMinor_acds_release_number 0x%02x\n",
			status->version.response_version_member.minor_acds_release_number);
		sprintf(config_status_info + strlen(config_status_info),
			"\tMajor_acds_release_number 0x%02x\n",
			status->version.response_version_member.major_acds_release_number);
		sprintf(config_status_info + strlen(config_status_info),
			"\tQspi_flash_index 0x%02x\n",
			status->version.response_version_member.qspi_flash_index);

		sprintf(config_status_info + strlen(config_status_info),
			"\tConfig Pin Status 0x%08x\n", status->pin_status.pin_status);
		sprintf(config_status_info + strlen(config_status_info), "\tMSEL 0x%02x\n",
			status->pin_status.pin_status_member.msel);
		sprintf(config_status_info + strlen(config_status_info), "\tPMF_data 0x%02x\n",
			status->pin_status.pin_status_member.pmf_data);
		sprintf(config_status_info + strlen(config_status_info), "\tnconfig 0x%02x\n",
			status->pin_status.pin_status_member.nconfig);
		sprintf(config_status_info + strlen(config_status_info),
			"\tnconfig_status 0x%02x\n",
			status->pin_status.pin_status_member.nconfig_status);

		sprintf(config_status_info + strlen(config_status_info),
			"\tConfig Soft Function Status 0x%08x\n", status->soft_function_status);
		sprintf(config_status_info + strlen(config_status_info),
			"\tConfig Error location 0x%08x\n", status->error_location);

		ret = fpga_reconfig_status_validate(status);
		if (!ret) {
			sprintf(config_status_info + strlen(config_status_info),
				"\nFPGA Configuration OK ...\n");
		} else {
			sprintf(config_status_info + strlen(config_status_info),
				"\nFPGA Configuration Failed ...\n");
		}
	} else if (status->header.header_t.error_code == 0x2FF) {
		sprintf(config_status_info, "No reply will be provided. Query from proper client "
					    "OR Start Reconfig first");
	} else if (status->header.header_t.error_code == 0x04) {
		sprintf(config_status_info, "Invalid length or Indirect setting in header");
	} else {
		sprintf(config_status_info, "Invalid header");
	}
}

/* Will send the SMC command to check the status of the FPGA configuration */
int32_t altera_fpga_config_status_detail(void *config_status_buf)
{
	int32_t ret = 0;
	struct fm_private_data priv_data;
	struct fm_lock_data lock;

	if (!config_status_buf) {
		return -ENOMEM;
	}

	if (k_mutex_lock(&config_state, K_NO_WAIT)) {
		LOG_ERR("Failed to acquire mutex lock");
		return -EBUSY;
	}

	memset(&priv_data, 0, sizeof(priv_data));
	memset(&lock, 0, sizeof(lock));
	priv_data.private_data_lock = &lock;
	k_sem_init(&(priv_data.private_data_lock->reconfig_data_sem), 0, 1);

	ret = svc_client_open();
	if (ret) {
		LOG_ERR("Client open Failed !!");
		k_mutex_unlock(&config_state);
		return ret;
	}

	curr_stage = FPGA_RECONFIG_CHECK_STATUS;

	if (smc_send(SIP_SVC_PROTO_CMD_ASYNC, SMC_FUNC_ID_MAILBOX_SEND_COMMAND,
		     FPGA_RECONFIG_STATUS, &priv_data)) {
		LOG_ERR("Failed to send the Mailbox Command !!");
		ret = -ECANCELED;
	} else {
		k_sem_take(&(priv_data.private_data_lock->reconfig_data_sem), K_FOREVER);
		get_config_status_info(&priv_data.config_status, config_status_buf);
	}

	if (svc_client_close()) {
		LOG_ERR("SIP SVC client closing failed");
	}

	k_mutex_unlock(&config_state);

	return ret;
}

SYS_INIT(altera_fpga_config_init, POST_KERNEL, CONFIG_APPLICATION_INIT_PRIORITY);
