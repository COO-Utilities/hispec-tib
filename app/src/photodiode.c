/**
 * @file photodiode.c
 * @brief ADS1115 photodiode sampler, rolling status windows, and dark calibration.
 */


#include <zephyr/kernel.h>             // k_sleep, thread declarations, etc.
#include <zephyr/device.h>             // DEVICE_DT_GET, device_is_ready
#include <zephyr/devicetree.h>         // DT_NODELABEL, DT_CHILD
#include <zephyr/drivers/adc.h>        // ADC API
#include <zephyr/logging/log.h>        // LOG_ERR, LOG_WRN, etc.
#include <zephyr/sys/util.h>
#include <errno.h>
#include <stdint.h>                    // int16_t, int64_t, etc.
#include <string.h>
#include <math.h>

#include "photodiode.h"
#include "app_settings.h"
#include "command.h"
#include "devices.h"


LOG_MODULE_REGISTER(photodiode, LOG_LEVEL_INF);

// More ADC info:
// https://github.com/zephyrproject-rtos/zephyr/blob/main/samples/drivers/adc/adc_dt/src/main.c
// https://docs.zephyrproject.org/apidoc/latest/group__adc__interface.html
// https://github.com/zephyrproject-rtos/zephyr/blob/main/drivers/adc/adc_ads1x1x.c#L617

static const struct adc_channel_cfg yj_cfg_dt =
    ADC_CHANNEL_CFG_DT(DT_CHILD(DT_NODELABEL(adc1115), channel_0));

static const struct adc_channel_cfg hk_cfg_dt =
    ADC_CHANNEL_CFG_DT(DT_CHILD(DT_NODELABEL(adc1115), channel_2));

static const struct adc_channel_cfg *const pd_adc_cfg[PHOTODIODE_CHANNEL_COUNT] = {
    &yj_cfg_dt,
    &hk_cfg_dt,
};

#define PD_YJ_ADC_CHANNEL_NODE DT_CHILD(DT_NODELABEL(adc1115), channel_0)
#define PD_HK_ADC_CHANNEL_NODE DT_CHILD(DT_NODELABEL(adc1115), channel_2)
#define ADS1115_DT_RESOLUTION DT_PROP(PD_YJ_ADC_CHANNEL_NODE, zephyr_resolution)
#define ADS1115_DT_ACQ_TIME DT_PROP(PD_YJ_ADC_CHANNEL_NODE, zephyr_acquisition_time)
#define ADS1115_I2C_HZ DT_PROP(DT_PARENT(DT_NODELABEL(adc1115)), clock_frequency)

BUILD_ASSERT(DT_PROP(PD_HK_ADC_CHANNEL_NODE, zephyr_resolution) == ADS1115_DT_RESOLUTION,
             "photodiode ADS1115 channels must use the same resolution");
BUILD_ASSERT(DT_PROP(PD_HK_ADC_CHANNEL_NODE, zephyr_acquisition_time) == ADS1115_DT_ACQ_TIME,
             "photodiode ADS1115 channels must use the same data rate");

/* Zephyr's ADS1115 driver exposes the muxed device as ADC channel 0 only.
 * The physical ADS input is selected by input_positive from devicetree.
 * For single-ended ADS1115 reads, Zephyr expects the usable positive code
 * range: one bit less than the signed conversion register width.
 */
#define ADS1115_ZEPHYR_CHANNEL_ID 0U
#define ADS1115_SINGLE_ENDED_SEQUENCE_RESOLUTION (ADS1115_DT_RESOLUTION - 1U)

const char *const photodiode_channel_names[PHOTODIODE_CHANNEL_COUNT] = {
    "yj",
    "hk",
};

static void photodiode_sample_timer_handler(struct k_timer *timer);

static K_SEM_DEFINE(pd_sample_sem, 0, 1);
static K_TIMER_DEFINE(pd_sample_timer, photodiode_sample_timer_handler, NULL);

/* Hardware docs specify ADS1115 +/-6.144 V full scale at ADC_GAIN_1_3, which
 * gives 187.5 uV per signed 16-bit count.
 */
#define PD_ADC_UV_PER_COUNT_NUM 1875
#define PD_ADC_UV_PER_COUNT_DEN 10
#define PD_HARDWARE_LOG_RATELIMIT_MS 10000U
#define PD_TIMING_STATS_INTERVAL_MS 10000U
#define PD_ADS1115_WAKE_US 25U
/* Rough ADS1115 sample-time floor for timing diagnostics, ignoring scheduler
 * overhead inside Zephyr's ADC driver path.
 */
