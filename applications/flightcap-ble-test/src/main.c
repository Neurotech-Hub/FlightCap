/*
 * FlightCap BLE bring-up: GATT LED, magnet-wake deep sleep emulation (system off).
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <errno.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/conn.h>
#include <zephyr/bluetooth/gatt.h>
#include <zephyr/bluetooth/hci.h>
#include <zephyr/bluetooth/uuid.h>
#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/sys/poweroff.h>
#include <zephyr/sys/util.h>

#include "vbatt_zephyr.h"

LOG_MODULE_REGISTER(flightcap_ble_test, LOG_LEVEL_INF);

/* Custom 128-bit UUIDs (test-only). */
#define BT_UUID_FLIGHTCAP_LED_SVC_VAL                                                              \
	BT_UUID_128_ENCODE(0xad2a98a4, 0x2148, 0x4b58, 0x9e14, 0x7e2cbb6c7a01)
#define BT_UUID_FLIGHTCAP_LED_CHR_VAL                                                              \
	BT_UUID_128_ENCODE(0xad2a98a4, 0x2148, 0x4b58, 0x9e14, 0x7e2cbb6c7a02)
#define BT_UUID_FLIGHTCAP_SLEEP_CHR_VAL                                                            \
	BT_UUID_128_ENCODE(0xad2a98a4, 0x2148, 0x4b58, 0x9e14, 0x7e2cbb6c7a04)

static const struct bt_uuid_128 flightcap_led_svc_uuid =
	BT_UUID_INIT_128(BT_UUID_FLIGHTCAP_LED_SVC_VAL);
static const struct bt_uuid_128 flightcap_led_chr_uuid =
	BT_UUID_INIT_128(BT_UUID_FLIGHTCAP_LED_CHR_VAL);
static const struct bt_uuid_128 flightcap_sleep_chr_uuid =
	BT_UUID_INIT_128(BT_UUID_FLIGHTCAP_SLEEP_CHR_VAL);

static const struct gpio_dt_spec led0 = GPIO_DT_SPEC_GET(DT_ALIAS(led0), gpios);
static const struct gpio_dt_spec led1 = GPIO_DT_SPEC_GET(DT_ALIAS(led1), gpios);
static const struct gpio_dt_spec magnet = GPIO_DT_SPEC_GET(DT_ALIAS(magnet_sensor), gpios);

static void leds_set(int value)
{
	(void)gpio_pin_set_dt(&led0, value);
	(void)gpio_pin_set_dt(&led1, value);
}

/** Backing value for GATT read (0 = off, 1 = on). */
static uint8_t led_gatt_value;

/** When cleared, disconnected_cb will not restart advertising (entering sleep path). */
static atomic_t reconnect_adv_after_disconnect = ATOMIC_INIT(1);

static ssize_t led_chr_read(struct bt_conn *conn, const struct bt_gatt_attr *attr, void *buf,
			    uint16_t len, uint16_t offset)
{
	const uint8_t *v = attr->user_data;

	return bt_gatt_attr_read(conn, attr, buf, len, offset, v, sizeof(uint8_t));
}

static ssize_t led_chr_write(struct bt_conn *conn, const struct bt_gatt_attr *attr,
			     const void *buf, uint16_t len, uint16_t offset, uint8_t flags)
{
	uint8_t *st = attr->user_data;

	if (offset != 0U) {
		return BT_GATT_ERR(BT_ATT_ERR_INVALID_OFFSET);
	}
	if (len < 1U) {
		return BT_GATT_ERR(BT_ATT_ERR_INVALID_ATTRIBUTE_LEN);
	}

	*st = ((const uint8_t *)buf)[0] ? 1U : 0U;
	leds_set(*st);
	LOG_INF("LED GATT write -> %u", *st);

	return len;
}

static void deep_sleep_run(struct k_work *work);

static K_WORK_DEFINE(deep_sleep_work, deep_sleep_run);

