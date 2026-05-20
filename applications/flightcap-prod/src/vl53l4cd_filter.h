#ifndef FLIGHTCAP_VL53L4CD_FILTER_H
#define FLIGHTCAP_VL53L4CD_FILTER_H

#include <stdint.h>

/*
 * Rolling, trimmed-mean filter for VL53L4CD samples.
 *
 * The reader thread pushes valid millimeter readings into a fixed-capacity
 * circular buffer. The main 1 s loop calls snapshot() to grab a sorted
 * trimmed mean across the currently buffered samples (drop top/bottom
 * VL53L4CD_FILTER_TRIM_NUM/VL53L4CD_FILTER_TRIM_DEN, average the middle).
 *
 * - Same trimmed-mean shape as the Arduino reference, but non-draining so
 *   the main loop can sample it at any cadence without starving the reader.
 * - Mutex-protected so push() (background thread) and snapshot() (main
 *   thread) can race safely.
 */
#define VL53L4CD_FILTER_CAP       16   /* rolling buffer capacity */
#define VL53L4CD_FILTER_MIN_VALID 4    /* require >= 4 samples to return a value */
#define VL53L4CD_FILTER_TRIM_NUM  1    /* trim 1/5 of samples top and bottom */
#define VL53L4CD_FILTER_TRIM_DEN  5

/** Idempotent. Safe to call before or after K_MUTEX_DEFINE initialization. */
void vl53l4cd_filter_init(void);

/** Append a valid millimeter sample (overwrites oldest when full). */
void vl53l4cd_filter_push(uint16_t mm);

/** Drop all buffered samples — used on wake-from-sleep. */
void vl53l4cd_filter_clear(void);

/**
 * Snapshot the current trimmed-mean and sample count.
 *
 * @param out_mm     [out] Filtered millimeter value (only written on success).
 * @param out_count  [out] Number of samples present at snapshot time (always written).
 *
 * @retval 0        Success.
 * @retval -EAGAIN  Fewer than VL53L4CD_FILTER_MIN_VALID samples buffered.
 */
int vl53l4cd_filter_snapshot(uint16_t *out_mm, uint16_t *out_count);

#endif /* FLIGHTCAP_VL53L4CD_FILTER_H */
