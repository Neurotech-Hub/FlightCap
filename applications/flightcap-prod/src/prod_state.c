#include "prod_state.h"

#include <errno.h>
#include <limits.h>

#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/irq.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/random/random.h>
#include <zephyr/sys/util.h>

#include "dfu_mode.h"
#include "telemetry_adv.h"
#include "vl53l0x_zephyr.h"

LOG_MODULE_REGISTER(prod_state, LOG_LEVEL_INF);

#define MAGNET_PAIR_HOLD_MS 3000
#define MAGNET_DFU_ENTER_MS 10000
#define MAGNET_HOLD_GAP_MS 500
#define LED_BOOT_BLINK_COUNT 3
#define LED_BOOT_BLINK_ON_MS 100
#define LED_BOOT_BLINK_OFF_MS 100
#define LED_ADV_PULSE_MS 50
#define LED_PAIR_BLINK_HALF_MS 100
#define FACE_UP_Z_MIN_MG 12000
#define FACE_UP_ENTER_SAMPLES 3
#define SHELF_LEAVE_SAMPLES 2
#define MOTION_THRESHOLD_MG 3
#define MOTION_DURATION_SAMPLES 0
#define TOF_SAMPLES 10U
#define ADV_WINDOW_MS 10000U
#define SLEEP_MS 10000U
#define SLEEP_JITTER_MS 1000U
#define ADV_UPDATE_MS 500U
#define PROD_WAIT_SLICE_MS 50U

enum magnet_hold_phase {
	MAGNET_HOLD_IDLE,
	MAGNET_HOLD_HOLDING,
	MAGNET_HOLD_HOLD_MET,
	MAGNET_HOLD_COOLDOWN,
};

enum prod_wait_result {
	PROD_WAIT_OK,
	PROD_WAIT_PAIR_TOGGLE,
	PROD_WAIT_DFU_ENTER,
	PROD_WAIT_DFU_EXIT,
	PROD_WAIT_SHELF_EXIT,
	PROD_WAIT_FACE_UP,
};

static enum magnet_hold_phase magnet_phase = MAGNET_HOLD_IDLE;
static int64_t magnet_phase_start_ms;
static int64_t magnet_hold_origin_ms;
static bool magnet_pair_toggle_pending;
static bool magnet_dfu_enter_pending;
static bool magnet_dfu_exit_pending;
static bool dfu_magnet_must_release;
enum prod_wait_mode {
	PROD_WAIT_MODE_RUN,
	PROD_WAIT_MODE_RUN_ADV,
	PROD_WAIT_MODE_SHELF,
	PROD_WAIT_MODE_PAIR,
	PROD_WAIT_MODE_DFU,
};

static uint8_t shelf_int2_baseline;
static uint8_t face_up_streak;
static uint8_t shelf_leave_streak;

static void leds_set(const struct prod_context *ctx, int value)
{
	(void)gpio_pin_set_dt(ctx->led0, value);
	(void)gpio_pin_set_dt(ctx->led1, value);
}

static void leds_pulse_once(const struct prod_context *ctx, uint32_t ms)
{
	leds_set(ctx, 1);
	k_sleep(K_MSEC(ms));
	leds_set(ctx, 0);
}

static int64_t pair_led_phase_ms;
static bool pair_led_on;

static void pair_led_blink_reset(void)
{
	pair_led_phase_ms = 0;
	pair_led_on = false;
}

static void pair_led_blink_stop(const struct prod_context *ctx)
{
	pair_led_blink_reset();
	leds_set(ctx, 0);
}

static void pair_led_blink_poll(const struct prod_context *ctx)
{
	int64_t now = k_uptime_get();

	if (pair_led_phase_ms == 0) {
		pair_led_phase_ms = now;
		pair_led_on = true;
		leds_set(ctx, 1);
		return;
	}

	if ((now - pair_led_phase_ms) >= LED_PAIR_BLINK_HALF_MS) {
		pair_led_phase_ms = now;
		pair_led_on = !pair_led_on;
		leds_set(ctx, pair_led_on ? 1 : 0);
	}
}

