/*
 * Copyright (c) 2026 Muhammad Waleed Badar
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Ported from zephyrproject-rtos/zephyr main for SoCFPGA v4.3 APIs
 * (getopt / struct sockaddr tftpc.server).
 */

#include <zephyr/logging/log.h>
LOG_MODULE_DECLARE(net_shell);

#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/net/net_ip.h>
#include <zephyr/net/tftp.h>
#include <zephyr/shell/shell.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/sys/util.h>

#include "net_shell_private.h"

#define DEFAULT_TFTP_PORT 69

#if defined(CONFIG_TFTP_LIB)
static const struct shell *tftp_shell;

/* Upstream-style context for get / put. */
struct tftp_client_shell_ctx {
	struct tftpc client;
	uint8_t *file_name;
	uint8_t *data;
	uint8_t *mode;
};

static void tftp_client_event_cb(const struct tftp_evt *evt)
{
	switch (evt->type) {
	case TFTP_EVT_DATA:
		shell_hexdump(tftp_shell, evt->param.data.data_ptr, evt->param.data.len);
		break;
	case TFTP_EVT_ERROR:
		shell_error(tftp_shell, "Error code %d msg: %s", evt->param.error.code,
			    evt->param.error.msg);
		break;
	default:
		break;
	}
}

static int tftp_client_args_to_params(const struct shell *sh, struct tftp_client_shell_ctx *ctx,
				      size_t argc, char *argv[])
{
	struct getopt_state *state;
	int opt_index = 0;
	int opt;

	static const struct option long_opts[] = {
		{"server", required_argument, 0, 's'},
		{"file", required_argument, 0, 'f'},
		{"mode", required_argument, 0, 'm'},
		{"data", required_argument, 0, 'd'},
		{0, 0, 0, 0}};

	ARG_UNUSED(sh);

	/* Defaults */
	memset(ctx, 0, sizeof(*ctx));
	ctx->client.callback = tftp_client_event_cb;
	ctx->mode = "octet";
	ctx->data = NULL;

	while ((opt = getopt_long(argc, argv, "s:f:m:d:", long_opts, &opt_index)) != -1) {
		state = getopt_state_get();
		switch (opt) {
		case 's': {
			memset(&ctx->client.server, 0, sizeof(ctx->client.server));
			if (!net_ipaddr_parse(state->optarg, strlen(state->optarg),
					      &ctx->client.server)) {
				PR_ERROR("Invalid server address: %s\n", state->optarg);
				return -EINVAL;
			}
			break;
		}
		case 'f':
			ctx->file_name = (uint8_t *)state->optarg;
			break;
		case 'm':
			ctx->mode = (uint8_t *)state->optarg;
			break;
		case 'd':
			ctx->data = (uint8_t *)state->optarg;
			break;
		case '?':
		default:
			PR_ERROR("Invalid option or option usage: %s\n", argv[opt_index + 1]);
			return -ENOEXEC;
		}
	}

	if (net_sin(&ctx->client.server)->sin_port == 0) {
		net_sin(&ctx->client.server)->sin_port = htons(DEFAULT_TFTP_PORT);
	}

	return 0;
}

/* Cap DDR write when -a is used without -z. */
#define TFTP_SHELL_DDR_DEFAULT_MAX (16U * 1024U * 1024U)

struct tftp_ddr_shell_ctx {
	struct tftpc client;
	uint8_t *file_name;
	uint8_t *mode;
	uint8_t *ddr_base;
	size_t ddr_max;
	size_t ddr_loaded;
	bool ddr_overflow;
};

static struct tftp_ddr_shell_ctx *tftp_ddr_active_ctx;

#if defined(CONFIG_SOC_AGILEX5)
#include <zephyr/devicetree.h>

/*
 * HPS low DDR window used for -a validation (Agilex5 only).
 * Restricted sub-ranges (boot / Zephyr / mailbox / fpga_config) are denied below.
 */
#define TFTP_SHELL_DDR_LOW_BASE  ((uintptr_t)0x80000000ULL)
#define TFTP_SHELL_DDR_LOW_END   ((uintptr_t)0x100000000ULL)

