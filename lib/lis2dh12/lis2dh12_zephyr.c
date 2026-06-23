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

int lis2dh12_zephyr_check(struct lis2dh12_dev *dev)
{
	uint8_t whoami = 0;
	int ret;

	if (!dev) {
		return -EINVAL;
	}

	dev->bus.bus = DEVICE_DT_GET(DT_ALIAS(i2c_bus));
	dev->bus.addr = LIS2DH12_I2C_ADDR;

	if (!device_is_ready(dev->bus.bus)) {
		LOG_ERR("LIS2DH12 check FAIL: I2C bus not ready");
		return -ENODEV;
	}

	dev->initialized = true;
	g_dev = dev;

	ret = lis2dh12_zephyr_read_whoami(dev, &whoami);
	if (ret < 0) {
		LOG_ERR("LIS2DH12 check FAIL: WHO_AM_I read (%d)", ret);
		dev->initialized = false;
		return ret;
	}

	if (whoami != 0x33U) {
		LOG_ERR("LIS2DH12 check FAIL: unexpected WHO_AM_I=0x%02x", whoami);
		dev->initialized = false;
		return -ENODEV;
	}

	LOG_INF("LIS2DH12 check PASS WHO_AM_I=0x33 I2C addr=0x%02x", LIS2DH12_I2C_ADDR);
	return 0;
}

