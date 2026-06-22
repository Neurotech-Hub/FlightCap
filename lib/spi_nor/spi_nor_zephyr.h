#ifndef SPI_NOR_ZEPHYR_H
#define SPI_NOR_ZEPHYR_H

#include <zephyr/device.h>

int spi_nor_zephyr_check(const struct device **flash_out);
int spi_nor_zephyr_verify(const struct device *flash);

#endif