#define PD_ADC_I2C_WIRE_BITS_PER_SAMPLE 126U
#define PD_NOISE_WARNING_COOLDOWN_MS 60000U
#define PD_WINDOW_DEFAULT_DURATION_MS PHOTODIODE_FIXED_WINDOW_MS
#define PD_WINDOW_MAX_DURATION_MS APP_PD_DARK_DURATION_MAX_MS
#define PD_WINDOW_MAX_SAMPLES (PD_WINDOW_MAX_DURATION_MS / PUBLISH_INTERVAL_MS)
#define PD_STEP_MIN_UV 5000U
#define PD_STEP_MIN_MV ((double)PD_STEP_MIN_UV / 1000.0)
#define PD_STEP_RMS_MULT 8.0
#define PLANCK_J_S 6.62607015e-34
#define LIGHT_M_PER_S 299792458.0

BUILD_ASSERT(PD_WINDOW_MAX_SAMPLES > 0U &&
	     PD_WINDOW_MAX_SAMPLES <= UINT16_MAX,
	     "photodiode windows must fit in uint16_t sample counters");
BUILD_ASSERT(PD_STEP_MIN_UV > (PD_ADC_UV_PER_COUNT_NUM / PD_ADC_UV_PER_COUNT_DEN),
	     "photodiode step threshold must exceed one ADC LSB");

struct photodiode_wavelength_coefficient {
    double wavelength_nm;
    double coefficient;
};

static const struct photodiode_wavelength_coefficient wavelength_coefficients[] = {
    { 1028.01, 1.0 },
    { 1270.0, 1.0 },
    { 1430.0, 1.0 },
    { 1510.0, 1.0 },
    { 2329.81, 1.0 },
};

static void photodiode_sample_timer_handler(struct k_timer *timer)
{
    ARG_UNUSED(timer);

    /* Timer expiry is interrupt context; ADS1115 I/O stays in the photodiode
     * thread so ADC bus transactions never run in the ISR.
     */
    k_sem_give(&pd_sample_sem);
}

struct pd_window_runtime {
	uint16_t target_samples;
	uint16_t index;
	uint16_t filled;
	double mv[PD_WINDOW_MAX_SAMPLES];
	double net_mv[PD_WINDOW_MAX_SAMPLES];
	int16_t raw[PD_WINDOW_MAX_SAMPLES];
	bool good[PD_WINDOW_MAX_SAMPLES];
	struct photodiode_window_result current;
	struct photodiode_window_result last;
};

struct photodiode_runtime_channel {
	int16_t raw;
	double mv;
	double net_mv;
	double net_err_mv;
	double power_uw;
	double power_err_uw;
	int64_t updated_ms;
	int64_t next_noise_warning_ms;
	struct pd_window_runtime configurable_window;
	struct pd_window_runtime fixed_window;
};

static struct photodiode_runtime_channel pd_runtime[PHOTODIODE_CHANNEL_COUNT];
static K_MUTEX_DEFINE(pd_runtime_lock);

struct photodiode_loop_timing {
    uint64_t worst_loop_us;
    uint64_t worst_non_adc_us;
    uint64_t worst_adc_us[PHOTODIODE_CHANNEL_COUNT];
    uint64_t worst_adc_over_us[PHOTODIODE_CHANNEL_COUNT];
    int64_t min_margin_us;
};

struct photodiode_timing_stats {
    uint32_t samples;
    uint32_t missed_intervals;
    uint32_t overruns;
    uint32_t settings_busy;
    uint32_t adc_errors[PHOTODIODE_CHANNEL_COUNT];
    struct photodiode_loop_timing worst;
};

static struct photodiode_timing_stats pd_timing_stats;
static int64_t pd_timing_next_log_ms;

extern bool app_timing_summary_logs_enabled(void);

static uint64_t pd_i2c_wire_us_for_bits(uint32_t bits)
{
    return ((uint64_t)bits * 1000000ULL + ADS1115_I2C_HZ - 1ULL) /
           ADS1115_I2C_HZ;
}

static uint32_t pd_ads1115_conversion_us(void)
{
    switch (ADC_ACQ_TIME_VALUE(ADS1115_DT_ACQ_TIME)) {
    case 0:
        return 125000U + PD_ADS1115_WAKE_US;
    case 1:
        return 62500U + PD_ADS1115_WAKE_US;
    case 2:
        return 31250U + PD_ADS1115_WAKE_US;
    case 3:
        return 15625U + PD_ADS1115_WAKE_US;
    case 4:
        return 7813U + PD_ADS1115_WAKE_US;
    case 5:
        return 4000U + PD_ADS1115_WAKE_US;
    case 6:
        return 2105U + PD_ADS1115_WAKE_US;
    case 7:
        return 1163U + PD_ADS1115_WAKE_US;
    default:
        return 0U;
    }
}

static uint64_t pd_ads1115_i2c_wire_us_per_sample(void)
{
    return pd_i2c_wire_us_for_bits(PD_ADC_I2C_WIRE_BITS_PER_SAMPLE);
}

