#ifndef PROD_STATE_H
#define PROD_STATE_H

#include <stdbool.h>
#include <stdint.h>

#include <zephyr/device.h>
#include <zephyr/drivers/gpio.h>

#include "lis2dh12_zephyr.h"

enum prod_mode {
	PROD_MODE_RUN,
	PROD_MODE_SHELF,
	PROD_MODE_PAIR,
	PROD_MODE_DFU,
};

struct prod_context {
	struct lis2dh12_dev *lis2dh;
	const struct device *tof_dev;
	const struct gpio_dt_spec *led0;
	const struct gpio_dt_spec *led1;
	const struct gpio_dt_spec *magnet;
	const struct gpio_dt_spec *accel_int;
	const struct gpio_dt_spec *accel_int2;
	struct gpio_callback *accel_int_cb;
	struct gpio_callback *accel_int2_cb;
	volatile uint32_t *interactions;
	volatile bool *orientation_irq_pending;
	bool pair_active;
	bool dfu_active;
	bool bt_was_disabled;
	uint32_t cycle;
};

void prod_run(struct prod_context *ctx);

#endif
