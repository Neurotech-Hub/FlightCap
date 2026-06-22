#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include "flightcap_hw.h"
#include "spi_nor_zephyr.h"
#include "vbatt_zephyr.h"

LOG_MODULE_REGISTER(flightcap_mem, LOG_LEVEL_INF);

#define SAMPLE_PERIOD_MS 1000U
#define LED_PULSE_MS 50U

static const struct gpio_dt_spec led0 = GPIO_DT_SPEC_GET(DT_ALIAS(led0), gpios);
static const struct gpio_dt_spec led1 = GPIO_DT_SPEC_GET(DT_ALIAS(led1), gpios);

static void leds_set(int value)
{
	(void)gpio_pin_set_dt(&led0, value);
	(void)gpio_pin_set_dt(&led1, value);
}

int main(void)
{
	struct flightcap_hw_status hw = {0};
	const struct device *flash = NULL;
	bool mem_pass = false;
	int ret;

	if (!gpio_is_ready_dt(&led0) || !gpio_is_ready_dt(&led1)) {
		LOG_ERR("LED GPIOs not ready");
		return -ENODEV;
	}

	ret = gpio_pin_configure_dt(&led0, GPIO_OUTPUT_INACTIVE);
	ret |= gpio_pin_configure_dt(&led1, GPIO_OUTPUT_INACTIVE);
	if (ret < 0) {
		LOG_ERR("LED GPIO configuration failed (%d)", ret);
		return ret;
	}

	ret = flightcap_hw_check(FLIGHTCAP_HW_VBATT | FLIGHTCAP_HW_FLASH, &hw);
	if (hw.flash_ok) {
		flash = DEVICE_DT_GET(DT_ALIAS(spi_flash));
		ret = spi_nor_zephyr_verify(flash);
	} else {
		ret = -EIO;
	}

	mem_pass = (ret == 0);
	if (mem_pass) {
		LOG_INF("MEM verification: PASS");
	} else {
		LOG_ERR("MEM verification: FAIL (%d)", ret);
	}

	while (1) {
		int32_t vdd_mv = 0;

		(void)vbatt_read_mv(&vdd_mv);
		LOG_INF("MEM status=%s vdd_mv=%d", mem_pass ? "PASS" : "FAIL", (int)vdd_mv);

		if (mem_pass) {
			leds_set(1);
			k_sleep(K_MSEC(LED_PULSE_MS));
			leds_set(0);
		}

		k_sleep(K_MSEC(SAMPLE_PERIOD_MS));
	}

	return 0;
}
