#include "vbatt_zephyr.h"

#include <errno.h>

#include <zephyr/device.h>
#include <zephyr/drivers/adc.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(vbatt_zephyr, LOG_LEVEL_INF);

/*
 * Board must define &adc channel@0 with NRF_SAADC_VDD and a zephyr,user node
 * with io-channels = <&adc 0>. Use DT_NODE_EXISTS (not DT_HAS_COMPAT_STATUS_OKAY)
 * — zephyr,user is a special node name, not a compatible string.
 */
#if DT_NODE_EXISTS(DT_PATH(zephyr_user)) && DT_NODE_HAS_PROP(DT_PATH(zephyr_user), io_channels)
static const struct adc_dt_spec vbatt_adc = ADC_DT_SPEC_GET(DT_PATH(zephyr_user));
static bool vbatt_ready;
#define VBATT_ADC_AVAILABLE 1
#else
#define VBATT_ADC_AVAILABLE 0
#endif

int vbatt_init(void)
{
#if VBATT_ADC_AVAILABLE
	int ret;

	if (!adc_is_ready_dt(&vbatt_adc)) {
		LOG_ERR("ADC device not ready");
		return -ENODEV;
	}

	ret = adc_channel_setup_dt(&vbatt_adc);
	if (ret < 0) {
		LOG_ERR("ADC channel setup failed (%d)", ret);
		return ret;
	}

	vbatt_ready = true;
	LOG_INF("VDD SAADC ready (channel %d)", vbatt_adc.channel_id);
	return 0;
#else
	LOG_ERR("Board has no zephyr,user io-channels for VDD ADC");
	return -ENODEV;
#endif
}

int vbatt_read_mv(int32_t *mv_out)
{
#if VBATT_ADC_AVAILABLE
	int16_t sample;
	int32_t mv;
	int ret;

	if (!mv_out) {
		return -EINVAL;
	}

	if (!vbatt_ready) {
		return -ENODEV;
	}

	struct adc_sequence seq = {
		.buffer = &sample,
		.buffer_size = sizeof(sample),
	};

	adc_sequence_init_dt(&vbatt_adc, &seq);

	ret = adc_read_dt(&vbatt_adc, &seq);
	if (ret < 0) {
		return ret;
	}

	mv = sample;
	ret = adc_raw_to_millivolts_dt(&vbatt_adc, &mv);
	if (ret < 0) {
		return ret;
	}

	*mv_out = mv;
	return 0;
#else
	ARG_UNUSED(mv_out);
	return -ENODEV;
#endif
}
