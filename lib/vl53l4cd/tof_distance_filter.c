#include "tof_distance_filter.h"

#include <errno.h>
#include <string.h>

static uint16_t cycle_ring[TOF_CYCLE_HISTORY];
static uint16_t cycle_head;
static uint16_t cycle_count;

static void insertion_sort_u16(uint16_t *arr, uint16_t n)
{
	for (uint16_t i = 1; i < n; i++) {
		uint16_t key = arr[i];
		int j = (int)i - 1;

		while (j >= 0 && arr[j] > key) {
			arr[j + 1] = arr[j];
			j--;
		}
		arr[j + 1] = key;
	}
}

void tof_cycle_filter_init(void)
{
	tof_cycle_filter_clear();
}

void tof_cycle_filter_clear(void)
{
	cycle_head = 0U;
	cycle_count = 0U;
	memset(cycle_ring, 0, sizeof(cycle_ring));
}

int tof_trimmed_mean(const uint16_t *samples, uint16_t count, uint16_t *out_mm)
{
	uint16_t scratch[TOF_BURST_SAMPLES];
	uint16_t trim;
	uint16_t lo;
	uint16_t hi;
	uint32_t sum;
	uint32_t mid_count;

	if (samples == NULL || out_mm == NULL || count < TOF_BURST_MIN_VALID ||
	    count > TOF_BURST_SAMPLES) {
		return -EINVAL;
	}

	memcpy(scratch, samples, (size_t)count * sizeof(scratch[0]));
	insertion_sort_u16(scratch, count);

	trim = (uint16_t)((uint32_t)count * TOF_TRIM_NUM / TOF_TRIM_DEN);
	lo = trim;
	hi = (uint16_t)(count - trim);

	if (hi <= lo) {
		*out_mm = scratch[count / 2U];
		return 0;
	}

	sum = 0U;
	for (uint16_t i = lo; i < hi; i++) {
		sum += scratch[i];
	}

	mid_count = (uint32_t)(hi - lo);
	*out_mm = (uint16_t)((sum + (mid_count / 2U)) / mid_count);
	return 0;
}

int tof_cycle_filter_push_median(uint16_t cycle_mm, uint16_t *out_mm)
{
	uint16_t scratch[TOF_CYCLE_HISTORY];
	uint16_t n;

	if (out_mm == NULL) {
		return -EINVAL;
	}

	cycle_ring[cycle_head] = cycle_mm;
	cycle_head = (uint16_t)((cycle_head + 1U) % TOF_CYCLE_HISTORY);
	if (cycle_count < TOF_CYCLE_HISTORY) {
		cycle_count++;
	}

	n = cycle_count;
	if (n == TOF_CYCLE_HISTORY) {
		memcpy(scratch, cycle_ring, sizeof(cycle_ring));
	} else {
		memcpy(scratch, cycle_ring, (size_t)n * sizeof(scratch[0]));
	}
	insertion_sort_u16(scratch, n);
	*out_mm = scratch[n / 2U];
	return 0;
}
