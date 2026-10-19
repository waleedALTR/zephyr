/*
 * SPDX-FileCopyrightText: Copyright (c) 2026, Altera Corporation.
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Device ID of Altera SoC FPGAs, read with the Secure Device Manager (SDM)
 * mailbox GET_CHIPID command.
 */

#include <errno.h>
#include <string.h>
#include <zephyr/device.h>
#include <zephyr/drivers/hwinfo.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/sys/util.h>

#include <altera_sdm.h>

#define SDM_CMD_GET_CHIPID 0x12U

#define SDM_CHIPID_WORDS 2U

static const struct device *const sdm = DEVICE_DT_GET_ONE(altr_socfpga_sdm);

ssize_t z_impl_hwinfo_get_device_id(uint8_t *buffer, size_t length)
{
	uint32_t resp[SDM_CHIPID_WORDS];
	size_t resp_len = ARRAY_SIZE(resp);
	uint8_t id[SDM_CHIPID_WORDS * sizeof(uint32_t)];
	int ret;

	ret = altera_sdm_mailbox_send(sdm, SDM_CMD_GET_CHIPID, NULL, 0U, resp, &resp_len);
	if (ret != 0) {
		return ret;
	}

	if (resp_len != SDM_CHIPID_WORDS) {
		return -EIO;
	}

	/* The SDM returns the low word first; present the ID most significant byte first */
	sys_put_be32(resp[1], &id[0]);
	sys_put_be32(resp[0], &id[4]);

	length = MIN(length, sizeof(id));
	memcpy(buffer, id, length);

	return (ssize_t)length;
}
