/*
 * Copyright (c) 2023, Intel Corporation. All rights reserved.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#define DT_DRV_COMPAT altr_pio_1_0

#include <zephyr/device.h>
#include <zephyr/kernel.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/gpio/gpio_utils.h>

#define ALTERA_AVALON_PIO_DATA_OFFSET		0x00
#define ALTERA_AVALON_PIO_DIRECTION_OFFSET	0x04
#define ALTERA_AVALON_PIO_IRQMASK_OFFSET	0x08
#define ALTERA_AVALON_PIO_EDGE_CAP_OFFSET	0x0c
#define ALTERA_AVALON_PIO_SET_BITS		0x10
#define ALTERA_AVALON_PIO_CLEAR_BITS		0x14

/* Matches direction enum order in altr,pio-1.0.yaml */
#define ALTERA_PIO_DIR_BIDIR	0
#define ALTERA_PIO_DIR_INPUT	1
#define ALTERA_PIO_DIR_INOUT	2
#define ALTERA_PIO_DIR_OUTPUT	3

/* Required by DEVICE_MMIO_NAMED_* macros */
#define DEV_CFG(_dev) ((const struct gpio_altera_config *)(_dev)->config)
#define DEV_DATA(_dev) ((struct gpio_altera_data *)(_dev)->data)

typedef void (*altera_cfg_func_t)(void);

struct gpio_altera_config {
	struct gpio_driver_config	common;
	DEVICE_MMIO_NAMED_ROM(mm_reg_base);
	uint32_t			irq_num;
	uint8_t				direction;
	uint8_t				outset;
	uint8_t				outclear;
	altera_cfg_func_t		cfg_func;
};

struct gpio_altera_data {
	/* gpio_driver_data needs to be first */
	struct gpio_driver_data common;
	DEVICE_MMIO_NAMED_RAM(mm_reg_base);
	/* list of callbacks */
	sys_slist_t cb;
	struct k_spinlock lock;
};

static bool altera_pio_pin_is_output(const struct device *dev, uint32_t pin_mask)
{
	const struct gpio_altera_config *cfg = dev->config;
	const int direction = cfg->direction;
	mm_reg_t mm_reg_base = DEVICE_MMIO_NAMED_GET(dev, mm_reg_base);
	uint32_t addr;
	uint32_t pin_direction;

	if (pin_mask == 0) {
		return false;
	}

	if (direction == ALTERA_PIO_DIR_OUTPUT) {
		return true;
	}

	if (direction == ALTERA_PIO_DIR_INPUT) {
		return false;
	}

	/* Bidirectional / inout: direction register bit set means output */
	addr = mm_reg_base + ALTERA_AVALON_PIO_DIRECTION_OFFSET;
	pin_direction = sys_read32(addr);

	return (pin_direction & pin_mask) == pin_mask;
}

static int gpio_altera_configure(const struct device *dev,
				 gpio_pin_t pin, gpio_flags_t flags)
{
	const struct gpio_altera_config *cfg = dev->config;
	const int port_pin_mask = cfg->common.port_pin_mask;
	const int direction = cfg->direction;
	const bool is_input = (flags & GPIO_INPUT) != 0;
	const bool is_output = (flags & GPIO_OUTPUT) != 0;

	/* Check if pin number is within range */
	if ((port_pin_mask & BIT(pin)) == 0) {
		return -EINVAL;
	}

	if (is_output && direction == ALTERA_PIO_DIR_INPUT) {
		return -EINVAL;
	}

	if (is_input && !is_output && direction == ALTERA_PIO_DIR_OUTPUT) {
		return -EINVAL;
	}

	if (!is_input && !is_output) {
		return -EINVAL;
	}

	/* Direction register is only present for bidir/inout PIO */
	if (direction == ALTERA_PIO_DIR_BIDIR || direction == ALTERA_PIO_DIR_INOUT) {
		struct gpio_altera_data *const data = dev->data;
		mm_reg_t mm_reg_base = DEVICE_MMIO_NAMED_GET(dev, mm_reg_base);
		uint32_t addr = mm_reg_base + ALTERA_AVALON_PIO_DIRECTION_OFFSET;
		k_spinlock_key_t key = k_spin_lock(&data->lock);

		if (is_output) {
			sys_set_bits(addr, BIT(pin));
		} else {
			sys_clear_bits(addr, BIT(pin));
		}

		k_spin_unlock(&data->lock, key);
	}

	return 0;
}

static int gpio_altera_port_get_raw(const struct device *dev, uint32_t *value)
{
	mm_reg_t mm_reg_base = DEVICE_MMIO_NAMED_GET(dev, mm_reg_base);
	uint32_t addr;

	addr = mm_reg_base + ALTERA_AVALON_PIO_DATA_OFFSET;

	if (value == NULL) {
		return -EINVAL;
	}

	*value = sys_read32((addr));

	return 0;
}