static bool magnet_is_active(const struct prod_context *ctx)
{
	int state = gpio_pin_get_dt(ctx->magnet);

	return state > 0;
}

static void magnet_hold_to_idle(void)
{
	magnet_phase = MAGNET_HOLD_IDLE;
	magnet_hold_origin_ms = 0;
}

static bool magnet_hold_reached_ms(int64_t now, uint32_t ms)
{
	if (magnet_hold_origin_ms == 0) {
		return false;
	}

	return (now - magnet_hold_origin_ms) >= (int64_t)ms;
}

static void magnet_try_dfu_enter(const struct prod_context *ctx, bool active, int64_t now)
{
	if (!active || ctx->dfu_active) {
		return;
	}

	if (magnet_hold_reached_ms(now, MAGNET_DFU_ENTER_MS)) {
		magnet_dfu_enter_pending = true;
		magnet_pair_toggle_pending = false;
		magnet_hold_to_idle();
		LOG_INF("magnet_hold dfu enter");
	}
}

static void magnet_hold_poll(const struct prod_context *ctx)
{
	bool active = magnet_is_active(ctx);
	int64_t now = k_uptime_get();

	if (ctx->dfu_active) {
		switch (magnet_phase) {
		case MAGNET_HOLD_IDLE:
			if (dfu_magnet_must_release) {
				if (!active) {
					dfu_magnet_must_release = false;
				}
				break;
			}
			if (active) {
				magnet_phase = MAGNET_HOLD_HOLDING;
				magnet_hold_origin_ms = now;
				magnet_phase_start_ms = now;
				LOG_INF("magnet_hold dfu-exit phase=Holding");
			}
			break;
		case MAGNET_HOLD_HOLDING:
			if (!active) {
				magnet_hold_to_idle();
				break;
			}
			if ((now - magnet_phase_start_ms) >= MAGNET_PAIR_HOLD_MS) {
				magnet_phase = MAGNET_HOLD_HOLD_MET;
				magnet_phase_start_ms = now;
				LOG_INF("magnet_hold dfu-exit phase=HoldMet");
			}
			break;
		case MAGNET_HOLD_HOLD_MET:
			if ((now - magnet_phase_start_ms) >= MAGNET_HOLD_GAP_MS) {
				magnet_phase = MAGNET_HOLD_COOLDOWN;
				LOG_INF("magnet_hold dfu-exit phase=Cooldown");
			}
			break;
		case MAGNET_HOLD_COOLDOWN:
			if (!active) {
				magnet_dfu_exit_pending = true;
				magnet_hold_to_idle();
				LOG_INF("magnet_hold dfu exit");
			}
			break;
		}
		return;
	}

	switch (magnet_phase) {
	case MAGNET_HOLD_IDLE:
		if (active) {
			magnet_phase = MAGNET_HOLD_HOLDING;
			magnet_hold_origin_ms = now;
			magnet_phase_start_ms = now;
			leds_set(ctx, 1);
			LOG_INF("magnet_hold phase=Holding");
		}
		break;
	case MAGNET_HOLD_HOLDING:
		if (!active) {
			magnet_hold_to_idle();
			leds_set(ctx, 0);
			break;
		}
		magnet_try_dfu_enter(ctx, active, now);
		if (magnet_dfu_enter_pending) {
			leds_set(ctx, 1);
			break;
		}
		if ((now - magnet_phase_start_ms) >= MAGNET_PAIR_HOLD_MS) {
			magnet_phase = MAGNET_HOLD_HOLD_MET;
			magnet_phase_start_ms = now;
			leds_set(ctx, 0);
			LOG_INF("magnet_hold phase=HoldMet");
		}
		break;
	case MAGNET_HOLD_HOLD_MET:
		magnet_try_dfu_enter(ctx, active, now);
		if (magnet_dfu_enter_pending) {
			leds_set(ctx, 1);
			break;
		}
		if ((now - magnet_phase_start_ms) >= MAGNET_HOLD_GAP_MS) {
			magnet_phase = MAGNET_HOLD_COOLDOWN;
			LOG_INF("magnet_hold phase=Cooldown");
		}
		break;
	case MAGNET_HOLD_COOLDOWN:
		magnet_try_dfu_enter(ctx, active, now);
		if (magnet_dfu_enter_pending) {
			leds_set(ctx, 1);
			break;
		}
		if (!active) {
			if (!magnet_hold_reached_ms(now, MAGNET_DFU_ENTER_MS)) {
				magnet_pair_toggle_pending = true;
				LOG_INF("magnet_hold pair toggle");
			}
			magnet_hold_to_idle();
		}
		break;
	}
}

