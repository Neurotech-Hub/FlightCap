#include <errno.h>

#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include "flightcap_hw.h"
#include "lis2dh12_zephyr.h"
#include "prod_state.h"

LOG_MODULE_REGISTER(flightcap_prod, LOG_LEVEL_INF);

#define ACCEL_INT_NODE DT_ALIAS(accel_int)
#define ACCEL_INT2_NODE DT_ALIAS(accel_int2)
#define MAGNET_NODE DT_ALIAS(magnet_sensor)
#define LED0_NODE DT_ALIAS(led0)
#define LED1_NODE DT_ALIAS(led1)
#define I2C_NODE DT_ALIAS(i2c_bus)

#define MOTION_THRESHOLD_MG 3
#define MOTION_DURATION_SAMPLES 0

static const struct gpio_dt_spec led0 = GPIO_DT_SPEC_GET(LED0_NODE, gpios);
static const struct gpio_dt_spec led1 = GPIO_DT_SPEC_GET(LED1_NODE, gpios);
static const struct gpio_dt_spec magnet = GPIO_DT_SPEC_GET(MAGNET_NODE, gpios);
static const struct gpio_dt_spec accel_int = GPIO_DT_SPEC_GET(ACCEL_INT_NODE, gpios);
static const struct gpio_dt_spec accel_int2 = GPIO_DT_SPEC_GET(ACCEL_INT2_NODE, gpios);
static const struct device *const i2c_dev = DEVICE_DT_GET(I2C_NODE);
static const struct device *tof_dev;

static struct gpio_callback accel_int_cb;
static struct gpio_callback accel_int2_cb;
static volatile uint32_t interactions;
static volatile bool orientation_irq_pending;
static struct lis2dh12_dev lis2dh12;

static void accel_int_callback(const struct device *port, struct gpio_callback *cb, uint32_t pins)
{
	ARG_UNUSED(port);
	ARG_UNUSED(cb);
	ARG_UNUSED(pins);

	interactions++;
}

static void accel_int2_callback(const struct device *port, struct gpio_callback *cb, uint32_t pins)
{
	ARG_UNUSED(port);
	ARG_UNUSED(cb);
	ARG_UNUSED(pins);

	orientation_irq_pending = true;
}

int main(void)
{
	int ret;
	struct prod_context ctx = {
		.lis2dh = &lis2dh12,
		.tof_dev = NULL,
		.led0 = &led0,
		.led1 = &led1,
		.magnet = &magnet,
		.accel_int = &accel_int,
		.accel_int2 = &accel_int2,
		.accel_int_cb = &accel_int_cb,
		.accel_int2_cb = &accel_int2_cb,
		.interactions = &interactions,
		.orientation_irq_pending = &orientation_irq_pending,
		.pair_active = false,
		.dfu_active = false,
		.bt_was_disabled = false,
		.cycle = 0U,
	};

	if (!gpio_is_ready_dt(&led0) || !gpio_is_ready_dt(&led1) ||
	    !gpio_is_ready_dt(&magnet) || !gpio_is_ready_dt(&accel_int) ||
	    !gpio_is_ready_dt(&accel_int2)) {
		LOG_ERR("GPIO dependency not ready");
		return -ENODEV;
	}

	if (!device_is_ready(i2c_dev)) {
		LOG_ERR("I2C bus not ready");
		return -ENODEV;
	}

	ret = gpio_pin_configure_dt(&led0, GPIO_OUTPUT_INACTIVE);
	ret |= gpio_pin_configure_dt(&led1, GPIO_OUTPUT_INACTIVE);
	ret |= gpio_pin_configure_dt(&magnet, GPIO_INPUT);
	if (ret < 0) {
		LOG_ERR("GPIO configuration failed (%d)", ret);
		return ret;
	}

	{
		struct flightcap_hw_status hw = {0};

		(void)flightcap_hw_check(FLIGHTCAP_HW_VBATT | FLIGHTCAP_HW_ACCEL | FLIGHTCAP_HW_TOF,
					 &hw);
		if (hw.tof_ok) {
			tof_dev = DEVICE_DT_GET(DT_ALIAS(tof_sensor));
			ctx.tof_dev = tof_dev;
			LOG_INF("VL53L0X ready at boot (probe_mm=%u)", hw.tof_probe_mm);
		} else {
			LOG_WRN("VL53L0X not available — distance invalid in beacon");
		}
	}

	ret = gpio_pin_configure_dt(&accel_int, GPIO_INPUT);
	ret |= gpio_pin_configure_dt(&accel_int2, GPIO_INPUT);
	if (ret < 0) {
		LOG_ERR("accel INT GPIO configure failed (%d)", ret);
		return ret;
	}

	ret = lis2dh12_zephyr_init(&lis2dh12);
	if (ret < 0) {
		LOG_ERR("LIS2DH12 init failed (%d)", ret);
		return ret;
	}

	ret = lis2dh12_zephyr_config_motion_and_orientation(&lis2dh12, MOTION_THRESHOLD_MG,
							    MOTION_DURATION_SAMPLES);
	if (ret < 0) {
		LOG_ERR("LIS2DH12 motion+orientation config failed (%d)", ret);
		return ret;
	}

	gpio_init_callback(&accel_int_cb, accel_int_callback, BIT(accel_int.pin));
	ret = gpio_add_callback(accel_int.port, &accel_int_cb);
	ret |= gpio_pin_interrupt_configure_dt(&accel_int, GPIO_INT_EDGE_TO_ACTIVE);
	if (ret < 0) {
		LOG_ERR("INT1 callback setup failed (%d)", ret);
		return ret;
	}

	gpio_init_callback(&accel_int2_cb, accel_int2_callback, BIT(accel_int2.pin));
	ret = gpio_add_callback(accel_int2.port, &accel_int2_cb);
	ret |= gpio_pin_interrupt_configure_dt(&accel_int2, GPIO_INT_EDGE_TO_ACTIVE);
	if (ret < 0) {
		LOG_ERR("INT2 callback setup failed (%d)", ret);
		return ret;
	}

	ret = bt_enable(NULL);
	if (ret != 0) {
		LOG_ERR("bt_enable failed (%d)", ret);
		return ret;
	}

	LOG_INF("FlightCap prod state machine starting");
	prod_run(&ctx);

	return 0;
}
