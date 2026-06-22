#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/conn.h>
#include <zephyr/bluetooth/gatt.h>
#include <zephyr/bluetooth/hci.h>
#include <zephyr/bluetooth/uuid.h>
#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/i2c.h>
#include <zephyr/irq.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/mgmt/mcumgr/transport/smp_bt.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/sys/byteorder.h>

#include "flightcap_hw.h"
#include "lis2dh12_zephyr.h"
#include "vl53l0x_zephyr.h"
#include "vl53l4cd_filter.h"

LOG_MODULE_REGISTER(flightcap_demo, LOG_LEVEL_INF);

#define LED0_NODE DT_ALIAS(led0)
#define LED1_NODE DT_ALIAS(led1)
#define ACCEL_INT_NODE DT_ALIAS(accel_int)
#define MAGNET_NODE DT_ALIAS(magnet_sensor)
#define I2C_NODE DT_ALIAS(i2c_bus)

#define WINDOW_MS 1000U
#define LED_FLASH_MS 50U
#define FAULT_BLINK_MS 100U
#define MOTION_THRESHOLD_MG 3
#define MOTION_DURATION_SAMPLES 0
#define TOF_POLL_INTERVAL_MS 5U
#define TOF_THREAD_STACK_SIZE 2048
#define TOF_THREAD_PRIORITY K_PRIO_PREEMPT(7)

static const struct gpio_dt_spec led0 = GPIO_DT_SPEC_GET(LED0_NODE, gpios);
static const struct gpio_dt_spec led1 = GPIO_DT_SPEC_GET(LED1_NODE, gpios);
static const struct gpio_dt_spec accel_int = GPIO_DT_SPEC_GET(ACCEL_INT_NODE, gpios);
static const struct gpio_dt_spec magnet = GPIO_DT_SPEC_GET(MAGNET_NODE, gpios);
static const struct device *const i2c_dev = DEVICE_DT_GET(I2C_NODE);
static const struct device *tof_dev;

static struct gpio_callback accel_int_cb;
static volatile uint32_t motion_events;
static struct lis2dh12_dev lis2dh12;

static void leds_set(int value)
{
	(void)gpio_pin_set_dt(&led0, value);
	(void)gpio_pin_set_dt(&led1, value);
}

/*
 * tof_present is set true only after the Zephyr VL53L0X driver reports ready
 * and a probe sample_fetch succeeds at boot. When false the reader thread is
 * left suspended, the filter is never fed, and the main loop publishes
 * dist_mm = 0 (the iOS-spec invalid sentinel) while substituting a
 * continuous fast-blink for the normal heartbeat.
 */
static bool tof_present;

/* ------------------------------------------------------------------------- */
/* BLE GATT telemetry service: motion + distance, 1 Hz notify when connected. */
/* See docs/FlightCap_BLE_Spec.md for the iOS-facing contract.                */
/* ------------------------------------------------------------------------- */

#define BT_UUID_FCP_SVC_VAL                                                                        \
	BT_UUID_128_ENCODE(0xad2a98a4, 0x2148, 0x4b58, 0x9e14, 0x7e2cbb6c7b01)
#define BT_UUID_FCP_MOTION_VAL                                                                     \
	BT_UUID_128_ENCODE(0xad2a98a4, 0x2148, 0x4b58, 0x9e14, 0x7e2cbb6c7b02)
#define BT_UUID_FCP_DIST_VAL                                                                       \
	BT_UUID_128_ENCODE(0xad2a98a4, 0x2148, 0x4b58, 0x9e14, 0x7e2cbb6c7b03)

static const struct bt_uuid_128 fcp_svc_uuid = BT_UUID_INIT_128(BT_UUID_FCP_SVC_VAL);
static const struct bt_uuid_128 fcp_motion_uuid = BT_UUID_INIT_128(BT_UUID_FCP_MOTION_VAL);
static const struct bt_uuid_128 fcp_dist_uuid = BT_UUID_INIT_128(BT_UUID_FCP_DIST_VAL);

static atomic_t latest_motion = ATOMIC_INIT(0);
static atomic_t latest_dist = ATOMIC_INIT(0);

static atomic_t motion_notify_enabled = ATOMIC_INIT(0);
static atomic_t dist_notify_enabled = ATOMIC_INIT(0);

static atomic_t reconnect_adv_after_disconnect = ATOMIC_INIT(1);
static struct bt_conn *active_conn;

static ssize_t motion_chr_read(struct bt_conn *conn, const struct bt_gatt_attr *attr, void *buf,
			       uint16_t len, uint16_t offset)
{
	uint8_t out[4];

	sys_put_le32((uint32_t)atomic_get(&latest_motion), out);
	return bt_gatt_attr_read(conn, attr, buf, len, offset, out, sizeof(out));
}

