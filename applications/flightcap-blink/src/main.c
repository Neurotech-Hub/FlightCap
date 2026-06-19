#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include "vbatt_zephyr.h"

LOG_MODULE_REGISTER(flightcap_blink, LOG_LEVEL_INF);

#define BLINK_PERIOD_MS 200U
#define VBATT_LOG_PERIOD_MS 5000U

static const struct gpio_dt_spec led0 = GPIO_DT_SPEC_GET(DT_ALIAS(led0), gpios);
static const struct gpio_dt_spec led1 = GPIO_DT_SPEC_GET(DT_ALIAS(led1), gpios);
static const struct gpio_dt_spec magnet = GPIO_DT_SPEC_GET(DT_ALIAS(magnet_sensor), gpios);

static void leds_set(int value)
{
	(void)gpio_pin_set_dt(&led0, value);
	(void)gpio_pin_set_dt(&led1, value);
}

int main(void)
{
	int ret;
	int magnet_state = 0;
	int previous_magnet_state = -1;
	bool led_blink_state = false;
	uint32_t last_vbatt_log_ms = 0;

	if (!gpio_is_ready_dt(&led0) || !gpio_is_ready_dt(&led1) ||
	    !gpio_is_ready_dt(&magnet)) {
		LOG_ERR("GPIOs not ready (LED0, LED1, or magnet input)");
		return -ENODEV;
	}

	ret = gpio_pin_configure_dt(&led0, GPIO_OUTPUT_INACTIVE);
	ret |= gpio_pin_configure_dt(&led1, GPIO_OUTPUT_INACTIVE);
	ret |= gpio_pin_configure_dt(&magnet, GPIO_INPUT);
	if (ret != 0) {
		LOG_ERR("GPIO configuration failed (%d)", ret);
		return ret;
	}

	ret = vbatt_init();
	if (ret != 0) {
		LOG_ERR("VDD ADC init failed (%d)", ret);
		return ret;
	}

	LOG_INF("FlightCap blink bring-up started");
	LOG_INF("Behavior: 200 ms blink on LED0+LED1 when magnet absent, solid ON when present");

	while (1) {
		int32_t vdd_mv = 0;
		uint32_t now_ms = k_uptime_get_32();

		magnet_state = gpio_pin_get_dt(&magnet);
		if (magnet_state < 0) {
			LOG_ERR("MAG_INT read failed (%d)", magnet_state);
			return magnet_state;
		}

		if (magnet_state != previous_magnet_state) {
			LOG_INF("MAG_INT is %s",
				magnet_state ? "ACTIVE (magnet present, LEDs ON)"
					     : "inactive (magnet absent, blink mode)");
			previous_magnet_state = magnet_state;
		}

		if ((now_ms - last_vbatt_log_ms) >= VBATT_LOG_PERIOD_MS) {
			if (vbatt_read_mv(&vdd_mv) == 0) {
				LOG_INF("vdd_mv=%d", (int)vdd_mv);
			}
			last_vbatt_log_ms = now_ms;
		}

		if (magnet_state) {
			leds_set(1);
			k_sleep(K_MSEC(50));
		} else {
			led_blink_state = !led_blink_state;
			leds_set(led_blink_state ? 1 : 0);
			k_sleep(K_MSEC(BLINK_PERIOD_MS));
		}
	}

	return 0;
}
