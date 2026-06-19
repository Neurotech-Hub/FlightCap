#include "lis2dh12_zephyr.h"

#include <errno.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include "lis2dh12_reg.h"

LOG_MODULE_REGISTER(lis2dh12_zephyr, LOG_LEVEL_INF);

#define LIS2DH12_I2C_ADDR 0x19U

static struct lis2dh12_dev *g_dev;

static int platform_read(uint8_t reg, uint8_t *data, uint16_t len)
{
	if (!g_dev || !g_dev->initialized) {
		return -ENODEV;
	}

	if (len > 16U) {
		return -EINVAL;
	}

	if (len == 1U) {
		return i2c_write_read_dt(&g_dev->bus, &reg, 1, data, len);
	}

	uint8_t addr = reg | 0x80U;

	return i2c_write_read_dt(&g_dev->bus, &addr, 1, data, len);
}

static int platform_write(uint8_t reg, const uint8_t *data, uint16_t len)
{
	if (!g_dev || !g_dev->initialized) {
		return -ENODEV;
	}

	if (len > 16U) {
		return -EINVAL;
	}

	uint8_t buf[1U + 16U];

	buf[0] = (len == 1U) ? reg : (reg | 0x80U);
	memcpy(&buf[1], data, len);

	return i2c_write_dt(&g_dev->bus, buf, 1U + len);
}

int lis2dh12_zephyr_read_whoami(struct lis2dh12_dev *dev, uint8_t *whoami)
{
	if (!dev || !dev->initialized || !whoami) {
		return -EINVAL;
	}

	return platform_read(LIS2DH12_WHO_AM_I, whoami, 1);
}

int lis2dh12_zephyr_init(struct lis2dh12_dev *dev)
{
	if (!dev) {
		return -EINVAL;
	}

	dev->bus.bus = DEVICE_DT_GET(DT_ALIAS(i2c_bus));
	dev->bus.addr = LIS2DH12_I2C_ADDR;

	if (!device_is_ready(dev->bus.bus)) {
		LOG_ERR("LIS2DH12 I2C bus not ready");
		return -ENODEV;
	}

	dev->initialized = true;
	g_dev = dev;

	uint8_t whoami = 0;
	int ret = lis2dh12_zephyr_read_whoami(dev, &whoami);

	if (ret < 0) {
		return ret;
	}
	if (whoami != 0x33U) {
		LOG_ERR("Unexpected WHO_AM_I=0x%02x", whoami);
		return -ENODEV;
	}

	uint8_t ctrl_reg1 = 0x2F; /* 10Hz, LP mode, XYZ on */
	uint8_t ctrl_reg4 = 0x80;   /* BDU on, +-2g */
	uint8_t temp_cfg = 0xC0;    /* temperature enabled */

	ret = platform_write(0x20, &ctrl_reg1, 1);
	ret |= platform_write(0x23, &ctrl_reg4, 1);
	ret |= platform_write(0x1F, &temp_cfg, 1);
	if (ret < 0) {
		LOG_ERR("Failed LIS2DH12 base config");
		return ret;
	}

	k_sleep(K_MSEC(50));
	LOG_INF("LIS2DH12 ready WHO_AM_I=0x%02x I2C addr=0x%02x", whoami, LIS2DH12_I2C_ADDR);
	return 0;
}

int lis2dh12_zephyr_read_accel_mg(struct lis2dh12_dev *dev, int16_t *x_mg, int16_t *y_mg,
				  int16_t *z_mg)
{
	if (!dev || !dev->initialized || !x_mg || !y_mg || !z_mg) {
		return -EINVAL;
	}

	uint8_t raw[6] = {0};
	int ret = platform_read(0x28, raw, 6);

	if (ret < 0) {
		return ret;
	}

	*x_mg = (int16_t)((raw[1] << 8) | raw[0]);
	*y_mg = (int16_t)((raw[3] << 8) | raw[2]);
	*z_mg = (int16_t)((raw[5] << 8) | raw[4]);
	return 0;
}

int lis2dh12_zephyr_read_temp_c(struct lis2dh12_dev *dev, int8_t *temp_c)
{
	if (!dev || !dev->initialized || !temp_c) {
		return -EINVAL;
	}

	uint8_t temp_l = 0;
	uint8_t temp_h = 0;
	int ret = platform_read(0x0C, &temp_l, 1);

	ret |= platform_read(0x0D, &temp_h, 1);
	if (ret < 0) {
		return ret;
	}

	int16_t lsb = ((int16_t)temp_h << 8) | temp_l;

	*temp_c = (int8_t)lis2dh12_from_lsb_lp_to_celsius(lsb);
	return 0;
}

int lis2dh12_zephyr_config_motion(struct lis2dh12_dev *dev, uint8_t threshold, uint8_t duration)
{
	if (!dev || !dev->initialized) {
		return -EINVAL;
	}

	uint8_t ctrl1 = 0x57;    /* 100Hz XYZ on */
	uint8_t ctrl2 = 0x09;    /* HP filter on INT1 */
	uint8_t ctrl3 = 0x40;    /* route activity interrupt to INT1 */
	uint8_t ctrl4 = 0x00;    /* +-2g */
	uint8_t ctrl5 = 0x00;    /* non-latched INT1 */
	uint8_t int1_cfg = 0x2A; /* XH/YH/ZH OR */
	uint8_t reference = 0;
	uint8_t int1_src = 0;

	int ret = 0;

	ret |= platform_write(0x20, &ctrl1, 1);
	ret |= platform_write(0x21, &ctrl2, 1);
	ret |= platform_write(0x22, &ctrl3, 1);
	ret |= platform_write(0x23, &ctrl4, 1);
	ret |= platform_write(0x24, &ctrl5, 1);
	ret |= platform_write(0x32, &threshold, 1);
	ret |= platform_write(0x33, &duration, 1);
	ret |= platform_read(0x26, &reference, 1);
	ret |= platform_write(0x30, &int1_cfg, 1);
	ret |= platform_read(0x31, &int1_src, 1);
	if (ret < 0) {
		LOG_ERR("Failed motion setup");
		return ret;
	}

	LOG_INF("LIS2DH12 motion configured threshold=%u duration=%u", threshold, duration);
	return 0;
}

int lis2dh12_zephyr_power_down(struct lis2dh12_dev *dev)
{
	if (!dev || !dev->initialized) {
		return -EINVAL;
	}

	uint8_t ctrl1 = 0;
	int ret = platform_read(0x20, &ctrl1, 1);

	if (ret < 0) {
		return ret;
	}

	ctrl1 &= 0x0FU; /* ODR[3:0] = 0000: power-down mode. */
	ret = platform_write(0x20, &ctrl1, 1);
	if (ret < 0) {
		LOG_ERR("Failed LIS2DH12 power-down");
		return ret;
	}

	LOG_INF("LIS2DH12 powered down");
	return 0;
}

int lis2dh12_zephyr_read_int1_src(struct lis2dh12_dev *dev, uint8_t *int1_src)
{
	if (!dev || !dev->initialized || !int1_src) {
		return -EINVAL;
	}

	return platform_read(0x31, int1_src, 1);
}
