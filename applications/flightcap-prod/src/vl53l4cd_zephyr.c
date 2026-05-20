/*
 * VL53L4CD ToF distance sensor driver — Zephyr port of the relevant subset
 * of Pololu's Arduino library (which itself is derived from ST's Ultra Lite
 * Driver, STSW-IMG026). The default 88-byte configuration block at
 * 0x30..0x87 is byte-identical to the Pololu source for easy auditing.
 */

#include "vl53l4cd_zephyr.h"

#include <errno.h>
#include <string.h>

#include <zephyr/drivers/i2c.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/util.h>

LOG_MODULE_REGISTER(vl53l4cd, LOG_LEVEL_INF);

/* ------------------------------------------------------------------------- */
/* Register addresses (subset of ST ULD constants).                           */
/* ------------------------------------------------------------------------- */
#define VL53L4CD_OSC_FREQUENCY                     0x0006
#define VL53L4CD_VHV_CONFIG_TIMEOUT_MACROP_LOOP_BD 0x0008
#define VL53L4CD_GPIO_HV_MUX_CTRL                  0x0030
#define VL53L4CD_GPIO_TIO_HV_STATUS                0x0031
#define VL53L4CD_RANGE_CONFIG_A                    0x005E
#define VL53L4CD_RANGE_CONFIG_B                    0x0061
#define VL53L4CD_INTERMEASUREMENT_MS               0x006C
#define VL53L4CD_SYSTEM_INTERRUPT_CLEAR            0x0086
#define VL53L4CD_SYSTEM_START                      0x0087
#define VL53L4CD_RESULT_RANGE_STATUS               0x0089
#define VL53L4CD_RESULT_OSC_CALIBRATE_VAL          0x00DE
#define VL53L4CD_FIRMWARE_SYSTEM_STATUS            0x00E5
#define VL53L4CD_IDENTIFICATION_MODEL_ID           0x010F

#define VL53L4CD_MODEL_ID                          0xEBAA

/* Timeouts for polled waits in init(). */
#define VL53L4CD_BOOT_TIMEOUT_MS                   100
#define VL53L4CD_VHV_TIMEOUT_MS                    100

/*
 * ULD default configuration block written at 0x30..0x87 during init().
 * Byte-identical to Pololu's VL53L4CD_DEFAULT_CONFIGURATION[].
 */
static const uint8_t vl53l4cd_default_config[88] = {
	0x11, /* 0x30 : bit 4 = 1 -> active-low INT */
	0x02, /* 0x31 */
	0x00, /* 0x32 */
	0x02, /* 0x33 */
	0x08, /* 0x34 */
	0x00, /* 0x35 */
	0x08, /* 0x36 */
	0x10, /* 0x37 */
	0x01, /* 0x38 */
	0x01, /* 0x39 */
	0x00, /* 0x3a */
	0x00, /* 0x3b */
	0x00, /* 0x3c */
	0x00, /* 0x3d */
	0xff, /* 0x3e */
	0x00, /* 0x3f */
	0x0F, /* 0x40 */
	0x00, /* 0x41 */
	0x00, /* 0x42 */
	0x00, /* 0x43 */
	0x00, /* 0x44 */
	0x00, /* 0x45 */
	0x20, /* 0x46 : interrupt config — new sample ready */
	0x0b, /* 0x47 */
	0x00, /* 0x48 */
	0x00, /* 0x49 */
	0x02, /* 0x4a */
	0x14, /* 0x4b */
	0x21, /* 0x4c */
	0x00, /* 0x4d */
	0x00, /* 0x4e */
	0x05, /* 0x4f */
	0x00, /* 0x50 */
	0x00, /* 0x51 */
	0x00, /* 0x52 */
	0x00, /* 0x53 */
	0xc8, /* 0x54 */
	0x00, /* 0x55 */
	0x00, /* 0x56 */
	0x38, /* 0x57 */
	0xff, /* 0x58 */
	0x01, /* 0x59 */
	0x00, /* 0x5a */
	0x08, /* 0x5b */
	0x00, /* 0x5c */
	0x00, /* 0x5d */
	0x01, /* 0x5e */
	0xcc, /* 0x5f */
	0x07, /* 0x60 */
	0x01, /* 0x61 */
	0xf1, /* 0x62 */
	0x05, /* 0x63 */
	0x00, /* 0x64 : sigma threshold MSB (default 90 mm) */
	0xa0, /* 0x65 : sigma threshold LSB */
	0x00, /* 0x66 : min count rate MSB */
	0x80, /* 0x67 : min count rate LSB */
	0x08, /* 0x68 */
	0x38, /* 0x69 */
	0x00, /* 0x6a */
	0x00, /* 0x6b */
	0x00, /* 0x6c : inter-measurement period MSB */
	0x00, /* 0x6d */
	0x0f, /* 0x6e */
	0x89, /* 0x6f : inter-measurement period LSB */
	0x00, /* 0x70 */
	0x00, /* 0x71 */
	0x00, /* 0x72 */
	0x00, /* 0x73 */
	0x00, /* 0x74 */
	0x00, /* 0x75 */
	0x00, /* 0x76 */
	0x01, /* 0x77 */
	0x07, /* 0x78 */
	0x05, /* 0x79 */
	0x06, /* 0x7a */
	0x06, /* 0x7b */
	0x00, /* 0x7c */
	0x00, /* 0x7d */
	0x02, /* 0x7e */
	0xc7, /* 0x7f */
	0xff, /* 0x80 */
	0x9B, /* 0x81 */
	0x00, /* 0x82 */
	0x00, /* 0x83 */
	0x00, /* 0x84 */
	0x01, /* 0x85 */
	0x00, /* 0x86 : clear interrupt */
	0x00, /* 0x87 : start ranging (0 = off, 0x40 = autonomous, 0x21 = continuous) */
};