static uint64_t pd_ads1115_adc_floor_us(void)
{
    return pd_ads1115_conversion_us() + pd_ads1115_i2c_wire_us_per_sample();
}

static uint64_t pd_adc_over_us(uint64_t adc_elapsed_us)
{
    const uint64_t floor_us = pd_ads1115_adc_floor_us();

    return adc_elapsed_us > floor_us ? adc_elapsed_us - floor_us : 0U;
}

static void pd_timing_note_missed_intervals(uint32_t elapsed_samples)
{
    if (elapsed_samples > 1U) {
        pd_timing_stats.missed_intervals += elapsed_samples - 1U;
    }
}

static void pd_timing_note_adc(enum photodiode_channel channel, int rc)
{
    if (channel < 0 || channel >= PHOTODIODE_CHANNEL_COUNT) {
        return;
    }

    if (rc != 0) {
        pd_timing_stats.adc_errors[channel]++;
    }
}

static void pd_timing_note_settings(bool refreshed)
{
    if (!refreshed) {
        pd_timing_stats.settings_busy++;
    }
}

static void pd_timing_note_loop(struct photodiode_loop_timing *loop,
                                uint64_t elapsed_us,
                                uint64_t adc_total_us)
{
    if (loop == NULL) {
        return;
    }

    loop->worst_loop_us = elapsed_us;
    loop->min_margin_us = ((int64_t)PUBLISH_INTERVAL_MS * 1000LL) -
                          (int64_t)elapsed_us;
    /* Earlier phase probes showed settings refresh, rolling-stat updates, and
     * shared-state updates are normally sub-100 us. Keep the durable metric as
     * "not inside ADC calls"; use Zephyr runtime/tracing tools to identify
     * what preempted or blocked the thread during any large value.
     */
    loop->worst_non_adc_us =
        elapsed_us > adc_total_us ? elapsed_us - adc_total_us : 0U;

    if (loop->min_margin_us < 0) {
        pd_timing_stats.overruns++;
    }
    if (pd_timing_stats.samples == 0U ||
        loop->worst_loop_us > pd_timing_stats.worst.worst_loop_us) {
        pd_timing_stats.worst = *loop;
    }
    pd_timing_stats.samples++;
}

static bool pd_timing_has_anomaly(void)
{
    return pd_timing_stats.missed_intervals > 0U ||
           pd_timing_stats.overruns > 0U ||
           pd_timing_stats.adc_errors[0] > 0U ||
           pd_timing_stats.adc_errors[1] > 0U;
}

static void pd_timing_log_snapshot(bool anomaly)
{
    if (anomaly) {
        LOG_WRN("ADC timing: samples=%u missed=%u overruns=%u settings_busy=%u "
                "worst_loop_us=%llu min_margin_us=%lld worst_non_adc_us=%llu "
                "adc_floor_us=%llu adc_us=%llu/%llu adc_over_us=%llu/%llu "
                "errors=%u/%u",
                (unsigned int)pd_timing_stats.samples,
                (unsigned int)pd_timing_stats.missed_intervals,
                (unsigned int)pd_timing_stats.overruns,
                (unsigned int)pd_timing_stats.settings_busy,
                (unsigned long long)pd_timing_stats.worst.worst_loop_us,
                (long long)pd_timing_stats.worst.min_margin_us,
                (unsigned long long)pd_timing_stats.worst.worst_non_adc_us,
                (unsigned long long)pd_ads1115_adc_floor_us(),
                (unsigned long long)pd_timing_stats.worst.worst_adc_us[0],
                (unsigned long long)pd_timing_stats.worst.worst_adc_us[1],
                (unsigned long long)pd_timing_stats.worst.worst_adc_over_us[0],
                (unsigned long long)pd_timing_stats.worst.worst_adc_over_us[1],
                (unsigned int)pd_timing_stats.adc_errors[0],
                (unsigned int)pd_timing_stats.adc_errors[1]);
        return;
    }

    LOG_INF("ADC timing: samples=%u missed=%u overruns=%u settings_busy=%u "
            "worst_loop_us=%llu min_margin_us=%lld worst_non_adc_us=%llu "
            "adc_floor_us=%llu adc_us=%llu/%llu adc_over_us=%llu/%llu "
            "errors=%u/%u",
            (unsigned int)pd_timing_stats.samples,
            (unsigned int)pd_timing_stats.missed_intervals,
            (unsigned int)pd_timing_stats.overruns,
            (unsigned int)pd_timing_stats.settings_busy,
            (unsigned long long)pd_timing_stats.worst.worst_loop_us,
            (long long)pd_timing_stats.worst.min_margin_us,
            (unsigned long long)pd_timing_stats.worst.worst_non_adc_us,
            (unsigned long long)pd_ads1115_adc_floor_us(),
            (unsigned long long)pd_timing_stats.worst.worst_adc_us[0],
            (unsigned long long)pd_timing_stats.worst.worst_adc_us[1],
            (unsigned long long)pd_timing_stats.worst.worst_adc_over_us[0],
            (unsigned long long)pd_timing_stats.worst.worst_adc_over_us[1],
            (unsigned int)pd_timing_stats.adc_errors[0],
            (unsigned int)pd_timing_stats.adc_errors[1]);
}

