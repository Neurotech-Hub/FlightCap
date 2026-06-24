#include "telemetry_adv.h"

#include <string.h>

#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/hci.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/util.h>

LOG_MODULE_REGISTER(telemetry_adv, LOG_LEVEL_INF);

#define ADV_INTERVAL_BASE_UNITS 0x00A0U /* 100 ms (0.625 ms units) */
#define ADV_INTERVAL_DITHER_UNIT 0x0020U /* 25 ms per step, max +175 ms */

#define DEVICE_NAME CONFIG_BT_DEVICE_NAME
#define DEVICE_NAME_LEN (sizeof(DEVICE_NAME) - 1U)

static TelemetryAdv adv_payload = {
	.company_id = TELEM_ADV_COMPANY_ID,
	.magic = TELEM_ADV_MAGIC,
	.version = TELEM_ADV_VERSION,
};

static int16_t last_distance_mm = INT16_MIN;
static uint16_t last_interactions;
static bool adv_running;
static bool device_addr_logged;
static struct bt_le_adv_param adv_param;
static uint16_t adv_interval_units;

static void telemetry_adv_param_update(void)
{
	uint16_t dither = (uint16_t)(((uint32_t)adv_payload.device_addr[5] & 0x07U) *
				       ADV_INTERVAL_DITHER_UNIT);

	adv_interval_units = ADV_INTERVAL_BASE_UNITS + dither;
	adv_param = (struct bt_le_adv_param)BT_LE_ADV_PARAM_INIT(
		BT_LE_ADV_OPT_USE_IDENTITY | BT_LE_ADV_OPT_SCANNABLE, adv_interval_units,
		adv_interval_units, NULL);
}

static void telemetry_refresh_device_addr(void)
{
	bt_addr_le_t addrs[1];
	size_t count = 1;

	bt_id_get(addrs, &count);
	if (count == 0U) {
		memset(adv_payload.device_addr, 0, sizeof(adv_payload.device_addr));
		return;
	}

	memcpy(adv_payload.device_addr, addrs[0].a.val, sizeof(adv_payload.device_addr));
	telemetry_adv_param_update();

	if (!device_addr_logged) {
		LOG_INF("telemetry device_addr %02X:%02X:%02X:%02X:%02X:%02X",
			adv_payload.device_addr[0], adv_payload.device_addr[1],
			adv_payload.device_addr[2], adv_payload.device_addr[3],
			adv_payload.device_addr[4], adv_payload.device_addr[5]);
		device_addr_logged = true;
	}
}

/*
 * Primary AD: flags + manufacturer telemetry only (passive-scan friendly).
 * Device name goes in scan response (scannable non-connectable ADV_SCAN_IND).
 */
static struct bt_data ad[] = {
	BT_DATA_BYTES(BT_DATA_FLAGS, (BT_LE_AD_GENERAL | BT_LE_AD_NO_BREDR)),
	BT_DATA(BT_DATA_MANUFACTURER_DATA, (uint8_t *)&adv_payload, sizeof(adv_payload)),
};

static const struct bt_data sd[] = {
	BT_DATA(BT_DATA_NAME_COMPLETE, DEVICE_NAME, DEVICE_NAME_LEN),
};

/* On-air AD element size: 1 len + 1 type + payload. */
#define AD_ELEM_ON_AIR_LEN(payload_len) (2U + (payload_len))
#define ADV_PRIMARY_ON_AIR_LEN                                                             \
	(AD_ELEM_ON_AIR_LEN(1U) + AD_ELEM_ON_AIR_LEN(sizeof(TelemetryAdv)))
#define ADV_SCAN_RSP_ON_AIR_LEN AD_ELEM_ON_AIR_LEN(DEVICE_NAME_LEN)

BUILD_ASSERT(ADV_PRIMARY_ON_AIR_LEN <= BT_GAP_ADV_MAX_ADV_DATA_LEN);
BUILD_ASSERT(ADV_SCAN_RSP_ON_AIR_LEN <= BT_GAP_ADV_MAX_ADV_DATA_LEN);

static void telemetry_adv_param_init(void)
{
	memset(adv_payload.device_addr, 0, sizeof(adv_payload.device_addr));
	telemetry_adv_param_update();
}

int telemetry_adv_publish(int16_t distance_mm, uint16_t interactions, uint8_t flags)
{
	bool changed = (distance_mm != last_distance_mm) || (interactions != last_interactions);

	adv_payload.distance_mm = distance_mm;
	adv_payload.interactions = interactions;
	adv_payload.flags = flags;

	if (changed) {
		adv_payload.seq++;
		last_distance_mm = distance_mm;
		last_interactions = interactions;
	}

	if (!adv_running) {
		return 0;
	}

	int err = bt_le_adv_update_data(ad, ARRAY_SIZE(ad), sd, ARRAY_SIZE(sd));

	if (err != 0) {
		LOG_ERR("bt_le_adv_update_data failed (%d)", err);
		return err;
	}

	return 0;
}

int telemetry_adv_start(void)
{
	int err;

	if (adv_running) {
		return 0;
	}

	telemetry_refresh_device_addr();

	err = bt_le_adv_start(&adv_param, ad, ARRAY_SIZE(ad), sd, ARRAY_SIZE(sd));
	if (err != 0) {
		LOG_ERR("bt_le_adv_start failed (%d)", err);
		return err;
	}

	adv_running = true;
	LOG_INF("BLE beacon started (non-conn scannable, %u ms interval, name \"%s\")",
		(adv_interval_units * 625U) / 1000U, DEVICE_NAME);
	return 0;
}

int telemetry_adv_stop(void)
{
	int err = 0;

	if (!adv_running) {
		return 0;
	}

	err = bt_le_adv_stop();
	adv_running = false;
	if (err != 0) {
		LOG_ERR("bt_le_adv_stop failed (%d)", err);
		return err;
	}

	return 0;
}

void telemetry_adv_bt_disabled(void)
{
	adv_running = false;
	device_addr_logged = false;
	memset(adv_payload.device_addr, 0, sizeof(adv_payload.device_addr));
	telemetry_adv_param_init();
}

void telemetry_adv_get_device_addr(uint8_t addr[6])
{
	if (addr == NULL) {
		return;
	}

	memcpy(addr, adv_payload.device_addr, 6U);
}

bool telemetry_adv_active(void)
{
	return adv_running;
}

uint16_t telemetry_adv_seq(void)
{
	return adv_payload.seq;
}