static int gpio_altera_port_set_bits_raw(const struct device *dev, gpio_port_pins_t mask)
{
	const struct gpio_altera_config *cfg = dev->config;
	struct gpio_altera_data * const data = dev->data;
	const uint8_t outset = cfg->outset;
	const int port_pin_mask = cfg->common.port_pin_mask;
	mm_reg_t mm_reg_base = DEVICE_MMIO_NAMED_GET(dev, mm_reg_base);
	uint32_t addr;
	k_spinlock_key_t key;

	if ((port_pin_mask & mask) == 0) {
		return -EINVAL;
	}

	if (cfg->direction == ALTERA_PIO_DIR_BIDIR ||
	    cfg->direction == ALTERA_PIO_DIR_INOUT) {
		if (!altera_pio_pin_is_output(dev, mask)) {
			return -EINVAL;
		}
	} else if (cfg->direction == ALTERA_PIO_DIR_INPUT) {
		return -EINVAL;
	}

	key = k_spin_lock(&data->lock);

	if (outset) {
		addr = mm_reg_base + ALTERA_AVALON_PIO_SET_BITS;
		sys_write32(mask, addr);
	} else {
		addr = mm_reg_base + ALTERA_AVALON_PIO_DATA_OFFSET;
		sys_set_bits(addr, mask);
	}

	k_spin_unlock(&data->lock, key);

	return 0;
}

static int gpio_altera_port_clear_bits_raw(const struct device *dev, gpio_port_pins_t mask)
{
	const struct gpio_altera_config *cfg = dev->config;
	struct gpio_altera_data * const data = dev->data;
	const uint8_t outclear = cfg->outclear;
	const int port_pin_mask = cfg->common.port_pin_mask;
	mm_reg_t mm_reg_base = DEVICE_MMIO_NAMED_GET(dev, mm_reg_base);
	uint32_t addr;
	k_spinlock_key_t key;

	/* Check if mask range within 32 */
	if ((port_pin_mask & mask) == 0) {
		return -EINVAL;
	}

	if (cfg->direction == ALTERA_PIO_DIR_BIDIR ||
	    cfg->direction == ALTERA_PIO_DIR_INOUT) {
		if (!altera_pio_pin_is_output(dev, mask)) {
			return -EINVAL;
		}
	} else if (cfg->direction == ALTERA_PIO_DIR_INPUT) {
		return -EINVAL;
	}

	key = k_spin_lock(&data->lock);

	if (outclear) {
		addr = mm_reg_base + ALTERA_AVALON_PIO_CLEAR_BITS;
		sys_write32(mask, addr);
	} else {
		addr = mm_reg_base + ALTERA_AVALON_PIO_DATA_OFFSET;
		sys_clear_bits(addr, mask);
	}

	k_spin_unlock(&data->lock, key);

	return 0;
}

static int gpio_altera_port_toggle_bits(const struct device *dev, gpio_port_pins_t mask)
{
	uint32_t val = 0;

	gpio_altera_port_get_raw(dev, &val);
	if ((val & mask)) {
		gpio_altera_port_clear_bits_raw(dev, mask);
	} else {
		gpio_altera_port_set_bits_raw(dev, mask);
	}
	return 0;
}

static int gpio_init(const struct device *dev)
{
	const struct gpio_altera_config *cfg = dev->config;

	DEVICE_MMIO_NAMED_MAP(dev, mm_reg_base, K_MEM_CACHE_NONE);

	/* Configure GPIO device */
	cfg->cfg_func();

	return 0;
}

static int gpio_altera_pin_interrupt_configure(const struct device *dev,
						gpio_pin_t pin,
						enum gpio_int_mode mode,
						enum gpio_int_trig trig)
{
	ARG_UNUSED(trig);

	const struct gpio_altera_config *cfg = dev->config;
	struct gpio_altera_data * const data = dev->data;
	mm_reg_t mm_reg_base = DEVICE_MMIO_NAMED_GET(dev, mm_reg_base);
	const int port_pin_mask = cfg->common.port_pin_mask;
	uint32_t addr;
	k_spinlock_key_t key;

	/* Check if pin number is within range */
	if ((port_pin_mask & BIT(pin)) == 0) {
		return -EINVAL;
	}

	/* Output-only PIO cannot generate input interrupts */
	if (cfg->direction == ALTERA_PIO_DIR_OUTPUT) {
		return -EINVAL;
	}

	/* For bidir/inout, pin must be configured as input */
	if ((cfg->direction == ALTERA_PIO_DIR_BIDIR ||
	     cfg->direction == ALTERA_PIO_DIR_INOUT) &&
	    altera_pio_pin_is_output(dev, BIT(pin))) {
		return -EINVAL;
	}

	addr = mm_reg_base + ALTERA_AVALON_PIO_IRQMASK_OFFSET;

	key = k_spin_lock(&data->lock);