static void leds_timed(const struct prod_context *ctx, int on, uint32_t ms)
{
	uint32_t end_ms = k_uptime_get_32() + ms;

	if (on) {
		leds_set(ctx, 1);
	} else {
		leds_set(ctx, 0);
	}

	while ((int32_t)(end_ms - k_uptime_get_32()) > 0) {
		uint32_t slice = MIN(PROD_WAIT_SLICE_MS, end_ms - k_uptime_get_32());

		magnet_hold_poll(ctx);
		k_sleep(K_MSEC(slice));
	}
}

static void leds_boot_blink(const struct prod_context *ctx)
{
	for (int i = 0; i < LED_BOOT_BLINK_COUNT; i++) {
		leds_timed(ctx, 1, LED_BOOT_BLINK_ON_MS);
		leds_timed(ctx, 0, LED_BOOT_BLINK_OFF_MS);
	}
}

static bool orientation_face_up(const struct prod_context *ctx)
{
	int ret = lis2dh12_zephyr_is_face_up(ctx->lis2dh, FACE_UP_Z_MIN_MG);

	return ret > 0;
}

static bool face_up_debounced(const struct prod_context *ctx)
{
	if (orientation_face_up(ctx)) {
		if (face_up_streak < UINT8_MAX) {
			face_up_streak++;
		}
	} else {
		face_up_streak = 0U;
	}

	return face_up_streak >= FACE_UP_ENTER_SAMPLES;
}

static uint32_t run_sleep_ms(void)
{
	return SLEEP_MS + (sys_rand32_get() % (SLEEP_JITTER_MS + 1U));
}

static bool shelf_poll_wake(const struct prod_context *ctx, const char **reason_out)
{
	uint8_t pos6d = 0;
	bool left_face_up = !orientation_face_up(ctx);
	bool sector_changed = false;

	if (lis2dh12_zephyr_read_6d_position(ctx->lis2dh, &pos6d) == 0) {
		sector_changed = (pos6d != shelf_int2_baseline);
	}

	if (left_face_up || sector_changed) {
		if (shelf_leave_streak < UINT8_MAX) {
			shelf_leave_streak++;
		}
	} else {
		shelf_leave_streak = 0U;
	}

	if (shelf_leave_streak < SHELF_LEAVE_SAMPLES) {
		return false;
	}

	if (reason_out != NULL) {
		if (sector_changed) {
			*reason_out = "6D sector changed";
		} else {
			*reason_out = "left face-up Z";
		}
	}

	return true;
}

static void log_orientation_z(const struct prod_context *ctx, const char *tag)
{
	int16_t x_mg = 0;
	int16_t y_mg = 0;
	int16_t z_mg = 0;
	uint8_t pos6d = 0;

	if (lis2dh12_zephyr_read_accel_mg(ctx->lis2dh, &x_mg, &y_mg, &z_mg) == 0) {
		int face_up = 0;

		(void)lis2dh12_zephyr_read_6d_position(ctx->lis2dh, &pos6d);
		face_up = (z_mg > FACE_UP_Z_MIN_MG) ? 1 : 0;
		LOG_INF("%s z=%d 6d=0x%02x face_up=%d baseline=0x%02x", tag, z_mg, pos6d, face_up,
			shelf_int2_baseline);
	}
}

