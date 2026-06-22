#ifndef VL53L0X_ZEPHYR_H
#define VL53L0X_ZEPHYR_H

#include <stdint.h>

#include <zephyr/device.h>

int vl53l0x_zephyr_power_on(void);
int vl53l0x_zephyr_power_off(void);
int vl53l0x_zephyr_check(const struct device **dev_out, uint16_t *probe_mm_out);
int vl53l0x_zephyr_read_mm(const struct device *dev, uint16_t *mm_out);

#endif