static ssize_t sleep_chr_write(struct bt_conn *conn, const struct bt_gatt_attr *attr,
			       const void *buf, uint16_t len, uint16_t offset, uint8_t flags)
{
	ARG_UNUSED(attr);
	ARG_UNUSED(conn);
	ARG_UNUSED(flags);

	if (offset != 0U) {
		return BT_GATT_ERR(BT_ATT_ERR_INVALID_OFFSET);
	}
	if (len < 1U) {
		return BT_GATT_ERR(BT_ATT_ERR_INVALID_ATTRIBUTE_LEN);
	}

	if (((const uint8_t *)buf)[0] != 1U) {
		return len;
	}

	LOG_INF("Deep sleep armed over GATT; stopping RF then System OFF until MAG wakes");
	k_work_submit(&deep_sleep_work);
	return len;
}

BT_GATT_SERVICE_DEFINE(
	flightcap_svc,
	BT_GATT_PRIMARY_SERVICE(&flightcap_led_svc_uuid.uuid),
	BT_GATT_CHARACTERISTIC(&flightcap_led_chr_uuid.uuid,
			       BT_GATT_CHRC_READ | BT_GATT_CHRC_WRITE |
				       BT_GATT_CHRC_WRITE_WITHOUT_RESP,
			       BT_GATT_PERM_READ | BT_GATT_PERM_WRITE, led_chr_read, led_chr_write,
			       &led_gatt_value),
	BT_GATT_CHARACTERISTIC(&flightcap_sleep_chr_uuid.uuid,
			       BT_GATT_CHRC_WRITE | BT_GATT_CHRC_WRITE_WITHOUT_RESP,
			       BT_GATT_PERM_WRITE, NULL, sleep_chr_write, NULL),
);

#define DEVICE_NAME     CONFIG_BT_DEVICE_NAME
#define DEVICE_NAME_LEN (sizeof(DEVICE_NAME) - 1U)

static const struct bt_data ad[] = {
	BT_DATA_BYTES(BT_DATA_FLAGS, (BT_LE_AD_GENERAL | BT_LE_AD_NO_BREDR)),
	BT_DATA_BYTES(BT_DATA_UUID128_ALL, BT_UUID_FLIGHTCAP_LED_SVC_VAL),
};

static const struct bt_data sd[] = {
	BT_DATA(BT_DATA_NAME_COMPLETE, DEVICE_NAME, DEVICE_NAME_LEN),
};

static int advertising_start(void)
{
	int err = bt_le_adv_start(BT_LE_ADV_CONN_FAST_1, ad, ARRAY_SIZE(ad), sd, ARRAY_SIZE(sd));

	if (err != 0) {
		LOG_ERR("Advertising failed (%d)", err);
	} else {
		LOG_INF("Advertising started (connectable)");
	}

	return err;
}

static void adv_restart_work_handler(struct k_work *work)
{
	ARG_UNUSED(work);
	(void)advertising_start();
}

static K_WORK_DEFINE(adv_restart_work, adv_restart_work_handler);

static void disconnect_conn_sink(struct bt_conn *conn, void *data)
{
	struct bt_conn_info info;
	int err;

	ARG_UNUSED(data);

	err = bt_conn_get_info(conn, &info);
	if (err != 0 || info.state != BT_CONN_STATE_CONNECTED) {
		return;
	}

	err = bt_conn_disconnect(conn, BT_HCI_ERR_REMOTE_POWER_OFF);
	if (err != 0 && err != -ENOTCONN) {
		LOG_WRN("bt_conn_disconnect (%d)", err);
	}
}

