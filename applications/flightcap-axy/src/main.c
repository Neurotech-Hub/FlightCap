#include <zephyr/device.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/irq.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <hal/nrf_gpio.h>

#include "lis2dh12_zephyr.h"

LOG_MODULE_REGISTER(flightcap_axy, LOG_LEVEL_INF);

#define MAGNET_NODE DT_ALIAS(magnet_sensor)
#define LED_NODE DT_ALIAS(led0)
#define ACCEL_INT_NODE DT_ALIAS(accel_int)

#define SAMPLE_PERIOD_MS 1000
#define MOTION_THRESHOLD_MG 16
#define MOTION_DURATION_SAMPLES 0

static const struct gpio_dt_spec magnet = GPIO_DT_SPEC_GET(MAGNET_NODE, gpios);
static const struct gpio_dt_spec led = GPIO_DT_SPEC_GET(LED_NODE, gpios);
static const struct gpio_dt_spec accel_int = GPIO_DT_SPEC_GET(ACCEL_INT_NODE, gpios);
static const struct device *const gpio1_dev = DEVICE_DT_GET(DT_NODELABEL(gpio1));

static struct gpio_callback accel_int_cb;
static volatile uint32_t motion_events;
static struct lis2dh12_dev lis2dh12;

static int magnet_read_fallback(void)
{
	uint32_t absolute_pin = magnet.pin;

	if (magnet.port == gpio1_dev)
	{
		absolute_pin += 32U;
	}

	return nrf_gpio_pin_read(absolute_pin) ? 1 : 0;
}

static void accel_int_callback(const struct device *port, struct gpio_callback *cb, uint32_t pins)
{
	ARG_UNUSED(port);
	ARG_UNUSED(cb);
	ARG_UNUSED(pins);

	motion_events++;
}

int main(void)
{
	int ret;
	uint32_t sample_seq = 0;
	bool warned_magnet_fallback = false;

	if (!gpio_is_ready_dt(&led) || !gpio_is_ready_dt(&magnet) || !gpio_is_ready_dt(&accel_int))
	{
		LOG_ERR("GPIO dependency not ready");
		return -ENODEV;
	}

	ret = gpio_pin_configure_dt(&led, GPIO_OUTPUT_INACTIVE);
	ret |= gpio_pin_configure_dt(&magnet, GPIO_INPUT);
	ret |= gpio_pin_configure_dt(&accel_int, GPIO_INPUT);
	if (ret < 0)
	{
		LOG_ERR("GPIO configuration failed (%d)", ret);
		return ret;
	}

	ret = lis2dh12_zephyr_init(&lis2dh12);
	if (ret < 0)
	{
		LOG_ERR("LIS2DH12 init failed (%d)", ret);
		return ret;
	}

	ret = lis2dh12_zephyr_config_motion(&lis2dh12, MOTION_THRESHOLD_MG, MOTION_DURATION_SAMPLES);
	if (ret < 0)
	{
		LOG_ERR("LIS2DH12 motion config failed (%d)", ret);
		return ret;
	}

	gpio_init_callback(&accel_int_cb, accel_int_callback, BIT(accel_int.pin));
	ret = gpio_add_callback(accel_int.port, &accel_int_cb);
	ret |= gpio_pin_interrupt_configure_dt(&accel_int, GPIO_INT_EDGE_TO_ACTIVE);
	if (ret < 0)
	{
		LOG_ERR("INT1 callback setup failed (%d)", ret);
		return ret;
	}

	LOG_INF("AXY tester started: periodic=%dms, mode=SPI0, motion_ths=%u",
			SAMPLE_PERIOD_MS, MOTION_THRESHOLD_MG);

	while (1)
	{
		uint32_t period_motion;
		unsigned int irq_key = irq_lock();

		period_motion = motion_events;
		motion_events = 0U;
		irq_unlock(irq_key);

		int16_t x_mg = 0;
		int16_t y_mg = 0;
		int16_t z_mg = 0;
		int8_t temp_c = 0;
		uint8_t int1_src = 0;
		int magnet_state = gpio_pin_get_dt(&magnet);

		if (magnet_state < 0)
		{
			if (!warned_magnet_fallback)
			{
				LOG_WRN("MAG_INT read failed (%d), using fallback", magnet_state);
				warned_magnet_fallback = true;
			}
			magnet_state = magnet_read_fallback();
		}

		ret = lis2dh12_zephyr_read_accel_mg(&lis2dh12, &x_mg, &y_mg, &z_mg);
		ret |= lis2dh12_zephyr_read_temp_c(&lis2dh12, &temp_c);
		ret |= lis2dh12_zephyr_read_int1_src(&lis2dh12, &int1_src);

		if (ret < 0)
		{
			LOG_ERR("Sample read failed (ret=%d) motion_1s=%u", ret, period_motion);
		}
		else
		{
			LOG_INF("AXY t=%u seq=%u motion_1s=%u x_mg=%d y_mg=%d z_mg=%d temp_c=%d mag=%d int1=0x%02x",
					k_uptime_get_32(), sample_seq, period_motion, x_mg, y_mg, z_mg, temp_c,
					magnet_state, int1_src);
		}

		if (period_motion > 0U)
		{
			(void)gpio_pin_set_dt(&led, 1);
			k_sleep(K_MSEC(50));
			(void)gpio_pin_set_dt(&led, 0);
		}

		sample_seq++;
		k_sleep(K_MSEC(SAMPLE_PERIOD_MS));
	}

	return 0;
}