#define TFTP_SHELL_MEM0_NODE         DT_NODELABEL(mem0)
#define TFTP_SHELL_MBOX_NODE         DT_NODELABEL(mbox_ddr)
#define TFTP_SHELL_FPGA_CONFIG_NODE  DT_NODELABEL(fpga_config)

struct tftp_shell_deny_range {
	uintptr_t base;
	size_t size;
	const char *name;
};

static bool tftp_shell_ranges_overlap(uintptr_t a, size_t a_len, uintptr_t b, size_t b_len)
{
	if (a_len == 0U || b_len == 0U) {
		return false;
	}
	return (a < (b + b_len)) && (b < (a + a_len));
}

/*
 * Allow any HPS low-DDR address except known restricted regions:
 *  - FSBL/BL staging below Zephyr RAM
 *  - Zephyr SRAM (mem0)
 *  - TF-A SiP mailbox (mbox_ddr)
 *  - fpga_config buffer
 */
static int tftp_shell_validate_ddr_dest(const struct shell *sh, uintptr_t addr,
					size_t *max_inout)
{
	size_t max_len = *max_inout;
	size_t room;
	size_t i;
	struct tftp_shell_deny_range deny[4];
	size_t deny_cnt = 0;

	if (addr < TFTP_SHELL_DDR_LOW_BASE || addr >= TFTP_SHELL_DDR_LOW_END) {
		shell_error(sh,
			    "Address 0x%lx is outside HPS low DDR "
			    "[0x%lx .. 0x%lx)",
			    (unsigned long)addr,
			    (unsigned long)TFTP_SHELL_DDR_LOW_BASE,
			    (unsigned long)TFTP_SHELL_DDR_LOW_END);
		return -EINVAL;
	}

#if DT_NODE_EXISTS(TFTP_SHELL_MEM0_NODE)
	{
		const uintptr_t mem0_base = (uintptr_t)DT_REG_ADDR(TFTP_SHELL_MEM0_NODE);
		const size_t mem0_size = (size_t)DT_REG_SIZE(TFTP_SHELL_MEM0_NODE);

		/* Boot / FSBL area from DDR base up to Zephyr RAM. */
		if (mem0_base > TFTP_SHELL_DDR_LOW_BASE) {
			deny[deny_cnt].base = TFTP_SHELL_DDR_LOW_BASE;
			deny[deny_cnt].size = (size_t)(mem0_base - TFTP_SHELL_DDR_LOW_BASE);
			deny[deny_cnt].name = "FSBL/BL DDR";
			deny_cnt++;
		}
		deny[deny_cnt].base = mem0_base;
		deny[deny_cnt].size = mem0_size;
		deny[deny_cnt].name = "Zephyr RAM (mem0)";
		deny_cnt++;
	}
#endif
#if DT_NODE_EXISTS(TFTP_SHELL_MBOX_NODE)
	deny[deny_cnt].base = (uintptr_t)DT_REG_ADDR(TFTP_SHELL_MBOX_NODE);
	deny[deny_cnt].size = (size_t)DT_REG_SIZE(TFTP_SHELL_MBOX_NODE);
	deny[deny_cnt].name = "TF-A mailbox (mbox_ddr)";
	deny_cnt++;
#endif
#if DT_NODE_EXISTS(TFTP_SHELL_FPGA_CONFIG_NODE)
	deny[deny_cnt].base = (uintptr_t)DT_REG_ADDR(TFTP_SHELL_FPGA_CONFIG_NODE);
	deny[deny_cnt].size = (size_t)DT_REG_SIZE(TFTP_SHELL_FPGA_CONFIG_NODE);
	deny[deny_cnt].name = "fpga_config";
	deny_cnt++;
#endif

	room = (size_t)(TFTP_SHELL_DDR_LOW_END - addr);

	for (i = 0; i < deny_cnt; i++) {
		const uintptr_t dbase = deny[i].base;
		const size_t dsize = deny[i].size;

		if (addr >= dbase && addr < dbase + dsize) {
			shell_error(sh,
				    "DDR address 0x%lx is in restricted region %s "
				    "[0x%lx .. 0x%lx)",
				    (unsigned long)addr, deny[i].name,
				    (unsigned long)dbase,
				    (unsigned long)(dbase + dsize));
			return -EINVAL;
		}
		/* Stop before the next restricted region above this address. */
		if (dbase > addr) {
			size_t until = (size_t)(dbase - addr);

			if (until < room) {
				room = until;
			}
		}
	}

	if (room == 0U) {
		shell_error(sh, "No writable DDR room at 0x%lx", (unsigned long)addr);
		return -EINVAL;
	}

	if (max_len == 0U || max_len > room) {
		max_len = room;
	}

	for (i = 0; i < deny_cnt; i++) {
		if (tftp_shell_ranges_overlap(addr, max_len, deny[i].base, deny[i].size)) {
			shell_error(sh,
				    "Transfer [0x%lx + %zu) hits restricted region %s",
				    (unsigned long)addr, max_len, deny[i].name);
			return -EINVAL;
		}
	}

	*max_inout = max_len;
	return 0;
}
#endif /* CONFIG_SOC_AGILEX5 */

