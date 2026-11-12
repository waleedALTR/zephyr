/*
 * Copyright (c) 2021 Antmicro <www.antmicro.com>
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>
#include <zephyr/shell/shell.h>
#include <zephyr/version.h>
#include <stdlib.h>
#include <zephyr/drivers/fpga.h>

#if defined(CONFIG_ALTERA_AGILEX_BRIDGE_FPGA)
/* SDM RECONFIG_STATUS detail from drivers/fpga/fpga_altera_agilex_config.c */
#define FPGA_STATUS_DETAIL_BUF_SIZE 400
int32_t altera_fpga_config_status_detail(void *config_status_buf);
#endif

static int parse_common_args(const struct shell *sh, char **argv,
			     const struct device **dev)
{
	*dev = shell_device_get_binding(argv[1]);
	if (!*dev) {
		shell_error(sh, "FPGA device %s not found", argv[1]);
		return -ENODEV;
	}
	return 0;
}

static int cmd_on(const struct shell *sh, size_t argc, char **argv)
{
	const struct device *dev;
	int err;

	err = parse_common_args(sh, argv, &dev);
	if (err < 0) {
		return err;
	}

	shell_print(sh, "%s: turning on", dev->name);

	err = fpga_on(dev);
	if (err) {
		shell_error(sh, "Error: %d", err);
	}

	return err;
}

static int cmd_off(const struct shell *sh, size_t argc, char **argv)
{
	const struct device *dev;
	int err;

	err = parse_common_args(sh, argv, &dev);
	if (err < 0) {
		return err;
	}

	shell_print(sh, "%s: turning off", dev->name);

	err = fpga_off(dev);
	if (err) {
		shell_error(sh, "Error: %d", err);
	}

	return err;
}

static int cmd_reset(const struct shell *sh, size_t argc, char **argv)
{
	const struct device *dev;
	int err;

	err = parse_common_args(sh, argv, &dev);
	if (err < 0) {
		return err;
	}

	shell_print(sh, "%s: resetting FPGA", dev->name);

	err = fpga_reset(dev);
	if (err) {
		shell_error(sh, "Error: %d", err);
	}

	return err;
}

/*
 * The bitstream may be anywhere in memory the driver can reach; the Altera
 * driver rejects a buffer outside the first 4 GiB of DRAM (-EFAULT).
 */
static int cmd_load(const struct shell *sh, size_t argc, char **argv)
{
	const struct device *dev;
	int err;
	char *endptr;
	unsigned long addr;
	unsigned long size;

	err = parse_common_args(sh, argv, &dev);
	if (err < 0) {
		return err;
	}

	addr = strtoul(argv[2], &endptr, 0);
	if (*endptr != '\0') {
		shell_error(sh, "Invalid address '%s'", argv[2]);
		return -EINVAL;
	}

	size = strtoul(argv[3], &endptr, 0);
	if (*endptr != '\0' || size == 0UL) {
		shell_error(sh, "Invalid size '%s'", argv[3]);
		return -EINVAL;
	}

	shell_print(sh, "%s: loading bitstream @ 0x%lx (%lu bytes)",
		    dev->name, addr, size);

	err = fpga_load(dev, (uint32_t *)addr, (uint32_t)size);
	if (err) {
		shell_error(sh, "Error: %d", err);
	}

	return err;
}

static int cmd_get_status(const struct shell *sh, size_t argc, char **argv)
{
	const struct device *dev;
	int err;
	enum FPGA_status st;

	err = parse_common_args(sh, argv, &dev);
	if (err < 0) {
		return err;
	}

	st = fpga_get_status(dev);
	shell_print(sh, "%s status: %d (%s)", dev->name, st,
		    st == FPGA_STATUS_ACTIVE ? "ACTIVE" : "INACTIVE");

#if defined(CONFIG_ALTERA_AGILEX_BRIDGE_FPGA)
	{
		char *detail = k_malloc(FPGA_STATUS_DETAIL_BUF_SIZE);
		int32_t ret;

		if (!detail) {
			shell_error(sh, "Failed to allocate status buffer");
			return -ENOMEM;
		}

		ret = altera_fpga_config_status_detail(detail);
		if (ret) {
			shell_error(sh, "SDM status query failed: %d", ret);
			k_free(detail);
			return ret;
		}

		shell_print(sh, "%s", detail);
		k_free(detail);
	}
#endif

	return 0;
}

static int cmd_get_info(const struct shell *sh, size_t argc, char **argv)
{
	const struct device *dev;
	int err;

	err = parse_common_args(sh, argv, &dev);
	if (err < 0) {
		return err;
	}

	shell_print(sh, "%s", fpga_get_info(dev));

	return err;
}

SHELL_STATIC_SUBCMD_SET_CREATE(
	sub_fpga, SHELL_CMD_ARG(off, NULL, "<device>", cmd_off, 2, 0),
	SHELL_CMD_ARG(on, NULL, "<device>", cmd_on, 2, 0),
	SHELL_CMD_ARG(reset, NULL, "<device>", cmd_reset, 2, 0),
	SHELL_CMD_ARG(load, NULL,
		      "<device> <address> <size> — full load",
		      cmd_load, 4, 0),
	SHELL_CMD_ARG(get_status, NULL,
		      "<device> — ACTIVE/INACTIVE; Agilex also prints SDM RECONFIG_STATUS",
		      cmd_get_status, 2, 0),
	SHELL_CMD_ARG(get_info, NULL, "<device>", cmd_get_info, 2, 0),
	SHELL_SUBCMD_SET_END);

SHELL_CMD_REGISTER(fpga, &sub_fpga, "FPGA commands", NULL);