static bool shelf_orientation_wake(const struct prod_context *ctx, uint8_t int2_src)
{
	int wake = lis2dh12_zephyr_int2_left_shelf_position(ctx->lis2dh, shelf_int2_baseline,
							    int2_src);

	return wake > 0;
}

static void handle_orientation_irq(const struct prod_context *ctx, uint8_t int2_src)
{
	int pin = gpio_pin_get_dt(ctx->accel_int2);

	LOG_INF("INT2_SRC=0x%02x int2_pin=%d orientation_irq", int2_src, pin);
	log_orientation_z(ctx, "INT2");
}

static uint32_t interactions_snapshot(const struct prod_context *ctx)
{
	unsigned int key = irq_lock();
	uint32_t count = *ctx->interactions;

	irq_unlock(key);
	return count;
}

static void interactions_reset(const struct prod_context *ctx)
{
	unsigned int key = irq_lock();

	*ctx->interactions = 0U;
	irq_unlock(key);
}

static int accel_int1_enable(const struct prod_context *ctx, bool enable)
{
	int ret;

	if (enable) {
		ret = gpio_pin_interrupt_configure_dt(ctx->accel_int, GPIO_INT_EDGE_TO_ACTIVE);
	} else {
		ret = gpio_pin_interrupt_configure_dt(ctx->accel_int, GPIO_INT_DISABLE);
	}

	return ret;
}

static int restore_run_sensors(const struct prod_context *ctx)
{
	int ret;

	ret = lis2dh12_zephyr_init(ctx->lis2dh);
	if (ret < 0) {
		return ret;
	}

	ret = lis2dh12_zephyr_config_motion_and_orientation(ctx->lis2dh, MOTION_THRESHOLD_MG,
							    MOTION_DURATION_SAMPLES);
	if (ret < 0) {
		return ret;
	}

	ret = accel_int1_enable(ctx, true);
	if (ret < 0) {
		return ret;
	}

	*ctx->orientation_irq_pending = false;
	return 0;
}

static int ensure_bt_enabled(struct prod_context *ctx)
{
	int ret = 0;

	if (!ctx->bt_was_disabled) {
		return 0;
	}

	ret = bt_enable(NULL);
	if (ret != 0) {
		LOG_ERR("bt_enable failed (%d)", ret);
		return ret;
	}

	ctx->bt_was_disabled = false;

	if (!bt_is_ready()) {
		LOG_ERR("bt_enable ok but stack not ready");
		return -EAGAIN;
	}

	return 0;
}

static void enter_shelf(struct prod_context *ctx)
{
	(void)telemetry_adv_stop();
	(void)bt_disable();
	telemetry_adv_bt_disabled();
	ctx->bt_was_disabled = true;

	if (ctx->tof_dev != NULL) {
		(void)vl53l0x_zephyr_power_off();
	}

	(void)accel_int1_enable(ctx, false);
	(void)lis2dh12_zephyr_enter_shelf(ctx->lis2dh);
	(void)lis2dh12_zephyr_shelf_capture_baseline(ctx->lis2dh, &shelf_int2_baseline);
	*ctx->orientation_irq_pending = false;
	face_up_streak = FACE_UP_ENTER_SAMPLES;
	shelf_leave_streak = 0U;
	leds_set(ctx, 0);

	log_orientation_z(ctx, "enter_shelf");
	LOG_INF("state RUN->SHELF");
}

static int exit_shelf_to_run(struct prod_context *ctx)
{
	int ret = restore_run_sensors(ctx);

	if (ret < 0) {
		LOG_ERR("shelf exit sensor restore failed (%d)", ret);
		return ret;
	}

	ret = ensure_bt_enabled(ctx);
	if (ret < 0) {
		LOG_ERR("bt_enable after shelf failed (%d)", ret);
		return ret;
	}

	leds_boot_blink(ctx);
	log_orientation_z(ctx, "exit_shelf");
	face_up_streak = 0U;
	shelf_leave_streak = 0U;
	LOG_INF("state SHELF->RUN");
	return 0;
}

