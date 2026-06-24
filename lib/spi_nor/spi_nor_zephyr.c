#include "spi_nor_zephyr.h"

#include <errno.h>
#include <string.h>

#include <zephyr/devicetree.h>
#include <zephyr/drivers/flash.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(spi_nor_zephyr, LOG_LEVEL_INF);

#define FLASH_NODE DT_ALIAS(spi_flash)

#define EXPECTED_JEDEC_0 0xC2U
#define EXPECTED_JEDEC_1 0x28U
#define EXPECTED_JEDEC_2 0x14U
/* jedec,spi-nor `size` devicetree property is in bits; driver reports bytes. */
#define EXPECTED_FLASH_SIZE (DT_PROP(DT_ALIAS(spi_flash), size) / 8U)
#define VERIFY_SECTOR_SIZE 4096U
#define VERIFY_PATTERN_LEN 256U

static int spi_nor_zephyr_check_impl(const struct device **flash_out, bool quiet)
{
#if DT_NODE_EXISTS(FLASH_NODE)
	const struct device *flash = DEVICE_DT_GET(FLASH_NODE);
	uint8_t jedec[3];
	uint64_t size = 0;
	int ret;

	if (flash_out) {
		*flash_out = NULL;
	}

	if (!device_is_ready(flash)) {
		if (quiet) {
			LOG_DBG("SPI NOR not ready");
		} else {
			LOG_ERR("SPI NOR check FAIL: flash device not ready");
		}
		return -ENODEV;
	}

#if IS_ENABLED(CONFIG_FLASH_JESD216_API)
	ret = flash_read_jedec_id(flash, jedec);
	if (ret < 0) {
		if (quiet) {
			LOG_DBG("SPI NOR JEDEC read failed (%d)", ret);
		} else {
			LOG_ERR("SPI NOR check FAIL: JEDEC read (%d)", ret);
		}
		return ret;
	}
#else
	if (quiet) {
		LOG_DBG("SPI NOR probe needs CONFIG_FLASH_JESD216_API");
	} else {
		LOG_ERR("SPI NOR check FAIL: enable CONFIG_FLASH_JESD216_API");
	}
	return -ENOTSUP;
#endif

	if (jedec[0] != EXPECTED_JEDEC_0 || jedec[1] != EXPECTED_JEDEC_1 ||
	    jedec[2] != EXPECTED_JEDEC_2) {
		if (quiet) {
			LOG_DBG("SPI NOR JEDEC %02x %02x %02x (expected c2 28 14)", jedec[0],
				jedec[1], jedec[2]);
		} else {
			LOG_ERR("SPI NOR check FAIL: JEDEC %02x %02x %02x (expected c2 28 14)",
				jedec[0], jedec[1], jedec[2]);
		}
		return -ENODEV;
	}

	if (!quiet) {
		LOG_INF("JEDEC ID: %02x %02x %02x OK", jedec[0], jedec[1], jedec[2]);
	}

	ret = flash_get_size(flash, &size);
	if (ret < 0) {
		if (quiet) {
			LOG_DBG("SPI NOR size read failed (%d)", ret);
		} else {
			LOG_ERR("SPI NOR check FAIL: size read (%d)", ret);
		}
		return ret;
	}

	if (size != EXPECTED_FLASH_SIZE) {
		if (quiet) {
			LOG_DBG("SPI NOR size %llu (expected %u)", size, EXPECTED_FLASH_SIZE);
		} else {
			LOG_ERR("SPI NOR check FAIL: size %llu (expected %u)", size,
				EXPECTED_FLASH_SIZE);
		}
		return -ENODEV;
	}

	if (!quiet) {
		LOG_INF("flash size: %u bytes OK", (unsigned int)size);
	}

	if (flash_out) {
		*flash_out = flash;
	}

	if (!quiet) {
		LOG_INF("SPI NOR check PASS");
	}
	return 0;
#else
	ARG_UNUSED(flash_out);
	ARG_UNUSED(quiet);
	if (!quiet) {
		LOG_ERR("SPI NOR check FAIL: board has no spi-flash alias");
	}
	return -ENODEV;
#endif
}

int spi_nor_zephyr_probe(const struct device **flash_out)
{
	return spi_nor_zephyr_check_impl(flash_out, true);
}

int spi_nor_zephyr_check(const struct device **flash_out)
{
	return spi_nor_zephyr_check_impl(flash_out, false);
}

int spi_nor_zephyr_verify(const struct device *flash)
{
#if DT_NODE_EXISTS(FLASH_NODE)
	uint8_t pattern[VERIFY_PATTERN_LEN];
	uint8_t readback[VERIFY_PATTERN_LEN];
	uint64_t size = 0;
	off_t offset;
	int ret;

	if (!flash) {
		return -EINVAL;
	}

	ret = spi_nor_zephyr_check(NULL);
	if (ret < 0) {
		return ret;
	}

	ret = flash_get_size(flash, &size);
	if (ret < 0) {
		return ret;
	}

	if (size < VERIFY_SECTOR_SIZE) {
		LOG_ERR("SPI NOR verify FAIL: flash too small");
		return -EINVAL;
	}

	offset = (off_t)(size - VERIFY_SECTOR_SIZE);
	LOG_INF("R/W test @ offset 0x%lx", (long)offset);

	ret = flash_erase(flash, offset, VERIFY_SECTOR_SIZE);
	if (ret < 0) {
		LOG_ERR("SPI NOR verify FAIL: erase (%d)", ret);
		return ret;
	}

	for (size_t i = 0; i < VERIFY_PATTERN_LEN; i++) {
		pattern[i] = (uint8_t)(0xA5U ^ (uint8_t)i);
	}

	ret = flash_write(flash, offset, pattern, VERIFY_PATTERN_LEN);
	if (ret < 0) {
		LOG_ERR("SPI NOR verify FAIL: write (%d)", ret);
		return ret;
	}

	ret = flash_read(flash, offset, readback, VERIFY_PATTERN_LEN);
	if (ret < 0) {
		LOG_ERR("SPI NOR verify FAIL: read (%d)", ret);
		return ret;
	}

	if (memcmp(pattern, readback, VERIFY_PATTERN_LEN) != 0) {
		LOG_ERR("SPI NOR verify FAIL: data mismatch");
		return -EIO;
	}

	LOG_INF("R/W test @ offset 0x%lx: PASS", (long)offset);
	return 0;
#else
	ARG_UNUSED(flash);
	return -ENODEV;
#endif
}
