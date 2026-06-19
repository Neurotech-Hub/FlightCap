#include "lis2dh12_reg.h"

float lis2dh12_from_lsb_lp_to_celsius(int16_t lsb)
{
	return ((float)lsb / 256.0f) + 25.0f;
}
