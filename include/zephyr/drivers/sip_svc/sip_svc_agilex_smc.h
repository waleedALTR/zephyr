/*
 * Copyright (c) 2022-2023, Intel Corporation.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef ZEPHYR_INCLUDE_SIP_SVC_AGILEX_SMC_H_
#define ZEPHYR_INCLUDE_SIP_SVC_AGILEX_SMC_H_

/**
 * @file
 * @brief Intel SoC FPGA Agilex customized Arm SiP Services
 *        SMC protocol.
 */

/* @brief SMC return status
 */

#define SMC_STATUS_INVALID     0xFFFFFFFF
#define SMC_STATUS_OKAY	       0
#define SMC_STATUS_BUSY	       1
#define SMC_STATUS_REJECT      2
#define SMC_STATUS_NO_RESPONSE 3
#define SMC_STATUS_ERROR       4

/* @brief SMC Intel Header at a1
 *
 * bit
 *  7: 0   Transaction ID
 * 59: 8   Reserved
 * 63:60   Version
 */
#define SMC_PLAT_PROTO_VER 0x0

#define SMC_PLAT_PROTO_HEADER_TRANS_ID_OFFSET 0
#define SMC_PLAT_PROTO_HEADER_TRANS_ID_MASK   0xFF

#define SMC_PLAT_PROTO_HEADER_VER_OFFSET 60
#define SMC_PLAT_PROTO_HEADER_VER_MASK	 0xF

#define SMC_PLAT_PROTO_HEADER                                                                      \
	((SMC_PLAT_PROTO_VER & SMC_PLAT_PROTO_HEADER_VER_MASK) << SMC_PLAT_PROTO_HEADER_VER_OFFSET)

#define SMC_PLAT_PROTO_HEADER_SET_TRANS_ID(header, trans_id)                                       \
	(header) &=                                                                                \
		~(SMC_PLAT_PROTO_HEADER_TRANS_ID_MASK << SMC_PLAT_PROTO_HEADER_TRANS_ID_OFFSET);   \
	(header) |= (((trans_id)&SMC_PLAT_PROTO_HEADER_TRANS_ID_MASK)                              \
		     << SMC_PLAT_PROTO_HEADER_TRANS_ID_OFFSET);

/* @brief SYNC SMC Function IDs (SiP SVC V2)
 */

#define SMC_FUNC_ID_GET_SVC_VERSION 0xC2000400
#define SMC_FUNC_ID_REG_READ	    0xC2000401
#define SMC_FUNC_ID_REG_WRITE	    0xC2000402
#define SMC_FUNC_ID_REG_UPDATE	    0xC2000403
#define SMC_FUNC_ID_SET_HPS_BRIDGES 0xC2000404
#define SMC_FUNC_ID_RSU_UPDATE_ADDR 0xC2000405

/*
 * V1 FPGA_CONFIG_START — Agilex5 uses this (like U-Boot) so stock BL31
 * remapper_init() programs AR/AW before Zephyr V2 CANCEL/RECONFIG/DATA.
 * a1=1 matches U-Boot intel_sdm_mb_load().
 */
#define SMC_FUNC_ID_FPGA_CONFIG_START		0xC2000001
#define SMC_FPGA_CONFIG_FLAG_PARTIAL		(1U << 0)

/*
 * Agilex5 SDM BE remapper (ATF INTEL_SIP_SMC_FCS_SDM_REMAPPER_CONFIG).
 * Stock BL31 enable is a no-op; prefer FPGA_CONFIG_START above.
 * a1=0 enable, a1=1 bypass.
 */
#define SMC_FUNC_ID_SDM_REMAPPER_CONFIG		0xC2000201
#define SMC_SDM_REMAPPER_ENABLE			0U
#define SMC_SDM_REMAPPER_BYPASS			1U

/* @brief ASYNC SMC Function IDs (SiP SVC V2 raw mailbox)
 */

#define SMC_FUNC_ID_MAILBOX_SEND_COMMAND  0xC2000420
#define SMC_FUNC_ID_MAILBOX_POLL_RESPONSE 0xC2000421

/* @brief SDM mailbox CANCEL command
 */
#define MAILBOX_CANCEL_COMMAND 0x03

#endif /* ZEPHYR_INCLUDE_SIP_SVC_AGILEX_SMC_H_ */