static void deep_sleep_run(struct k_work *work)
{
	int err;

	ARG_UNUSED(work);

	err = gpio_pin_configure_dt(&magnet, GPIO_INPUT);
	if (err != 0) {
		LOG_ERR("gpio magnet cfg (%d)", err);
		return;
	}

	/*
	 * DTS: MAG is GPIO_ACTIVE_LOW — magnet present pulls the pad electrically LOW,
	 * and gpio_pin_get_dt() is non-zero in that logical "active" state (same sense as blink).
	 * Sleep only while magnet is away (released / electrically high).
	 */
	if (gpio_pin_get_dt(&magnet) != 0) {
		LOG_WRN("Deep sleep cancelled: magnet present (line LOW)—remove magnet, retry `0x01`");
		return;
	}

	atomic_set(&reconnect_adv_after_disconnect, 0);

	(void)bt_le_adv_stop();
	bt_conn_foreach(BT_CONN_TYPE_LE, disconnect_conn_sink, NULL);

	k_sleep(K_MSEC(200));

	err = bt_disable();
	if (err != 0) {
		LOG_ERR("bt_disable failed (%d); resuming BLE", err);
		atomic_set(&reconnect_adv_after_disconnect, 1);
		(void)advertising_start();
		return;
	}

	(void)gpio_pin_configure_dt(&magnet, GPIO_INPUT);
	err = gpio_pin_interrupt_configure_dt(&magnet, GPIO_INT_LEVEL_LOW);
	if (err != 0) {
		int en;

		LOG_ERR("MAG wake sense cfg failed (%d); trying to resume BLE", err);
		(void)gpio_pin_interrupt_configure_dt(&magnet, GPIO_INT_DISABLE);

		en = bt_enable(NULL);
		if (en == 0) {
			atomic_set(&reconnect_adv_after_disconnect, 1);
			(void)advertising_start();
		} else {
			LOG_ERR("bt_enable resume failed (%d)", en);
		}
		return;
	}

	(void)gpio_pin_set_dt(&led0, 0);
	(void)gpio_pin_set_dt(&led1, 0);

	LOG_INF("Entering System OFF — wake when MAG pad goes LOW (magnet present) -> cold boot");

	sys_poweroff();
}

static void connected_cb(struct bt_conn *conn, uint8_t err)
{
	char addr[BT_ADDR_LE_STR_LEN];

	bt_addr_le_to_str(bt_conn_get_dst(conn), addr, sizeof(addr));

	if (err != 0) {
		LOG_ERR("Connection failed (%s) err 0x%02x %s", addr, err, bt_hci_err_to_str(err));
		return;
	}

	LOG_INF("Connected %s", addr);
}

static void disconnected_cb(struct bt_conn *conn, uint8_t reason)
{
	char addr[BT_ADDR_LE_STR_LEN];

	bt_addr_le_to_str(bt_conn_get_dst(conn), addr, sizeof(addr));
	LOG_INF("Disconnected %s reason 0x%02x %s", addr, reason, bt_hci_err_to_str(reason));

	if (atomic_get(&reconnect_adv_after_disconnect) != 0) {
		k_work_submit(&adv_restart_work);
	}
}

BT_CONN_CB_DEFINE(conn_callbacks) = {
	.connected = connected_cb,
	.disconnected = disconnected_cb,
};

#define VBATT_LOG_PERIOD_MS 30000U

int main(void)
{
	int err;

	if (!gpio_is_ready_dt(&led0) || !gpio_is_ready_dt(&led1)) {
		LOG_ERR("LED GPIO not ready");
		return -ENODEV;
	}

	err = gpio_pin_configure_dt(&led0, GPIO_OUTPUT_INACTIVE);
	err |= gpio_pin_configure_dt(&led1, GPIO_OUTPUT_INACTIVE);
	if (err != 0) {
		LOG_ERR("gpio_pin_configure_dt(led) failed: %d", err);
		return err;
	}

	if (!gpio_is_ready_dt(&magnet)) {
		LOG_ERR("Magnet GPIO not ready");
		return -ENODEV;
	}

	led_gatt_value = 0U;

	err = vbatt_init();
	if (err != 0) {
		LOG_ERR("VDD ADC init failed (%d)", err);
		return err;
	}

	err = bt_enable(NULL);
	if (err != 0) {
		LOG_ERR("Bluetooth init failed (%d)", err);
		return err;
	}

	LOG_INF("Bluetooth initialized, name \"%s\"", CONFIG_BT_DEVICE_NAME);

	err = advertising_start();
	if (err != 0) {
		return err;
	}

	uint32_t last_vbatt_log_ms = k_uptime_get_32();

	for (;;) {
		int32_t vdd_mv = 0;
		uint32_t now_ms = k_uptime_get_32();

		if ((now_ms - last_vbatt_log_ms) >= VBATT_LOG_PERIOD_MS) {
			if (vbatt_read_mv(&vdd_mv) == 0) {
				LOG_INF("vdd_mv=%d", (int)vdd_mv);
			}
			last_vbatt_log_ms = now_ms;
		}

		k_sleep(K_SECONDS(1));
	}

	return 0;
}
