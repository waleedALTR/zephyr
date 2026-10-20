/*
 * Copyright (c) 2023, Intel Corporation.
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * FPGA manager mailbox client shell to communicate sip_svc service.
 */

#include <stdlib.h>

#include <zephyr/device.h>
#include <zephyr/drivers/flash.h>
#include <zephyr/fpga_manager/fpga_manager.h>
#include <zephyr/shell/shell.h>

#if DT_NODE_EXISTS(DT_NODELABEL(bitstream_partition))
#define BITSTREAM_QSPI_OFFSET DT_REG_ADDR(DT_NODELABEL(bitstream_partition))
#define BITSTREAM_QSPI_MAX    DT_REG_SIZE(DT_NODELABEL(bitstream_partition))
#endif

static void print_fpga_load_error(const struct shell *sh, int32_t err)
{
	if (err == -EBUSY) {
		shell_print(sh, "FPGA manager is busy !!");
	} else if (err == -ENOSR) {
		shell_print(sh, "Insufficient memory");
	} else if (err == -ENOSYS) {
		shell_print(sh, "Vendor API not implemented !!");
	} else if (err == -ENOTSUP) {
		shell_print(sh, "FPGA configuration not supported");
	} else if (err == -ENODEV) {
		shell_print(sh, "No such device found");
	} else if (err == -EFBIG) {
		shell_print(sh, "Bitstream larger than FPGA_CONFIG / QSPI partition");
	} else if (err == -EFAULT) {
		shell_print(sh, "Invalid FPGA_CONFIG staging address/range");
	} else if (err == -EIO) {
		shell_print(sh, "I/O error reading bitstream (err %d)", err);
	} else {
		shell_print(sh, "Failed to start the reconfiguration (err %d)", err);
	}
}

/* Use bitstream already in FPGA_CONFIG (e.g. after 'rbf load' / 'rbf load_qspi'). */
static int32_t cmd_fpga_load_mem(const struct shell *sh, size_t argc, char **argv)
{
	int32_t err;
	uint32_t img_size;
	uint32_t config_type;
	char *endptr;
	char *fpga_mem = NULL;
	uint32_t fpga_mem_size = 0;

	img_size = (uint32_t)strtoul(argv[1], &endptr, 0);
	if (*endptr != '\0' || img_size == 0U) {
		shell_error(sh, "Invalid size '%s'", argv[1]);
		return -EINVAL;
	}

	config_type = (uint32_t)strtol(argv[2], &endptr, 10);
	if (*endptr != '\0') {
		shell_error(sh, "Please provide correct configuration type");
		return -EINVAL;
	}

	err = fpga_get_memory(&fpga_mem, &fpga_mem_size);
	if (err || !fpga_mem) {
		shell_error(sh, "FPGA_CONFIG region unavailable (%d)", err);
		return err ? err : -EFAULT;
	}

	if (img_size > fpga_mem_size) {
		shell_error(sh, "size %u exceeds FPGA_CONFIG %u", img_size, fpga_mem_size);
		return -EFBIG;
	}

	shell_print(sh, "Starting reconfig from DDR 0x%lx (%u bytes, type %u)",
		    (unsigned long)fpga_mem, img_size, config_type);

	err = fpga_load(fpga_mem, img_size, config_type, (argc > 3) ? argv[3] : NULL);
	if (err) {
		print_fpga_load_error(sh, err);
	} else {
		shell_print(sh, "Reconfiguration thread started");
	}

	return err;
}

