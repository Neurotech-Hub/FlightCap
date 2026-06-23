#include "telemetry_adv.h"

#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/hci.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/util.h>

LOG_MODULE_REGISTER(telemetry_adv, LOG_LEVEL_INF);

#define ADV_INTERVAL_UNITS 0x0320U /* 500 ms (0.625 ms units) */

static TelemetryAdv adv_payload = {
	.company_id = TELEM_ADV_COMPANY_ID,
	.magic = TELEM_ADV_MAGIC,
	.version = TELEM_ADV_VERSION,
};

static int16_t last_distance_mm = INT16_MIN;
static uint16_t last_interactions;
static bool adv_running;

static struct bt_data ad[] = {
	BT_DATA_BYTES(BT_DATA_FLAGS, (BT_LE_AD_GENERAL | BT_LE_AD_NO_BREDR)),
	BT_DATA(BT_DATA_MANUFACTURER_DATA, (uint8_t *)&adv_payload, sizeof(adv_payload)),
};

static const struct bt_le_adv_param adv_param =
	BT_LE_ADV_PARAM_INIT(0, ADV_INTERVAL_UNITS, ADV_INTERVAL_UNITS, NULL);

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

	int err = bt_le_adv_update_data(ad, ARRAY_SIZE(ad), NULL, 0);

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

	err = bt_le_adv_start(&adv_param, ad, ARRAY_SIZE(ad), NULL, 0);
	if (err != 0) {
		LOG_ERR("bt_le_adv_start failed (%d)", err);
		return err;
	}

	adv_running = true;
	LOG_INF("BLE beacon started (non-conn, %u ms interval)", (ADV_INTERVAL_UNITS * 625U) / 1000U);
	return 0;
}

int telemetry_adv_stop(void)
{
	int err = 0;

	if (!adv_running) {
		return 0;
	}

	err = bt_le_adv_stop();
	if (err != 0) {
		LOG_ERR("bt_le_adv_stop failed (%d)", err);
		return err;
	}

	adv_running = false;
	return 0;
}

bool telemetry_adv_active(void)
{
	return adv_running;
}

uint16_t telemetry_adv_seq(void)
{
	return adv_payload.seq;
}
