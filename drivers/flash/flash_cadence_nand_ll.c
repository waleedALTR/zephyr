/*
 * Copyright (c) 2023, Intel Corporation.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <string.h>
#include <zephyr/cache.h>
#include <zephyr/kernel/mm.h>
#include <zephyr/sys/util.h>
#include "flash_cadence_nand_ll.h"

LOG_MODULE_REGISTER(flash_cdns_nand_ll, CONFIG_FLASH_LOG_LEVEL);

static inline void cdns_nand_dump_regs(mem_addr_t base_address)
{
	uint32_t i, v1, v2, v3, v4;
	mem_addr_t va = base_address;
	uint32_t pa = 0x10b80000;

	for (i = 0; i < 540; i++) {
		v1 = sys_read32(va + 0x0);
		v2 = sys_read32(va + 0x4);
		v3 = sys_read32(va + 0x8);
		v4 = sys_read32(va + 0xc);
		if (v1 || v2 || v3 || v4) {
			printk("%08x: %08x %08x %08x %08x\n", pa, v1, v2, v3, v4);
		}
		va += 16;
		pa += 16;
	}
}

/**
 * Wait for the Cadence NAND controller to become idle.
 *
 * @param base_address The base address of the Cadence NAND controller.
 * @retval  0 on success or -ETIMEDOUT error value on failure.
 */
static inline int32_t cdns_nand_wait_idle(uintptr_t base_address)
{
	/* Wait status command response ready */
	if (!WAIT_FOR(CNF_GET_CTRL_BUSY(sys_read32(CNF_CMDREG(base_address, CTRL_STATUS))) == 0U,
		      IDLE_TIME_OUT, k_msleep(1))) {
		LOG_ERR("Timed out waiting for wait idle response");
		return -ETIMEDOUT;
	}
	return 0;
}

/**
 * Set the row address for a NAND flash memory device using the Cadence NAND controller.
 *
 * @param params The Cadence NAND parameters structure.
 * @param local_row_address The row address.
 * @param page_set The page set number.
 */
static void row_address_set(struct cadence_nand_params *params, uint32_t *local_row_address,
			    uint32_t page_set)
{
	uint32_t block_number = 0;

	block_number = ((page_set) / (params->npages_per_block));
	*local_row_address = 0;
	*local_row_address |= ROW_VAL_SET((params->page_size_bit) - 1, 0,
					  ((page_set) % (params->npages_per_block)));
	*local_row_address |=
		ROW_VAL_SET((params->block_size_bit) - 1, (params->page_size_bit), block_number);
	*local_row_address |= ROW_VAL_SET((params->lun_size_bit) - 1, (params->block_size_bit),
					  (block_number / params->nblocks_per_lun));
}

/**
 * Retrieve information about the NAND flash device using the Cadence NAND controller.
 *
 * @param params The Cadence NAND parameters structure.
 * @retval  0 on success or -ENXIO error value on failure.
 */
static int cdns_nand_device_info(struct cadence_nand_params *params)
{
	struct nf_ctrl_version *nf_ver;
	uintptr_t base_address;
	uint32_t reg_value = 0;
	uint8_t type;

	base_address = params->nand_base;

	/* Read flash device version information */
	reg_value = sys_read32(CNF_CTRLPARAM(base_address, VERSION));
	nf_ver = (struct nf_ctrl_version *)&reg_value;

	LOG_INF("NAND Flash Version Information");
	LOG_INF("HPNFC Magic Number 0x%x", nf_ver->hpnfc_magic_number);
	LOG_INF("Fixed number 0x%x", nf_ver->ctrl_fix);
	LOG_INF("Controller Revision Number 0x%x", nf_ver->ctrl_rev);

	/* Interface Type */
	reg_value = sys_read32(CNF_CTRLPARAM(base_address, DEV_PARAMS0));
	type = CNF_GET_DEV_TYPE(reg_value);
	if (type == CNF_DT_UNKNOWN) {
		LOG_ERR("%s: device type unknown", __func__);
		return -ENXIO;
	}

	params->nluns = CNF_GET_NLUNS(reg_value);
	LOG_INF("Number of LUMs %hhx", params->nluns);

	/* Pages per block */
	reg_value = sys_read32(CNF_CTRLCFG(base_address, DEV_LAYOUT));
	params->npages_per_block = GET_PAGES_PER_BLOCK(reg_value);

	/* Page size and spare size */
	reg_value = sys_read32(CNF_CTRLPARAM(base_address, DEV_AREA));
	params->page_size = GET_PAGE_SIZE(reg_value);
	params->spare_size = GET_SPARE_SIZE(reg_value);

	/* Device blocks per LUN */
	params->nblocks_per_lun = sys_read32(CNF_CTRLPARAM(base_address, DEV_BLOCKS_PLUN));

	/* Calculate block size and total device size */
	params->block_size = (params->npages_per_block * params->page_size);
	params->device_size = ((long long)params->block_size *
			       (long long)(params->nblocks_per_lun * params->nluns));
	LOG_INF("block size %uK total device size %lluM", params->block_size >> 10, params->device_size >> 20);

	/* Calculate bit size of page, block and lun*/
	params->page_size_bit = find_msb_set((params->npages_per_block) - 1);
	params->block_size_bit = find_msb_set((params->nblocks_per_lun) - 1);
	params->lun_size_bit = find_msb_set((params->nluns) - 1);
	return 0;
}

#if 0
/**
 * Retrieve the status of a specific thread in the Cadence NAND controller.
 *
 * @param base_address The base address of the Cadence NAND controller.
 * @param thread The thread identifier.
 * @retval  The status of the thread.
 */
static uint32_t cdns_nand_get_thrd_status(uintptr_t base_address, uint8_t thread)
{
	uint32_t status;

	sys_write32(THREAD_VAL(thread), (base_address + CMD_STATUS_PTR_ADDR));
	status = sys_read32((base_address + CMD_STAT_CMD_STATUS));
	return status;
}
#endif

/**
 * Wait for a specific thread in the Cadence controller to complete.
 *
 * @param base_address The base address of the Cadence controller.
 * @param thread The thread identifier to wait for.
 * @retval  0 on success or -ETIMEDOUT error value on failure.
 */
static int cdns_wait_for_thread(uintptr_t base_address, uint8_t thread)
{

	if (!WAIT_FOR((sys_read32((base_address) + THR_STATUS) & BIT(thread)) == 0U,
		      THREAD_IDLE_TIME_OUT, k_msleep(1))) {
		LOG_ERR("Timed out waiting for thread response");
		return -ETIMEDOUT;
	}

	return 0;
}

#if 0
/**
 * Set features in the Cadence NAND controller using PIO operations.
 *
 * @param base_address The base address of the Cadence NAND controller.
 * @param feat_addr The address of the feature to be set.
 * @param feat_val The value of the feature to be set.
 * @param thread The thread identifier for the PIO operation.
 * @param vol_id The volume identifier for the feature set operation.
 * @param use_intr Flag indicating whether to use interrupts during the operation.
 * @retval  0 on success or -ETIMEDOUT error value on failure.
 */
static int cdns_nand_pio_set_features(uintptr_t base_address, uint8_t feat_addr, uint8_t feat_val,
				      uint8_t thread, uint8_t vol_id)
{
	uint32_t status = 0;
	int ret = 0;

	ret = cdns_wait_for_thread(base_address, thread);

	if (ret != 0) {
		return ret;
	}

	sys_write32(SET_FEAT_ADDR(feat_addr), (base_address + CDNS_CMD_REG1));
	sys_write32(feat_val, (base_address + CDNS_CMD_REG2));
	status = CMD_0_THREAD_POS_SET(thread);
	status |= CMD_0_C_MODE_SET(CT_PIO_MODE);
	status |= PIO_CMD0_CT_SET(PIO_SET_FEA_MODE);
	status |= CMD_0_VOL_ID_SET(vol_id);
	sys_write32(status, (base_address + CDNS_CMD_REG0));
	return 0;
}

/**
 * Check whether a transfer complete for PIO operation in the Cadence controller has finished.
 *
 * @param base_address The base address of the Cadence controller.
 * @param thread The thread identifier for the PIO operation.
 * @retval  0 on success or negative error value on failure.
 */
static int cdns_pio_transfer_complete(uintptr_t base_address, uint8_t thread)
{
	uint32_t status;

	status = WAIT_FOR(((cdns_nand_get_thrd_status(base_address, thread)) != 0), IDLE_TIME_OUT,
			  k_msleep(1));

	if (status == 0) {
		LOG_ERR("Timed out waiting for thread status response");
		return -ETIMEDOUT;
	}

	if ((status & (BIT(F_CSTAT_COMP)))) {
		if ((status & (BIT(F_CSTAT_FAIL)))) {
			LOG_ERR("Cadence status operation failed %s", __func__);
			return -EIO;
		}
	} else {
		LOG_ERR("Cadence status complete failed %s", __func__);
		return -EIO;
	}
	return 0;
}
#endif

/*
 * ONFI SDR timing modes 0–5 (picoseconds). Fields used by
 * cadence_setup_data_interface only; from U-Boot nand_timings.c.
 */
struct cdns_nand_sdr_timings {
	uint32_t tCCS_min;
	uint32_t tADL_min;
	uint32_t tALS_min;
	uint32_t tCEH_min;
	uint32_t tCS_min;
	uint32_t tFEAT_max;
	uint32_t tRC_min;
	uint32_t tREA_max;
	uint32_t tREH_min;
	uint32_t tRHOH_min;
	uint32_t tRHW_min;
	uint32_t tRHZ_max;
	uint32_t tRP_min;
	uint32_t tWB_max;
	uint32_t tWC_min;
	uint32_t tWH_min;
	uint32_t tWHR_min;
	uint32_t tWP_min;
};