#if DT_NODE_EXISTS(DT_NODELABEL(bitstream_partition))
static int32_t cmd_fpga_load_qspi(const struct shell *sh, size_t argc, char **argv)
{
	int32_t err;
	uint32_t img_size;
	uint32_t config_type = 0;
	char *endptr;
	char *fpga_mem = NULL;
	uint32_t fpga_mem_size = 0;
	const struct device *flash_dev = DEVICE_DT_GET(DT_NODELABEL(qspi));

	img_size = (uint32_t)strtoul(argv[1], &endptr, 0);
	if (*endptr != '\0' || img_size == 0) {
		shell_error(sh, "Invalid bitstream size");
		return -EINVAL;
	}

	if (argc > 2) {
		config_type = (uint32_t)strtol(argv[2], &endptr, 10);
		if (*endptr != '\0') {
			shell_error(sh, "Please provide correct configuration type");
			return -EINVAL;
		}
	}

	if (img_size > BITSTREAM_QSPI_MAX) {
		shell_error(sh, "Size 0x%x exceeds QSPI bitstream partition (max 0x%x @ 0x%x)",
			    img_size, (uint32_t)BITSTREAM_QSPI_MAX,
			    (uint32_t)BITSTREAM_QSPI_OFFSET);
		return -EFBIG;
	}

	if (!device_is_ready(flash_dev)) {
		shell_error(sh, "QSPI flash device not ready");
		return -ENODEV;
	}

	err = fpga_get_memory(&fpga_mem, &fpga_mem_size);
	if (err || fpga_mem == NULL) {
		shell_error(sh, "Failed to get FPGA reserved memory");
		return -EFAULT;
	}

	if (img_size > fpga_mem_size) {
		shell_error(sh, "Size 0x%x exceeds FPGA reserved memory 0x%x", img_size,
			    fpga_mem_size);
		return -EFBIG;
	}

	shell_print(sh, "Reading 0x%x bytes from QSPI offset 0x%x -> %p", img_size,
		    (uint32_t)BITSTREAM_QSPI_OFFSET, fpga_mem);

	err = flash_read(flash_dev, BITSTREAM_QSPI_OFFSET, fpga_mem, img_size);
	if (err) {
		print_fpga_load_error(sh, -EIO);
		return err;
	}

	err = fpga_load(fpga_mem, img_size, config_type, (argc > 3) ? argv[3] : NULL);
	if (err) {
		print_fpga_load_error(sh, err);
	} else {
		shell_print(sh, "Reconfiguration thread started");
	}

	return err;
}
#endif /* DT_NODE_EXISTS(bitstream_partition) */

static int32_t cmd_fpga_status(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);
	int32_t ret = 0;

	void *config_status = k_malloc(FPGA_RECONFIG_STATUS_BUF_SIZE);

	if (!config_status) {
		shell_print(sh, "Failed to allocate the memory");
		return -EFAULT;
	}

	ret = fpga_get_status(config_status);
	if (ret) {
		if (ret == -ECANCELED) {
			shell_print(sh, "Failed to get the status");
		} else if (ret == -EBUSY) {
			shell_print(sh, "FPGA manager is busy !!");
		} else if (ret == -ENOSYS) {
			shell_print(sh, "Vendor API not implemented !!");
		} else if (ret == -ENOMEM) {
			shell_print(sh, "Invalid Memory Address");
		} else {
			shell_print(sh, "Failed to open connection");
		}
	} else {
		shell_print(sh, "%s", (char *)config_status);
	}

	k_free(config_status);
	return ret;
}

SHELL_STATIC_SUBCMD_SET_CREATE(
	sub_fpga_manager,
	SHELL_CMD_ARG(load_mem, NULL,
		      "Configure from FPGA_CONFIG DDR <size> [type 0:FULL 1:PARTIAL] [device]\n"
		      "Example after rbf load: fpga_manager load_mem 2256896 0",
		      cmd_fpga_load_mem, 3, 1),
#if DT_NODE_EXISTS(DT_NODELABEL(bitstream_partition))
	SHELL_CMD_ARG(load_qspi, NULL,
		      "Configure FPGA from QSPI bitstream partition <size> [type] [device]\n"
		      "Example: fpga_manager load_qspi 2256896 0",
		      cmd_fpga_load_qspi, 2, 2),
#endif
	SHELL_CMD_ARG(status, NULL, "Get FPGA configuration status", cmd_fpga_status, 1, 0),
	SHELL_SUBCMD_SET_END /* Array terminated */
);

SHELL_CMD_REGISTER(fpga_manager, &sub_fpga_manager, "FPGA manager commands", NULL);