static enum prod_wait_result prod_wait_ms(struct prod_context *ctx, uint32_t ms,
					  enum prod_wait_mode wait_mode)
{
	uint32_t end_ms = k_uptime_get_32() + ms;

	while ((int32_t)(end_ms - k_uptime_get_32()) > 0) {
		uint32_t remaining = end_ms - k_uptime_get_32();
		uint32_t slice = MIN(PROD_WAIT_SLICE_MS, remaining);

		magnet_hold_poll(ctx);
		if (magnet_dfu_enter_pending) {
			return PROD_WAIT_DFU_ENTER;
		}
		if (magnet_dfu_exit_pending) {
			return PROD_WAIT_DFU_EXIT;
		}
		if (magnet_pair_toggle_pending) {
			return PROD_WAIT_PAIR_TOGGLE;
		}

		if (wait_mode == PROD_WAIT_MODE_SHELF) {
			if (*ctx->orientation_irq_pending) {
				uint8_t int2_src = 0;
				int ret = lis2dh12_zephyr_read_int2_src(ctx->lis2dh, &int2_src);

				*ctx->orientation_irq_pending = false;
				if (ret < 0) {
					LOG_WRN("INT2_SRC read failed (%d)", ret);
				} else {
					handle_orientation_irq(ctx, int2_src);
					if (shelf_orientation_wake(ctx, int2_src)) {
						LOG_INF("shelf wake: 6D INT2");
						return PROD_WAIT_SHELF_EXIT;
					}
				}
			}
			{
				const char *reason = NULL;

				if (shelf_poll_wake(ctx, &reason)) {
					LOG_INF("shelf wake: %s", reason);
					return PROD_WAIT_SHELF_EXIT;
				}
			}
		}

		if (wait_mode == PROD_WAIT_MODE_RUN) {
			if (face_up_debounced(ctx)) {
				return PROD_WAIT_FACE_UP;
			}
		}

		if (wait_mode == PROD_WAIT_MODE_PAIR && magnet_phase == MAGNET_HOLD_IDLE) {
			pair_led_blink_poll(ctx);
		}

		k_sleep(K_MSEC(slice));
	}

	return PROD_WAIT_OK;
}

static int sample_tof_average(const struct prod_context *ctx, int16_t *distance_mm_out,
				uint8_t *flags_out)
{
	uint32_t sum = 0U;
	int samples_ok = 0;
	int ret;
	uint8_t flags = TELEM_FLAG_INTERACT_VALID;

	if (!distance_mm_out || !flags_out) {
		return -EINVAL;
	}

	if (ctx->tof_dev == NULL) {
		*distance_mm_out = INT16_MIN;
		*flags_out = flags | TELEM_FLAG_TOF_ERR;
		return -ENODEV;
	}

	ret = vl53l0x_zephyr_power_on();
	if (ret < 0) {
		*distance_mm_out = INT16_MIN;
		*flags_out = flags | TELEM_FLAG_TOF_ERR;
		return ret;
	}

	ret = vl53l0x_zephyr_ensure_ready(ctx->tof_dev);
	if (ret < 0) {
		(void)vl53l0x_zephyr_power_off();
		*distance_mm_out = INT16_MIN;
		*flags_out = flags | TELEM_FLAG_TOF_ERR;
		return ret;
	}

	for (uint8_t i = 0; i < TOF_SAMPLES; i++) {
		uint16_t mm = 0;

		ret = vl53l0x_zephyr_read_mm(ctx->tof_dev, &mm);
		if (ret == 0) {
			sum += mm;
			samples_ok++;
		}
	}

	(void)vl53l0x_zephyr_power_off();

	if (samples_ok == 0) {
		*distance_mm_out = INT16_MIN;
		*flags_out = flags | TELEM_FLAG_TOF_ERR;
		return -EIO;
	}

	int32_t avg = (int32_t)(sum / (uint32_t)samples_ok);

	if (avg > INT16_MAX) {
		avg = INT16_MAX;
	}
	if (avg < INT16_MIN) {
		avg = INT16_MIN;
	}

	*distance_mm_out = (int16_t)avg;
	*flags_out = flags | TELEM_FLAG_DIST_VALID;
	return 0;
}