/*
 * Pololu/ULD raw status code remap. Index = (RESULT_RANGE_STATUS[0] & 0x1F),
 * value = documented status (0 == valid measurement). 255 = undocumented.
 */
static const uint8_t vl53l4cd_status_rtn[24] = {
	255, 255, 255, 5, 2, 4, 1, 7, 3,
	0,   255, 255, 9, 13, 255, 255, 255, 255, 10, 6,
	255, 255, 11, 12,
};

/* ------------------------------------------------------------------------- */
/* Low-level register I/O with 16-bit register addresses (MSB first).         */
/* ------------------------------------------------------------------------- */

static int reg_read(struct vl53l4cd_dev *dev, uint16_t reg, uint8_t *buf, size_t len)
{
	uint8_t addr_be[2] = { (uint8_t)(reg >> 8), (uint8_t)(reg & 0xFF) };

	return i2c_write_read(dev->i2c, dev->addr, addr_be, sizeof(addr_be), buf, len);
}

static int reg_write(struct vl53l4cd_dev *dev, uint16_t reg, const uint8_t *data, size_t len)
{
	uint8_t buf[2 + 32];

	if (len > sizeof(buf) - 2) {
		return -EINVAL;
	}
	buf[0] = (uint8_t)(reg >> 8);
	buf[1] = (uint8_t)(reg & 0xFF);
	memcpy(&buf[2], data, len);

	return i2c_write(dev->i2c, buf, 2 + len, dev->addr);
}

static int read_u8(struct vl53l4cd_dev *dev, uint16_t reg, uint8_t *val)
{
	return reg_read(dev, reg, val, 1);
}

static int read_u16(struct vl53l4cd_dev *dev, uint16_t reg, uint16_t *val)
{
	uint8_t b[2];
	int err = reg_read(dev, reg, b, sizeof(b));

	if (err == 0) {
		*val = ((uint16_t)b[0] << 8) | b[1];
	}
	return err;
}

static int read_u32(struct vl53l4cd_dev *dev, uint16_t reg, uint32_t *val)
{
	uint8_t b[4];
	int err = reg_read(dev, reg, b, sizeof(b));

	if (err == 0) {
		*val = ((uint32_t)b[0] << 24) | ((uint32_t)b[1] << 16) |
		       ((uint32_t)b[2] << 8) | b[3];
	}
	return err;
}

