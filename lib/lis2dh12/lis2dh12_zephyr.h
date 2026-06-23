#ifndef FLIGHTCAP_LIS2DH12_ZEPHYR_H
#define FLIGHTCAP_LIS2DH12_ZEPHYR_H

#include <stdbool.h>
#include <stdint.h>

#include <zephyr/drivers/i2c.h>

struct lis2dh12_dev {
	struct i2c_dt_spec bus;
	bool initialized;
};

int lis2dh12_zephyr_check(struct lis2dh12_dev *dev);
int lis2dh12_zephyr_init(struct lis2dh12_dev *dev);
int lis2dh12_zephyr_read_whoami(struct lis2dh12_dev *dev, uint8_t *whoami);
int lis2dh12_zephyr_read_accel_mg(struct lis2dh12_dev *dev, int16_t *x_mg, int16_t *y_mg,
				  int16_t *z_mg);
int lis2dh12_zephyr_read_temp_c(struct lis2dh12_dev *dev, int8_t *temp_c);
int lis2dh12_zephyr_config_motion(struct lis2dh12_dev *dev, uint8_t threshold, uint8_t duration);
int lis2dh12_zephyr_config_orientation_int2(struct lis2dh12_dev *dev);
int lis2dh12_zephyr_config_motion_and_orientation(struct lis2dh12_dev *dev, uint8_t threshold,
						  uint8_t duration);
int lis2dh12_zephyr_enter_shelf(struct lis2dh12_dev *dev);
int lis2dh12_zephyr_is_face_up(struct lis2dh12_dev *dev, int16_t z_min_mg);
int lis2dh12_zephyr_is_face_down(struct lis2dh12_dev *dev, int16_t z_max_mg);
int lis2dh12_zephyr_read_6d_position(struct lis2dh12_dev *dev, uint8_t *position);
int lis2dh12_zephyr_shelf_capture_baseline(struct lis2dh12_dev *dev, uint8_t *baseline);
int lis2dh12_zephyr_int2_left_shelf_position(struct lis2dh12_dev *dev, uint8_t baseline,
					     uint8_t int2_src);
int lis2dh12_zephyr_power_down(struct lis2dh12_dev *dev);
int lis2dh12_zephyr_read_int1_src(struct lis2dh12_dev *dev, uint8_t *int1_src);
int lis2dh12_zephyr_read_int2_src(struct lis2dh12_dev *dev, uint8_t *int2_src);

#endif