static int run_advertise_window(struct prod_context *ctx, int16_t distance_mm, uint8_t flags,
				enum prod_wait_mode wait_mode)
{
	int ret;
	int64_t end_ms = k_uptime_get() + ADV_WINDOW_MS;

	ret = telemetry_adv_publish(distance_mm,
				    (uint16_t)MIN(interactions_snapshot(ctx), UINT16_MAX), flags);
	if (ret < 0) {
		return ret;
	}

	ret = telemetry_adv_start();
	if (ret < 0) {
		return ret;
	}

	while (k_uptime_get() < end_ms) {
		uint32_t raw = interactions_snapshot(ctx);
		uint16_t count = (raw > UINT16_MAX) ? UINT16_MAX : (uint16_t)raw;
		uint32_t remaining = (uint32_t)(end_ms - k_uptime_get());
		uint32_t step = MIN(ADV_UPDATE_MS, remaining);

		ret = telemetry_adv_publish(distance_mm, count, flags);
		if (ret < 0) {
			(void)telemetry_adv_stop();
			return ret;
		}

		enum prod_wait_result wait = prod_wait_ms(ctx, step, wait_mode);

		if (wait == PROD_WAIT_DFU_ENTER) {
			(void)telemetry_adv_stop();
			return 2;
		}
		if (wait == PROD_WAIT_PAIR_TOGGLE) {
			(void)telemetry_adv_stop();
			return 1;
		}
	}

	return telemetry_adv_stop();
}

static enum prod_mode enter_dfu_mode(struct prod_context *ctx)
{
	int ret;

	magnet_dfu_enter_pending = false;
	magnet_pair_toggle_pending = false;
	pair_led_blink_stop(ctx);
	ctx->pair_active = false;
	(void)telemetry_adv_stop();

	if (ctx->tof_dev != NULL) {
		(void)vl53l0x_zephyr_power_off();
	}

	(void)accel_int1_enable(ctx, false);

	ret = ensure_bt_enabled(ctx);
	if (ret < 0) {
		LOG_ERR("DFU enter bt_enable failed (%d)", ret);
		return PROD_MODE_RUN;
	}

	ret = dfu_mode_enter(ctx);
	if (ret < 0) {
		LOG_ERR("DFU enter failed (%d)", ret);
		return PROD_MODE_RUN;
	}

	ctx->dfu_active = true;
	dfu_magnet_must_release = magnet_is_active(ctx);
	magnet_hold_to_idle();
	leds_set(ctx, 1);
	LOG_INF("state ->DFU");
	return PROD_MODE_DFU;
}

static enum prod_mode exit_dfu_mode(struct prod_context *ctx)
{
	int ret;

	magnet_dfu_exit_pending = false;

	ret = dfu_mode_exit(ctx);
	if (ret < 0) {
		LOG_ERR("DFU exit failed (%d)", ret);
	}

	ctx->dfu_active = false;
	leds_set(ctx, 0);

	ret = restore_run_sensors(ctx);
	if (ret < 0) {
		LOG_ERR("DFU exit sensor restore failed (%d)", ret);
	}

	if (orientation_face_up(ctx)) {
		LOG_INF("state DFU->SHELF");
		enter_shelf(ctx);
		return PROD_MODE_SHELF;
	}

	leds_boot_blink(ctx);
	LOG_INF("state DFU->RUN");
	return PROD_MODE_RUN;
}