static int write_u8(struct vl53l4cd_dev *dev, uint16_t reg, uint8_t val)
{
	return reg_write(dev, reg, &val, 1);
}

static int write_u16(struct vl53l4cd_dev *dev, uint16_t reg, uint16_t val)
{
	uint8_t b[2] = { (uint8_t)(val >> 8), (uint8_t)(val & 0xFF) };

	return reg_write(dev, reg, b, sizeof(b));
}

static int write_u32(struct vl53l4cd_dev *dev, uint16_t reg, uint32_t val)
{
	uint8_t b[4] = {
		(uint8_t)(val >> 24), (uint8_t)(val >> 16),
		(uint8_t)(val >> 8),  (uint8_t)(val & 0xFF),
	};

	return reg_write(dev, reg, b, sizeof(b));
}

/* ------------------------------------------------------------------------- */
/* Public API.                                                                */
/* ------------------------------------------------------------------------- */

int vl53l4cd_data_ready(struct vl53l4cd_dev *dev, bool *ready)
{
	uint8_t status;
	int err;

	if (!dev || !dev->initialized || !ready) {
		return -EINVAL;
	}

	/*
	 * The ULD default config (0x30 byte) puts INT into active-low mode.
	 * GPIO_TIO_HV_STATUS bit 0 reflects the current INT line state; data
	 * is ready when the line is electrically LOW (bit 0 == 0).
	 */
	err = read_u8(dev, VL53L4CD_GPIO_TIO_HV_STATUS, &status);
	if (err != 0) {
		return err;
	}

	*ready = ((status & 0x01) == 0);
	return 0;
}

int vl53l4cd_clear_interrupt(struct vl53l4cd_dev *dev)
{
	if (!dev || !dev->initialized) {
		return -EINVAL;
	}
	return write_u8(dev, VL53L4CD_SYSTEM_INTERRUPT_CLEAR, 0x01);
}

int vl53l4cd_start_continuous(struct vl53l4cd_dev *dev)
{
	uint32_t inter_meas;
	int err;

	if (!dev || !dev->initialized) {
		return -EINVAL;
	}

	/*
	 * Mirrors Pololu startContinuous(): pick the SYSTEM_START opcode based
	 * on whether INTERMEASUREMENT_MS is zero (continuous, 0x21) or non-zero
	 * (autonomous low power, 0x40).
	 */
	err = read_u32(dev, VL53L4CD_INTERMEASUREMENT_MS, &inter_meas);
	if (err != 0) {
		return err;
	}

	return write_u8(dev, VL53L4CD_SYSTEM_START, inter_meas == 0 ? 0x21 : 0x40);
}

int vl53l4cd_stop_continuous(struct vl53l4cd_dev *dev)
{
	if (!dev || !dev->initialized) {
		return -EINVAL;
	}
	return write_u8(dev, VL53L4CD_SYSTEM_START, 0x80);
}