static void pd_timing_maybe_log(int64_t now_ms)
{
    bool anomaly;

    if (pd_timing_next_log_ms == 0) {
        pd_timing_next_log_ms = now_ms + PD_TIMING_STATS_INTERVAL_MS;
        return;
    }
    if (now_ms < pd_timing_next_log_ms) {
        return;
    }

    anomaly = pd_timing_has_anomaly();
    if (anomaly || app_timing_summary_logs_enabled()) {
        pd_timing_log_snapshot(anomaly);
    }

    memset(&pd_timing_stats, 0, sizeof(pd_timing_stats));
    pd_timing_next_log_ms = now_ms + PD_TIMING_STATS_INTERVAL_MS;
}

/* Read one ADS1115 physical mux input through Zephyr's ADC driver. This can
 * sleep/block in the driver for I2C and conversion time; it does not publish,
 * enqueue, or update shared photodiode status.
 */
static int pd_read_raw(enum photodiode_channel channel, int16_t *raw)
{
    struct adc_channel_cfg cfg;
    struct adc_sequence seq = {
        .channels = BIT(ADS1115_ZEPHYR_CHANNEL_ID),
        .buffer = raw,
        .buffer_size = sizeof(*raw),
        .resolution = ADS1115_SINGLE_ENDED_SEQUENCE_RESOLUTION,
        .oversampling = 0,
        .calibrate = false,
    };
    int rc;

    if (raw == NULL || channel < 0 || channel >= PHOTODIODE_CHANNEL_COUNT) {
        return -EINVAL;
    }
    if (adc_dev == NULL || !device_is_ready(adc_dev)) {
        return -ENODEV;
    }

    cfg = *pd_adc_cfg[channel];
    cfg.channel_id = ADS1115_ZEPHYR_CHANNEL_ID;
    rc = adc_channel_setup(adc_dev, &cfg);
    if (rc == 0) {
        rc = adc_read(adc_dev, &seq);
    }
    return rc;
}

double photodiode_power_uw_from_mv(double net_mv,
                                   const struct app_pd_channel_settings *settings)
{
    double signal_v;
    double power_w;

    if (settings == NULL || net_mv <= 0.0 ||
        settings->responsivity_a_per_w <= 0.0 ||
        settings->transimpedance_v_per_a <= 0.0) {
        return 0.0;
    }

    signal_v = net_mv / 1000.0;
    power_w = signal_v / (settings->transimpedance_v_per_a *
                          settings->responsivity_a_per_w);
    return power_w * 1.0e6;
}

static double photodiode_nearest_wavelength_coefficient(double wavelength_nm)
{
    double best_delta;
    double best_coeff;

    if (wavelength_nm <= 0.0 || !isfinite(wavelength_nm)) {
        return 0.0;
    }

    best_delta = fabs(wavelength_nm - wavelength_coefficients[0].wavelength_nm);
    best_coeff = wavelength_coefficients[0].coefficient;
    for (size_t i = 1U; i < ARRAY_SIZE(wavelength_coefficients); ++i) {
        double delta = fabs(wavelength_nm - wavelength_coefficients[i].wavelength_nm);

        if (delta < best_delta) {
            best_delta = delta;
            best_coeff = wavelength_coefficients[i].coefficient;
        }
    }

    return best_coeff;
}

double photodiode_power_uw_from_mv_at_wavelength(
    double net_mv,
    double wavelength_nm,
    const struct app_pd_channel_settings *settings)
{
    double coefficient = photodiode_nearest_wavelength_coefficient(wavelength_nm);

    if (coefficient <= 0.0) {
        return 0.0;
    }

    return photodiode_power_uw_from_mv(net_mv, settings) * coefficient;
}

double photodiode_photon_flux_from_mv(double net_mv,
                                      double wavelength_nm,
                                      const struct app_pd_channel_settings *settings)
{
    double power_w;
    double photon_j;

    if (wavelength_nm <= 0.0) {
        return 0.0;
    }

    power_w = photodiode_power_uw_from_mv_at_wavelength(
        net_mv, wavelength_nm, settings) * 1.0e-6;
    if (power_w <= 0.0) {
        return 0.0;
    }

    photon_j = PLANCK_J_S * LIGHT_M_PER_S / (wavelength_nm * 1.0e-9);
    return power_w / photon_j;
}