static void tftp_ddr_event_cb(const struct tftp_evt *evt)
{
	struct tftp_ddr_shell_ctx *ctx = tftp_ddr_active_ctx;

	switch (evt->type) {
	case TFTP_EVT_DATA:
		if (ctx != NULL && ctx->ddr_base != NULL) {
			size_t len = evt->param.data.len;

			if (ctx->ddr_overflow) {
				break;
			}
			if (ctx->ddr_loaded + len > ctx->ddr_max) {
				shell_error(tftp_shell,
					    "DDR buffer full (%zu + %zu > %zu)",
					    ctx->ddr_loaded, len, ctx->ddr_max);
				ctx->ddr_overflow = true;
				break;
			}
			memcpy(ctx->ddr_base + ctx->ddr_loaded,
			       evt->param.data.data_ptr, len);
			ctx->ddr_loaded += len;
		}
		break;
	case TFTP_EVT_ERROR:
		shell_error(tftp_shell, "Error code %d msg: %s", evt->param.error.code,
			    evt->param.error.msg);
		break;
	default:
		break;
	}
}

static int tftp_ddr_args_to_params(const struct shell *sh, struct tftp_ddr_shell_ctx *ctx,
				   size_t argc, char *argv[], bool size_required)
{
	struct getopt_state *state;
	int opt_index = 0;
	int opt;
	int err = 0;
	int ret;
	bool size_set = false;
	size_t requested_size = 0;

	static const struct option long_opts[] = {
		{"server", required_argument, 0, 's'},
		{"file", required_argument, 0, 'f'},
		{"mode", required_argument, 0, 'm'},
		{"addr", required_argument, 0, 'a'},
		{"size", required_argument, 0, 'z'},
		{0, 0, 0, 0}};

	memset(ctx, 0, sizeof(*ctx));
	ctx->client.callback = tftp_ddr_event_cb;
	ctx->mode = "octet";
	ctx->ddr_base = NULL;
	/* get_ddr: optional -z with 16 MiB default; put_ddr: -z required, no default. */
	ctx->ddr_max = size_required ? 0U : TFTP_SHELL_DDR_DEFAULT_MAX;

	while ((opt = getopt_long(argc, argv, "s:f:m:a:z:", long_opts, &opt_index)) != -1) {
		state = getopt_state_get();
		switch (opt) {
		case 's': {
			memset(&ctx->client.server, 0, sizeof(ctx->client.server));
			if (!net_ipaddr_parse(state->optarg, strlen(state->optarg),
					      &ctx->client.server)) {
				PR_ERROR("Invalid server address: %s\n", state->optarg);
				return -EINVAL;
			}
			break;
		}
		case 'f':
			ctx->file_name = (uint8_t *)state->optarg;
			break;
		case 'm':
			ctx->mode = (uint8_t *)state->optarg;
			break;
		case 'a': {
			unsigned long long addr;

			addr = shell_strtoull(state->optarg, 0, &err);
			if (err != 0) {
				PR_ERROR("Invalid DDR address: %s\n", state->optarg);
				return -EINVAL;
			}
			ctx->ddr_base = (uint8_t *)(uintptr_t)addr;
			break;
		}
		case 'z': {
			unsigned long long sz;

			sz = shell_strtoull(state->optarg, 0, &err);
			if (err != 0 || sz == 0) {
				PR_ERROR("Invalid max size: %s\n", state->optarg);
				return -EINVAL;
			}
			if (sz > UINT32_MAX) {
				PR_ERROR("Size exceeds uint32_t max: %s\n", state->optarg);
				return -EINVAL;
			}
			ctx->ddr_max = (size_t)sz;
			size_set = true;
			break;
		}
		case '?':
		default:
			PR_ERROR("Invalid option or option usage: %s\n", argv[opt_index + 1]);
			return -ENOEXEC;
		}
	}

	if (net_sin(&ctx->client.server)->sin_port == 0) {
		net_sin(&ctx->client.server)->sin_port = htons(DEFAULT_TFTP_PORT);
	}

	if (ctx->ddr_base == NULL) {
		PR_ERROR("DDR address required (-a)\n");
		return -EINVAL;
	}

	if (size_required && !size_set) {
		PR_ERROR("Size required (-z)\n");
		return -EINVAL;
	}

	requested_size = ctx->ddr_max;

#if defined(CONFIG_SOC_AGILEX5)
	/* Restricted-region check is Agilex5-only (FSBL/mem0/mbox/fpga_config). */
	ret = tftp_shell_validate_ddr_dest(sh, (uintptr_t)ctx->ddr_base, &ctx->ddr_max);
	if (ret < 0) {
		ctx->ddr_base = NULL;
		return ret;
	}
	/* put_ddr: do not silently shorten the upload if the range was clamped. */
	if (size_required && ctx->ddr_max < requested_size) {
		PR_ERROR("Size %zu exceeds writable DDR room %zu at 0x%lx\n", requested_size,
			 ctx->ddr_max, (unsigned long)(uintptr_t)ctx->ddr_base);
		ctx->ddr_base = NULL;
		return -EINVAL;
	}
#else
	ARG_UNUSED(sh);
	ARG_UNUSED(ret);
	ARG_UNUSED(requested_size);
#endif

	return 0;
}
#endif /* CONFIG_TFTP_LIB */