int vl53l4cd_set_range_timing(struct vl53l4cd_dev *dev,
			      uint8_t budget_ms,
			      uint32_t inter_meas_ms)
{
	uint16_t osc_frequency = 0;
	uint16_t clock_pll_raw = 0;
	uint32_t timing_budget_us;
	uint32_t macro_period_us;
	uint32_t tmp;
	uint16_t ls_byte;
	uint8_t  ms_byte;
	int err;

	if (!dev || !dev->initialized) {
		return -EINVAL;
	}
	if (budget_ms < 10 || budget_ms > 200) {
		return -EINVAL;
	}
	if (inter_meas_ms != 0 && inter_meas_ms <= budget_ms) {
		return -EINVAL;
	}

	err = read_u16(dev, VL53L4CD_OSC_FREQUENCY, &osc_frequency);
	if (err != 0) {
		return err;
	}
	if (osc_frequency == 0) {
		return -ENODEV;
	}

	timing_budget_us = (uint32_t)budget_ms * 1000U;
	macro_period_us  = ((uint32_t)2304U * (0x40000000U / osc_frequency)) >> 6;

	if (inter_meas_ms == 0) {
		err = write_u32(dev, VL53L4CD_INTERMEASUREMENT_MS, 0);
		if (err != 0) {
			return err;
		}
		timing_budget_us -= 2500U;
	} else {
		uint32_t inter_measurement;

		err = read_u16(dev, VL53L4CD_RESULT_OSC_CALIBRATE_VAL, &clock_pll_raw);
		if (err != 0) {
			return err;
		}

		/*
		 * Pololu uses a float factor of 1.055; we use the fixed-point
		 * form (ms * pll * 1055 / 1000) to keep soft-float out of the
		 * image. clock_pll value is the low 10 bits of OSC_CALIBRATE.
		 */
		uint32_t clock_pll = (uint32_t)(clock_pll_raw & 0x3FFU);

		inter_measurement = (inter_meas_ms * clock_pll * 1055U) / 1000U;
		err = write_u32(dev, VL53L4CD_INTERMEASUREMENT_MS, inter_measurement);
		if (err != 0) {
			return err;
		}
		timing_budget_us -= 4300U;
		timing_budget_us /= 2U;
	}

	timing_budget_us <<= 12;

	tmp = (macro_period_us * 16U) >> 6;
	ls_byte = (uint16_t)(((timing_budget_us + (tmp >> 1)) / tmp) - 1U);
	ms_byte = 0;
	while (ls_byte > 0xFFU) {
		ls_byte >>= 1;
		ms_byte++;
	}
	err = write_u16(dev, VL53L4CD_RANGE_CONFIG_A,
			((uint16_t)ms_byte << 8) | ls_byte);
	if (err != 0) {
		return err;
	}

	tmp = (macro_period_us * 12U) >> 6;
	ls_byte = (uint16_t)(((timing_budget_us + (tmp >> 1)) / tmp) - 1U);
	ms_byte = 0;
	while (ls_byte > 0xFFU) {
		ls_byte >>= 1;
		ms_byte++;
	}
	err = write_u16(dev, VL53L4CD_RANGE_CONFIG_B,
			((uint16_t)ms_byte << 8) | ls_byte);
	if (err != 0) {
		return err;
	}

	return 0;
}

int vl53l4cd_read_range_mm(struct vl53l4cd_dev *dev, uint16_t *range_mm, uint8_t *status_out)
{
	uint8_t buf[15];
	uint8_t raw_status;
	int err;

	if (!dev || !dev->initialized || !range_mm || !status_out) {
		return -EINVAL;
	}

	/*
	 * Block-read 15 bytes from RESULT_RANGE_STATUS (0x0089) — same span as
	 * Pololu readResults(). We only consume bytes [0] (raw status, low 5
	 * bits) and [13..14] (distance, big-endian uint16 mm); SPAD count,
	 * signal/ambient rates, and sigma are ignored for this phase.
	 */
	err = reg_read(dev, VL53L4CD_RESULT_RANGE_STATUS, buf, sizeof(buf));
	if (err != 0) {
		return err;
	}

	raw_status = buf[0] & 0x1FU;
	*status_out = (raw_status < ARRAY_SIZE(vl53l4cd_status_rtn))
			? vl53l4cd_status_rtn[raw_status] : 255U;

	*range_mm = ((uint16_t)buf[13] << 8) | buf[14];
	return 0;
}

static int wait_data_ready(struct vl53l4cd_dev *dev, uint32_t timeout_ms)
{
	int64_t deadline = k_uptime_get() + (int64_t)timeout_ms;
	bool ready = false;
	int err;

	while (k_uptime_get() < deadline) {
		err = vl53l4cd_data_ready(dev, &ready);
		if (err == 0 && ready) {
			return 0;
		}
		k_sleep(K_MSEC(1));
	}
	return -ETIMEDOUT;
}

