#include "flightcap_hw.h"

#include <errno.h>
#include <string.h>

#include <zephyr/logging/log.h>

#include "lis2dh12_zephyr.h"
#include "spi_nor_zephyr.h"
#include "vbatt_zephyr.h"
#include "vl53l0x_zephyr.h"

LOG_MODULE_REGISTER(flightcap_hw, LOG_LEVEL_INF);

static const char *hw_result(bool requested, bool ok)
{
	if (!requested) {
		return "SKIP";
	}

	return ok ? "OK" : "FAIL";
}

int flightcap_hw_check(unsigned int mask, struct flightcap_hw_status *status)
{
	struct flightcap_hw_status local = {0};
	struct flightcap_hw_status *out = status ? status : &local;
	int ret = 0;
	int err;

	if (status) {
		memset(status, 0, sizeof(*status));
	}

	if (mask & FLIGHTCAP_HW_VBATT) {
		err = vbatt_check(&out->vbatt_mv);
		out->vbatt_ok = (err == 0);
		if (!out->vbatt_ok) {
			ret = -EIO;
		}
	}

	if (mask & FLIGHTCAP_HW_ACCEL) {
		struct lis2dh12_dev accel = {0};

		err = lis2dh12_zephyr_check(&accel);
		out->accel_ok = (err == 0);
		if (!out->accel_ok) {
			ret = -EIO;
		}
	}

	if (mask & FLIGHTCAP_HW_TOF) {
		err = vl53l0x_zephyr_check(NULL, &out->tof_probe_mm);
		out->tof_ok = (err == 0);
		if (!out->tof_ok) {
			ret = -EIO;
		}
	}

	if (mask & FLIGHTCAP_HW_FLASH) {
		err = spi_nor_zephyr_check(NULL);
		out->flash_ok = (err == 0);
		if (!out->flash_ok) {
			ret = -EIO;
		}
	}

	LOG_INF("HW check: vbatt=%s accel=%s tof=%s flash=%s",
		hw_result((mask & FLIGHTCAP_HW_VBATT) != 0, out->vbatt_ok),
		hw_result((mask & FLIGHTCAP_HW_ACCEL) != 0, out->accel_ok),
		hw_result((mask & FLIGHTCAP_HW_TOF) != 0, out->tof_ok),
		hw_result((mask & FLIGHTCAP_HW_FLASH) != 0, out->flash_ok));

	return ret;
}