static enum prod_mode apply_pair_toggle(struct prod_context *ctx)
{
	magnet_pair_toggle_pending = false;

	if (ctx->pair_active) {
		ctx->pair_active = false;
		if (orientation_face_up(ctx)) {
			LOG_INF("state PAIR->SHELF");
			enter_shelf(ctx);
			return PROD_MODE_SHELF;
		}

		LOG_INF("state PAIR->RUN");
		return PROD_MODE_RUN;
	}

	ctx->pair_active = true;
	pair_led_blink_reset();
	LOG_INF("state ->PAIR");
	return PROD_MODE_PAIR;
}

static enum prod_mode prepare_pair_from_shelf(struct prod_context *ctx)
{
	int ret = restore_run_sensors(ctx);

	if (ret < 0) {
		LOG_ERR("shelf->pair sensor restore failed (%d)", ret);
		enter_shelf(ctx);
		return PROD_MODE_SHELF;
	}

	ret = ensure_bt_enabled(ctx);
	if (ret < 0) {
		LOG_ERR("shelf->pair bt_enable failed (%d)", ret);
		enter_shelf(ctx);
		return PROD_MODE_SHELF;
	}

	leds_set(ctx, 0);
	return apply_pair_toggle(ctx);
}

static enum prod_mode run_loop(struct prod_context *ctx)
{
	int ret;

	while (1) {
		int16_t distance_mm = INT16_MIN;
		uint8_t flags = TELEM_FLAG_INTERACT_VALID;

		if (face_up_debounced(ctx)) {
			enter_shelf(ctx);
			return PROD_MODE_SHELF;
		}

		ret = sample_tof_average(ctx, &distance_mm, &flags);
		if (ret < 0) {
			LOG_WRN("cycle=%u ToF sample failed (%d)", ctx->cycle, ret);
		}

		leds_pulse_once(ctx, LED_ADV_PULSE_MS);

		ret = run_advertise_window(ctx, distance_mm, flags, PROD_WAIT_MODE_RUN_ADV);
		if (ret == 2) {
			return enter_dfu_mode(ctx);
		}
		if (ret == 1) {
			return apply_pair_toggle(ctx);
		}
		if (ret < 0) {
			LOG_ERR("cycle=%u advertise window failed (%d)", ctx->cycle, ret);
		}

		{
			uint32_t raw_interact = interactions_snapshot(ctx);
			uint16_t interact_end =
				(raw_interact > UINT16_MAX) ? UINT16_MAX : (uint16_t)raw_interact;
			bool magnet_present = magnet_is_active(ctx);

			LOG_INF("cycle=%u dist_mm=%d interactions=%u seq=%u flags=0x%02x magnet=%s",
				ctx->cycle, distance_mm, interact_end, telemetry_adv_seq(), flags,
				magnet_present ? "present" : "absent");
		}

		interactions_reset(ctx);
		ctx->cycle++;

		enum prod_wait_result wait = prod_wait_ms(ctx, run_sleep_ms(), PROD_WAIT_MODE_RUN);

		if (wait == PROD_WAIT_DFU_ENTER) {
			return enter_dfu_mode(ctx);
		}
		if (wait == PROD_WAIT_PAIR_TOGGLE) {
			return apply_pair_toggle(ctx);
		}
		if (wait == PROD_WAIT_FACE_UP) {
			enter_shelf(ctx);
			return PROD_MODE_SHELF;
		}
	}
}

static enum prod_mode shelf_loop(struct prod_context *ctx)
{
	while (1) {
		enum prod_wait_result wait = prod_wait_ms(ctx, 1000U, PROD_WAIT_MODE_SHELF);

		if (wait == PROD_WAIT_DFU_ENTER) {
			return enter_dfu_mode(ctx);
		}
		if (wait == PROD_WAIT_PAIR_TOGGLE) {
			return prepare_pair_from_shelf(ctx);
		}
		if (wait == PROD_WAIT_SHELF_EXIT) {
			if (exit_shelf_to_run(ctx) < 0) {
				k_sleep(K_MSEC(1000));
				continue;
			}
			return PROD_MODE_RUN;
		}
	}
}