static const struct cdns_nand_sdr_timings onfi_sdr_timings[] = {
	{ /* mode 0 */
		.tCCS_min = 500000, .tADL_min = 400000, .tALS_min = 50000,
		.tCEH_min = 20000, .tCS_min = 70000, .tFEAT_max = 1000000,
		.tRC_min = 100000, .tREA_max = 40000, .tREH_min = 30000,
		.tRHOH_min = 0, .tRHW_min = 200000, .tRHZ_max = 200000,
		.tRP_min = 50000, .tWB_max = 200000, .tWC_min = 100000,
		.tWH_min = 30000, .tWHR_min = 120000, .tWP_min = 50000,
	},
	{ /* mode 1 */
		.tCCS_min = 500000, .tADL_min = 400000, .tALS_min = 25000,
		.tCEH_min = 20000, .tCS_min = 35000, .tFEAT_max = 1000000,
		.tRC_min = 50000, .tREA_max = 30000, .tREH_min = 15000,
		.tRHOH_min = 15000, .tRHW_min = 100000, .tRHZ_max = 100000,
		.tRP_min = 25000, .tWB_max = 100000, .tWC_min = 45000,
		.tWH_min = 15000, .tWHR_min = 80000, .tWP_min = 25000,
	},
	{ /* mode 2 */
		.tCCS_min = 500000, .tADL_min = 400000, .tALS_min = 15000,
		.tCEH_min = 20000, .tCS_min = 25000, .tFEAT_max = 1000000,
		.tRC_min = 35000, .tREA_max = 25000, .tREH_min = 15000,
		.tRHOH_min = 15000, .tRHW_min = 100000, .tRHZ_max = 100000,
		.tRP_min = 17000, .tWB_max = 100000, .tWC_min = 35000,
		.tWH_min = 15000, .tWHR_min = 80000, .tWP_min = 17000,
	},
	{ /* mode 3 */
		.tCCS_min = 500000, .tADL_min = 400000, .tALS_min = 10000,
		.tCEH_min = 20000, .tCS_min = 25000, .tFEAT_max = 1000000,
		.tRC_min = 30000, .tREA_max = 20000, .tREH_min = 10000,
		.tRHOH_min = 15000, .tRHW_min = 100000, .tRHZ_max = 100000,
		.tRP_min = 15000, .tWB_max = 100000, .tWC_min = 30000,
		.tWH_min = 10000, .tWHR_min = 80000, .tWP_min = 15000,
	},
	{ /* mode 4 */
		.tCCS_min = 500000, .tADL_min = 400000, .tALS_min = 10000,
		.tCEH_min = 20000, .tCS_min = 20000, .tFEAT_max = 1000000,
		.tRC_min = 25000, .tREA_max = 20000, .tREH_min = 10000,
		.tRHOH_min = 15000, .tRHW_min = 100000, .tRHZ_max = 100000,
		.tRP_min = 12000, .tWB_max = 100000, .tWC_min = 25000,
		.tWH_min = 10000, .tWHR_min = 80000, .tWP_min = 12000,
	},
	{ /* mode 5 */
		.tCCS_min = 500000, .tADL_min = 400000, .tALS_min = 10000,
		.tCEH_min = 20000, .tCS_min = 15000, .tFEAT_max = 1000000,
		.tRC_min = 20000, .tREA_max = 16000, .tREH_min = 7000,
		.tRHOH_min = 15000, .tRHW_min = 100000, .tRHZ_max = 100000,
		.tRP_min = 10000, .tWB_max = 100000, .tWC_min = 20000,
		.tWH_min = 7000, .tWHR_min = 80000, .tWP_min = 10000,
	},
};

struct cadence_nand_timings {
	uint32_t async_toggle_timings;
	uint32_t timings0;
	uint32_t timings1;
	uint32_t timings2;
	uint32_t dll_phy_ctrl;
	uint32_t phy_ctrl;
	uint32_t phy_dqs_timing;
	uint32_t phy_gate_lpbk_ctrl;
};

static int calc_cycl(uint32_t timing, uint32_t clock)
{
	if ((timing == 0U) || (clock == 0U)) {
		return 0;
	}

	if ((timing % clock) > 0U) {
		return (int)(timing / clock);
	}

	return (int)(timing / clock) - 1;
}

static inline uint32_t calc_tdvw_max(uint32_t trp_cnt, uint32_t clk_period,
				     uint32_t trhoh_min, uint32_t board_delay_skew_min,
				     uint32_t ext_mode)
{
	if (ext_mode == 0U) {
		clk_period /= 2U;
	}

	return (trp_cnt + 1U) * clk_period + trhoh_min + board_delay_skew_min;
}

static inline uint32_t calc_tdvw(uint32_t trp_cnt, uint32_t clk_period,
				 uint32_t trhoh_min, uint32_t trea_max, uint32_t ext_mode)
{
	if (ext_mode == 0U) {
		clk_period /= 2U;
	}

	return (trp_cnt + 1U) * clk_period + trhoh_min - trea_max;
}

static inline bool cdns_field_fit(uint32_t mask, uint32_t val)
{
	uint32_t max = mask >> __builtin_ctz(mask);

	return val <= max;
}

/**
 * Port of U-Boot cadence_setup_data_interface() for ONFI SDR timing @mode.
 */
static int cdns_nand_setup_data_interface(struct cadence_nand_params *params,
					  int mode, bool is_phy_type_dll,
					  struct cadence_nand_timings *t)
{
	const struct cdns_nand_sdr_timings *sdr;
	uint32_t board_delay = params->board_delay_ps;
	uint32_t nf_clk = params->nf_clk_rate;
	uint32_t clk_period;
	uint32_t if_skew = 0U;
	uint32_t board_delay_skew_min;
	uint32_t board_delay_skew_max;
	uint32_t tceh_cnt, tcs_cnt, tadl_cnt, tccs_cnt;
	uint32_t tfeat_cnt, trhz_cnt, tvdly_cnt;
	uint32_t trhw_cnt, twb_cnt, twh_cnt = 0U, twhr_cnt;
	uint32_t twp_cnt = 0U, trp_cnt = 0U, trh_cnt = 0U;
	uint32_t dqs_sampl_res, phony_dqs_mod;
	uint32_t tdvw, tdvw_min, tdvw_max;
	uint32_t ext_rd_mode, ext_wr_mode;
	uint32_t dll_phy_dqs_timing = 0U, phony_dqs_timing = 0U, rd_del_sel = 0U;
	uint32_t sampling_point;
	uint32_t reg;

	if ((mode < 0) || (mode >= (int)ARRAY_SIZE(onfi_sdr_timings))) {
		return -EINVAL;
	}

	if (nf_clk == 0U) {
		nf_clk = CNF_DEFAULT_NF_CLK_RATE_HZ;
	}
	if (board_delay == 0U) {
		board_delay = CNF_DEFAULT_BOARD_DELAY_PS;
	}

	sdr = &onfi_sdr_timings[mode];
	clk_period = (uint32_t)(1000000000000ULL / nf_clk);
	board_delay_skew_min = board_delay - if_skew;
	board_delay_skew_max = board_delay + if_skew;

	(void)memset(t, 0, sizeof(*t));

	phony_dqs_mod = is_phy_type_dll ? 2U : 1U;
	dqs_sampl_res = clk_period / phony_dqs_mod;
	tdvw_min = sdr->tREA_max + board_delay_skew_max;

	if ((sdr->tRC_min <= clk_period) &&
	    (sdr->tRP_min <= (clk_period / 2U)) &&
	    (sdr->tREH_min <= (clk_period / 2U))) {
		ext_rd_mode = 0U;
		tdvw = calc_tdvw(trp_cnt, clk_period, sdr->tRHOH_min,
				 sdr->tREA_max, ext_rd_mode);
		tdvw_max = calc_tdvw_max(trp_cnt, clk_period, sdr->tRHOH_min,
					 board_delay_skew_min, ext_rd_mode);
		if (tdvw > 0U) {
			if ((tdvw_max <= tdvw_min) || ((tdvw_max % dqs_sampl_res) == 0U)) {
				ext_rd_mode = 1U;
			}
		} else {
			trp_cnt = (sdr->tREA_max + board_delay_skew_max + dqs_sampl_res) /
				  clk_period;
			ext_rd_mode = 1U;
		}
	} else {
		uint32_t trh;

		ext_rd_mode = 1U;
		trp_cnt = (uint32_t)calc_cycl(sdr->tRP_min, clk_period);
		trh = sdr->tRC_min - ((trp_cnt + 1U) * clk_period);
		if (sdr->tREH_min >= trh) {
			trh_cnt = (uint32_t)calc_cycl(sdr->tREH_min, clk_period);
		} else {
			trh_cnt = (uint32_t)calc_cycl(trh, clk_period);
		}

		tdvw = calc_tdvw(trp_cnt, clk_period, sdr->tRHOH_min,
				 sdr->tREA_max, ext_rd_mode);
		if (tdvw > 0U) {
			tdvw_max = calc_tdvw_max(trp_cnt, clk_period, sdr->tRHOH_min,
						 board_delay_skew_min, ext_rd_mode);
			if ((((tdvw_max / dqs_sampl_res) * dqs_sampl_res) <= tdvw_min) ||
			    (((tdvw_max % dqs_sampl_res) == 0U) &&
			     ((((tdvw_max / dqs_sampl_res) - 1U) * dqs_sampl_res) <= tdvw_min))) {
				trp_cnt = trp_cnt + 1U;
			}
		} else {
			trp_cnt = (sdr->tREA_max + board_delay_skew_max + dqs_sampl_res) /
				  clk_period;
		}
	}

	tdvw_max = calc_tdvw_max(trp_cnt, clk_period, sdr->tRHOH_min,
				 board_delay_skew_min, ext_rd_mode);

	if ((sdr->tWC_min <= clk_period) &&
	    ((sdr->tWP_min + if_skew) <= (clk_period / 2U)) &&
	    ((sdr->tWH_min + if_skew) <= (clk_period / 2U))) {
		ext_wr_mode = 0U;
	} else {
		uint32_t twh;

		ext_wr_mode = 1U;
		twp_cnt = (uint32_t)calc_cycl(sdr->tWP_min + if_skew, clk_period);
		if (((twp_cnt + 1U) * clk_period) < (sdr->tALS_min + if_skew)) {
			twp_cnt = (uint32_t)calc_cycl(sdr->tALS_min + if_skew, clk_period);
		}

		twh = sdr->tWC_min - (twp_cnt + 1U) * clk_period;
		if (sdr->tWH_min >= twh) {
			twh = sdr->tWH_min;
		}
		twh_cnt = (uint32_t)calc_cycl(twh + if_skew, clk_period);
	}

