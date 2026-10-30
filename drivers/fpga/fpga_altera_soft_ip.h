/*
 * Copyright (c) 2026, Intel Corporation.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef ZEPHYR_DRIVERS_FPGA_FPGA_ALTERA_SOFT_IP_H_
#define ZEPHYR_DRIVERS_FPGA_FPGA_ALTERA_SOFT_IP_H_

/**
 * @brief Initialize fabric soft-IP listed in the FPGA node's soft-ip property.
 *
 * Call only after HPS↔FPGA bridges are enabled. Nodes with status disabled
 * are skipped. Expects soft-IP to use zephyr,deferred-init.
 */
void altera_fpga_soft_ip_init(void);

/**
 * @brief De-initialize fabric soft-IP listed in soft-ip.
 *
 * Call while bridges are still up, before isolating them.
 */
void altera_fpga_soft_ip_deinit(void);

#endif /* ZEPHYR_DRIVERS_FPGA_FPGA_ALTERA_SOFT_IP_H_ */