static bool pd_dark_result_valid(const struct app_pd_dark_result *dark)
{
	if (dark == NULL) {
		return false;
	}

	return dark->length_ms <= APP_PD_DARK_DURATION_MAX_MS &&
	       isfinite((double)dark->mean_mv) &&
	       dark->mean_mv >= PHOTODIODE_DARK_MIN_MV &&
	       dark->mean_mv <= PHOTODIODE_DARK_MAX_MV &&
	       isfinite((double)dark->rms_mv) &&
	       dark->rms_mv >= PHOTODIODE_NOISE_RMS_MIN_MV &&
	       dark->rms_mv <= PHOTODIODE_NOISE_RMS_MAX_MV &&
	       isfinite((double)dark->min_mv) &&
	       dark->min_mv >= PHOTODIODE_DARK_MIN_MV &&
	       dark->min_mv <= PHOTODIODE_DARK_MAX_MV &&
	       isfinite((double)dark->max_mv) &&
	       dark->max_mv >= PHOTODIODE_DARK_MIN_MV &&
	       dark->max_mv <= PHOTODIODE_DARK_MAX_MV &&
	       dark->min_mv <= dark->max_mv;
}

bool photodiode_settings_valid(const struct app_pd_channel_settings *settings)
{
    if (settings == NULL) {
        return false;
    }

    return pd_dark_result_valid(&settings->dark) &&
           (!settings->lowest_dark_valid ||
	    pd_dark_result_valid(&settings->lowest_dark)) &&
           isfinite((double)settings->noise_warn_rms_mv) &&
           settings->noise_warn_rms_mv >= PHOTODIODE_NOISE_RMS_MIN_MV &&
           settings->noise_warn_rms_mv <= PHOTODIODE_NOISE_RMS_MAX_MV &&
           isfinite(settings->responsivity_a_per_w) &&
           settings->responsivity_a_per_w >= PHOTODIODE_RESPONSIVITY_MIN_A_PER_W &&
           settings->responsivity_a_per_w <= PHOTODIODE_RESPONSIVITY_MAX_A_PER_W &&
           isfinite(settings->transimpedance_v_per_a) &&
           settings->transimpedance_v_per_a >= PHOTODIODE_TRANSIMPEDANCE_MIN_V_PER_A &&
           settings->transimpedance_v_per_a <= PHOTODIODE_TRANSIMPEDANCE_MAX_V_PER_A &&
           settings->power >= APP_PD_POWER_AUTO &&
           settings->power <= APP_PD_POWER_OVERRIDE_OFF;
}

static uint16_t pd_window_duration_to_samples(uint32_t duration_ms)
{
	uint32_t requested_ms = duration_ms == 0U ? PUBLISH_INTERVAL_MS : duration_ms;
	uint32_t samples;

	if (requested_ms >= PD_WINDOW_MAX_DURATION_MS) {
		return (uint16_t)PD_WINDOW_MAX_SAMPLES;
	}

	requested_ms += PUBLISH_INTERVAL_MS / 2U;
	samples = requested_ms / PUBLISH_INTERVAL_MS;
	return (uint16_t)CLAMP(samples, 1U, PD_WINDOW_MAX_SAMPLES);
}

static void pd_window_result_clear(struct photodiode_window_result *result)
{
	if (result == NULL) {
		return;
	}

	memset(result, 0, sizeof(*result));
	result->mean_mv = NAN;
	result->mean_net_mv = NAN;
	result->rms_mv = NAN;
	result->mean_net_err_mv = NAN;
	result->min_mv = NAN;
	result->max_mv = NAN;
	result->power_uw = NAN;
	result->power_err_uw = NAN;
}

static void pd_window_reset_current(struct pd_window_runtime *window)
{
	if (window == NULL) {
		return;
	}

	window->index = 0U;
	window->filled = 0U;
	memset(window->good, 0, sizeof(window->good));
	pd_window_result_clear(&window->current);
}

static void pd_window_set_target(struct pd_window_runtime *window,
				 uint16_t target_samples)
{
	if (window == NULL) {
		return;
	}

	window->target_samples = CLAMP(target_samples, 1U,
				       (uint16_t)PD_WINDOW_MAX_SAMPLES);
	pd_window_reset_current(window);
	pd_window_result_clear(&window->last);
}

static void pd_windows_ensure_locked(struct photodiode_runtime_channel *channel)
{
	if (channel == NULL) {
		return;
	}
	if (channel->fixed_window.target_samples == 0U) {
		pd_window_set_target(&channel->fixed_window,
				     pd_window_duration_to_samples(PHOTODIODE_FIXED_WINDOW_MS));
	}
	if (channel->configurable_window.target_samples == 0U) {
		pd_window_set_target(&channel->configurable_window,
				     pd_window_duration_to_samples(PD_WINDOW_DEFAULT_DURATION_MS));
	}
}

static void pd_window_close_current(struct pd_window_runtime *window)
{
	if (window == NULL || window->target_samples == 0U) {
		return;
	}

	window->last = window->current;
	pd_window_reset_current(window);
}