	reg = FIELD_PREP(CNF_ASYNC_TOGGLE_TRH, trh_cnt) |
	      FIELD_PREP(CNF_ASYNC_TOGGLE_TRP, trp_cnt) |
	      FIELD_PREP(CNF_ASYNC_TOGGLE_TWH, twh_cnt) |
	      FIELD_PREP(CNF_ASYNC_TOGGLE_TWP, twp_cnt);
	t->async_toggle_timings = reg;

	tadl_cnt = (uint32_t)calc_cycl(sdr->tADL_min + if_skew, clk_period);
	tccs_cnt = (uint32_t)calc_cycl(sdr->tCCS_min + if_skew, clk_period);
	twhr_cnt = (uint32_t)calc_cycl(sdr->tWHR_min + if_skew, clk_period);
	trhw_cnt = (uint32_t)calc_cycl(sdr->tRHW_min + if_skew, clk_period);
	reg = FIELD_PREP(CNF_TIMINGS0_TADL, tadl_cnt);
	if (cdns_field_fit(CNF_TIMINGS0_TCCS, tccs_cnt)) {
		reg |= FIELD_PREP(CNF_TIMINGS0_TCCS, tccs_cnt);
	} else {
		reg |= CNF_TIMINGS0_TCCS;
	}
	reg |= FIELD_PREP(CNF_TIMINGS0_TWHR, twhr_cnt) |
	       FIELD_PREP(CNF_TIMINGS0_TRHW, trhw_cnt);
	t->timings0 = reg;

	trhz_cnt = (uint32_t)calc_cycl(sdr->tRHZ_max, clk_period) + 1U;
	twb_cnt = (uint32_t)calc_cycl(sdr->tWB_max + board_delay, clk_period) + 3U + 5U;
	tvdly_cnt = (uint32_t)calc_cycl(500000U + if_skew, clk_period);
	t->timings1 = FIELD_PREP(CNF_TIMINGS1_TRHZ, trhz_cnt) |
		      FIELD_PREP(CNF_TIMINGS1_TWB, twb_cnt) |
		      FIELD_PREP(CNF_TIMINGS1_TVDLY, tvdly_cnt);

	tfeat_cnt = (uint32_t)calc_cycl(sdr->tFEAT_max, clk_period);
	if (tfeat_cnt < twb_cnt) {
		tfeat_cnt = twb_cnt;
	}
	tceh_cnt = (uint32_t)calc_cycl(sdr->tCEH_min, clk_period);
	tcs_cnt = (uint32_t)calc_cycl(sdr->tCS_min + if_skew, clk_period);
	t->timings2 = FIELD_PREP(CNF_TIMINGS2_TFEAT, tfeat_cnt) |
		      FIELD_PREP(CNF_TIMINGS2_CS_HOLD, tceh_cnt) |
		      FIELD_PREP(CNF_TIMINGS2_CS_SETUP, tcs_cnt);

	if (is_phy_type_dll) {
		reg = CNF_DLL_PHY_CTRL_DLL_RST_N;
		if (ext_wr_mode != 0U) {
			reg |= CNF_DLL_PHY_CTRL_EXT_WR;
		}
		if (ext_rd_mode != 0U) {
			reg |= CNF_DLL_PHY_CTRL_EXT_RD;
		}
		reg |= FIELD_PREP(CNF_DLL_PHY_CTRL_RS_HIGH_WAIT, 7U) |
		       FIELD_PREP(CNF_DLL_PHY_CTRL_RS_IDLE, 7U);
		t->dll_phy_ctrl = reg;
	}

	if ((tdvw_max % dqs_sampl_res) > 0U) {
		sampling_point = tdvw_max / dqs_sampl_res;
	} else {
		sampling_point = (tdvw_max / dqs_sampl_res) - 1U;
	}

	if ((sampling_point * dqs_sampl_res) > tdvw_min) {
		dll_phy_dqs_timing = FIELD_PREP(CNF_PHY_DQS_SEL_OE_END, 4U) |
				    CNF_PHY_DQS_USE_PHONY_DQS;
		phony_dqs_timing = sampling_point / phony_dqs_mod;

		if ((sampling_point % 2U) > 0U) {
			dll_phy_dqs_timing |= CNF_PHY_DQS_PHONY_DQS_SEL;
			if ((tdvw_max % dqs_sampl_res) == 0U) {
				phony_dqs_timing = (tdvw_max / dqs_sampl_res) /
						   phony_dqs_mod - 1U;
			}
			if (!is_phy_type_dll) {
				phony_dqs_timing--;
			}
		} else {
			phony_dqs_timing--;
		}
		rd_del_sel = phony_dqs_timing + 3U;
	} else {
		LOG_WRN("No valid sampling point for ONFI SDR mode %d", mode);
	}

	reg = FIELD_PREP(CNF_PHY_CTRL_PHONY_DQS, phony_dqs_timing);
	if (is_phy_type_dll) {
		reg |= CNF_PHY_CTRL_SDR_DQS;
	}
	t->phy_ctrl = reg;

	if (is_phy_type_dll) {
		t->phy_dqs_timing = dll_phy_dqs_timing;
		t->phy_gate_lpbk_ctrl = FIELD_PREP(CNF_PHY_GATE_LPBK_RDS, rd_del_sel);
	}

	LOG_INF("setup_data_interface mode=%d clk=%uHz period=%ups async=0x%x t0=0x%x t1=0x%x t2=0x%x",
		mode, nf_clk, clk_period, t->async_toggle_timings, t->timings0,
		t->timings1, t->timings2);

	return 0;
}

static void cdns_nand_set_timings(struct cadence_nand_params *params,
				  const struct cadence_nand_timings *t,
				  bool is_phy_type_dll)
{
	uintptr_t base_address = params->nand_base;

	sys_write32(t->async_toggle_timings,
		    CNF_MINICTRL(base_address, ASYNC_TOGGLE_TIMINGS));
	sys_write32(t->timings0, CNF_MINICTRL(base_address, TIMINGS0));
	sys_write32(t->timings1, CNF_MINICTRL(base_address, TIMINGS1));
	sys_write32(t->timings2, CNF_MINICTRL(base_address, TIMINGS2));

	if (is_phy_type_dll) {
		sys_write32(t->dll_phy_ctrl, CNF_MINICTRL(base_address, DLL_PHY_CTRL));
	}

	sys_write32(t->phy_ctrl, base_address + PHY_CTRL_REG_OFFSET);

	if (is_phy_type_dll) {
		sys_write32(0U, base_address + PHY_TSEL_REG_OFFSET);
		sys_write32(2U, base_address + PHY_DQ_TIMING_REG_OFFSET);
		sys_write32(t->phy_dqs_timing, base_address + PHY_DQS_TIMING_REG_OFFSET);
		sys_write32(t->phy_gate_lpbk_ctrl, base_address + PHY_GATE_LPBK_OFFSET);
		sys_write32(CNF_PHY_DLL_MASTER_BYPASS, base_address + PHY_DLL_MASTER_OFFSET);
		sys_write32(0U, base_address + PHY_DLL_SLAVE_OFFSET);
	}
}

/**
 * Set the operational mode for the Cadence NAND controller.
 *
 * @param params Cadence NAND parameters (base, clock, board delay).
 * @param opr_mode The operational mode SDR / NVDDR to set.
 * @retval  0 on success or negative error value on failure.
 */
static int cdns_nand_set_opr_mode(struct cadence_nand_params *params, uint8_t opr_mode)
{
	uintptr_t base_address = params->nand_base;
	struct cadence_nand_timings timings;
	uint8_t device_type;
	uint32_t timing_mode = 0;
	uint32_t status;
	uint32_t features;
	bool is_phy_type_dll;
	int ret;

	features = sys_read32(CNF_CTRLPARAM(base_address, FEATURE));
	is_phy_type_dll = CNF_HW_NVDDR_SUPPORT(features) != 0U;

	if (opr_mode == CNF_OPR_WORK_MODE_SDR) {
		status = ONFI_TIMING_MODE_SDR(
			sys_read32(CNF_CTRLPARAM(base_address, ONFI_TIMING_0)));
		/* Highest supported ONFI mode (U-Boot / Linux prefer fastest). */
		timing_mode = find_msb_set(status);
		if (timing_mode == 0U) {
			timing_mode = 0U;
		} else {
			timing_mode -= 1U;
		}

		ret = cdns_nand_setup_data_interface(params, (int)timing_mode,
						     is_phy_type_dll, &timings);
		if (ret != 0) {
			LOG_ERR("setup_data_interface failed (%d), mode %u",
				ret, timing_mode);
			return ret;
		}
		cdns_nand_set_timings(params, &timings, is_phy_type_dll);

		sys_clear_bits(CNF_MINICTRL(base_address, CMN_SETTINGS),
			       CNF_OPR_WORK_MODE_SDR_MASK);
	} else {
		/* NVDDR MODE — keep legacy PHY constants for now */
		status = ONFI_TIMING_MODE_NVDDR(
			sys_read32(CNF_CTRLPARAM(base_address, ONFI_TIMING_0)));
		timing_mode = find_msb_set(status);
		if (timing_mode != 0U) {
			timing_mode -= 1U;
		}

		sys_write32(PHY_CTRL_REG_DDR, (base_address + PHY_CTRL_REG_OFFSET));
		sys_write32(PHY_TSEL_REG_DDR, (base_address + PHY_TSEL_REG_OFFSET));
		sys_write32(PHY_DQ_TIMING_REG_DDR, (base_address + PHY_DQ_TIMING_REG_OFFSET));
		sys_write32(PHY_DQS_TIMING_REG_DDR, (base_address + PHY_DQS_TIMING_REG_OFFSET));
		sys_write32(PHY_GATE_LPBK_CTRL_REG_DDR, (base_address + PHY_GATE_LPBK_OFFSET));
		sys_write32(PHY_DLL_MASTER_CTRL_REG_DDR, (base_address + PHY_DLL_MASTER_OFFSET));
		sys_set_bits(CNF_MINICTRL(base_address, CMN_SETTINGS),
			     CNF_OPR_WORK_MODE_NVDDR_MASK);
	}

	ARG_UNUSED(timing_mode);

	ret = cdns_nand_wait_idle(base_address);
	if (ret != 0) {
		LOG_ERR("Wait for controller to be in idle state Failed");
		return ret;
	}

	device_type = CNF_GET_DEV_TYPE(sys_read32(CNF_CTRLPARAM(base_address, DEV_PARAMS0)));
	if (device_type != ONFI_INTERFACE) {
		LOG_ERR("Driver does not support this interface");
		return -ENOTSUP;
	}

	sys_clear_bit(CNF_MINICTRL(base_address, DLL_PHY_CTRL), CNF_DLL_PHY_RST_N);

	ret = cdns_nand_wait_idle(base_address);
	if (ret != 0) {
		LOG_ERR("Wait for controller to be in idle state Failed");
		return ret;
	}

	sys_set_bit(CNF_MINICTRL(base_address, DLL_PHY_CTRL), CNF_DLL_PHY_RST_N);

	ret = cdns_nand_wait_idle(base_address);
	if (ret != 0) {
		LOG_ERR("Wait for controller to be in idle state Failed");
		return ret;
	}

	return 0;
}

