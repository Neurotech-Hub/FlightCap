#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <hal/nrf_gpio.h>

LOG_MODULE_REGISTER(flightcap_blink, LOG_LEVEL_INF);

#define BLINK_PERIOD_MS 200U

static const struct gpio_dt_spec led = GPIO_DT_SPEC_GET(DT_ALIAS(led0), gpios);
static const struct gpio_dt_spec magnet = GPIO_DT_SPEC_GET(DT_ALIAS(magnet_sensor), gpios);
static const struct device *const gpio1_dev = DEVICE_DT_GET(DT_NODELABEL(gpio1));

static int magnet_read_fallback(void)
{
	uint32_t absolute_pin = magnet.pin;

	if (magnet.port == gpio1_dev) {
		absolute_pin += 32U;
	}

	return nrf_gpio_pin_read(absolute_pin) ? 1 : 0;
}

int main(void)
{
	int ret;
	int magnet_state = 0;
	int previous_magnet_state = -1;
	bool led_blink_state = false;
	bool warned_fallback = false;

	if (!gpio_is_ready_dt(&led) || !gpio_is_ready_dt(&magnet)) {
		LOG_ERR("GPIOs not ready (LED or magnet input)");
		return -ENODEV;
	}

	ret = gpio_pin_configure_dt(&led, GPIO_OUTPUT_INACTIVE);
	if (ret != 0) {
		LOG_ERR("Failed to configure LED0 (%d)", ret);
		return ret;
	}

	ret = gpio_pin_configure_dt(&magnet, GPIO_INPUT);
	if (ret != 0) {
		LOG_ERR("Failed to configure MAG_INT (%d)", ret);
		return ret;
	}

	LOG_INF("FlightCap blink bring-up started");
	LOG_INF("Behavior: 200 ms blink when MAG_INT low, solid ON when MAG_INT high");
	LOG_INF("LED0 mapped to P%d.%02d", (led.port == gpio1_dev) ? 1 : 0, led.pin);

	while (1) {
		magnet_state = gpio_pin_get_dt(&magnet);
		if (magnet_state < 0) {
			if (!warned_fallback) {
				LOG_WRN("gpio_pin_get_dt(MAG_INT) failed (%d), using HAL fallback",
					magnet_state);
				warned_fallback = true;
			}
			magnet_state = magnet_read_fallback();
		}

		if (magnet_state != previous_magnet_state) {
			LOG_INF("MAG_INT is %s", magnet_state ? "HIGH (LED forced ON)" : "LOW (blink mode)");
			previous_magnet_state = magnet_state;
		}

		if (magnet_state) {
			(void)gpio_pin_set_dt(&led, 1);
			k_sleep(K_MSEC(50));
		} else {
			led_blink_state = !led_blink_state;
			(void)gpio_pin_set_dt(&led, led_blink_state ? 1 : 0);
			k_sleep(K_MSEC(BLINK_PERIOD_MS));
		}
	}

	return 0;
}
