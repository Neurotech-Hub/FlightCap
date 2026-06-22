#ifndef FLIGHTCAP_HW_H
#define FLIGHTCAP_HW_H

#include <stdbool.h>
#include <stdint.h>

#include <zephyr/sys/util.h>

#define FLIGHTCAP_HW_VBATT BIT(0)
#define FLIGHTCAP_HW_ACCEL BIT(1)
#define FLIGHTCAP_HW_TOF   BIT(2)
#define FLIGHTCAP_HW_FLASH BIT(3)

struct flightcap_hw_status {
	bool vbatt_ok;
	bool accel_ok;
	bool tof_ok;
	bool flash_ok;
	int32_t vbatt_mv;
	uint16_t tof_probe_mm;
};

int flightcap_hw_check(unsigned int mask, struct flightcap_hw_status *status);

#endif