/**
 * ECC bytes per sector — same formula as U-Boot cadence_nand_calc_ecc_bytes().
 * Depends on the controller's maximum BCH step size and the chosen strength.
 */
static uint32_t cdns_nand_calc_ecc_bytes(uint32_t max_step_size, uint32_t strength)
{
	uint32_t nbytes;

	if ((max_step_size == 0U) || (strength == 0U)) {
		return 0U;
	}

	nbytes = DIV_ROUND_UP(find_msb_set(8U * max_step_size) * strength, 8U);
	return ROUND_UP(nbytes, 2U);
}

/**
 * available_spare = spare_size - ecc_size, capped by BCH metadata size
 * (U-Boot cadence_nand attach path). Used as OOB added to last_sector_size.
 */
static uint32_t cdns_nand_avail_spare(struct cadence_nand_params *params,
				      uint32_t sector_cnt)
{
	uintptr_t base_address = params->nand_base;
	uint32_t strengths[CNF_BCH_MAX_CORR_CAPS];
	uint32_t max_step_size;
	uint32_t strength;
	uint32_t ecc_bytes;
	uint32_t ecc_size;
	uint32_t avail;
	uint32_t metadata;
	uint32_t reg;
	uint32_t corr_idx = CNF_ECC_CORR_STR;

	reg = sys_read32(CNF_CTRLPARAM(base_address, BCH_CFG0));
	strengths[0] = FIELD_GET(CNF_BCH_CFG0_CORR_CAP_0, reg);
	strengths[1] = FIELD_GET(CNF_BCH_CFG0_CORR_CAP_1, reg);
	strengths[2] = FIELD_GET(CNF_BCH_CFG0_CORR_CAP_2, reg);
	strengths[3] = FIELD_GET(CNF_BCH_CFG0_CORR_CAP_3, reg);

	reg = sys_read32(CNF_CTRLPARAM(base_address, BCH_CFG1));
	strengths[4] = FIELD_GET(CNF_BCH_CFG1_CORR_CAP_4, reg);
	strengths[5] = FIELD_GET(CNF_BCH_CFG1_CORR_CAP_5, reg);
	strengths[6] = FIELD_GET(CNF_BCH_CFG1_CORR_CAP_6, reg);
	strengths[7] = FIELD_GET(CNF_BCH_CFG1_CORR_CAP_7, reg);

	reg = sys_read32(CNF_CTRLPARAM(base_address, BCH_CFG2));
	max_step_size = FIELD_GET(CNF_BCH_CFG2_SECT_0, reg);
	reg = FIELD_GET(CNF_BCH_CFG2_SECT_1, reg);
	if (reg > max_step_size) {
		max_step_size = reg;
	}

	reg = sys_read32(CNF_CTRLPARAM(base_address, BCH_CFG3));
	metadata = FIELD_GET(CNF_BCH_CFG3_METADATA_SIZE, reg);

	if ((corr_idx >= CNF_BCH_MAX_CORR_CAPS) || (strengths[corr_idx] == 0U) ||
	    (max_step_size == 0U) || (sector_cnt == 0U) ||
	    (params->spare_size == 0U)) {
		return 0U;
	}

	strength = strengths[corr_idx];
	ecc_bytes = cdns_nand_calc_ecc_bytes(max_step_size, strength);
	if (ecc_bytes == 0U) {
		return 0U;
	}

	ecc_size = sector_cnt * ecc_bytes;
	if (ecc_size >= params->spare_size) {
		LOG_ERR("ECC size %u exceeds spare %u (str=%u step_max=%u)",
			ecc_size, params->spare_size, strength, max_step_size);
		return 0U;
	}

	avail = params->spare_size - ecc_size;
	if ((metadata != 0U) && (avail > metadata)) {
		avail = metadata;
	}

	/* Same BBM reservation check as U-Boot (bbm_len == skip marker bytes). */
	if ((avail + CNF_NSKIP_BYTES + ecc_size) > params->spare_size) {
		if (avail > 4U) {
			avail -= 4U;
		}
	}

	LOG_INF("BCH strength=%u max_step=%u ecc/sec=%u ecc_total=%u meta=%u avail_oob=%u",
		strength, max_step_size, ecc_bytes, ecc_size, metadata, avail);

	return avail;
}

/**
 * Configure the transfer settings of the Cadence NAND controller.
 *
 * Packs TRANS_CFG0/1 like ATF. last_sector_size uses U-Boot's
 * available_spare = spare_size - ecc_size (capped by BCH metadata).
 *
 * @param params The Cadence NAND parameters structure.
 * @retval  0 on success or -ETIMEDOUT error value on failure.
 */
static int cdns_nand_transfer_config(struct cadence_nand_params *params)
{
	uintptr_t base_address = params->nand_base;
	uint32_t sector_cnt = CNF_SECTOR_CNT;
	uint32_t sector_size = CNF_SECTOR_SIZE;
	uint32_t last_sector_size = CNF_LAST_SECTOR_SIZE;
	uint32_t skip_offset = CNF_SKIP_BYTES_OFFSET_VAL;
	uint32_t avail_oob;
	uint32_t trans_cfg0;
	uint32_t trans_cfg1;
	int ret;

	ret = cdns_nand_wait_idle(base_address);
	if (ret != 0) {
		LOG_ERR("Wait for controller to be in idle state Failed");
		return ret;
	}

	/*
	 * U-Boot TT_MAIN_OOB_AREAS:
	 *   ecc_size         = sector_cnt * calc_ecc_bytes(max_step, strength)
	 *   available_spare  = min(spare_size - ecc_size, bch_metadata_size)
	 *   last_sector_size = sector_size + available_spare
	 */
	if ((params->page_size != 0U) &&
	    ((params->page_size % CNF_SECTOR_SIZE) == 0U)) {
		sector_size = CNF_SECTOR_SIZE;
		sector_cnt = params->page_size / sector_size;
		avail_oob = cdns_nand_avail_spare(params, sector_cnt);
		if (avail_oob != 0U) {
			last_sector_size = sector_size + avail_oob;
		}
		skip_offset = params->page_size;
	}

	trans_cfg0 = sector_cnt |
		     FIELD_PREP(CNF_SECTOR_OFFSET_MASK, CNF_SECTOR_OFFSET);
	trans_cfg1 = sector_size |
		     FIELD_PREP(CNF_LAST_SECTOR_MASK, last_sector_size);

	sys_write32(trans_cfg0, CNF_CTRLCFG(base_address, TRANS_CFG0));
	sys_write32(trans_cfg1, CNF_CTRLCFG(base_address, TRANS_CFG1));

	/* Disable cache and multiplane. */
	sys_write32(DISABLE, CNF_CTRLCFG(base_address, MULTIPLANE_CFG));
	sys_write32(DISABLE, CNF_CTRLCFG(base_address, CACHE_CFG));

	/* ECC / skip — same packing as ATF CNF_ECC_CFG0_VAL / SKIP_BYTES_* */
	sys_write32(CNF_ECC_CFG0_VAL, CNF_CTRLCFG(base_address, ECC_CFG0));
	sys_write32(CNF_ECC_CFG1_VAL, CNF_CTRLCFG(base_address, ECC_CFG1));
	sys_write32(CNF_SKIP_BYTES_CFG_VAL, CNF_MINICTRL(base_address, SKIP_BYTES_CFG));
	sys_write32(skip_offset, CNF_MINICTRL(base_address, SKIP_BYTES_OFFSET));

	/* TIMINGS and PHY programmed by cdns_nand_setup_data_interface() */

	LOG_INF("TRANS_CFG0=0x%x TRANS_CFG1=0x%x (sec=%u cnt=%u last=%u)",
		trans_cfg0, trans_cfg1, sector_size, sector_cnt, last_sector_size);

	sys_write32(CLEAR_ALL_INTERRUPT, (base_address + INTR_STATUS));
	return 0;
}

/**
 * Initialize the Cadence NAND controller.
 *
 * @param params The Cadence NAND parameters structure.
 * @retval  0 on success or negative error value on failure.
 */