	switch (mode) {
	case GPIO_INT_MODE_DISABLED:
		/* Disable this pin in irqmask; GIC only if no pins left */
		sys_clear_bits(addr, BIT(pin));
		if (cfg->irq_num != 0 && sys_read32(addr) == 0U) {
			irq_disable(cfg->irq_num);
		}
		break;
	case GPIO_INT_MODE_LEVEL:
	case GPIO_INT_MODE_EDGE:
		/* Clear any stale edge and enable interrupt mask */
		sys_write32(BIT(pin), mm_reg_base + ALTERA_AVALON_PIO_EDGE_CAP_OFFSET);
		sys_set_bits(addr, BIT(pin));
		if (cfg->irq_num != 0) {
			irq_enable(cfg->irq_num);
		}
		break;
	default:
		k_spin_unlock(&data->lock, key);
		return -EINVAL;
	}

	k_spin_unlock(&data->lock, key);

	return 0;
}

static int gpio_altera_manage_callback(const struct device *dev,
					struct gpio_callback *callback,
					bool set)
{

	struct gpio_altera_data * const data = dev->data;

	return gpio_manage_callback(&data->cb, callback, set);
}

static void gpio_altera_irq_handler(const struct device *dev)
{
	struct gpio_altera_data * const data = dev->data;
	mm_reg_t mm_reg_base = DEVICE_MMIO_NAMED_GET(dev, mm_reg_base);
	uint32_t port_value;
	uint32_t addr;
	k_spinlock_key_t key;

	addr = mm_reg_base + ALTERA_AVALON_PIO_EDGE_CAP_OFFSET;

	key = k_spin_lock(&data->lock);

	/* Read and clear edge-capture bits (Avalon PIO address 3) */
	port_value = sys_read32(addr);
	if (port_value != 0U) {
		sys_write32(port_value, addr);
	}

	k_spin_unlock(&data->lock, key);

	if (port_value != 0U) {
		gpio_fire_callbacks(&data->cb, dev, port_value);
	}
}

static DEVICE_API(gpio, gpio_altera_driver_api) = {
	.pin_configure		 = gpio_altera_configure,
	.port_get_raw		 = gpio_altera_port_get_raw,
	.port_set_masked_raw	 = NULL,
	.port_set_bits_raw	 = gpio_altera_port_set_bits_raw,
	.port_clear_bits_raw	 = gpio_altera_port_clear_bits_raw,
	.port_toggle_bits	 = gpio_altera_port_toggle_bits,
	.pin_interrupt_configure = gpio_altera_pin_interrupt_configure,
	.manage_callback	 = gpio_altera_manage_callback
};

#define GPIO_CFG_IRQ_FLAGS(idx, n)						\
	COND_CODE_1(DT_INST_IRQ_HAS_CELL_AT_IDX(n, idx, flags),			\
		(DT_INST_IRQ_BY_IDX(n, idx, flags)), (0))

#define GPIO_CFG_IRQ(idx, n)							\
		IRQ_CONNECT(DT_INST_IRQ_BY_IDX(n, idx, irq),			\
			    COND_CODE_1(DT_INST_IRQ_HAS_CELL_AT_IDX(n, idx, priority), \
				(DT_INST_IRQ_BY_IDX(n, idx, priority)), (0)),	\
			    gpio_altera_irq_handler,				\
			    DEVICE_DT_INST_GET(n),				\
			    GPIO_CFG_IRQ_FLAGS(idx, n));			\

#define CREATE_GPIO_DEVICE(n)						\
	static void gpio_altera_cfg_func_##n(void);			\
	static struct gpio_altera_data gpio_altera_data_##n;		\
	static struct gpio_altera_config gpio_config_##n = {		\
		.common		= {					\
			.port_pin_mask	=				\
				  GPIO_PORT_PIN_MASK_FROM_DT_INST(n),	\
		},						        \
		DEVICE_MMIO_NAMED_ROM_INIT(mm_reg_base, DT_DRV_INST(n)),\
		.direction	= DT_INST_ENUM_IDX(n, direction),	\
		.irq_num	= COND_CODE_1(DT_INST_IRQ_HAS_IDX(n, 0), (DT_INST_IRQN(n)), (0)),\
		.cfg_func	= gpio_altera_cfg_func_##n,		\
		.outset		= DT_INST_PROP(n, outset),	\
		.outclear	= DT_INST_PROP(n, outclear),	\
	};								\
							\
	DEVICE_DT_INST_DEFINE(n,			\
			      gpio_init,		\
			      NULL,			\
			      &gpio_altera_data_##n,	\
			      &gpio_config_##n,		\
			      POST_KERNEL,		\
			      CONFIG_GPIO_INIT_PRIORITY,	\
			      &gpio_altera_driver_api);		        \
									\
	static void gpio_altera_cfg_func_##n(void)			\
	{								\
		LISTIFY(DT_NUM_IRQS(DT_DRV_INST(n)), GPIO_CFG_IRQ, (), n)\
	}

DT_INST_FOREACH_STATUS_OKAY(CREATE_GPIO_DEVICE)