static int cmd_tftp_get(const struct shell *sh, size_t argc, char *argv[])
{
#if defined(CONFIG_TFTP_LIB)
	struct tftp_client_shell_ctx ctx;
	int ret;

	tftp_shell = sh;
	ret = tftp_client_args_to_params(sh, &ctx, argc, argv);
	if (ret < 0) {
		return ret;
	}

	if (ctx.file_name == NULL) {
		PR_ERROR("Invalid argument(s)\n");
		return -EINVAL;
	}

	ret = tftp_get(&ctx.client, ctx.file_name, ctx.mode);
	if (ret < 0) {
		PR_ERROR("Error while getting file (%d)\n", ret);
		return ret;
	}
#else
	PR_INFO("Set %s to enable %s support.\n", "CONFIG_TFTP_LIB", "TFTP client");
#endif
	return 0;
}

static int cmd_tftp_put(const struct shell *sh, size_t argc, char *argv[])
{
#if defined(CONFIG_TFTP_LIB)
	struct tftp_client_shell_ctx ctx;
	int ret;

	tftp_shell = sh;
	ret = tftp_client_args_to_params(sh, &ctx, argc, argv);
	if (ret < 0) {
		return ret;
	}

	if (ctx.file_name == NULL || ctx.data == NULL) {
		PR_ERROR("Invalid argument(s)\n");
		return -EINVAL;
	}

	ret = tftp_put(&ctx.client, ctx.file_name, ctx.mode, ctx.data, strlen(ctx.data));
	if (ret < 0) {
		PR_ERROR("Error while putting file (%d)\n", ret);
		return ret;
	}
#else
	PR_INFO("Set %s to enable %s support.\n", "CONFIG_TFTP_LIB", "TFTP client");
#endif
	return 0;
}