int cdns_nand_init(struct cadence_nand_params *params)
{
	uintptr_t base_address = params->nand_base;
	uint8_t datarate_mode = params->datarate_mode;
	int ret;

	if (!WAIT_FOR(CNF_GET_INIT_COMP(sys_read32(CNF_CMDREG(base_address, CTRL_STATUS))) != 0U,
		      IDLE_TIME_OUT, k_msleep(1))) {
		LOG_ERR("Timed out waiting for NAND Controller Init complete status response");
		return -ETIMEDOUT;
	}

	if (CNF_GET_INIT_FAIL(sys_read32(CNF_CMDREG(base_address, CTRL_STATUS))) != 0) {
		LOG_ERR("NAND Controller Init complete Failed!!!");
		return -ENODEV;
	}

	ret = cdns_nand_device_info(params);
	if (ret != 0) {
		return ret;
	}

#if 0
	/* Hardware Support Features */
	reg_value_read = sys_read32(CNF_CTRLPARAM(base_address, FEATURE));
	/* Enable data integrity parity check if the data integrity parity mechanism is */
	/* supported by the device */
	if (CNF_HW_DI_PR_SUPPORT(reg_value_read) != 0) {
		sys_set_bit(CNF_DI(base_address, CONTROL), CNF_DI_PAR_EN);
	}

	/* Enable data integrity CRC check if the data integrity CRC mechanism is */
	/* supported by the device */
	if (CNF_HW_DI_CRC_SUPPORT(reg_value_read) != 0) {
		sys_set_bit(CNF_DI(base_address, CONTROL), CNF_DI_CRC_EN);
	}

	/* Status polling mode, device control and status register */
	ret = cdns_nand_wait_idle(base_address);

	if (ret != 0) {
		LOG_ERR("Wait for controller to be in idle state Failed");
		return ret;
	}
	sys_write32(DEV_STAT_DEF_VALUE, CNF_CTRLCFG(base_address, DEV_STAT));
#endif
	/* Set operation work mode (runs cadence_setup_data_interface for SDR) */
	ret = cdns_nand_set_opr_mode(params, datarate_mode);
	if (ret != 0) {
		return ret;
	}

	/* Set data transfer configuration parameters */
	ret = cdns_nand_transfer_config(params);
	if (ret != 0) {
		return ret;
	}

	/* Wait for controller to be in idle state */
	ret = cdns_nand_wait_idle(base_address);
	if (ret != 0) {
		LOG_ERR("Wait for controller to be in idle state Failed");
		return ret;
	}
	/* DMA Setting */
	sys_write32((F_BURST_SEL_SET(NF_TDEF_BURST_SEL)) | (BIT(F_OTE)),
		    (base_address + NF_DMA_SETTING));

	/* Pre fetch */
	sys_write32(((NF_FIFO_TRIGG_LVL_SET(PRE_FETCH_VALUE)) |
		     (NF_DMA_PACKAGE_SIZE_SET(PRE_FETCH_VALUE))),
		    (base_address + NF_PRE_FETCH));

	/* Total bits in row addressing*/
	params->total_bit_row = find_msb_set(((params->npages_per_block) - 1)) +
				find_msb_set((params->nblocks_per_lun) - 1);

	if (ret != 0) {
		LOG_ERR("Failed to establish device access width!");
		return -EINVAL;
	}
	/* Enable Global Interrupt for NAND*/
#ifdef CONFIG_CDNS_NAND_INTERRUPT_SUPPORT
	sys_set_bits((base_address + INTERRUPT_STATUS_REG),
			(BIT(GINTR_ENABLE) | BIT(CMD_ETE_INTR_ENABLE)));
	sys_write32(ENABLE, base_address + THREAD_INTERRUPT_ERROR_ENABLE);
#endif

	k_msleep(100);
	cdns_nand_dump_regs(base_address);
	return 0;
}

#if CONFIG_CDNS_NAND_CDMA_MODE
/**
 *
 * This function performs Command descriptor structure prepareation.
 *
 * @param nf_mem determine which NF memory bank will be selected
 * @param flash_ptr start ROW address in NF memory
 * @param mem_ptr system memory pointer
 * @param ctype Command type (read/write/erase)
 * @param cmd_cnt counter for commands
 * @param dma_sel select DMA engine (0 - slave DMA, 1 - master DMA)
 * @param vol_id specify target volume ID
 *
 */
void cdns_nand_cdma_prepare(char nf_mem, uint32_t flash_ptr, char *mem_ptr, uint16_t ctype,
			    int32_t cmd_cnt, uint8_t dma_sel, uint8_t vol_id,
			    struct cdns_cdma_command_descriptor *desc)
{
	struct cdns_cdma_command_descriptor *cdma_desc;

	cdma_desc = desc;
	memset(cdma_desc, 0, sizeof(*cdma_desc));
	/* set fields for one descriptor */
	cdma_desc->flash_pointer = flash_ptr;
	cdma_desc->bank_number = nf_mem;
	cdma_desc->command_flags = CDMA_CF_DMA_MASTER_SET(dma_sel) | F_CFLAGS_VOL_ID_SET(vol_id);
	cdma_desc->memory_pointer = (mem_ptr != NULL) ? k_mem_phys_addr(mem_ptr) : 0;
	cdma_desc->status = 0;
	cdma_desc->sync_flag_pointer = 0;
	cdma_desc->sync_arguments = 0;
	cdma_desc->ctrl_data_ptr = 0;
	cdma_desc->command_type = ctype;
	if (cmd_cnt > 1) {
		cdma_desc->next_pointer = k_mem_phys_addr(desc + 1);
		cdma_desc->command_flags |= CFLAGS_CONT_SET;
	} else {
		cdma_desc->next_pointer = 0;
#ifdef CONFIG_CDNS_NAND_INTERRUPT_SUPPORT
		cdma_desc->command_flags |= CDMA_CF_INT_SET;
#endif
	}
	sys_cache_data_flush_and_invd_range(cdma_desc, sizeof(*cdma_desc));
}

/**
 * Check a command descriptor transfer complete status in the Cadence NAND controller.
 *
 * @param desc_ptr The pointer to the command descriptor structure.
 * @param params The Cadence NAND parameters structure.
 * @retval 0 on success or negative error value on failure.
 */
static int cdns_transfer_complete(struct cdns_cdma_command_descriptor *desc_ptr,
				  struct cadence_nand_params *params)
{
#ifdef CONFIG_CDNS_NAND_INTERRUPT_SUPPORT
	uint32_t status = 0;

	NAND_INT_SEM_TAKE(params);
	sys_write32(NF_TDEF_TRD_NUM, (params->nand_base + CMD_STATUS_PTR_ADDR));
	status = sys_read32((params->nand_base + CMD_STAT_CMD_STATUS));
	if ((status & (BIT(F_CSTAT_COMP)))) {
		if ((status & (BIT(F_CSTAT_FAIL)))) {
			LOG_ERR("Cadence status operation failed %x", status);
			return -EIO;
		}
	} else {
		LOG_ERR("Cadence status complete failed %s", __func__);
		return -EIO;
	}
#else
	ARG_UNUSED(params);

	sys_cache_data_invd_range(desc_ptr, sizeof(*desc_ptr));
	if (!WAIT_FOR(((desc_ptr->status & (BIT(F_CSTAT_COMP))) != 0), IDLE_TIME_OUT, ({
			      k_msleep(1);
			      sys_cache_data_invd_range(desc_ptr, sizeof(*desc_ptr));
		      }))) {
		LOG_ERR("Timed out waiting for thread status response");
		return -ETIMEDOUT;
	}
	if ((desc_ptr->status & (BIT(F_CSTAT_FAIL))) != 0) {
		LOG_ERR("Cadence status operation failed %llx", desc_ptr->status);
		return -EIO;
	}
#endif
	return 0;
}

/**
 * Send a command descriptor to the Cadence NAND controller for execution.
 *
 * @param base_address The base address of the Cadence NAND controller.
 * @param desc_ptr The pointer to the command descriptor.
 * @param thread The thread number for the execution.
 * @retval 0 on success or -ETIMEDOUT error value on failure.
 */
static int cdns_nand_send(uintptr_t base_address, char *desc_ptr, uint8_t thread)
{
	uint64_t desc_address;
	uint32_t status;
	int ret;

	desc_address = k_mem_phys_addr(desc_ptr);

	ret = cdns_wait_for_thread(base_address, thread);

	if (ret != 0) {
		return ret;
	}
	/* desc_ptr address passing */
	sys_write32(desc_address & U32_MASK_VAL, (base_address + CDNS_CMD_REG2));
	sys_write32((desc_address >> 32) & U32_MASK_VAL, (base_address + CDNS_CMD_REG3));
	/* Thread selection */
	status = CMD_0_THREAD_POS_SET(thread);
	/* CDMA Mode selection */
	status |= CMD_0_C_MODE_SET(CT_CDMA_MODE);
	/* CMD 0 Reg write*/
	sys_write32(status, (base_address + CDNS_CMD_REG0));
	return 0;
}

static int cdns_cdma_desc_transfer_finish(struct cadence_nand_params *params, uint32_t page_count,
					  uint32_t max_page_desc, uint32_t ctype,
					  uint32_t cond_start, char *buffer)
{
	uint32_t page_count_pass = 0;
	uint32_t row_address = 0;
	uint32_t base_address;
	int ret;

	if (max_page_desc > CDNS_NAND_CDMA_DESC_MAX) {
		LOG_ERR("CDMA descriptor count exceeds static buffer");
		return -EINVAL;
	}

	base_address = params->nand_base;

	while (page_count > 0) {
		uint32_t batch_start = cond_start;
		uint32_t addr_step = (ctype == CNF_CMD_ERASE) ? params->npages_per_block : 1U;

		if (page_count > max_page_desc) {
			page_count_pass = max_page_desc;
			page_count = page_count - max_page_desc;
		} else {
			page_count_pass = page_count;
			page_count = 0;
		}
		cond_start = batch_start + (page_count_pass * addr_step);

		for (int index = 0; index < page_count_pass; index++) {
			char *page_buf = NULL;

			row_address_set(params, &row_address, batch_start + (index * addr_step));
			if (ctype != CNF_CMD_ERASE && buffer != NULL) {
				page_buf = buffer + (index * params->page_size);
			}
			cdns_nand_cdma_prepare(NF_TDEF_DEV_NUM, row_address, page_buf,
					       ctype, (page_count_pass - index),
					       DMA_MS_SEL, VOL_ID, &params->cdma_desc[index]);
		}
		ret = cdns_nand_send(base_address, (char *)params->cdma_desc, NF_TDEF_TRD_NUM);

		if (ret != 0) {
			return ret;
		}

		if (ctype != CNF_CMD_ERASE && buffer != NULL) {
			buffer = buffer + (page_count_pass * params->page_size);
		}

		ret = cdns_transfer_complete(params->cdma_desc, params);

		if (ret != 0) {
			return ret;
		}
	}

	return 0;
}
/**
 * Perform a CDMA write operation for the Cadence NAND controller.
 *
 * @param params The Cadence NAND parameters structure.
 * @param start_page_number The starting page number for the write operation.
 * @param buffer The buffer containing the data to be written.
 * @param page_count The number of pages to be written.
 * @retval  0 on success or negative error value on failure.
 */