static void pd_window_recompute(struct pd_window_runtime *window,
				const struct app_pd_channel_settings *settings,
				int64_t now_ms)
{
	struct photodiode_window_result next;
	double sum_mv = 0.0;
	double sum_net_mv = 0.0;
	double mean;
	double m2 = 0.0;
	uint16_t good_count = 0U;
	uint16_t failed = 0U;

	if (window == NULL) {
		return;
	}

	pd_window_result_clear(&next);
	next.sample_length = window->filled;
	next.end_ms = now_ms;
	next.max_raw = INT16_MIN;

	for (uint16_t i = 0U; i < window->filled; ++i) {
		if (!window->good[i]) {
			failed++;
			continue;
		}
		if (good_count == 0U) {
			next.min_mv = window->mv[i];
			next.max_mv = window->mv[i];
			next.max_raw = window->raw[i];
		} else {
			next.min_mv = MIN(next.min_mv, window->mv[i]);
			next.max_mv = MAX(next.max_mv, window->mv[i]);
			next.max_raw = MAX(next.max_raw, window->raw[i]);
		}
		sum_mv += window->mv[i];
		sum_net_mv += window->net_mv[i];
		good_count++;
	}

	next.failed_samples = failed;
	if (good_count == 0U) {
		next.max_raw = 0;
		window->current = next;
		return;
	}

	mean = sum_mv / (double)good_count;
	for (uint16_t i = 0U; i < window->filled; ++i) {
		double delta;

		if (!window->good[i]) {
			continue;
		}
		delta = window->mv[i] - mean;
		m2 += delta * delta;
	}

	next.valid = true;
	next.mean_mv = mean;
	next.mean_net_mv = sum_net_mv / (double)good_count;
	next.rms_mv = sqrt(m2 / (double)good_count);
	next.mean_net_err_mv = next.rms_mv / sqrt((double)good_count);
	if (settings != NULL && settings->dark.rms_mv > 0.0) {
		next.mean_net_err_mv = sqrt(next.mean_net_err_mv * next.mean_net_err_mv +
					    settings->dark.rms_mv *
						    settings->dark.rms_mv);
	}
	if (settings != NULL) {
		next.power_uw = photodiode_power_uw_from_mv(next.mean_net_mv, settings);
		next.power_err_uw = photodiode_power_uw_from_mv(next.mean_net_err_mv,
								settings);
	}
	window->current = next;
}

static void pd_window_add_sample(struct pd_window_runtime *window,
				 int rc, int16_t raw, double mv, double net_mv,
				 const struct app_pd_channel_settings *settings,
				 int64_t now_ms)
{
	uint16_t slot;

	if (window == NULL || window->target_samples == 0U) {
		return;
	}

	slot = window->index;
	window->good[slot] = rc == 0;
	window->raw[slot] = raw;
	window->mv[slot] = rc == 0 ? mv : (double)NAN;
	window->net_mv[slot] = rc == 0 ? net_mv : (double)NAN;
	window->index = (uint16_t)((slot + 1U) % window->target_samples);
	if (window->filled < window->target_samples) {
		window->filled++;
	}
	pd_window_recompute(window, settings, now_ms);
}

static bool pd_sample_is_step(const struct photodiode_runtime_channel *channel,
			      double mv)
{
	double threshold = PD_STEP_MIN_MV;

	if (channel == NULL || channel->updated_ms <= 0) {
		return false;
	}
	if (channel->fixed_window.current.valid &&
	    channel->fixed_window.current.rms_mv > 0.0) {
		threshold = MAX(threshold,
				PD_STEP_RMS_MULT * channel->fixed_window.current.rms_mv);
	}

	return fabs(mv - channel->mv) > threshold;
}

static struct photodiode_window_result
pd_dark_window_from_settings(const struct app_pd_dark_result *dark,
			     bool valid,
			     const struct app_pd_channel_settings *settings)
{
	struct photodiode_window_result result;

	pd_window_result_clear(&result);
	if (!valid || dark == NULL || settings == NULL ||
	    !pd_dark_result_valid(dark)) {
		return result;
	}

	result.valid = true;
	result.sample_length = dark->length_ms == 0U ?
			       0U :
			       pd_window_duration_to_samples(dark->length_ms);
	result.failed_samples = dark->failed_samples;
	result.end_ms = 0;
	result.mean_mv = dark->mean_mv;
	result.mean_net_mv = 0.0;
	result.rms_mv = dark->rms_mv;
	result.mean_net_err_mv = dark->rms_mv;
	result.min_mv = dark->min_mv;
	result.max_mv = dark->max_mv;
	result.power_uw = 0.0;
	result.power_err_uw = photodiode_power_uw_from_mv(dark->rms_mv, settings);
	result.max_raw = dark->max_raw;
	return result;
}

