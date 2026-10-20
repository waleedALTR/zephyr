/*
 * Copyright (c) 2026, Intel Corporation.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stdlib.h>
#include <string.h>

#include <zephyr/cache.h>
#include <zephyr/kernel.h>
#include <zephyr/shell/shell.h>
#include <zephyr/fs/fs.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/flash.h>
#include <zephyr/sys/crc.h>

#if !DT_NODE_EXISTS(DT_NODELABEL(fpga_config))
#error "fpga_config node is missing from devicetree"
#endif

#define FPGA_CONFIG_BASE ((uint8_t *)DT_REG_ADDR(DT_NODELABEL(fpga_config)))
#define FPGA_CONFIG_SIZE ((size_t)DT_REG_SIZE(DT_NODELABEL(fpga_config)))
#define RBF_CHUNK_SIZE   512

#if DT_NODE_EXISTS(DT_NODELABEL(bitstream_partition))
#define BITSTREAM_QSPI_OFFSET ((off_t)DT_REG_ADDR(DT_NODELABEL(bitstream_partition)))
#define BITSTREAM_QSPI_MAX    ((size_t)DT_REG_SIZE(DT_NODELABEL(bitstream_partition)))
#endif

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

#if DT_NODE_EXISTS(DT_NODELABEL(bitstream_partition))
static const struct device *rbf_qspi_dev(void)
{
	return DEVICE_DT_GET(DT_NODELABEL(qspi));
}

static int rbf_load_qspi(const struct shell *sh, size_t len)
{
	const struct device *flash_dev = rbf_qspi_dev();
	uint32_t crc;
	int err;

	if (len == 0 || len > FPGA_CONFIG_SIZE) {
		shell_error(sh, "size %zu invalid (FPGA_CONFIG %zu)", len, FPGA_CONFIG_SIZE);
		return -EINVAL;
	}

	if (len > BITSTREAM_QSPI_MAX) {
		shell_error(sh, "size %zu exceeds QSPI bitstream partition %zu @ 0x%lx",
			    len, BITSTREAM_QSPI_MAX, (unsigned long)BITSTREAM_QSPI_OFFSET);
		return -EFBIG;
	}

	if (!device_is_ready(flash_dev)) {
		shell_error(sh, "QSPI flash device not ready");
		return -ENODEV;
	}

	shell_print(sh, "Reading %zu bytes from QSPI 0x%lx -> DDR 0x%lx ...",
		    len, (unsigned long)BITSTREAM_QSPI_OFFSET,
		    (unsigned long)FPGA_CONFIG_BASE);

	err = flash_read(flash_dev, BITSTREAM_QSPI_OFFSET, FPGA_CONFIG_BASE, len);
	if (err) {
		shell_error(sh, "flash_read failed (%d)", err);
		return err;
	}

	/* Ensure SDM can see the image even if the region is cached. */
	sys_cache_data_flush_range(FPGA_CONFIG_BASE, len);

	rbf_loaded_len = len;
	crc = crc32_ieee(FPGA_CONFIG_BASE, len);

	shell_print(sh, "QSPI->DDR OK: %zu bytes CRC32=0x%08x  first4=%02x %02x %02x %02x",
		    len, crc, FPGA_CONFIG_BASE[0], FPGA_CONFIG_BASE[1],
		    FPGA_CONFIG_BASE[2], FPGA_CONFIG_BASE[3]);
	return 0;
}

static int rbf_save_qspi(const struct shell *sh)
{
	const struct device *flash_dev = rbf_qspi_dev();
	size_t len = rbf_loaded_len;
	size_t erase_len;
	struct flash_pages_info info;
	int err;

	if (len == 0) {
		shell_error(sh, "nothing in DDR; run 'rbf load' (SD) first");
		return -EINVAL;
	}

	if (len > BITSTREAM_QSPI_MAX) {
		shell_error(sh, "DDR len %zu exceeds QSPI partition %zu", len, BITSTREAM_QSPI_MAX);
		return -EFBIG;
	}

	if (!device_is_ready(flash_dev)) {
		shell_error(sh, "QSPI flash device not ready");
		return -ENODEV;
	}

	err = flash_get_page_info_by_offs(flash_dev, BITSTREAM_QSPI_OFFSET, &info);
	if (err) {
		shell_error(sh, "flash_get_page_info_by_offs failed (%d)", err);
		return err;
	}

	erase_len = ROUND_UP(len, info.size);

	shell_print(sh, "Erasing QSPI 0x%lx len 0x%zx (page %zu)...",
		    (unsigned long)BITSTREAM_QSPI_OFFSET, erase_len, info.size);
	err = flash_erase(flash_dev, BITSTREAM_QSPI_OFFSET, erase_len);
	if (err) {
		shell_error(sh, "flash_erase failed (%d)", err);
		return err;
	}

	shell_print(sh, "Writing %zu bytes DDR 0x%lx -> QSPI 0x%lx ...",
		    len, (unsigned long)FPGA_CONFIG_BASE,
		    (unsigned long)BITSTREAM_QSPI_OFFSET);
	err = flash_write(flash_dev, BITSTREAM_QSPI_OFFSET, FPGA_CONFIG_BASE, len);
	if (err) {
		shell_error(sh, "flash_write failed (%d)", err);
		return err;
	}

	shell_print(sh, "QSPI program done (%zu bytes). Re-read with: rbf load_qspi %zu",
		    len, len);
	return 0;
}
#endif /* bitstream_partition */