static ssize_t dist_chr_read(struct bt_conn *conn, const struct bt_gatt_attr *attr, void *buf,
			     uint16_t len, uint16_t offset)
{
	uint32_t packed = (uint32_t)atomic_get(&latest_dist);
	uint8_t out[4];

	sys_put_le16((uint16_t)(packed & 0xFFFFU), &out[0]);
	sys_put_le16((uint16_t)((packed >> 16) & 0xFFFFU), &out[2]);
	return bt_gatt_attr_read(conn, attr, buf, len, offset, out, sizeof(out));
}

static void motion_ccc_cfg_changed(const struct bt_gatt_attr *attr, uint16_t value)
{
	ARG_UNUSED(attr);
	atomic_set(&motion_notify_enabled, (value == BT_GATT_CCC_NOTIFY) ? 1 : 0);
}

static void dist_ccc_cfg_changed(const struct bt_gatt_attr *attr, uint16_t value)
{
	ARG_UNUSED(attr);
	atomic_set(&dist_notify_enabled, (value == BT_GATT_CCC_NOTIFY) ? 1 : 0);
}

BT_GATT_SERVICE_DEFINE(fcp_svc,
	BT_GATT_PRIMARY_SERVICE(&fcp_svc_uuid.uuid),

	BT_GATT_CHARACTERISTIC(&fcp_motion_uuid.uuid,
			       BT_GATT_CHRC_READ | BT_GATT_CHRC_NOTIFY,
			       BT_GATT_PERM_READ,
			       motion_chr_read, NULL, NULL),
	BT_GATT_CCC(motion_ccc_cfg_changed, BT_GATT_PERM_READ | BT_GATT_PERM_WRITE),

	BT_GATT_CHARACTERISTIC(&fcp_dist_uuid.uuid,
			       BT_GATT_CHRC_READ | BT_GATT_CHRC_NOTIFY,
			       BT_GATT_PERM_READ,
			       dist_chr_read, NULL, NULL),
	BT_GATT_CCC(dist_ccc_cfg_changed, BT_GATT_PERM_READ | BT_GATT_PERM_WRITE),
);

#define FCP_ADV_INT_MIN 0x0320U
#define FCP_ADV_INT_MAX 0x03C0U

#define DEVICE_NAME     CONFIG_BT_DEVICE_NAME
#define DEVICE_NAME_LEN (sizeof(DEVICE_NAME) - 1U)

static const struct bt_data ad[] = {
	BT_DATA_BYTES(BT_DATA_FLAGS, (BT_LE_AD_GENERAL | BT_LE_AD_NO_BREDR)),
	BT_DATA_BYTES(BT_DATA_UUID128_SOME, BT_UUID_FCP_SVC_VAL),
};

static const struct bt_data sd[] = {
	BT_DATA(BT_DATA_NAME_COMPLETE, DEVICE_NAME, DEVICE_NAME_LEN),
	BT_DATA_BYTES(BT_DATA_UUID128_SOME, SMP_BT_SVC_UUID_VAL),
};

static int advertising_start(void)
{
	const struct bt_le_adv_param p =
		BT_LE_ADV_PARAM_INIT(BT_LE_ADV_OPT_CONN, FCP_ADV_INT_MIN, FCP_ADV_INT_MAX, NULL);
	int err = bt_le_adv_start(&p, ad, ARRAY_SIZE(ad), sd, ARRAY_SIZE(sd));

	if (err == 0) {
		LOG_INF("BLE advertising started (~500-600 ms, name \"%s\")", DEVICE_NAME);
	} else {
		LOG_ERR("bt_le_adv_start failed (%d)", err);
	}
	return err;
}

static void adv_restart_work_handler(struct k_work *work)
{
	ARG_UNUSED(work);
	(void)advertising_start();
}

static K_WORK_DEFINE(adv_restart_work, adv_restart_work_handler);

static void connected_cb(struct bt_conn *conn, uint8_t err)
{
	char addr[BT_ADDR_LE_STR_LEN];

	bt_addr_le_to_str(bt_conn_get_dst(conn), addr, sizeof(addr));

	if (err != 0) {
		LOG_ERR("BLE connect failed (%s) err 0x%02x %s", addr, err, bt_hci_err_to_str(err));
		return;
	}

	active_conn = bt_conn_ref(conn);
	LOG_INF("BLE connected %s", addr);
}

static void disconnected_cb(struct bt_conn *conn, uint8_t reason)
{
	char addr[BT_ADDR_LE_STR_LEN];

	bt_addr_le_to_str(bt_conn_get_dst(conn), addr, sizeof(addr));
	LOG_INF("BLE disconnected %s reason 0x%02x %s", addr, reason, bt_hci_err_to_str(reason));

	if (active_conn != NULL) {
		bt_conn_unref(active_conn);
		active_conn = NULL;
	}

	atomic_set(&motion_notify_enabled, 0);
	atomic_set(&dist_notify_enabled, 0);

	if (atomic_get(&reconnect_adv_after_disconnect) != 0) {
		k_work_submit(&adv_restart_work);
	}
}

