/*
 * Copyright (c) 2023, Intel Corporation.
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef ZEPHYR_INCLUDE_FPGA_MANAGER_H_
#define ZEPHYR_INCLUDE_FPGA_MANAGER_H_

#include <zephyr/kernel.h>

#define FPGA_RECONFIG_STATUS_BUF_SIZE 400

/**
 * @file
 *
 * @brief API for FPGA Manager
 */

/**
 * @brief Check the status of fpga configuration
 *
 * @param[out] status Pointer to store the FPGA configuration status.
 * The value is valid only when the return value is 0.
 *
 * @return 0 on success, negative errno code on fail
 */
int32_t fpga_get_status(void *status);

/**
 * @brief Configures the FPGA with given pointer to location that contains the
 * bitstream.
 *
 * @param[in] image_ptr Pointer within the FPGA_CONFIG reserved memory that
 * contains the bitstream.
 * @param[in] byte_size Bitstream size in byte
 * @param[in] config_type FPGA configuration type
 * @param[in] dev Freeze controller device
 *
 * @return 0 on success, negative errno code on fail
 */
int32_t fpga_load(char *image_ptr, uint32_t byte_size, uint32_t config_type, char *dev);

/**
 * @brief Get the memory address and size to store the bitstream.
 *
 * @param[out] phyaddr Pointer to store the start address
 * @param[out] size Pointer to store the reserved memory size
 *
 * @return 0 on success, negative errno code on fail
 */
int32_t fpga_get_memory(char **phyaddr, uint32_t *size);

#endif
