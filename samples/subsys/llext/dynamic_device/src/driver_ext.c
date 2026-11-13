/*
 * Copyright The Zephyr Project Contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * Built into each extension. DRIVER_EXT_NODE (a node label) and DRIVER_EXT_SYM
 * are set per extension in CMakeLists.txt.
 */

#include "driver_module.h"

DRIVER_MODULE_EXPORT(DT_NODELABEL(DRIVER_EXT_NODE), DRIVER_EXT_SYM);
