#ifndef TELEMETRY_ADV_H
#define TELEMETRY_ADV_H

#include <stdbool.h>
#include <stdint.h>

#include <zephyr/sys/util.h>

/*
 * Provisional Bluetooth manufacturer company ID for Neurotech Hub (WashU).
 * ASCII 'N' | ('H' << 8) — replace with a Bluetooth SIG assigned ID before
 * field deployment outside the lab.
 */
#define TELEM_ADV_COMPANY_ID 0x4E48U // NH
#define TELEM_ADV_MAGIC 0xA5U
#define TELEM_ADV_VERSION 0x02U

#define TELEM_FLAG_DIST_VALID BIT(0)
#define TELEM_FLAG_INTERACT_VALID BIT(1)
#define TELEM_FLAG_TOF_ERR BIT(2)
#define TELEM_FLAG_STALE          BIT(3)
#define TELEM_FLAG_PAIR_MODE      BIT(4)

#pragma pack(push, 1)
typedef struct
{
	uint16_t company_id;
	uint8_t magic;
	uint8_t version;
	uint8_t device_addr[6]; /* BLE identity address (stable per device; MSB format) */
	uint16_t seq;
	int16_t distance_mm;
	uint16_t interactions;
	uint8_t flags;
} TelemetryAdv;
#pragma pack(pop)

int telemetry_adv_publish(int16_t distance_mm, uint16_t interactions, uint8_t flags);
int telemetry_adv_start(void);
int telemetry_adv_stop(void);
void telemetry_adv_bt_disabled(void);
void telemetry_adv_get_device_addr(uint8_t addr[6]);
bool telemetry_adv_active(void);
uint16_t telemetry_adv_seq(void);

#endif
