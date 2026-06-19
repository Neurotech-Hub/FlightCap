/*
 * Minimal LIS2DH12 register definitions used by lis2dh12_zephyr.
 * Derived from STMicroelectronics lis2dh12_reg.h (STSW-GRD02).
 */
#ifndef LIS2DH12_REG_H
#define LIS2DH12_REG_H

#include <stdint.h>

#define LIS2DH12_WHO_AM_I 0x0FU

float lis2dh12_from_lsb_lp_to_celsius(int16_t lsb);

#endif
