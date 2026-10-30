/*
 * Copyright (c) 2026, Intel Corporation.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#define DT_DRV_COMPAT altr_socfpga_agilex_fpga

#include <errno.h>
#include <zephyr/device.h>
#include <zephyr/logging/log.h>

#include "fpga_altera_soft_ip.h"

LOG_MODULE_REGISTER(fpga_altera_soft_ip, CONFIG_FPGA_LOG_LEVEL);

#define FPGA_SOFT_IP_INIT_BY_IDX(idx, node_id)					\
	do {									\
		const struct device *sip = DEVICE_DT_GET_OR_NULL(		\
			DT_PHANDLE_BY_IDX(node_id, soft_ip, idx));		\
		int sip_ret;							\
										\
		if (sip == NULL) {						\
			break;							\
		}								\
		sip_ret = device_init(sip);					\
		if (sip_ret && sip_ret != -EALREADY) {				\
			LOG_ERR("FPGA soft-IP %s init failed (%d)",		\
				sip->name, sip_ret);				\
		} else if (sip_ret == 0) {					\
			LOG_INF("FPGA soft-IP %s initialized", sip->name);	\
		}								\
	} while (0);

#define FPGA_SOFT_IP_DEINIT_BY_IDX(idx, node_id)				\
	do {									\
		const struct device *sip = DEVICE_DT_GET_OR_NULL(		\
			DT_PHANDLE_BY_IDX(node_id, soft_ip, idx));		\
		int sip_ret;							\
										\
		if (sip == NULL || !device_is_ready(sip)) {			\
			break;							\
		}								\
		sip_ret = device_deinit(sip);					\
		if (sip_ret) {							\
			LOG_ERR("FPGA soft-IP %s deinit failed (%d)",		\
				sip->name, sip_ret);				\
		} else {							\
			LOG_INF("FPGA soft-IP %s deinitialized", sip->name);	\
		}								\
	} while (0);

#define FPGA_SOFT_IP_INIT_INST(inst)						\
	LISTIFY(DT_INST_PROP_LEN_OR(inst, soft_ip, 0),				\
		FPGA_SOFT_IP_INIT_BY_IDX, (), DT_DRV_INST(inst))

#define FPGA_SOFT_IP_DEINIT_INST(inst)						\
	LISTIFY(DT_INST_PROP_LEN_OR(inst, soft_ip, 0),				\
		FPGA_SOFT_IP_DEINIT_BY_IDX, (), DT_DRV_INST(inst))

void altera_fpga_soft_ip_init(void)
{
	DT_INST_FOREACH_STATUS_OKAY(FPGA_SOFT_IP_INIT_INST);
}

void altera_fpga_soft_ip_deinit(void)
{
	DT_INST_FOREACH_STATUS_OKAY(FPGA_SOFT_IP_DEINIT_INST);
}
