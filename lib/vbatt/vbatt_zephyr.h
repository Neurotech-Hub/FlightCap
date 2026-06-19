#ifndef VBATT_ZEPHYR_H
#define VBATT_ZEPHYR_H

#include <stdint.h>

int vbatt_init(void);
int vbatt_read_mv(int32_t *mv_out);

#endif