int vl53l4cd_init(struct vl53l4cd_dev *dev, const struct device *i2c, uint16_t addr)
{
	uint16_t model_id = 0;
	uint8_t  fw_status = 0;
	int64_t  deadline;
	int err;

	if (!dev || !i2c) {
		return -EINVAL;
	}

	dev->i2c = i2c;
	dev->addr = addr;
	dev->initialized = true; /* allow the low-level helpers to run */

	if (!device_is_ready(i2c)) {
		dev->initialized = false;
		return -ENODEV;
	}

	err = read_u16(dev, VL53L4CD_IDENTIFICATION_MODEL_ID, &model_id);
	if (err != 0) {
		LOG_ERR("model-id read failed (%d)", err);
		dev->initialized = false;
		return -ENODEV;
	}
	if (model_id != VL53L4CD_MODEL_ID) {
		LOG_ERR("unexpected model id 0x%04x (expected 0x%04x)",
			model_id, VL53L4CD_MODEL_ID);
		dev->initialized = false;
		return -ENODEV;
	}

	/* Wait for FW boot status to reach 0x03 (max ~1.2 ms per datasheet). */
	deadline = k_uptime_get() + (int64_t)VL53L4CD_BOOT_TIMEOUT_MS;
	while (k_uptime_get() < deadline) {
		err = read_u8(dev, VL53L4CD_FIRMWARE_SYSTEM_STATUS, &fw_status);
		if (err == 0 && fw_status == 0x03U) {
			break;
		}
		k_sleep(K_MSEC(1));
	}
	if (fw_status != 0x03U) {
		LOG_ERR("firmware boot status stuck at 0x%02x", fw_status);
		dev->initialized = false;
		return -ETIMEDOUT;
	}

	/*
	 * Pololu init(true, false): 2V8 IO, fast-mode (not fast-mode-plus).
	 * 0x2D bit-5/2 control fast-mode-plus (we leave 0); 0x2E/0x2F set the
	 * I2C / GPIO IO voltage domain to 2.8 V.
	 */
	err  = write_u8(dev, 0x002D, 0x00);
	err |= write_u8(dev, 0x002E, 0x01);
	err |= write_u8(dev, 0x002F, 0x01);
	if (err != 0) {
		dev->initialized = false;
		return -EIO;
	}

	/*
	 * Load the ULD default configuration table at 0x30..0x87. Chunked into
	 * 30-byte blocks to stay byte-identical to the Pololu source (which
	 * chunks due to the Arduino Wire 32-byte TX buffer); on nRF52840 we
	 * could send the whole block in one transaction, but chunking keeps
	 * the port easy to audit against the upstream library.
	 */
	const uint8_t block_size = 30;
	for (uint8_t start_reg = 0x30; start_reg <= 0x87; start_reg += block_size) {
		uint8_t end_reg = MIN((uint8_t)(start_reg + block_size - 1U), (uint8_t)0x87);
		uint8_t n = end_reg - start_reg + 1U;

		err = reg_write(dev, (uint16_t)start_reg,
				&vl53l4cd_default_config[start_reg - 0x30], n);
		if (err != 0) {
			LOG_ERR("default config write at 0x%02x failed (%d)", start_reg, err);
			dev->initialized = false;
			return -EIO;
		}
	}

	/* Start VHV: required one-time temperature calibration on first ranging. */
	err = write_u8(dev, VL53L4CD_SYSTEM_START, 0x40);
	if (err != 0) {
		dev->initialized = false;
		return -EIO;
	}

	err = wait_data_ready(dev, VL53L4CD_VHV_TIMEOUT_MS);
	if (err != 0) {
		LOG_ERR("VHV calibration timed out");
		dev->initialized = false;
		return err;
	}

	err  = vl53l4cd_clear_interrupt(dev);
	err |= vl53l4cd_stop_continuous(dev);
	err |= write_u8(dev,  VL53L4CD_VHV_CONFIG_TIMEOUT_MACROP_LOOP_BD, 0x09);
	err |= write_u8(dev,  0x000B, 0x00);
	err |= write_u16(dev, 0x0024, 0x0500);
	if (err != 0) {
		dev->initialized = false;
		return -EIO;
	}

	/* Default 50 ms timing budget in continuous mode (inter-meas = 0). */
	err = vl53l4cd_set_range_timing(dev, 50, 0);
	if (err != 0) {
		dev->initialized = false;
		return err;
	}

	LOG_INF("VL53L4CD ready (addr=0x%02x, 50 ms timing budget, continuous)", addr);
	return 0;
}