static void pd_emit_adc_error_warning(enum photodiode_channel channel, int rc)
{
    char context[64];

    if (channel < 0 || channel >= PHOTODIODE_CHANNEL_COUNT || rc == 0) {
        return;
    }

    snprintk(context, sizeof(context), "channel=%s rc=%d",
             photodiode_channel_names[channel], rc);
    coo_cmd_runtime_emit(command_runtime_get(),
                         &(const struct coo_cmd_runtime_emit_args){
                             .type = COO_CMD_RUNTIME_EMIT_WARNING,
                             .delivery = COO_CMD_RUNTIME_EMIT_BEST_EFFORT,
                             .code = "photodiode_adc_error",
                             .msg = "photodiode ADC sample discarded",
                             .context = context,
                         });
}

static void pd_update_channel(enum photodiode_channel channel, int rc, int16_t raw,
                              const struct app_pd_channel_settings *settings)
{
	struct photodiode_runtime_channel *runtime;
	double mv = NAN;
	double net_mv = NAN;
	double net_err_mv = NAN;
	double noise_rms = 0.0;
	bool emit_noise_warning = false;
	int64_t now = k_uptime_get();

	if (rc == 0) {
		mv = ((double)raw * (double)PD_ADC_UV_PER_COUNT_NUM) /
		     ((double)PD_ADC_UV_PER_COUNT_DEN * 1000.0);
		net_mv = mv - settings->dark.mean_mv;
		net_err_mv = sqrt((PHOTODIODE_INSTANT_ERR_MV *
				   PHOTODIODE_INSTANT_ERR_MV) +
				  (settings->dark.rms_mv *
				   settings->dark.rms_mv));
	}

	k_mutex_lock(&pd_runtime_lock, K_FOREVER);
	runtime = &pd_runtime[channel];
	pd_windows_ensure_locked(runtime);

	if (rc == 0 && pd_sample_is_step(runtime, mv)) {
		pd_window_close_current(&runtime->configurable_window);
		pd_window_close_current(&runtime->fixed_window);
	}

	if (rc == 0) {
		runtime->raw = raw;
		runtime->mv = mv;
		runtime->net_mv = net_mv;
		runtime->net_err_mv = net_err_mv;
		runtime->power_uw = photodiode_power_uw_from_mv(net_mv, settings);
		runtime->power_err_uw = photodiode_power_uw_from_mv(net_err_mv,
								    settings);
		runtime->updated_ms = now;
	}

	pd_window_add_sample(&runtime->configurable_window, rc, raw, mv, net_mv, settings, now);
	pd_window_add_sample(&runtime->fixed_window, rc, raw, mv, net_mv, settings, now);

	if (rc == 0 && runtime->fixed_window.current.valid) {
		noise_rms = runtime->fixed_window.current.rms_mv;
		if (settings->noise_warn_rms_mv > 0.0 &&
		    noise_rms > settings->noise_warn_rms_mv &&
		    now >= runtime->next_noise_warning_ms) {
			runtime->next_noise_warning_ms =
				now + PD_NOISE_WARNING_COOLDOWN_MS;
			emit_noise_warning = true;
		}
	}
	k_mutex_unlock(&pd_runtime_lock);

	if (rc != 0) {
		pd_emit_adc_error_warning(channel, rc);
		return;
	}

	if (emit_noise_warning) {
		char context[128];

		snprintk(context, sizeof(context),
			 "channel=%s fixed_rms_mv=%.3f threshold_mv=%.3f",
			 photodiode_channel_names[channel],
			 (double)noise_rms,
			 (double)settings->noise_warn_rms_mv);
		coo_cmd_runtime_emit(command_runtime_get(),
				     &(const struct coo_cmd_runtime_emit_args){
					     .type = COO_CMD_RUNTIME_EMIT_WARNING,
					     .delivery = COO_CMD_RUNTIME_EMIT_BEST_EFFORT,
					     .code = "photodiode_noise",
					     .msg = "photodiode fixed-window RMS exceeded warning threshold",
					     .context = context,
				     });
	}
}

