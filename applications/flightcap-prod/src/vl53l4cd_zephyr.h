#ifndef FLIGHTCAP_VL53L4CD_ZEPHYR_H
#define FLIGHTCAP_VL53L4CD_ZEPHYR_H

#include <stdbool.h>
#include <stdint.h>

#include <zephyr/device.h>

/**
 * Minimal Zephyr port of the relevant subset of Pololu's VL53L4CD Arduino
 * library (https://github.com/pololu/vl53l4cd-arduino), which itself is
 * derived from ST's Ultra Lite Driver (STSW-IMG026).
 *
 * - 16-bit register addresses are encoded MSB first per transaction.
 * - The default polarity is active-low INT (set by the 0x30 byte of the ULD
 *   default config). vl53l4cd_data_ready() assumes this; do not change the
 *   polarity unless you also update that helper.
 * - Caller owns the I2C bus device pointer and the 7-bit slave address.
 */
struct vl53l4cd_dev {
	const struct device *i2c;
	uint16_t addr; /* 7-bit I2C address (default 0x29) */
	bool initialized;
};

/**
 * Bring the sensor up: model-ID check, boot wait, ULD default config block,
 * VHV start, and a default 50 ms timing budget in continuous mode (0 ms
 * inter-measurement). Mirrors Pololu's init(true, false) — 2V8 IO, fast-mode
 * (not fast-mode-plus).
 *
 * @retval 0          Success.
 * @retval -ENODEV    I2C bus not ready, or model-ID mismatch (no sensor).
 * @retval -ETIMEDOUT FW_SYSTEM_STATUS did not reach 0x03 within ~100 ms.
 * @retval -EIO       Other I2C transaction failure.
 */
int vl53l4cd_init(struct vl53l4cd_dev *dev, const struct device *i2c, uint16_t addr);

/**
 * Set the timing budget (10..200 ms) and inter-measurement period
 * (0 = continuous, otherwise must be > budget).
 */
int vl53l4cd_set_range_timing(struct vl53l4cd_dev *dev,
			      uint8_t budget_ms,
			      uint32_t inter_meas_ms);

int vl53l4cd_start_continuous(struct vl53l4cd_dev *dev);
int vl53l4cd_stop_continuous(struct vl53l4cd_dev *dev);

/** Non-blocking data-ready poll over I2C. */
int vl53l4cd_data_ready(struct vl53l4cd_dev *dev, bool *ready);

/**
 * Block-read the 15-byte result frame at 0x0089 and return the range plus
 * the remapped status code (0 == valid measurement).
 */
int vl53l4cd_read_range_mm(struct vl53l4cd_dev *dev,
			   uint16_t *range_mm,
			   uint8_t *status);

int vl53l4cd_clear_interrupt(struct vl53l4cd_dev *dev);

#endif /* FLIGHTCAP_VL53L4CD_ZEPHYR_H */
