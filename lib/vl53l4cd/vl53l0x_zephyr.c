#include "vl53l0x_zephyr.h"

#include <errno.h>

#include <zephyr/devicetree.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(vl53l0x_zephyr, LOG_LEVEL_INF);

#define TOF_NODE DT_ALIAS(tof_sensor)
#define TOF_EN_NODE DT_ALIAS(tof_enable)

/* TPS63900 soft-start + VL53L0X tBOOT after EN asserted. */
#define TOF_POWER_SETTLE_MS 10U

#if IS_ENABLED(CONFIG_VL53L0X) && DT_NODE_EXISTS(TOF_NODE)

#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/sensor.h>
#include <zephyr/init.h>

#if DT_NODE_EXISTS(TOF_EN_NODE)
static const struct gpio_dt_spec tof_en = GPIO_DT_SPEC_GET(TOF_EN_NODE, gpios);

static int tof_power_set(bool enable)
{
	int ret;

	if (!gpio_is_ready_dt(&tof_en)) {
		return -ENODEV;
	}

	ret = gpio_pin_set_dt(&tof_en, enable ? 1 : 0);
	if (ret < 0) {
		return ret;
	}

	if (enable) {
		k_sleep(K_MSEC(TOF_POWER_SETTLE_MS));
	}

	return 0;
}

/*
 * Assert TOF_EN before the Zephyr VL53L0X driver inits (default
 * CONFIG_SENSOR_INIT_PRIORITY is 90). Priority must be a plain integer
 * literal — CONFIG_SENSOR_INIT_PRIORITY - 1 breaks the linker in NCS 3.0.
 */
#define TOF_POWER_INIT_PRIO 50

static int tof_power_pre_sensor_init(void)
{
	int ret;

	ret = gpio_pin_configure_dt(&tof_en, GPIO_OUTPUT_INACTIVE);
	if (ret < 0) {
		return ret;
	}

	return tof_power_set(true);
}

SYS_INIT(tof_power_pre_sensor_init, POST_KERNEL, TOF_POWER_INIT_PRIO);
#endif /* DT_NODE_EXISTS(TOF_EN_NODE) */

int vl53l0x_zephyr_power_on(void)
{
#if DT_NODE_EXISTS(TOF_EN_NODE)
	return tof_power_set(true);
#else
	return 0;
#endif
}

int vl53l0x_zephyr_power_off(void)
{
#if DT_NODE_EXISTS(TOF_EN_NODE)
	return tof_power_set(false);
#else
	return 0;
#endif
}

int vl53l0x_zephyr_read_mm(const struct device *dev, uint16_t *mm_out)
{
	struct sensor_value dist;
	int ret;

	if (!dev || !mm_out) {
		return -EINVAL;
	}

	ret = sensor_sample_fetch(dev);
	if (ret != 0) {
		return ret;
	}

	ret = sensor_channel_get(dev, SENSOR_CHAN_DISTANCE, &dist);
	if (ret != 0) {
		return ret;
	}

	int32_t mm = dist.val1 * 1000 + dist.val2 / 1000;

	if (mm <= 0 || mm > UINT16_MAX) {
		return -EINVAL;
	}

	*mm_out = (uint16_t)mm;
	return 0;
}

int vl53l0x_zephyr_check(const struct device **dev_out, uint16_t *probe_mm_out)
{
	const struct device *dev = DEVICE_DT_GET(TOF_NODE);
	uint16_t probe_mm = 0;
	int ret;

	if (dev_out) {
		*dev_out = NULL;
	}

	ret = vl53l0x_zephyr_power_on();
	if (ret < 0) {
		LOG_ERR("VL53L0X check FAIL: power on (%d)", ret);
		return ret;
	}

	if (!device_is_ready(dev)) {
		LOG_ERR("VL53L0X check FAIL: device not ready");
		(void)vl53l0x_zephyr_power_off();
		return -ENODEV;
	}

	ret = vl53l0x_zephyr_read_mm(dev, &probe_mm);
	if (ret < 0) {
		LOG_ERR("VL53L0X check FAIL: probe read (%d)", ret);
		(void)vl53l0x_zephyr_power_off();
		return ret;
	}

	if (dev_out) {
		*dev_out = dev;
	}
	if (probe_mm_out) {
		*probe_mm_out = probe_mm;
	}

	LOG_INF("VL53L0X check PASS probe_mm=%u", probe_mm);
	return 0;
}

#else /* !CONFIG_VL53L0X */

int vl53l0x_zephyr_power_on(void)
{
	return -ENODEV;
}

int vl53l0x_zephyr_power_off(void)
{
	return -ENODEV;
}

int vl53l0x_zephyr_read_mm(const struct device *dev, uint16_t *mm_out)
{
	ARG_UNUSED(dev);
	ARG_UNUSED(mm_out);
	return -ENODEV;
}

int vl53l0x_zephyr_check(const struct device **dev_out, uint16_t *probe_mm_out)
{
	ARG_UNUSED(dev_out);
	ARG_UNUSED(probe_mm_out);
	LOG_ERR("VL53L0X check FAIL: CONFIG_VL53L0X not enabled");
	return -ENODEV;
}

#endif /* IS_ENABLED(CONFIG_VL53L0X) */