BT_CONN_CB_DEFINE(conn_callbacks) = {
	.connected = connected_cb,
	.disconnected = disconnected_cb,
};

static void publish_window(uint32_t motion, uint16_t dist_mm, uint16_t dist_n)
{
	atomic_set(&latest_motion, (atomic_val_t)motion);
	atomic_set(&latest_dist,
		   (atomic_val_t)((uint32_t)dist_mm | ((uint32_t)dist_n << 16)));

	if (atomic_get(&motion_notify_enabled) != 0) {
		uint8_t buf[4];
		const struct bt_gatt_attr *attr;

		sys_put_le32(motion, buf);
		attr = bt_gatt_find_by_uuid(fcp_svc.attrs, fcp_svc.attr_count, &fcp_motion_uuid.uuid);
		if (attr != NULL) {
			(void)bt_gatt_notify(NULL, attr, buf, sizeof(buf));
		}
	}

	if (atomic_get(&dist_notify_enabled) != 0) {
		uint8_t buf[4];
		const struct bt_gatt_attr *attr;

		sys_put_le16(dist_mm, &buf[0]);
		sys_put_le16(dist_n, &buf[2]);
		attr = bt_gatt_find_by_uuid(fcp_svc.attrs, fcp_svc.attr_count, &fcp_dist_uuid.uuid);
		if (attr != NULL) {
			(void)bt_gatt_notify(NULL, attr, buf, sizeof(buf));
		}
	}
}

static void accel_int_callback(const struct device *port, struct gpio_callback *cb, uint32_t pins)
{
	ARG_UNUSED(port);
	ARG_UNUSED(cb);
	ARG_UNUSED(pins);

	motion_events++;
}

static void fault_loop(const char *what, int err) __attribute__((noreturn));
static void fault_loop(const char *what, int err)
{
	LOG_ERR("FAULT: %s failed (%d) -- entering fast-blink fault loop", what, err);
	while (1) {
		leds_set(1);
		k_sleep(K_MSEC(FAULT_BLINK_MS));
		leds_set(0);
		k_sleep(K_MSEC(FAULT_BLINK_MS));
	}
}

static void window_fault_blink(void)
{
	const uint32_t cycles = WINDOW_MS / (2U * FAULT_BLINK_MS);

	for (uint32_t i = 0; i < cycles; i++) {
		leds_set(1);
		k_sleep(K_MSEC(FAULT_BLINK_MS));
		leds_set(0);
		k_sleep(K_MSEC(FAULT_BLINK_MS));
	}
}

static void tof_thread_fn(void *a, void *b, void *c)
{
	ARG_UNUSED(a);
	ARG_UNUSED(b);
	ARG_UNUSED(c);

	while (1) {
		uint16_t mm = 0;

		if (tof_dev != NULL && vl53l0x_zephyr_read_mm(tof_dev, &mm) == 0) {
			vl53l4cd_filter_push(mm);
		}

		k_sleep(K_MSEC(TOF_POLL_INTERVAL_MS));
	}
}

K_THREAD_DEFINE(tof_tid, TOF_THREAD_STACK_SIZE, tof_thread_fn, NULL, NULL, NULL,
		TOF_THREAD_PRIORITY, 0, 0);

static void enter_forced_sleep(void)
{
	unsigned int irq_key;

	LOG_INF("Magnet applied -> forced sleep (LEDs off, ToF %s, BLE off)",
		tof_present ? "polling paused" : "absent");

	atomic_set(&reconnect_adv_after_disconnect, 0);
	(void)bt_le_adv_stop();
	if (active_conn != NULL) {
		(void)bt_conn_disconnect(active_conn, BT_HCI_ERR_REMOTE_USER_TERM_CONN);
	}

	if (tof_present) {
		k_thread_suspend(tof_tid);
	}
	leds_set(0);

	do {
		k_sleep(K_MSEC(WINDOW_MS));
	} while (gpio_pin_get_dt(&magnet) > 0);

	LOG_INF("Magnet released -> resuming");
	if (tof_present) {
		k_thread_resume(tof_tid);
		vl53l4cd_filter_clear();
	}

	irq_key = irq_lock();
	motion_events = 0U;
	irq_unlock(irq_key);

	atomic_set(&reconnect_adv_after_disconnect, 1);
	(void)advertising_start();
}