int lis2dh12_zephyr_init(struct lis2dh12_dev *dev)
{
	uint8_t whoami = 0;
	int ret;

	if (!dev) {
		return -EINVAL;
	}

	ret = lis2dh12_zephyr_check(dev);
	if (ret < 0) {
		return ret;
	}

	ret = lis2dh12_zephyr_read_whoami(dev, &whoami);
	if (ret < 0) {
		return ret;
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

#define LIS2DH12_REG_CTRL2 0x21U
#define LIS2DH12_REG_CTRL3 0x22U
#define LIS2DH12_REG_INT2_CFG 0x34U
#define LIS2DH12_REG_INT2_SRC 0x35U
#define LIS2DH12_REG_CTRL5 0x24U
#define LIS2DH12_CTRL5_LIR_INT2 0x02U /* Latch INT2 until INT2_SRC read. */
#define LIS2DH12_INT2_6D_CFG 0x7FU /* 6D orientation on all axes */
/*
 * CTRL_REG2 layout: bit0 HP_IA1, bit1 HP_IA2, bit2 HPCLICK, bit3 FDS,
 * bits4-5 HPCF, bits6-7 HPM.
 * FDS=1 sends HP-filtered data to OUT registers (near-zero at rest) — bad for 6D/Z.
 * Use HP_IA1 only (0x01) so INT1 motion sees HP while OUT stays unfiltered.
 */
#define LIS2DH12_CTRL2_HP_INT1 0x01U
#define LIS2DH12_CTRL2_NONE 0x00U

int lis2dh12_zephyr_config_motion(struct lis2dh12_dev *dev, uint8_t threshold, uint8_t duration)
{
	if (!dev || !dev->initialized) {
		return -EINVAL;
	}

	uint8_t ctrl1 = 0x57;    /* 100Hz XYZ on */
	uint8_t ctrl2 = LIS2DH12_CTRL2_HP_INT1;
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

	LOG_INF("LIS2DH12 motion configured threshold=%u duration=%u CTRL2=0x%02x", threshold,
		duration, ctrl2);
	return 0;
}

static int lis2dh12_zephyr_apply_int2_orientation(struct lis2dh12_dev *dev, uint8_t ctrl1,
						 uint8_t ctrl2, uint8_t ctrl3, uint8_t int1_cfg)
{
	uint8_t ctrl5 = LIS2DH12_CTRL5_LIR_INT2;
	uint8_t int2_cfg = LIS2DH12_INT2_6D_CFG;
	uint8_t int2_src = 0;
	int ret;

	ret = platform_write(0x20, &ctrl1, 1);
	ret |= platform_write(0x21, &ctrl2, 1);
	ret |= platform_write(LIS2DH12_REG_CTRL3, &ctrl3, 1);
	ret |= platform_write(LIS2DH12_REG_CTRL5, &ctrl5, 1);
	ret |= platform_write(0x30, &int1_cfg, 1);
	ret |= platform_write(LIS2DH12_REG_INT2_CFG, &int2_cfg, 1);
	ret |= platform_read(LIS2DH12_REG_INT2_SRC, &int2_src, 1);
	if (ret < 0) {
		return ret;
	}

	return (int)int2_src;
}

int lis2dh12_zephyr_config_orientation_int2(struct lis2dh12_dev *dev)
{
	uint8_t ctrl3 = 0;
	uint8_t int2_src = 0;
	uint8_t int2_cfg = LIS2DH12_INT2_6D_CFG;
	int ret;

	if (!dev || !dev->initialized) {
		return -EINVAL;
	}

	ret = platform_read(LIS2DH12_REG_CTRL3, &ctrl3, 1);
	if (ret < 0) {
		return ret;
	}

	ctrl3 |= 0x80U; /* I2_IA2: route INT2 generic to INT2 pin. */
	ret = platform_write(LIS2DH12_REG_CTRL3, &ctrl3, 1);
	ret |= platform_write(LIS2DH12_REG_INT2_CFG, &int2_cfg, 1);
	ret |= platform_read(LIS2DH12_REG_INT2_SRC, &int2_src, 1);
	if (ret < 0) {
		LOG_ERR("Failed INT2 6D setup");
		return ret;
	}

	LOG_INF("LIS2DH12 INT2 6D configured (IA2->P0.15) INT2_SRC=0x%02x", int2_src);
	return 0;
}

int lis2dh12_zephyr_config_motion_and_orientation(struct lis2dh12_dev *dev, uint8_t threshold,
						  uint8_t duration)
{
	if (!dev || !dev->initialized) {
		return -EINVAL;
	}

	uint8_t ctrl1 = 0x57;    /* 100Hz XYZ on */
	uint8_t ctrl2 = LIS2DH12_CTRL2_HP_INT1;
	uint8_t ctrl3 = 0xC0;    /* I1_IA1 + I2_IA2 */
	uint8_t ctrl4 = 0x00;    /* +-2g */
	uint8_t int1_cfg = 0x2A; /* XH/YH/ZH OR */
	uint8_t reference = 0;
	uint8_t int1_src = 0;
	int int2_src;
	int ret = 0;

	ret |= platform_write(0x23, &ctrl4, 1);
	ret |= platform_write(0x32, &threshold, 1);
	ret |= platform_write(0x33, &duration, 1);
	ret |= platform_read(0x26, &reference, 1);
	ret |= platform_read(0x31, &int1_src, 1);
	int2_src = lis2dh12_zephyr_apply_int2_orientation(dev, ctrl1, ctrl2, ctrl3, int1_cfg);
	ret |= (int2_src < 0) ? int2_src : 0;
	if (ret < 0) {
		LOG_ERR("Failed motion+orientation setup");
		return ret;
	}

	LOG_INF("LIS2DH12 motion+INT2 6D configured ths=%u CTRL2=0x%02x INT2_SRC=0x%02x", threshold,
		ctrl2, (uint8_t)int2_src);
	return 0;
}

int lis2dh12_zephyr_enter_shelf(struct lis2dh12_dev *dev)
{
	if (!dev || !dev->initialized) {
		return -EINVAL;
	}

	uint8_t ctrl1 = 0x17; /* 1 Hz XYZ on */
	uint8_t ctrl3 = 0x80; /* INT2 only */
	int int2_src;
	int ret;

	int2_src = lis2dh12_zephyr_apply_int2_orientation(dev, ctrl1, LIS2DH12_CTRL2_NONE, ctrl3,
							0x00);
	ret = (int2_src < 0) ? int2_src : 0;
	if (ret < 0) {
		LOG_ERR("Failed LIS2DH12 shelf mode");
		return ret;
	}

	LOG_INF("LIS2DH12 shelf mode (1Hz, INT2 6D only) INT2_SRC=0x%02x", (uint8_t)int2_src);
	return 0;
}

int lis2dh12_zephyr_is_face_up(struct lis2dh12_dev *dev, int16_t z_min_mg)
{
	int16_t x_mg = 0;
	int16_t y_mg = 0;
	int16_t z_mg = 0;
	int ret;

	if (!dev || !dev->initialized) {
		return -EINVAL;
	}

	ret = lis2dh12_zephyr_read_accel_mg(dev, &x_mg, &y_mg, &z_mg);
	if (ret < 0) {
		return ret;
	}

	/* Face-up on shelf: +Z ~ +17000 (raw LSB, bench 2025). */
	return (z_mg > z_min_mg) ? 1 : 0;
}

int lis2dh12_zephyr_is_face_down(struct lis2dh12_dev *dev, int16_t z_max_mg)
{
	int16_t x_mg = 0;
	int16_t y_mg = 0;
	int16_t z_mg = 0;
	int ret;

	if (!dev || !dev->initialized) {
		return -EINVAL;
	}

	ret = lis2dh12_zephyr_read_accel_mg(dev, &x_mg, &y_mg, &z_mg);
	if (ret < 0) {
		return ret;
	}

	/* Face-down / worn run: Z ~ -16000 (raw LSB, bench 2025). */
	return (z_mg < z_max_mg) ? 1 : 0;
}

#define LIS2DH12_6D_AXIS_MG 8000

int lis2dh12_zephyr_read_6d_position(struct lis2dh12_dev *dev, uint8_t *position)
{
	int16_t x_mg = 0;
	int16_t y_mg = 0;
	int16_t z_mg = 0;
	int ret;

	if (!dev || !dev->initialized || !position) {
		return -EINVAL;
	}

	ret = lis2dh12_zephyr_read_accel_mg(dev, &x_mg, &y_mg, &z_mg);
	if (ret < 0) {
		return ret;
	}

	*position = 0U;
	if (x_mg < -LIS2DH12_6D_AXIS_MG) {
		*position |= 0x01U;
	}
	if (x_mg > LIS2DH12_6D_AXIS_MG) {
		*position |= 0x02U;
	}
	if (y_mg < -LIS2DH12_6D_AXIS_MG) {
		*position |= 0x04U;
	}
	if (y_mg > LIS2DH12_6D_AXIS_MG) {
		*position |= 0x08U;
	}
	if (z_mg < -LIS2DH12_6D_AXIS_MG) {
		*position |= 0x10U;
	}
	if (z_mg > LIS2DH12_6D_AXIS_MG) {
		*position |= 0x20U;
	}

	return 0;
}

int lis2dh12_zephyr_shelf_capture_baseline(struct lis2dh12_dev *dev, uint8_t *baseline)
{
	uint8_t int2_src = 0;
	int ret;

	if (!dev || !baseline) {
		return -EINVAL;
	}

	/* Allow one 1 Hz ODR sample after shelf register setup. */
	k_sleep(K_MSEC(1100));

	ret = lis2dh12_zephyr_read_6d_position(dev, baseline);
	if (ret < 0) {
		return ret;
	}

	ret = lis2dh12_zephyr_read_int2_src(dev, &int2_src);
	if (ret < 0) {
		return ret;
	}

	LOG_INF("LIS2DH12 shelf baseline 6D=0x%02x INT2_SRC=0x%02x", *baseline, int2_src);
	return 0;
}

int lis2dh12_zephyr_int2_left_shelf_position(struct lis2dh12_dev *dev, uint8_t baseline,
					     uint8_t int2_src)
{
	uint8_t current = int2_src & 0x3FU;
	int ret;

	if (!dev || !dev->initialized) {
		return -EINVAL;
	}

	if ((int2_src & 0x40U) == 0U) {
		return 0;
	}

	if (current == 0U) {
		ret = lis2dh12_zephyr_read_6d_position(dev, &current);
		if (ret < 0) {
			return ret;
		}
	}

	/* 6D sector change (~90 deg from face-up resting pose) wakes shelf. */
	return (current != baseline) ? 1 : 0;
}

int lis2dh12_zephyr_read_int2_src(struct lis2dh12_dev *dev, uint8_t *int2_src)
{
	if (!dev || !dev->initialized || !int2_src) {
		return -EINVAL;
	}

	return platform_read(LIS2DH12_REG_INT2_SRC, int2_src, 1);
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