static int cdns_nand_cdma_write(struct cadence_nand_params *params, uint32_t start_page_number,
				char *buffer, uint32_t page_count)
{
	int ret;

	ret = cdns_cdma_desc_transfer_finish(params, page_count, CONFIG_FLASH_CDNS_CDMA_PAGE_COUNT,
					     CNF_CMD_WR, start_page_number, buffer);

	return ret;
}

/**
 * Perform a CDMA read operation for the Cadence NAND controller.
 *
 * @param params The Cadence NAND parameters structure.
 * @param start_page_number The starting page number for the read operation.
 * @param buffer The buffer to store the read data.
 * @param page_count The number of pages to be read.
 * @retval  0 on success or negative error value on failure.
 */
static int cdns_nand_cdma_read(struct cadence_nand_params *params, uint32_t start_page_number,
			       char *buffer, uint32_t page_count)
{
	int ret;

	ret = cdns_cdma_desc_transfer_finish(params, page_count, CONFIG_FLASH_CDNS_CDMA_PAGE_COUNT,
					     CNF_CMD_RD, start_page_number, buffer);

	return ret;
}

/**
 * Perform a CDMA erase operation for the Cadence NAND controller.
 *
 * @param params The Cadence NAND parameters structure.
 * @param start_block_number The starting block number for the erase operation.
 * @param block_count The number of blocks to be erased.
 * @retval  0 on success or negative error value on failure.
 */
static int cdns_nand_cdma_erase(struct cadence_nand_params *params, uint32_t start_block_number,
				uint32_t block_count)
{
	int ret;

	ret = cdns_cdma_desc_transfer_finish(params, block_count,
					     CONFIG_FLASH_CDNS_CDMA_BLOCK_COUNT, CNF_CMD_ERASE,
					     start_block_number, NULL);

	return ret;
}
#endif

#if CONFIG_CDNS_NAND_PIO_MODE

/**
 * Perform an erase operation on the Cadence NAND controller using PIO.
 *
 * @param params The Cadence NAND parameters structure.
 * @param thread The thread identifier for the PIO operation.
 * @param bank The bank identifier for the erase operation.
 * @param start_block The starting block number for the erase operation.
 * @param ctype The command type for the erase operation.
 * @param block_count The number of blocks to be erased.
 * @retval   0 on success or negative error value on failure.
 */
static int cdns_nand_pio_erase(struct cadence_nand_params *params, uint8_t thread, uint8_t bank,
			       uint32_t start_block, uint16_t ctype, uint32_t block_count)
{
	uint32_t status;
	uintptr_t base_address;
	uint32_t row_address = 0;
	uint32_t index = 0;
	int ret;

	base_address = params->nand_base;
	for (index = 0; index < block_count; index++) {

		ret = cdns_wait_for_thread(base_address, thread);

		if (ret != 0) {
			return ret;
		}
		row_address_set(params, &row_address, (start_block * params->npages_per_block));
		sys_write32(row_address, (base_address + CDNS_CMD_REG1));
		start_block++;
		sys_write32((NF_CMD4_BANK_SET(bank)), (base_address + CDNS_CMD_REG4));
		status = CMD_0_THREAD_POS_SET(thread);
#ifdef CONFIG_CDNS_NAND_INTERRUPT_SUPPORT
		status |= BIT(PIO_CF_INT);
#endif
		status |= CMD_0_C_MODE_SET(CT_PIO_MODE);
		status |= PIO_CMD0_CT_SET(ctype);
		sys_write32(status, (base_address + CDNS_CMD_REG0));
		NAND_INT_SEM_TAKE(params);
		ret = cdns_pio_transfer_complete(base_address, thread);
		if (ret != 0) {
			return ret;
		}
	}

	return 0;
}

/**
 * Prepare for a PIO operation in the Cadence NAND controller.
 *
 * @param base_address The base address of the Cadence NAND controller.
 * @param thread The thread ID associated with the operation.
 * @param bank The bank ID for the operation.
 * @param row_address The row address for the operation.
 * @param buf The buffer containing the data for the operation.
 * @param ctype The command type for the operation.
 * @param dma_sel The DMA selection flag for the operation.
 * @param vol_id The volume ID for the operation.
 * @retval 0 on success or -ETIMEDOUT error value on failure.
 */
static int cdns_nand_pio_prepare(uintptr_t base_address, uint8_t thread, uint8_t bank,
				 uint32_t row_address, char *buf, uint16_t ctype, uint8_t dma_sel,
				 uint8_t vol_id)
{
	uint64_t buf_addr = (uintptr_t)buf;
	uint32_t status;
	int ret;

	ret = cdns_wait_for_thread(base_address, thread);

	if (ret != 0) {
		return ret;
	}

	sys_write32(row_address, (base_address + CDNS_CMD_REG1));
	sys_write32(NF_CMD4_BANK_SET(bank), (base_address + CDNS_CMD_REG4));
	sys_write32(buf_addr & U32_MASK_VAL, (base_address + CDNS_CMD_REG2));
	sys_write32((buf_addr >> 32) & U32_MASK_VAL, (base_address + CDNS_CMD_REG3));
	status = CMD_0_THREAD_POS_SET(thread);
#ifdef CONFIG_CDNS_NAND_INTERRUPT_SUPPORT
	status |= PIO_CF_INT_SET;
#endif
	status |= PIO_CF_DMA_MASTER_SET(dma_sel);
	status |= CMD_0_C_MODE_SET(CT_PIO_MODE);
	status |= PIO_CMD0_CT_SET(ctype);
	status |= CMD_0_VOL_ID_SET(vol_id);
	sys_write32(status, (base_address + CDNS_CMD_REG0));
	return 0;
}

/**
 * Perform a PIO write operation for the Cadence NAND controller.
 *
 * @param params The Cadence NAND parameters structure.
 * @param row_address The row address for the write operation.
 * @param buffer The buffer containing the data to be written.
 * @retval  0 on success or negative error value on failure.
 */
static int cdns_nand_pio_write(struct cadence_nand_params *params, uint32_t row_address,
			       char *buffer)
{
	uintptr_t base_address;
	int ret;

	base_address = params->nand_base;

	ret = cdns_nand_pio_prepare(base_address, NF_TDEF_TRD_NUM, NF_TDEF_DEV_NUM, row_address,
				    buffer, CNF_CMD_WR, DMA_MS_SEL, VOL_ID);

	if (ret != 0) {
		return ret;
	}
	NAND_INT_SEM_TAKE(params);
	ret = cdns_pio_transfer_complete(base_address, NF_TDEF_TRD_NUM);
	if (ret != 0) {
		return ret;
	}

	return 0;
}

/**
 * Perform a PIO read operation for the Cadence NAND controller.
 *
 * @param params The Cadence NAND parameters structure.
 * @param row_address The row address for the read operation.
 * @param buffer The buffer to store the read data.
 * @retval   0 on success or negative error value on failure.
 */
static int cdns_nand_pio_read(struct cadence_nand_params *params, uint32_t row_address,
			      char *buffer)
{
	uintptr_t base_address;
	int ret;

	base_address = params->nand_base;

	ret = cdns_nand_pio_prepare(base_address, NF_TDEF_TRD_NUM, NF_TDEF_DEV_NUM, row_address,
				    buffer, CNF_CMD_RD, DMA_MS_SEL, VOL_ID);

	if (ret != 0) {
		return ret;
	}

	NAND_INT_SEM_TAKE(params);
	ret = cdns_pio_transfer_complete(base_address, NF_TDEF_TRD_NUM);
	if (ret != 0) {
		return ret;
	}

	return 0;
}

/**
 * Perform a combined PIO read and write operation for the Cadence NAND controller.
 *
 * @param params The Cadence NAND parameters structure.
 * @param start_page_number The starting page number for the read/write operation.
 * @param buffer The buffer containing the data to be written or to store the read data.
 * @param page_count The number of pages to be read or written.
 * @param mode The mode of operation (read, write).
 * @retval  0 on success or negative error value on failure.
 */
static int cdns_nand_pio_read_write(struct cadence_nand_params *params, uint32_t start_page_number,
				    char *buffer, uint32_t page_count, uint8_t mode)
{
	uint32_t index;
	uint32_t pio_row_address = 0;
	int ret = 0;

	for (index = 0; index < page_count; index++) {
		row_address_set(params, &pio_row_address, start_page_number++);
		if (mode == CDNS_READ) {
			ret = cdns_nand_pio_read(params, pio_row_address,
						 buffer + (index * (params->page_size)));
		} else {
			ret = cdns_nand_pio_write(params, pio_row_address,
						  buffer + (index * (params->page_size)));
		}
	}
	return ret;
}
#endif

#if CONFIG_CDNS_NAND_GENERIC_MODE
/**
 * Send a generic command to the Cadence NAND controller.
 *
 * @param params The Cadence NAND parameters structure.
 * @param mini_ctrl_cmd The command to be sent.
 * @retval  0 on success or -ETIMEDOUT error value on failure.
 */