void photodiode_get_status(struct photodiode_status *out)
{
    struct app_photodiode_settings settings;
    int64_t now = k_uptime_get();

    if (out == NULL) {
        return;
    }

    memset(out, 0, sizeof(*out));
    app_settings_get_photodiode(&settings);

    k_mutex_lock(&pd_runtime_lock, K_FOREVER);
    for (uint8_t i = 0; i < PHOTODIODE_CHANNEL_COUNT; ++i) {
        const struct photodiode_runtime_channel *src = &pd_runtime[i];
        struct photodiode_channel_status *dst = &out->channel[i];
        const struct app_pd_channel_settings *ch = &settings.channel[i];

        if (src->updated_ms > 0) {
            dst->raw = src->raw;
            dst->mv = src->mv;
            dst->net_mv = src->net_mv;
            dst->net_err_mv = src->net_err_mv;
            dst->power_uw = src->power_uw;
            dst->power_err_uw = src->power_err_uw;
            dst->age_ms = (uint32_t)(now - src->updated_ms);
        } else {
            dst->raw = INT16_MIN;
            dst->mv = NAN;
            dst->net_mv = NAN;
            dst->net_err_mv = NAN;
            dst->power_uw = NAN;
            dst->power_err_uw = NAN;
            dst->age_ms = UINT32_MAX;
        }
        dst->configurable_window = src->configurable_window.current;
        dst->last_configurable_window = src->configurable_window.last;
        dst->fixed_window = src->fixed_window.current;
        dst->last_fixed_window = src->fixed_window.last;
        dst->dark_window = pd_dark_window_from_settings(&ch->dark, true, ch);
        dst->lowest_dark_window =
            pd_dark_window_from_settings(&ch->lowest_dark,
                                         ch->lowest_dark_valid,
                                         ch);
    }
    k_mutex_unlock(&pd_runtime_lock);
}

int photodiode_set_configurable_window_duration(enum photodiode_channel channel,
						uint32_t duration_ms)
{
	uint16_t sample_count;
	struct pd_window_runtime *window;

	if (channel < 0 || channel >= PHOTODIODE_CHANNEL_COUNT) {
		return -EINVAL;
	}
	if (adc_dev == NULL || !device_is_ready(adc_dev)) {
		return -ENODEV;
	}

	sample_count = pd_window_duration_to_samples(duration_ms);

	k_mutex_lock(&pd_runtime_lock, K_FOREVER);
	pd_windows_ensure_locked(&pd_runtime[channel]);
	window = &pd_runtime[channel].configurable_window;
	window->last = window->current;
	window->target_samples = sample_count;
	pd_window_reset_current(window);
	k_mutex_unlock(&pd_runtime_lock);
	return 0;
}

void photodiode_thread(void *p1, void *p2, void *p3)
{
    struct app_photodiode_settings settings;

    ARG_UNUSED(p1);
    ARG_UNUSED(p2);
    ARG_UNUSED(p3);

    k_sleep(K_MSEC(10));

    while (adc_dev == NULL || !device_is_ready(adc_dev)) {
        LOG_ERR_RATELIMIT_RATE(PD_HARDWARE_LOG_RATELIMIT_MS, "ADS1115 not ready");
        k_sleep(K_MSEC(500));
    }

    /* Load the first calibration snapshot before the sample timer starts. The
     * timed loop only uses non-blocking refreshes and reuses this cached copy
     * if the command thread is updating settings at the same instant.
     */
    app_settings_get_photodiode(&settings);

    k_timer_start(&pd_sample_timer, K_NO_WAIT, K_MSEC(PUBLISH_INTERVAL_MS));

    while (1) {
        struct photodiode_loop_timing loop_timing = {0};
        uint64_t loop_start_cycles;
        uint64_t adc_total_us = 0U;
        uint32_t elapsed_samples;
        bool settings_refreshed;

        k_sem_take(&pd_sample_sem, K_FOREVER);
        loop_start_cycles = k_cycle_get_64();
        elapsed_samples = k_timer_status_get(&pd_sample_timer);
        pd_timing_note_missed_intervals(elapsed_samples);

        settings_refreshed = app_settings_try_get_photodiode(&settings);
        pd_timing_note_settings(settings_refreshed);

        for (uint8_t i = 0; i < PHOTODIODE_CHANNEL_COUNT; ++i) {
            int16_t raw = 0;
            uint64_t adc_start_cycles;
            uint64_t adc_elapsed_us;
            int rc;

            adc_start_cycles = k_cycle_get_64();
            rc = pd_read_raw((enum photodiode_channel)i, &raw);
            adc_elapsed_us = k_cyc_to_us_floor64(k_cycle_get_64() - adc_start_cycles);
            adc_total_us += adc_elapsed_us;
            loop_timing.worst_adc_us[i] = adc_elapsed_us;
            loop_timing.worst_adc_over_us[i] = pd_adc_over_us(adc_elapsed_us);
            pd_timing_note_adc((enum photodiode_channel)i, rc);

            if (rc != 0) {
                LOG_ERR_RATELIMIT_RATE(PD_HARDWARE_LOG_RATELIMIT_MS,
                                       "ADC %s read failed (%d)",
                                       photodiode_channel_names[i], rc);
            }
            pd_update_channel((enum photodiode_channel)i, rc, raw, &settings.channel[i]);
        }

        pd_timing_note_loop(&loop_timing,
                            k_cyc_to_us_floor64(k_cycle_get_64() - loop_start_cycles),
                            adc_total_us);
        pd_timing_maybe_log(k_uptime_get());
    }
}
