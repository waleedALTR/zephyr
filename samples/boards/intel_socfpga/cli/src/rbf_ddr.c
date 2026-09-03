/*
 * Copyright (c) 2026, Intel Corporation.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/shell/shell.h>
#include <zephyr/fs/fs.h>
#include <zephyr/devicetree.h>
#include <zephyr/sys/crc.h>

#if !DT_NODE_EXISTS(DT_NODELABEL(fpga_config))
#error "fpga_config node is missing from devicetree"
#endif

#define FPGA_CONFIG_BASE ((uint8_t *)DT_REG_ADDR(DT_NODELABEL(fpga_config)))
#define FPGA_CONFIG_SIZE ((size_t)DT_REG_SIZE(DT_NODELABEL(fpga_config)))
#define RBF_CHUNK_SIZE   512

static size_t rbf_loaded_len;

static int rbf_compare_file(const struct shell *sh, const char *path, size_t len)
{
	uint8_t chunk[RBF_CHUNK_SIZE];
	struct fs_dirent dirent;
	struct fs_file_t file;
	size_t off = 0;
	uint32_t crc_file = 0;
	uint32_t crc_ddr;
	int err;

	if (len == 0) {
		shell_error(sh, "nothing in DDR to compare; run 'rbf load' first");
		return -EINVAL;
	}

	err = fs_stat(path, &dirent);
	if (err != 0) {
		shell_error(sh, "stat %s failed (%d)", path, err);
		return err;
	}

	if (dirent.type != FS_DIR_ENTRY_FILE) {
		shell_error(sh, "%s is not a file", path);
		return -EINVAL;
	}

	if (dirent.size != len) {
		shell_error(sh, "size mismatch: %s is %zu bytes, DDR is %zu bytes",
			    path, dirent.size, len);
		return -EINVAL;
	}

	fs_file_t_init(&file);
	err = fs_open(&file, path, FS_O_READ);
	if (err != 0) {
		shell_error(sh, "open %s failed (%d)", path, err);
		return err;
	}

	while (off < len) {
		size_t nreq = MIN(RBF_CHUNK_SIZE, len - off);
		ssize_t n = fs_read(&file, chunk, nreq);

		if (n != (ssize_t)nreq) {
			shell_error(sh, "verify read failed at offset %zu (%zd)", off, n);
			fs_close(&file);
			return (n < 0) ? (int)n : -EIO;
		}

		crc_file = crc32_ieee_update(crc_file, chunk, nreq);

		if (memcmp(chunk, FPGA_CONFIG_BASE + off, nreq) != 0) {
			for (size_t i = 0; i < nreq; i++) {
				if (chunk[i] != FPGA_CONFIG_BASE[off + i]) {
					shell_error(sh,
						    "mismatch at offset 0x%zx: file=0x%02x ddr=0x%02x",
						    off + i, chunk[i],
						    FPGA_CONFIG_BASE[off + i]);
					fs_close(&file);
					return -EIO;
				}
			}
		}

		off += nreq;
	}

	fs_close(&file);

	crc_ddr = crc32_ieee(FPGA_CONFIG_BASE, len);
	if (crc_file != crc_ddr) {
		shell_error(sh, "CRC mismatch: file=0x%08x ddr=0x%08x", crc_file, crc_ddr);
		return -EIO;
	}

	shell_print(sh, "Verify OK: %zu bytes match (%s <-> DDR 0x%lx) CRC32=0x%08x",
		    len, path, (unsigned long)FPGA_CONFIG_BASE, crc_ddr);
	return 0;
}

static int rbf_load_file(const struct shell *sh, const char *path)
{
	struct fs_dirent dirent;
	struct fs_file_t file;
	size_t total = 0;
	int err;

	err = fs_stat(path, &dirent);
	if (err != 0) {
		shell_error(sh, "stat %s failed (%d)", path, err);
		return err;
	}

	if (dirent.type != FS_DIR_ENTRY_FILE) {
		shell_error(sh, "%s is not a file", path);
		return -EINVAL;
	}

	if (dirent.size > FPGA_CONFIG_SIZE) {
		shell_error(sh, "file %zu bytes exceeds FPGA_CONFIG %zu bytes",
			    dirent.size, FPGA_CONFIG_SIZE);
		return -ENOMEM;
	}

	fs_file_t_init(&file);
	err = fs_open(&file, path, FS_O_READ);
	if (err != 0) {
		shell_error(sh, "open %s failed (%d)", path, err);
		return err;
	}

	while (total < dirent.size) {
		ssize_t n = fs_read(&file, FPGA_CONFIG_BASE + total,
				    MIN(RBF_CHUNK_SIZE, dirent.size - total));

		if (n < 0) {
			shell_error(sh, "read failed at offset %zu (%zd)", total, n);
			fs_close(&file);
			return (int)n;
		}
		if (n == 0) {
			break;
		}
		total += (size_t)n;
	}

	fs_close(&file);
	rbf_loaded_len = total;

	shell_print(sh, "Loaded %zu bytes from %s to 0x%lx",
		    total, path, (unsigned long)FPGA_CONFIG_BASE);

	return rbf_compare_file(sh, path, rbf_loaded_len);
}

static int rbf_save_file(const struct shell *sh, const char *path)
{
	struct fs_file_t file;
	size_t total = 0;
	size_t len = rbf_loaded_len;
	int err;

	if (len == 0) {
		shell_error(sh, "nothing loaded; run 'rbf load' first");
		return -EINVAL;
	}

	fs_file_t_init(&file);
	err = fs_open(&file, path, FS_O_CREATE | FS_O_WRITE);
	if (err != 0) {
		shell_error(sh, "open %s failed (%d)", path, err);
		return err;
	}

	err = fs_truncate(&file, 0);
	if (err != 0) {
		shell_error(sh, "trunc %s failed (%d)", path, err);
		fs_close(&file);
		return err;
	}

	while (total < len) {
		ssize_t n = fs_write(&file, FPGA_CONFIG_BASE + total,
				     MIN(RBF_CHUNK_SIZE, len - total));

		if (n < 0) {
			shell_error(sh, "write failed at offset %zu (%zd)", total, n);
			fs_close(&file);
			return (int)n;
		}
		if (n == 0) {
			shell_error(sh, "short write at offset %zu", total);
			fs_close(&file);
			return -EIO;
		}
		total += (size_t)n;
	}

	fs_close(&file);
	shell_print(sh, "Wrote %zu bytes from 0x%lx to %s",
		    total, (unsigned long)FPGA_CONFIG_BASE, path);

	return rbf_compare_file(sh, path, rbf_loaded_len);
}

static int cmd_rbf_info(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);

	shell_print(sh, "FPGA_CONFIG base: 0x%lx", (unsigned long)FPGA_CONFIG_BASE);
	shell_print(sh, "FPGA_CONFIG size: %zu bytes", FPGA_CONFIG_SIZE);
	shell_print(sh, "Last loaded:      %zu bytes", rbf_loaded_len);
	return 0;
}

static int cmd_rbf_load(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);

	return rbf_load_file(sh, argv[1]);
}

static int cmd_rbf_save(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);

	return rbf_save_file(sh, argv[1]);
}

static int cmd_rbf_verify(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);

	return rbf_compare_file(sh, argv[1], rbf_loaded_len);
}

SHELL_STATIC_SUBCMD_SET_CREATE(rbf_cmds,
	SHELL_CMD_ARG(info, NULL, "Show FPGA_CONFIG address and last load size",
		      cmd_rbf_info, 1, 0),
	SHELL_CMD_ARG(load, NULL, "Read RBF from SD into FPGA_CONFIG\n"
		      "Usage: rbf load <path>\n"
		      "Example: rbf load /SD:/BASELI~1.RBF",
		      cmd_rbf_load, 2, 0),
	SHELL_CMD_ARG(save, NULL, "Write FPGA_CONFIG back to SD\n"
		      "Usage: rbf save <path>\n"
		      "Example: rbf save /SD:/RBFCOPY.RBF",
		      cmd_rbf_save, 2, 0),
	SHELL_CMD_ARG(verify, NULL, "Compare SD file with FPGA_CONFIG DDR\n"
		      "Usage: rbf verify <path>\n"
		      "Example: rbf verify /SD:/RBFCOPY.RBF",
		      cmd_rbf_verify, 2, 0),
	SHELL_SUBCMD_SET_END
);

SHELL_CMD_REGISTER(rbf, &rbf_cmds, "Copy RBF between SD card and FPGA_CONFIG DDR", NULL);