static int cdns_generic_send_cmd(struct cadence_nand_params *params, uint64_t mini_ctrl_cmd)
{

	uint32_t mini_ctrl_cmd_l, mini_ctrl_cmd_h, status;
	uintptr_t base_address;
	int ret = 0;

	base_address = params->nand_base;
	mini_ctrl_cmd_l = mini_ctrl_cmd & U32_MASK_VAL;
	mini_ctrl_cmd_h = mini_ctrl_cmd >> 32;
	ret = cdns_nand_wait_idle(base_address);

	if (ret != 0) {
		LOG_ERR("Wait for controller to be in idle state Failed");
		return ret;
	}
	sys_write32(mini_ctrl_cmd_l, (base_address + CDNS_CMD_REG2));
	sys_write32(mini_ctrl_cmd_h, (base_address + CDNS_CMD_REG3));
	/* Select generic command. */
	status = CMD_0_THREAD_POS_SET(NF_TDEF_TRD_NUM);
#ifdef CONFIG_CDNS_NAND_INTERRUPT_SUPPORT
	status |= GEN_CF_INT_SET(GEN_CF_INT_ENABLE);
#endif
	status |= CMD_0_C_MODE_SET(CT_GENERIC_MODE);
	sys_write32(status, (base_address + CDNS_CMD_REG0));
	return 0;
}

/**
 * Send a generic command data to the Cadence NAND controller.
 *
 * @param params The Cadence NAND parameters structure.
 * @param mode The mode of operation (read, write).
 * @param data_length The length of the associated data.
 * @retval   0 on success or -ETIMEDOUT error value on failure.
 */
static int cdns_generic_cmd_data(struct cadence_nand_params *params, uint8_t mode,
				 uint32_t data_length)
{
	uint64_t mini_ctrl_cmd = 0;
	int ret = 0;

	mini_ctrl_cmd |= GCMD_TWB_VALUE;
	mini_ctrl_cmd |= GCMCD_DATA_SEQ;
	mini_ctrl_cmd |= GEN_SECTOR_COUNT_SET;
	mini_ctrl_cmd |= GEN_LAST_SECTOR_SIZE_SET((uint64_t)data_length);
	mini_ctrl_cmd |= GEN_DIR_SET((uint64_t)mode);
	mini_ctrl_cmd |= GEN_SECTOR_SET((uint64_t)data_length);
	ret = cdns_generic_send_cmd(params, mini_ctrl_cmd);
	return ret;
}

/**
 * Wait for the completion of an SDMA operation in the Cadence NAND controller.
 *
 * @param base_address The base address of the Cadence NAND controller.
 * @retval  0 on success or -ETIMEDOUT error value on failure.
 */
static int cdns_wait_sdma(uintptr_t base_address)
{

	if (!WAIT_FOR(((sys_read32(base_address + INTR_STATUS) & BIT(SDMA_TRIGG)) != 0),
		      IDLE_TIME_OUT, k_msleep(1))) {
		LOG_ERR("Timed out waiting for sdma response");
		return -ETIMEDOUT;
	}
	sys_set_bit((base_address + INTR_STATUS), SDMA_TRIGG);
	return 0;
}

/**
 * Perform buffer copying to SDMA regs in the Cadence NAND controller.
 *
 * @param sdma_base_address The base address of the SDMA in the Cadence NAND controller.
 * @param buffer The source or destination buffer for the copy operation.
 * @param data_length The length of the data to be copied.
 */
static void sdma_buffer_copy_in(uint32_t sdma_base_address, uint8_t *buffer, uint32_t data_length)
{
	uint32_t index;

	for (index = 0; index < data_length; index++) {
		sys_write8(*(buffer + index), sdma_base_address + index);
	}
}

/**
 * Perform buffer copying from SDMA regs in the Cadence NAND controller.
 *
 * @param sdma_base_address The base address of the SDMA in the Cadence NAND controller.
 * @param buffer The source or destination buffer for the copy operation.
 * @param data_length The length of the data to be copied.
 */
static void sdma_buffer_copy_out(uint32_t sdma_base_address, uint8_t *buffer, uint32_t data_length)
{
	uint32_t index;

	for (index = 0; index < data_length; index++) {
		*(buffer + index) = sys_read8(sdma_base_address + index);
	}
}

/**
 * Perform a generic page read operation in the Cadence NAND controller.
 *
 * @param params The Cadence NAND parameters structure.
 * @param read_address The address from which to read the page.
 * @param data_buffer The buffer to store the read data.
 * @retval  0 on success or negative error value on failure.
 */
static int cdns_generic_page_read(struct cadence_nand_params *params, uint64_t read_address,
				  void *data_buffer)
{
	uint64_t mini_ctrl_cmd = 0;
	uintptr_t base_address = params->nand_base;
	int ret;

	mini_ctrl_cmd = PAGE_READ_CMD;
	mini_ctrl_cmd |= GCMD_TWB_VALUE;
	if ((params->nluns > 1) || (params->total_bit_row > 16)) {
		mini_ctrl_cmd |= PAGE_MAX_BYTES(PAGE_MAX_SIZE);
	} else {
		mini_ctrl_cmd |= PAGE_MAX_BYTES(PAGE_MAX_SIZE - 1);
	}
	mini_ctrl_cmd |= read_address << 32;
	ret = cdns_generic_send_cmd(params, mini_ctrl_cmd);
	if (ret != 0) {
		return ret;
	}
	NAND_INT_SEM_TAKE(params);
	ret = cdns_generic_cmd_data(params, CDNS_READ, params->page_size);
	if (ret != 0) {
		return ret;
	}
	NAND_INT_SEM_TAKE(params);
	ret = cdns_wait_sdma(base_address);
	if (ret != 0) {
		return ret;
	}
	sdma_buffer_copy_out(params->sdma_base, data_buffer, params->page_size);
	return 0;
}

/**
 * Perform a generic page write operation in the Cadence NAND controller.
 *
 * @param params The Cadence NAND parameters structure.
 * @param write_address The address to which the page will be written.
 * @param data_buffer The buffer containing the data to be written.
 * @retval  0 on success or negative error value on failure.
 */
static int cdns_generic_page_write(struct cadence_nand_params *params, uint64_t write_address,
				   void *data_buffer)
{
	uint64_t mini_ctrl_cmd = 0;
	int ret;

	uintptr_t base_address = params->nand_base;

	mini_ctrl_cmd |= GCMD_TWB_VALUE;
	mini_ctrl_cmd |= GEN_ADDR_WRITE_DATA((uint32_t)write_address);
	if ((params->nluns > 1) || (params->total_bit_row > BIT16_CHECK)) {
		mini_ctrl_cmd |= PAGE_MAX_BYTES(PAGE_MAX_SIZE);
	} else {
		mini_ctrl_cmd |= PAGE_MAX_BYTES(PAGE_MAX_SIZE - 1);
	}
	mini_ctrl_cmd |= PAGE_WRITE_CMD;
	ret = cdns_generic_send_cmd(params, mini_ctrl_cmd);
	if (ret != 0) {
		return ret;
	}
	NAND_INT_SEM_TAKE(params);
	ret = cdns_generic_cmd_data(params, CDNS_WRITE, params->page_size);
	if (ret != 0) {
		return ret;
	}
	sdma_buffer_copy_in(params->sdma_base, data_buffer, params->page_size);
	NAND_INT_SEM_TAKE(params);
	mini_ctrl_cmd = 0;
	mini_ctrl_cmd |= PAGE_WRITE_10H_CMD;
	mini_ctrl_cmd |= GCMD_TWB_VALUE;
	mini_ctrl_cmd |= PAGE_CMOD_CMD;
	ret = cdns_generic_send_cmd(params, mini_ctrl_cmd);
	if (ret != 0) {
		return ret;
	}
	NAND_INT_SEM_TAKE(params);
	ret = cdns_wait_sdma(base_address);
	return ret;
}

/**
 * Perform a generic read or write operation for a range of pages in the Cadence NAND controller.
 *
 * @param params The Cadence NAND parameters structure.
 * @param start_page_number The starting page number for the read or write operation.
 * @param buffer The buffer containing the data to be written or to store the read data.
 * @param page_count The number of pages to be read or written.
 * @param mode The mode of operation (read, write).
 * @retval  0 on success or negative error value on failure.
 */
static int cdns_nand_gen_read_write(struct cadence_nand_params *params, uint32_t start_page_number,
				    char *buffer, uint32_t page_count, uint8_t mode)
{
	uint64_t address = 0;
	uint32_t index = 0;
	uint32_t gen_row_address = 0;
	int ret = 0;

	for (index = 0; index < page_count; index++) {
		row_address_set(params, &gen_row_address, start_page_number++);
		address = ((uint64_t)gen_row_address);
		if (mode == CDNS_READ) {
			ret = cdns_generic_page_read(params, address,
						     buffer + (index * (params->page_size)));
			if (ret != 0) {
				LOG_ERR("Cadence NAND Generic Page Read Error!!");
				return ret;
			}
		} else {
			ret = cdns_generic_page_write(params, address,
						      buffer + (index * (params->page_size)));
			if (ret != 0) {
				LOG_ERR("Cadence NAND Generic Page write Error!!");
				return ret;
			}
		}
	}
	return 0;
}

/**
 * Perform a generic erase operation for a range of blocks in the Cadence NAND controller.
 *
 * @param params The Cadence NAND parameters structure.
 * @param start_block The starting block number for the erase operation.
 * @param block_count The number of blocks to be erased.
 * @retval  0 on success or -ETIMEDOUT error value on failure.
 */
static int cdns_nand_gen_erase(struct cadence_nand_params *params, uint32_t start_block,
			       uint32_t block_count)
{
	uint64_t mini_ctrl_cmd = 0;
	uintptr_t base_address = 0;
	uint32_t gen_row_address = 0;
	uint32_t index = 0;
	int ret = 0;

	for (index = 0; index < block_count; index++) {
		row_address_set(params, &gen_row_address, (start_block * params->npages_per_block));
		start_block++;
		base_address = params->nand_base;
		mini_ctrl_cmd |= GCMD_TWB_VALUE;
		mini_ctrl_cmd |= ERASE_ADDR_SIZE;
		mini_ctrl_cmd |= ((gen_row_address) & (U32_MASK_VAL));
		mini_ctrl_cmd |= PAGE_ERASE_CMD;
		ret = cdns_generic_send_cmd(params, mini_ctrl_cmd);
		if (ret != 0) {
			return ret;
		}
		NAND_INT_SEM_TAKE(params);
	}
	return 0;
}
#endif

