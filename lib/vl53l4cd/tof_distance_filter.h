#ifndef TOF_DISTANCE_FILTER_H
#define TOF_DISTANCE_FILTER_H

#include <stdint.h>

#define TOF_BURST_DISCARD   2U
#define TOF_BURST_SAMPLES   16U
#define TOF_BURST_MIN_VALID 4U
#define TOF_BURST_READS     (TOF_BURST_DISCARD + TOF_BURST_SAMPLES)
#define TOF_CYCLE_HISTORY   5U

#define TOF_TRIM_NUM 1U
#define TOF_TRIM_DEN 5U

/** Idempotent; clears cross-cycle history. */
void tof_cycle_filter_init(void);

/** Drop all cross-cycle history (shelf entry, etc.). */
void tof_cycle_filter_clear(void);

/**
 * Trimmed mean of burst samples (drop top/bottom 1/5, round-to-nearest).
 *
 * @retval 0       Success; *out_mm set.
 * @retval -EINVAL Invalid args or fewer than TOF_BURST_MIN_VALID samples.
 */
int tof_trimmed_mean(const uint16_t *samples, uint16_t count, uint16_t *out_mm);

/**
 * Append a cycle estimate and return median of buffered history (up to 5).
 *
 * @retval 0       Success; *out_mm is median of history including @p cycle_mm.
 * @retval -EINVAL Invalid args.
 */
int tof_cycle_filter_push_median(uint16_t cycle_mm, uint16_t *out_mm);

#endif /* TOF_DISTANCE_FILTER_H */