static int cmd_tftp_get_ddr(const struct shell *sh, size_t argc, char *argv[])
{
#if defined(CONFIG_TFTP_LIB)
	struct tftp_ddr_shell_ctx ctx;
	int ret;

	tftp_shell = sh;
	ret = tftp_ddr_args_to_params(sh, &ctx, argc, argv, false);
	if (ret < 0) {
		return ret;
	}

	if (ctx.file_name == NULL) {
		PR_ERROR("Invalid argument(s)\n");
		return -EINVAL;
	}

	tftp_ddr_active_ctx = &ctx;
	ret = tftp_get(&ctx.client, ctx.file_name, ctx.mode);
	tftp_ddr_active_ctx = NULL;

	if (ret < 0) {
		PR_ERROR("Error while getting file (%d)\n", ret);
		return ret;
	}

	if (ctx.ddr_overflow) {
		PR_ERROR("Download truncated: DDR buffer full\n");
		return -ENOMEM;
	}

	PR_INFO("Wrote %zu bytes to DDR 0x%016lx (max %zu)\n", ctx.ddr_loaded,
		(unsigned long)(uintptr_t)ctx.ddr_base, ctx.ddr_max);
#else
	PR_INFO("Set %s to enable %s support.\n", "CONFIG_TFTP_LIB", "TFTP client");
#endif
	return 0;
}

static int cmd_tftp_put_ddr(const struct shell *sh, size_t argc, char *argv[])
{
#if defined(CONFIG_TFTP_LIB)
	struct tftp_ddr_shell_ctx ctx;
	int ret;

	tftp_shell = sh;
	ret = tftp_ddr_args_to_params(sh, &ctx, argc, argv, true);
	if (ret < 0) {
		return ret;
	}

	if (ctx.file_name == NULL) {
		PR_ERROR("Invalid argument(s)\n");
		return -EINVAL;
	}

	ret = tftp_put(&ctx.client, ctx.file_name, ctx.mode, ctx.ddr_base,
		       (uint32_t)ctx.ddr_max);
	if (ret < 0) {
		PR_ERROR("Error while putting file (%d)\n", ret);
		return ret;
	}

	PR_INFO("Sent %d bytes from DDR 0x%016lx\n", ret,
		(unsigned long)(uintptr_t)ctx.ddr_base);
#else
	PR_INFO("Set %s to enable %s support.\n", "CONFIG_TFTP_LIB", "TFTP client");
#endif
	return 0;
}

/* Use plain help strings (same style as other net shell cmds on this tree). */
SHELL_STATIC_SUBCMD_SET_CREATE(tftp_cmds,
	SHELL_CMD_ARG(get, NULL,
		      "net tftp get -s <addr>[:port] -f <file> [-m <mode>]",
		      cmd_tftp_get, 5, 2),
	SHELL_CMD_ARG(put, NULL,
		      "net tftp put -s <addr>[:port] -f <file> -d <data> [-m <mode>]",
		      cmd_tftp_put, 7, 2),
	SHELL_CMD_ARG(get_ddr, NULL,
		      "net tftp get_ddr -s <addr>[:port] -f <file> -a <ddr> "
		      "[-z <max>] [-m <mode>] "
		      "(Agilex5: rejects FSBL/mem0/mbox/fpga_config DDR)",
		      cmd_tftp_get_ddr, 7, 4),
	SHELL_CMD_ARG(put_ddr, NULL,
		      "net tftp put_ddr -s <addr>[:port] -f <file> -a <ddr> -z <size> "
		      "[-m <mode>] "
		      "(Agilex5: rejects FSBL/mem0/mbox/fpga_config DDR)",
		      cmd_tftp_put_ddr, 9, 2),
	SHELL_SUBCMD_SET_END
);

SHELL_SUBCMD_ADD((net), tftp, &tftp_cmds, "TFTP client commands", NULL, 1, 0);