static enum prod_mode pair_loop(struct prod_context *ctx)
{
	int16_t last_dist = INT16_MIN;
	int ret;

	while (1) {
		uint8_t flags = TELEM_FLAG_INTERACT_VALID | TELEM_FLAG_PAIR_MODE;

		ret = run_advertise_window(ctx, last_dist, flags, PROD_WAIT_MODE_PAIR);
		if (ret == 2) {
			return enter_dfu_mode(ctx);
		}
		if (ret == 1) {
			magnet_pair_toggle_pending = false;
			ctx->pair_active = false;
			pair_led_blink_stop(ctx);
			if (orientation_face_up(ctx)) {
				LOG_INF("state PAIR->SHELF");
				enter_shelf(ctx);
				return PROD_MODE_SHELF;
			}

			LOG_INF("state PAIR->RUN");
			return PROD_MODE_RUN;
		}
		if (ret < 0) {
			LOG_ERR("pair adv window failed (%d)", ret);
			k_sleep(K_MSEC(1000));
		}
	}
}

static enum prod_mode dfu_loop(struct prod_context *ctx)
{
	leds_set(ctx, 1);

	while (1) {
		enum prod_wait_result wait = prod_wait_ms(ctx, 1000U, PROD_WAIT_MODE_DFU);

		if (wait == PROD_WAIT_DFU_EXIT) {
			return exit_dfu_mode(ctx);
		}
	}
}

static bool sample_face_up_at_boot(const struct prod_context *ctx)
{
	face_up_streak = 0U;

	for (uint8_t i = 0; i < FACE_UP_ENTER_SAMPLES; i++) {
		if (orientation_face_up(ctx)) {
			face_up_streak++;
		}
		if ((i + 1U) < FACE_UP_ENTER_SAMPLES) {
			k_sleep(K_MSEC(100));
		}
	}

	return face_up_streak >= FACE_UP_ENTER_SAMPLES;
}

void prod_run(struct prod_context *ctx)
{
	enum prod_mode mode;

	if (!ctx || !ctx->lis2dh || !ctx->led0 || !ctx->led1 || !ctx->magnet ||
	    !ctx->accel_int || !ctx->accel_int2 || !ctx->interactions ||
	    !ctx->orientation_irq_pending) {
		LOG_ERR("prod_run: invalid context");
		return;
	}

	leds_boot_blink(ctx);
	log_orientation_z(ctx, "boot");

	if (sample_face_up_at_boot(ctx)) {
		mode = PROD_MODE_SHELF;
		enter_shelf(ctx);
	} else {
		mode = PROD_MODE_RUN;
		face_up_streak = 0U;
	}

	if (magnet_pair_toggle_pending) {
		if (mode == PROD_MODE_SHELF) {
			mode = prepare_pair_from_shelf(ctx);
		} else {
			mode = apply_pair_toggle(ctx);
		}
	}

	LOG_INF("boot mode=%s pair=%d dfu=%d", mode == PROD_MODE_RUN    ? "RUN"
						  : mode == PROD_MODE_SHELF  ? "SHELF"
						  : mode == PROD_MODE_PAIR   ? "PAIR"
						  : mode == PROD_MODE_DFU    ? "DFU"
									     : "?",
		ctx->pair_active, ctx->dfu_active);

	while (1) {
		switch (mode) {
		case PROD_MODE_RUN:
			mode = run_loop(ctx);
			break;
		case PROD_MODE_SHELF:
			mode = shelf_loop(ctx);
			break;
		case PROD_MODE_PAIR:
			mode = pair_loop(ctx);
			break;
		case PROD_MODE_DFU:
			mode = dfu_loop(ctx);
			break;
		default:
			mode = PROD_MODE_RUN;
			break;
		}
	}
}
