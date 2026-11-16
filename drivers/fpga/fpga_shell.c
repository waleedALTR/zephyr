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

#if defined(CONFIG_FPGA_SHELL_LOAD_IMAGE)
#include <zephyr/storage/flash_map.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/sys/util.h>

#include <bootutil/image.h>
#endif

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

#if defined(CONFIG_FPGA_SHELL_LOAD_IMAGE)
/*
 * Read the bitstream that follows the MCUboot image header of a flash area
 * into memory. The image hash is not verified.
 */
static int image_read(uint8_t area_id, void *buf, size_t buf_size, uint32_t *size)
{
	const struct flash_area *fa;
	struct image_header hdr;
	uint32_t hdr_size;
	uint32_t img_size = 0U;
	int err;

	err = flash_area_open(area_id, &fa);
	if (err != 0) {
		return err;
	}

	err = flash_area_read(fa, 0, &hdr, sizeof(hdr));
	if (err == 0) {
		/* All header fields are little-endian */
		hdr_size = sys_le16_to_cpu(hdr.ih_hdr_size);
		img_size = sys_le32_to_cpu(hdr.ih_img_size);

		if ((sys_le32_to_cpu(hdr.ih_magic) != IMAGE_MAGIC) || (hdr_size < sizeof(hdr)) ||
		    (hdr_size > fa->fa_size) || (img_size > (fa->fa_size - hdr_size))) {
			err = -ENOEXEC;
		} else if ((sys_le32_to_cpu(hdr.ih_flags) &
			    (ENCRYPTIONFLAGS | COMPRESSIONFLAGS)) != 0U) {
			err = -ENOTSUP;
		} else if (img_size > buf_size) {
			err = -EFBIG;
		} else {
			err = flash_area_read(fa, hdr_size, buf, img_size);
		}
	}

	flash_area_close(fa);

	*size = img_size;

	return err;
}

static int cmd_load_image(const struct shell *sh, size_t argc, char **argv)
{
	/* fpga_load() takes the whole bitstream as an array of 32-bit words */
	static uint32_t bitstream[DIV_ROUND_UP(CONFIG_FPGA_SHELL_LOAD_IMAGE_MAX_SIZE,
					       sizeof(uint32_t))];
	const struct device *dev;
	unsigned long area_id;
	uint32_t size;
	char *endptr;
	int err;

	err = parse_common_args(sh, argv, &dev);
	if (err < 0) {
		return err;
	}

	area_id = strtoul(argv[2], &endptr, 0);
	if ((*endptr != '\0') || (area_id > UINT8_MAX)) {
		shell_error(sh, "Invalid flash area ID '%s'", argv[2]);
		return -EINVAL;
	}

	err = image_read((uint8_t)area_id, bitstream, sizeof(bitstream), &size);
	if (err != 0) {
		shell_error(sh, "Failed to read the image from flash area %lu: %d", area_id, err);
		return err;
	}

	shell_print(sh, "%s: loading bitstream from flash area %lu (%u bytes)", dev->name,
		    area_id, size);

	err = fpga_load(dev, bitstream, size);
	if (err != 0) {
		shell_error(sh, "Error: %d", err);
	}

	return err;
}
#endif /* CONFIG_FPGA_SHELL_LOAD_IMAGE */

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
	IF_ENABLED(CONFIG_FPGA_SHELL_LOAD_IMAGE,
		   (SHELL_CMD_ARG(load_image, NULL,
				  "<device> <flash area ID>: load the bitstream of an MCUboot image",
				  cmd_load_image, 3, 0),))
	SHELL_CMD_ARG(get_status, NULL,
		      "<device> — ACTIVE/INACTIVE; Agilex also prints SDM RECONFIG_STATUS",
		      cmd_get_status, 2, 0),
	SHELL_CMD_ARG(get_info, NULL, "<device>", cmd_get_info, 2, 0),
	SHELL_SUBCMD_SET_END);

SHELL_CMD_REGISTER(fpga, &sub_fpga, "FPGA commands", NULL);