int main(void)
{
	int ret;
	uint32_t window_seq = 0;

	if (!gpio_is_ready_dt(&led0) || !gpio_is_ready_dt(&led1) ||
	    !gpio_is_ready_dt(&accel_int) || !gpio_is_ready_dt(&magnet)) {
		LOG_ERR("GPIO dependency not ready");
		return -ENODEV;
	}

	ret = gpio_pin_configure_dt(&led0, GPIO_OUTPUT_INACTIVE);
	ret |= gpio_pin_configure_dt(&led1, GPIO_OUTPUT_INACTIVE);
	ret |= gpio_pin_configure_dt(&accel_int, GPIO_INPUT);
	ret |= gpio_pin_configure_dt(&magnet, GPIO_INPUT);
	if (ret < 0) {
		LOG_ERR("GPIO configuration failed (%d)", ret);
		return ret;
	}

	if (!device_is_ready(i2c_dev)) {
		LOG_ERR("I2C bus not ready");
		return -ENODEV;
	}
	LOG_INF("I2C ready: %s (SDA P0.05, SCL P0.04, 100 kHz)", i2c_dev->name);

	{
		struct flightcap_hw_status hw = {0};

		(void)flightcap_hw_check(FLIGHTCAP_HW_VBATT | FLIGHTCAP_HW_ACCEL | FLIGHTCAP_HW_TOF,
					&hw);

		if (hw.tof_ok) {
			tof_present = true;
			tof_dev = DEVICE_DT_GET(DT_ALIAS(tof_sensor));
			LOG_INF("VL53L0X ready (probe_mm=%u, single-shot ~33 ms budget)",
				hw.tof_probe_mm);
		} else {
			LOG_ERR("VL53L0X probe failed -- continuing without ToF");
			tof_dev = NULL;
		}
	}

	ret = lis2dh12_zephyr_init(&lis2dh12);
	if (ret < 0) {
		LOG_ERR("LIS2DH12 init failed (%d)", ret);
		return ret;
	}

	ret = lis2dh12_zephyr_config_motion(&lis2dh12, MOTION_THRESHOLD_MG, MOTION_DURATION_SAMPLES);
	if (ret < 0) {
		LOG_ERR("LIS2DH12 motion config failed (%d)", ret);
		return ret;
	}

	gpio_init_callback(&accel_int_cb, accel_int_callback, BIT(accel_int.pin));
	ret = gpio_add_callback(accel_int.port, &accel_int_cb);
	ret |= gpio_pin_interrupt_configure_dt(&accel_int, GPIO_INT_EDGE_TO_ACTIVE);
	if (ret < 0) {
		LOG_ERR("INT1 callback setup failed (%d)", ret);
		return ret;
	}

	vl53l4cd_filter_init();

	/* Park reader until VL53L0X probe completes (thread auto-starts at boot). */
	k_thread_suspend(tof_tid);

	if (tof_present) {
		k_thread_resume(tof_tid);
	}

	ret = bt_enable(NULL);
	if (ret != 0) {
		fault_loop("bt_enable", ret);
	}

	ret = advertising_start();
	if (ret != 0) {
		fault_loop("bt_le_adv_start", ret);
	}

	LOG_INF("FlightCap prod started: window=%u ms, motion_ths=%u mg, ToF=%s, BLE=on (FCP+SMP DFU)",
		WINDOW_MS, MOTION_THRESHOLD_MG,
		tof_present ? "VL53L0X single-shot poll"
			    : "ABSENT (fast-blink, dist=0)");

	while (1) {
		uint32_t window_motion;
		unsigned int irq_key;
		uint16_t dist_mm = 0;
		uint16_t dist_n = 0;

		if (tof_present) {
			k_sleep(K_MSEC(WINDOW_MS));
		} else {
			window_fault_blink();
		}

		if (gpio_pin_get_dt(&magnet) > 0) {
			enter_forced_sleep();
			continue;
		}

		irq_key = irq_lock();
		window_motion = motion_events;
		motion_events = 0U;
		irq_unlock(irq_key);

		if (tof_present) {
			int dist_err = vl53l4cd_filter_snapshot(&dist_mm, &dist_n);

			if (dist_err == 0) {
				LOG_INF("seq=%u motion_1s=%u dist_mm=%u (n=%u)",
					window_seq++, window_motion, dist_mm, dist_n);
			} else {
				LOG_INF("seq=%u motion_1s=%u dist=err (n=%u)",
					window_seq++, window_motion, dist_n);
				dist_mm = 0U;
			}
		} else {
			LOG_INF("seq=%u motion_1s=%u dist=tof-absent",
				window_seq++, window_motion);
		}

		publish_window(window_motion, dist_mm, dist_n);

		if (tof_present) {
			leds_set(1);
			k_sleep(K_MSEC(LED_FLASH_MS));
			leds_set(0);
		}
	}

	return 0;
}
