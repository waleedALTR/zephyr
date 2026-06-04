/*
 * Copyright (c) 2016 Intel Corporation
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stdio.h>
#include <zephyr/kernel.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/fpga_bridge/fpga_bridge.h>

#define BRIDGE_ENABLE 1
#define BRIDGE_MASK 3

/* 1000 msec = 1 sec */
#define SLEEP_TIME_MS   1000

/* The devicetree node identifier for the "led0" alias. */
//#define LED0_NODE DT_ALIAS(led0)
#define LED0_NODE DT_NODELABEL(pio_led0)

/*
 * A build error on this line means your board is unsupported.
 * See the sample documentation for information on how to fix this.
 */
static const struct gpio_dt_spec led = GPIO_DT_SPEC_GET(LED0_NODE, gpios);

int main(void)
{
	int ret;
	bool led_state = true;
	
	ret = do_bridge_reset(BRIDGE_ENABLE, BRIDGE_MASK);
	if(!ret) {
		printk("Bridge enable success action=%d mask=%x\n", BRIDGE_ENABLE, BRIDGE_MASK);
	} else {
		printk("Bridge enable failed action=%d mask=%x ret=%d\n", BRIDGE_ENABLE, BRIDGE_MASK, ret);
	}

	if (!gpio_is_ready_dt(&led)) {
		printk("gpio_is_ready_dt failed\n");
		return 0;
	}

	ret = gpio_pin_configure_dt(&led, GPIO_OUTPUT_ACTIVE);
	if (ret < 0) {
		printk("gpio_pin_configure_dt failed ret=%d\n", ret);
		return 0;
	}
	
	printk("gpio_pin_configure_dt success\n");

	while (1) {
		ret = gpio_pin_toggle_dt(&led);
		if (ret < 0) {
			printk("gpio_pin_toggle_dt failed ret=%d\n", ret);
			return 0;
		}
		ret = gpio_pin_get_dt(&led);
		if(ret < 0) {
			printk("gpio_pin_get_dt failed ret=%d\n", ret);
		}
		led_state = !!ret;
		printf("LED state: %s\n", led_state ? "ON" : "OFF");
		k_msleep(SLEEP_TIME_MS);
	}
	return 0;
}