/**
 * Read data from the Cadence NAND controller into a buffer.
 */
static inline int cdns_read_data(struct cadence_nand_params *params, uint32_t start_page_number,
				 const void *buffer, uint32_t page_count)
{
	int ret;

#if CONFIG_CDNS_NAND_CDMA_MODE
	ret = cdns_nand_cdma_read(params, start_page_number, (char *)buffer, page_count);
#elif CONFIG_CDNS_NAND_PIO_MODE
	ret = cdns_nand_pio_read_write(params, start_page_number, (char *)buffer, page_count,
				       CDNS_READ);
#elif CONFIG_CDNS_NAND_GENERIC_MODE
	ret = cdns_nand_gen_read_write(params, start_page_number, (char *)buffer, page_count,
				       CDNS_READ);
#endif
	return ret;
}

/**
 * Write data pages to the Cadence NAND controller from a buffer.
 */
static inline int cdns_write_data(struct cadence_nand_params *params, uint32_t start_page_number,
				 const void *buffer, uint32_t page_count)
{
	int ret;

#if CONFIG_CDNS_NAND_CDMA_MODE
	ret = cdns_nand_cdma_write(params, start_page_number, (char *)buffer, page_count);
#elif CONFIG_CDNS_NAND_PIO_MODE
	ret = cdns_nand_pio_read_write(params, start_page_number, (char *)buffer, page_count,
				       CDNS_WRITE);
#elif CONFIG_CDNS_NAND_GENERIC_MODE
	ret = cdns_nand_gen_read_write(params, start_page_number, (char *)buffer, page_count,
			CDNS_WRITE);
#endif

	return ret;
}

/**
 * Erase blocks of the Cadence NAND controller.
 */
static inline int cdns_erase_block(struct cadence_nand_params *params, uint32_t start_page_number,
		uint32_t page_count)
{
	int ret;

#if CONFIG_CDNS_NAND_CDMA_MODE
	ret = cdns_nand_cdma_erase(params, start_page_number, page_count);
#elif CONFIG_CDNS_NAND_PIO_MODE
	ret = cdns_nand_pio_erase(params, NF_TDEF_TRD_NUM, NF_TDEF_DEV_NUM, start_page_number,
				  CNF_CMD_ERASE, page_count);
#elif CONFIG_CDNS_NAND_GENERIC_MODE
	ret = cdns_nand_gen_erase(params, start_page_number, page_count);
#endif

	return ret;
}

/**
 * Read data from the Cadence NAND controller into a buffer.
 *
 * @param params The Cadence NAND parameters structure.
 * @param buffer The buffer to store the read data.
 * @param offset The offset within the NAND to start reading from.
 * @param size The size of the data to read.
 * @retval  0 on success or negative error value on failure.
 */
int cdns_nand_read(struct cadence_nand_params *params, const void *buffer, uint32_t offset,
		   uint32_t size)
{
	uint32_t page_number;
	uint32_t end_page_number;
	int page_offset;
	int num_bytes;
	int page_buf_size;
	int ret = 0;
	char *ptr = (char *)buffer;
	uint32_t rem_bytes = size;

	if (params == NULL) {
		LOG_ERR("Wrong parameter passed!!");
		return -EINVAL;
	}

	if (size == 0) {
		return 0;
	}

	if ((offset >= params->device_size) || (size > (params->device_size - offset))) {
		LOG_ERR("Wrong offset or size value passed!!");
		return -EINVAL;
	}

	page_buf_size = params->page_size + params->spare_size;
	if (page_buf_size > CONFIG_FLASH_CDNS_NAND_PAGE_BUF_SIZE) {
		LOG_ERR("Page+spare %d exceeds static buffer %d", page_buf_size,
			CONFIG_FLASH_CDNS_NAND_PAGE_BUF_SIZE);
		return -ENOMEM;
	}

	page_number = offset / (params->page_size);
	end_page_number = ((offset + size) - 1) / ((params->page_size));
	page_offset = offset % (params->page_size);
	while (page_number <= end_page_number) {
		size_t cache_len = ROUND_UP(page_buf_size, CDNS_NAND_DMA_ALIGN);

		memset(params->page_buf, 0xFF, cache_len);
		sys_cache_data_flush_and_invd_range(params->page_buf, cache_len);
		ret = cdns_read_data(params, page_number, params->page_buf, 1);

		if (ret != 0) {
			LOG_ERR("Cadence driver read Failed!!!");
			break;
		}

		sys_cache_data_invd_range(params->page_buf, cache_len);
		num_bytes = MIN(params->page_size - page_offset, rem_bytes);
		memcpy(ptr, params->page_buf + page_offset, num_bytes);
		ptr += num_bytes;
		rem_bytes -= num_bytes;

		page_offset = 0;
		page_number++;
	}

	return ret;
}

/**
 * Write data from a buffer to the Cadence NAND controller.
 *
 * @param params The Cadence NAND parameters structure.
 * @param buffer The buffer containing the data to be written.
 * @param offset The offset within the NAND to start writing to.
 * @param len The length of the data to write.
 * @retval  0 on success or negative error value on failure.
 */
int cdns_nand_write(struct cadence_nand_params *params, const void *buffer, uint32_t offset,
		    uint32_t len)
{
	uint32_t page_number;
	uint32_t end_page_number;
	int ret = 0;
	char *ptr = (char *)buffer;
	int page_buf_size;

	if (params == NULL) {
		LOG_ERR("Wrong parameter passed!!");
		return -EINVAL;
	}

	if (len == 0) {
		return 0;
	}

	if ((offset >= params->device_size) || (len > (params->device_size - offset))) {
		LOG_ERR("Wrong offset or len value passed!!");
		return -EINVAL;
	}

	if ((offset % params->page_size) != 0) {
		LOG_ERR("offset not page aligned!!! Page size = 0x%x", params->page_size);
		return -EINVAL;
	}

	if ((len % params->page_size) != 0) {
		LOG_ERR("length not page aligned!!! Page size = 0x%x", params->page_size);
		return -EINVAL;
	}

	page_buf_size = params->page_size + params->spare_size;
	if (page_buf_size > CONFIG_FLASH_CDNS_NAND_PAGE_BUF_SIZE) {
		LOG_ERR("Page+spare %d exceeds static buffer %d", page_buf_size,
			CONFIG_FLASH_CDNS_NAND_PAGE_BUF_SIZE);
		return -ENOMEM;
	}

	page_number = offset / (params->page_size);
	end_page_number = ((offset + len) - 1) / ((params->page_size));
	while (page_number <= end_page_number) {
		size_t cache_len = ROUND_UP(page_buf_size, CDNS_NAND_DMA_ALIGN);

		memcpy(params->page_buf, ptr, params->page_size);
		memset(params->page_buf + params->page_size, 0xff,
		       cache_len - params->page_size);
		sys_cache_data_flush_range(params->page_buf, cache_len);
		ret = cdns_write_data(params, page_number, params->page_buf, 1);

		if (ret != 0) {
			LOG_ERR("Cadence driver write Failed!!!");
			break;
		}

		ptr += params->page_size;
		page_number++;
	}

	return ret;
}

/**
 * Perform an erase operation on the Cadence NAND controller.
 *
 * @param params The Cadence NAND parameters structure.
 * @param offset The offset within the NAND to start erasing.
 * @param size The size of the data to erase.
 * @retval  0 on success or negative error value on failure.
 */
int cdns_nand_erase(struct cadence_nand_params *params, uint32_t offset, uint32_t size)
{
	uint32_t page_number;
	uint32_t block_number;
	uint32_t end_block_number;
	int ret = 0;

	if (params == NULL) {
		LOG_ERR("Wrong parameter passed!!");
		return -EINVAL;
	}

	if (size == 0) {
		return 0;
	}

	if ((offset >= params->device_size) || (size > (params->device_size - offset))) {
		LOG_ERR("Wrong offset or size value passed!!");
		return -EINVAL;
	}
	if ((offset % (params->block_size)) != 0) {
		LOG_ERR("Offset value not aligned with block size!! Erase block size = %x",
			params->block_size);
		return -EINVAL;
	}
	if ((size % (params->block_size)) != 0) {
		LOG_ERR("Length value not aligned with block size!! Erase block size = %x",
			params->block_size);
		return -EINVAL;
	}

	block_number = (offset / ((params->page_size))) / (params->npages_per_block);
	end_block_number =
		(((offset + size) - 1) / ((params->page_size))) / (params->npages_per_block);
	while (block_number <= end_block_number) {
		page_number = block_number * params->npages_per_block;
		ret = cdns_erase_block(params, page_number, 1);

		if (ret != 0) {
			LOG_ERR("Cadence driver Erase Failed!!!");
			break;
		}
		block_number++;
	}

	return ret;
}

#if CONFIG_CDNS_NAND_INTERRUPT_SUPPORT
void cdns_nand_irq_handler_ll(struct cadence_nand_params *params)
{
	uint32_t status = 0;
	uint8_t thread_num = 0;

	status = sys_read32(params->nand_base + THREAD_INTERRUPT_STATUS);
	thread_num = find_lsb_set(status);

	if (GET_INIT_SET_CHECK(status, (thread_num - 1)) != 0) {
		/* Clear the interrupt*/
		sys_write32(BIT((thread_num - 1)), params->nand_base + THREAD_INTERRUPT_STATUS);
	}

	status = sys_read32(params->nand_base + THREAD_INTERRUPT_ERROR);
	thread_num = find_lsb_set(status);

	if (GET_INIT_SET_CHECK(status, (thread_num - 1)) != 0) {
		/* Clear the interrupt*/
		sys_write32(BIT((thread_num - 1)), params->nand_base + THREAD_INTERRUPT_ERROR);
	}
}
#endif