static int cmd_rbf_info(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);

	shell_print(sh, "FPGA_CONFIG base: 0x%lx", (unsigned long)FPGA_CONFIG_BASE);
	shell_print(sh, "FPGA_CONFIG size: %zu bytes", FPGA_CONFIG_SIZE);
	shell_print(sh, "Last loaded:      %zu bytes", rbf_loaded_len);
#if DT_NODE_EXISTS(DT_NODELABEL(bitstream_partition))
	shell_print(sh, "QSPI bitstream:   0x%lx size %zu",
		    (unsigned long)BITSTREAM_QSPI_OFFSET, BITSTREAM_QSPI_MAX);
#endif
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

/*
 * CPU-touch every byte then rewrite in place. A/B: if QSPI/Cadence DMA left
 * lines that SDM cannot see even after L3 flush, a CPU bounce may fix load_mem.
 */
static int cmd_rbf_bounce(const struct shell *sh, size_t argc, char **argv)
{
	char *endptr;
	unsigned long len;
	uint8_t *dst = FPGA_CONFIG_BASE;
	uint8_t tmp[RBF_CHUNK_SIZE];
	size_t off = 0;
	uint32_t crc;

	ARG_UNUSED(argc);

	len = strtoul(argv[1], &endptr, 0);
	if (*endptr != '\0' || len == 0UL || len > FPGA_CONFIG_SIZE) {
		shell_error(sh, "Invalid size '%s' (max %zu)", argv[1], FPGA_CONFIG_SIZE);
		return -EINVAL;
	}

	shell_print(sh, "CPU bounce %lu bytes at %p ...", len, dst);
	while (off < (size_t)len) {
		size_t n = MIN(sizeof(tmp), (size_t)len - off);

		memcpy(tmp, dst + off, n);
		memcpy(dst + off, tmp, n);
		off += n;
	}
	sys_cache_data_flush_range(dst, (size_t)len);
	crc = crc32_ieee(dst, (size_t)len);
	rbf_loaded_len = (size_t)len;
	shell_print(sh, "Bounce OK CRC32=0x%08x first4=%02x %02x %02x %02x",
		    crc, dst[0], dst[1], dst[2], dst[3]);
	return 0;
}

#if DT_NODE_EXISTS(DT_NODELABEL(bitstream_partition))
static int cmd_rbf_load_qspi(const struct shell *sh, size_t argc, char **argv)
{
	char *endptr;
	unsigned long len;

	ARG_UNUSED(argc);

	len = strtoul(argv[1], &endptr, 0);
	if (*endptr != '\0' || len == 0UL) {
		shell_error(sh, "Invalid size '%s'", argv[1]);
		return -EINVAL;
	}

	return rbf_load_qspi(sh, (size_t)len);
}

static int cmd_rbf_save_qspi(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);

	return rbf_save_qspi(sh);
}
#endif

SHELL_STATIC_SUBCMD_SET_CREATE(rbf_cmds,
	SHELL_CMD_ARG(info, NULL, "Show FPGA_CONFIG address and last load size",
		      cmd_rbf_info, 1, 0),
	SHELL_CMD_ARG(load, NULL, "Read RBF from SD into FPGA_CONFIG\n"
		      "Usage: rbf load <path>\n"
		      "Example: rbf load /SD:/baseline.core.rbf",
		      cmd_rbf_load, 2, 0),
	SHELL_CMD_ARG(save, NULL, "Write FPGA_CONFIG back to SD\n"
		      "Usage: rbf save <path>\n"
		      "Example: rbf save /SD:/RBFCOPY.RBF",
		      cmd_rbf_save, 2, 0),
	SHELL_CMD_ARG(verify, NULL, "Compare SD file with FPGA_CONFIG DDR\n"
		      "Usage: rbf verify <path>\n"
		      "Example: rbf verify /SD:/baseline.core.rbf",
		      cmd_rbf_verify, 2, 0),
	SHELL_CMD_ARG(bounce, NULL,
		      "CPU read/write rewrite of FPGA_CONFIG (A/B vs Cadence DMA)\n"
		      "Usage: rbf bounce <size>   (after rbf load_qspi / load)",
		      cmd_rbf_bounce, 2, 0),
#if DT_NODE_EXISTS(DT_NODELABEL(bitstream_partition))
	SHELL_CMD_ARG(load_qspi, NULL,
		      "Read RBF from QSPI bitstream partition into FPGA_CONFIG\n"
		      "Usage: rbf load_qspi <size>\n"
		      "Example: rbf load_qspi 2256896",
		      cmd_rbf_load_qspi, 2, 0),
	SHELL_CMD_ARG(save_qspi, NULL,
		      "Program last-loaded DDR RBF into QSPI bitstream partition\n"
		      "Usage: rbf save_qspi   (after rbf load from SD)",
		      cmd_rbf_save_qspi, 1, 0),
#endif
	SHELL_SUBCMD_SET_END
);

SHELL_CMD_REGISTER(rbf, &rbf_cmds, "Copy RBF between SD/QSPI and FPGA_CONFIG DDR", NULL);
