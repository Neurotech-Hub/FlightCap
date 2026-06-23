#include "dfu_mode.h"

#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/conn.h>
#include <zephyr/bluetooth/hci.h>
#include <zephyr/logging/log.h>
#include <zephyr/mgmt/mcumgr/transport/smp_bt.h>

LOG_MODULE_REGISTER(dfu_mode, LOG_LEVEL_INF);

#define DFU_ADV_INT_MIN 0x0320U /* 500 ms */
#define DFU_ADV_INT_MAX 0x0320U

#define DEVICE_NAME CONFIG_BT_DEVICE_NAME
#define DEVICE_NAME_LEN (sizeof(DEVICE_NAME) - 1U)

static struct bt_conn *dfu_conn;
static bool dfu_adv_running;

static const struct bt_data dfu_ad[] = {
	BT_DATA_BYTES(BT_DATA_FLAGS, (BT_LE_AD_GENERAL | BT_LE_AD_NO_BREDR)),
};

static const struct bt_data dfu_sd[] = {
	BT_DATA(BT_DATA_NAME_COMPLETE, DEVICE_NAME, DEVICE_NAME_LEN),
	BT_DATA_BYTES(BT_DATA_UUID128_SOME, SMP_BT_SVC_UUID_VAL),
};

static const struct bt_le_adv_param dfu_adv_param =
	BT_LE_ADV_PARAM_INIT(BT_LE_ADV_OPT_CONN, DFU_ADV_INT_MIN, DFU_ADV_INT_MAX, NULL);

static void dfu_connected(struct bt_conn *conn, uint8_t err)
{
	if (err != 0) {
		LOG_WRN("DFU connect failed (%u)", err);
		return;
	}

	if (dfu_conn != NULL) {
		bt_conn_unref(dfu_conn);
	}

	dfu_conn = bt_conn_ref(conn);
	LOG_INF("DFU central connected");
}

static void dfu_disconnected(struct bt_conn *conn, uint8_t reason)
{
	ARG_UNUSED(reason);

	if (dfu_conn == conn) {
		bt_conn_unref(dfu_conn);
		dfu_conn = NULL;
	}

	LOG_INF("DFU central disconnected");
}

BT_CONN_CB_DEFINE(dfu_conn_cb) = {
	.connected = dfu_connected,
	.disconnected = dfu_disconnected,
};

static int dfu_adv_start(void)
{
	int ret;

	if (dfu_adv_running) {
		return 0;
	}

	ret = bt_le_adv_start(&dfu_adv_param, dfu_ad, ARRAY_SIZE(dfu_ad), dfu_sd, ARRAY_SIZE(dfu_sd));
	if (ret < 0) {
		LOG_ERR("DFU adv start failed (%d)", ret);
		return ret;
	}

	dfu_adv_running = true;
	LOG_INF("DFU SMP advertising started (connectable, name \"%s\")", DEVICE_NAME);
	return 0;
}

static int dfu_adv_stop(void)
{
	int ret = 0;

	if (dfu_conn != NULL) {
		(void)bt_conn_disconnect(dfu_conn, BT_HCI_ERR_REMOTE_USER_TERM_CONN);
	}

	if (dfu_adv_running) {
		ret = bt_le_adv_stop();
		if (ret < 0) {
			LOG_ERR("DFU adv stop failed (%d)", ret);
			return ret;
		}
		dfu_adv_running = false;
	}

	return ret;
}

int dfu_mode_enter(struct prod_context *ctx)
{
	int ret;

	ARG_UNUSED(ctx);

	ret = dfu_adv_start();
	if (ret < 0) {
		return ret;
	}

	return 0;
}

int dfu_mode_exit(struct prod_context *ctx)
{
	ARG_UNUSED(ctx);

	return dfu_adv_stop();
}
