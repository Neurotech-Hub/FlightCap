#ifndef DFU_MODE_H
#define DFU_MODE_H

#include "prod_state.h"

int dfu_mode_enter(struct prod_context *ctx);
int dfu_mode_exit(struct prod_context *ctx);

#endif
