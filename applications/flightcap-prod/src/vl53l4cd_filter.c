#include "vl53l4cd_filter.h"

#include <errno.h>
#include <string.h>

#include <zephyr/kernel.h>

static K_MUTEX_DEFINE(filter_mu);

static uint16_t buf_data[VL53L4CD_FILTER_CAP];
static uint16_t buf_head;  /* index of next slot to write */
static uint16_t buf_count; /* number of valid entries (0..CAP) */

void vl53l4cd_filter_init(void)
{
	vl53l4cd_filter_clear();
}

void vl53l4cd_filter_push(uint16_t mm)
{
	k_mutex_lock(&filter_mu, K_FOREVER);
	buf_data[buf_head] = mm;
	buf_head = (uint16_t)((buf_head + 1U) % VL53L4CD_FILTER_CAP);
	if (buf_count < VL53L4CD_FILTER_CAP) {
		buf_count++;
	}
	k_mutex_unlock(&filter_mu);
}

void vl53l4cd_filter_clear(void)
{
	k_mutex_lock(&filter_mu, K_FOREVER);
	buf_head = 0;
	buf_count = 0;
	k_mutex_unlock(&filter_mu);
}

int vl53l4cd_filter_snapshot(uint16_t *out_mm, uint16_t *out_count)
{
	uint16_t scratch[VL53L4CD_FILTER_CAP];
	uint16_t n;

	if (!out_mm || !out_count) {
		return -EINVAL;
	}

	/*
	 * Take a quick snapshot under the lock, then sort/average outside the
	 * critical section so we never block the reader thread while doing
	 * O(N^2) insertion sort.
	 *
	 * When buf_count < CAP, valid entries live at buf_data[0..count-1].
	 * When buf_count == CAP the ring is full and every slot is valid (the
	 * order is shuffled by circular writes, but order doesn't matter for
	 * sort + trimmed mean).
	 */
	k_mutex_lock(&filter_mu, K_FOREVER);
	n = buf_count;
	if (n > 0) {
		memcpy(scratch, buf_data, (size_t)n * sizeof(buf_data[0]));
	}
	k_mutex_unlock(&filter_mu);

	*out_count = n;
	if (n < VL53L4CD_FILTER_MIN_VALID) {
		return -EAGAIN;
	}

	for (uint16_t i = 1; i < n; i++) {
		uint16_t key = scratch[i];
		int j = (int)i - 1;

		while (j >= 0 && scratch[j] > key) {
			scratch[j + 1] = scratch[j];
			j--;
		}
		scratch[j + 1] = key;
	}

	uint16_t trim = (uint16_t)((uint32_t)n * VL53L4CD_FILTER_TRIM_NUM /
				   VL53L4CD_FILTER_TRIM_DEN);
	uint16_t lo = trim;
	uint16_t hi = (uint16_t)(n - trim);

	if (hi <= lo) {
		/* Degenerate (shouldn't happen at n >= 4 / trim 1/5) — median. */
		lo = (uint16_t)(n / 2U);
		hi = (uint16_t)(lo + 1U);
	}

	uint32_t sum = 0;
	for (uint16_t i = lo; i < hi; i++) {
		sum += scratch[i];
	}

	*out_mm = (uint16_t)(sum / (uint32_t)(hi - lo));
	return 0;
}
