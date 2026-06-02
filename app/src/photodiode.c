/**
 * @file photodiode.c
 * @brief ADS1115 photodiode sampler, rolling status windows, and dark calibration.
 */


#include <zephyr/kernel.h>             // k_sleep, thread declarations, etc.
#include <zephyr/device.h>             // DEVICE_DT_GET, device_is_ready
#include <zephyr/devicetree.h>         // DT_NODELABEL, DT_CHILD
#include <zephyr/drivers/adc.h>        // ADC API
#include <zephyr/logging/log.h>        // LOG_ERR, LOG_WRN, etc.
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
#define PD_NOISE_ALPHA 0.02f
#define PD_HARDWARE_LOG_RATELIMIT_MS 10000U
#define PD_TIMING_STATS_INTERVAL_MS 10000U
#define PD_ADS1115_WAKE_US 25U
/* Rough ADS1115 sample-time floor for timing diagnostics, ignoring scheduler
 * overhead inside Zephyr's ADC driver path.
 */
#define PD_ADC_I2C_WIRE_BITS_PER_SAMPLE 126U
#define PD_NOISE_WARNING_COOLDOWN_MS 60000U
#define PD_DARK_DEFAULT_DURATION_MS (64U * PUBLISH_INTERVAL_MS)
#define PD_AVERAGE_MAX_DURATION_MS 2000U
#define PD_AVERAGE_MAX_SAMPLES (PD_AVERAGE_MAX_DURATION_MS / PUBLISH_INTERVAL_MS)
#define PD_MEAN_WINDOW_SAMPLES (1000U / PUBLISH_INTERVAL_MS)
#define PD_RMS_WINDOW_SAMPLES (500U / PUBLISH_INTERVAL_MS)
#define PLANCK_J_S 6.62607015e-34
#define LIGHT_M_PER_S 299792458.0

static void photodiode_sample_timer_handler(struct k_timer *timer)
{
    ARG_UNUSED(timer);

    /* Timer expiry is interrupt context; ADS1115 I/O stays in the photodiode
     * thread so ADC bus transactions never run in the ISR.
     */
    k_sem_give(&pd_sample_sem);
}

struct photodiode_runtime_channel {
    bool valid;
    int last_error;
    int16_t raw;
    float mv;
    float net_mv;
    float power_uw;
    float smooth_mv;
    float noise_var_mv2;
    float noise_rms_mv;
    float mean_window_mv[PD_MEAN_WINDOW_SAMPLES];
    float mean_sum_mv;
    uint8_t mean_index;
    uint8_t mean_count;
    float rms_window_mv[PD_RMS_WINDOW_SAMPLES];
    float rms_sum_mv;
    float rms_sum_sq_mv2;
    uint8_t rms_index;
    uint8_t rms_count;
    float mean_mv_1s;
    float rms_mv_0p5s;
    uint32_t sample_count;
    int64_t updated_ms;
    int64_t next_noise_warning_ms;
};

enum photodiode_average_owner {
    PHOTODIODE_AVERAGE_OWNER_NONE = 0,
    PHOTODIODE_AVERAGE_OWNER_USER,
    PHOTODIODE_AVERAGE_OWNER_DARK,
};

struct photodiode_average_request {
    enum photodiode_average_state state;
    enum photodiode_average_owner owner;
    bool store_dark;
    float sum_mv;
    float sum_net_mv;
    float sum_sq_mv2;
    float min_mv;
    float max_mv;
    int16_t max_raw;
    int last_error;
    struct photodiode_average_result result;
};

static struct photodiode_runtime_channel pd_runtime[PHOTODIODE_CHANNEL_COUNT];
static struct photodiode_average_request pd_average[PHOTODIODE_CHANNEL_COUNT];
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

const char *photodiode_average_state_name(enum photodiode_average_state state)
{
    switch (state) {
    case PHOTODIODE_AVERAGE_INACTIVE:
        return "inactive";
    case PHOTODIODE_AVERAGE_MEASURING:
        return "measuring";
    case PHOTODIODE_AVERAGE_COMPLETE:
        return "complete";
    case PHOTODIODE_AVERAGE_ERROR:
        return "error";
    default:
        return "unknown";
    }
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

double photodiode_photon_flux_from_mv(double net_mv,
                                      double wavelength_nm,
                                      const struct app_pd_channel_settings *settings)
{
    double power_w;
    double photon_j;

    if (wavelength_nm <= 0.0) {
        return 0.0;
    }

    power_w = photodiode_power_uw_from_mv(net_mv, settings) * 1.0e-6;
    if (power_w <= 0.0) {
        return 0.0;
    }

    photon_j = PLANCK_J_S * LIGHT_M_PER_S / (wavelength_nm * 1.0e-9);
    return power_w / photon_j;
}

static uint32_t pd_average_duration_to_samples(uint32_t duration_ms)
{
    uint32_t requested_ms = duration_ms == 0U ? PUBLISH_INTERVAL_MS : duration_ms;

    if (requested_ms >= PD_AVERAGE_MAX_DURATION_MS) {
        return PD_AVERAGE_MAX_SAMPLES;
    }

    requested_ms += PUBLISH_INTERVAL_MS / 2U;
    requested_ms /= PUBLISH_INTERVAL_MS;

    return requested_ms == 0U ? 1U : requested_ms;
}

static void pd_average_copy_status_locked(enum photodiode_channel channel,
                                          struct photodiode_average_status *out)
{
    const struct photodiode_average_request *avg = &pd_average[channel];

    if (out == NULL) {
        return;
    }

    memset(out, 0, sizeof(*out));
    out->channel = channel;
    out->state = avg->state;
    out->store_dark = avg->store_dark;
    out->last_error = avg->last_error;
    out->result = avg->result;
}

static void pd_average_start_locked(enum photodiode_channel channel,
                                    uint32_t sample_count,
                                    enum photodiode_average_owner owner,
                                    bool store_dark,
                                    struct photodiode_average_status *out)
{
    struct photodiode_average_request *avg = &pd_average[channel];

    memset(avg, 0, sizeof(*avg));
    avg->state = PHOTODIODE_AVERAGE_MEASURING;
    avg->owner = owner;
    avg->store_dark = store_dark;
    avg->result.channel = channel;
    avg->result.duration_ms = sample_count * PUBLISH_INTERVAL_MS;
    avg->result.target_samples = sample_count;
    pd_average_copy_status_locked(channel, out);
}

/* Called from the sampler thread when an average tagged as a dark measurement
 * completes. It may persist settings and can briefly extend that sampler pass.
 */
static void pd_average_finish_dark_locked(enum photodiode_channel channel,
                                          struct photodiode_average_request *avg)
{
    struct app_photodiode_settings settings;

    if (avg->owner != PHOTODIODE_AVERAGE_OWNER_DARK) {
        return;
    }

    app_settings_get_photodiode(&settings);

    if (avg->store_dark) {
        settings.channel[channel].dark_mv = avg->result.mean_mv;
        if (!settings.channel[channel].lowest_dark_valid ||
            avg->result.mean_mv < settings.channel[channel].lowest_dark_mv) {
            settings.channel[channel].lowest_dark_mv = avg->result.mean_mv;
            settings.channel[channel].lowest_dark_valid = true;
        }

        /* This settings write can briefly extend one sampler iteration, but it
         * happens only when a user-requested dark window completes.
         */
        app_settings_update_photodiode_channel((uint8_t)channel,
                                               &settings.channel[channel],
                                               true);
    }
}

static void pd_average_sample_locked(enum photodiode_channel channel,
                                     int rc, int16_t raw, float mv, float net_mv)
{
    struct photodiode_average_request *avg = &pd_average[channel];
    float mean;
    float variance;

    if (avg->state != PHOTODIODE_AVERAGE_MEASURING ||
        avg->result.samples >= avg->result.target_samples) {
        return;
    }

    if (rc != 0) {
        avg->state = PHOTODIODE_AVERAGE_ERROR;
        avg->last_error = rc;
        return;
    }

    if (avg->result.samples == 0U) {
        avg->min_mv = mv;
        avg->max_mv = mv;
        avg->max_raw = raw;
    } else {
        if (mv < avg->min_mv) {
            avg->min_mv = mv;
        }
        if (mv > avg->max_mv) {
            avg->max_mv = mv;
        }
        if (raw > avg->max_raw) {
            avg->max_raw = raw;
        }
    }

    avg->sum_mv += mv;
    avg->sum_net_mv += net_mv;
    avg->sum_sq_mv2 += mv * mv;
    avg->result.samples++;

    if (avg->result.samples < avg->result.target_samples) {
        return;
    }

    mean = avg->sum_mv / (float)avg->result.samples;
    variance = (avg->sum_sq_mv2 / (float)avg->result.samples) - (mean * mean);
    if (variance < 0.0f) {
        variance = 0.0f;
    }

    avg->result.channel = channel;
    avg->result.mean_mv = mean;
    avg->result.mean_net_mv = avg->sum_net_mv / (float)avg->result.samples;
    avg->result.rms_mv = sqrtf(variance);
    avg->result.min_mv = avg->min_mv;
    avg->result.max_mv = avg->max_mv;
    avg->result.max_raw = avg->max_raw;
    avg->last_error = 0;
    pd_average_finish_dark_locked(channel, avg);
    avg->state = PHOTODIODE_AVERAGE_COMPLETE;
}

static void pd_window_update(float mv, struct photodiode_runtime_channel *snapshot)
{
    float old_mv;
    float mean;
    float variance;

    if (snapshot->mean_count < PD_MEAN_WINDOW_SAMPLES) {
        snapshot->mean_count++;
    } else {
        snapshot->mean_sum_mv -= snapshot->mean_window_mv[snapshot->mean_index];
    }
    snapshot->mean_window_mv[snapshot->mean_index] = mv;
    snapshot->mean_sum_mv += mv;
    snapshot->mean_index = (snapshot->mean_index + 1U) % PD_MEAN_WINDOW_SAMPLES;
    snapshot->mean_mv_1s = snapshot->mean_sum_mv / (float)snapshot->mean_count;

    if (snapshot->rms_count < PD_RMS_WINDOW_SAMPLES) {
        snapshot->rms_count++;
    } else {
        old_mv = snapshot->rms_window_mv[snapshot->rms_index];
        snapshot->rms_sum_mv -= old_mv;
        snapshot->rms_sum_sq_mv2 -= old_mv * old_mv;
    }
    snapshot->rms_window_mv[snapshot->rms_index] = mv;
    snapshot->rms_sum_mv += mv;
    snapshot->rms_sum_sq_mv2 += mv * mv;
    snapshot->rms_index = (snapshot->rms_index + 1U) % PD_RMS_WINDOW_SAMPLES;

    mean = snapshot->rms_sum_mv / (float)snapshot->rms_count;
    variance = (snapshot->rms_sum_sq_mv2 / (float)snapshot->rms_count) - (mean * mean);
    if (variance < 0.0f) {
        variance = 0.0f;
    }
    snapshot->rms_mv_0p5s = sqrtf(variance);
}

static void pd_update_channel(enum photodiode_channel channel, int rc, int16_t raw,
                              const struct app_pd_channel_settings *settings)
{
    struct photodiode_runtime_channel snapshot;
    float mv = 0.0f;
    float residual = 0.0f;
    float noise_rms = 0.0f;
    int64_t now = k_uptime_get();

    k_mutex_lock(&pd_runtime_lock, K_FOREVER);
    snapshot = pd_runtime[channel];

    if (rc == 0) {
        mv = ((float)raw * (float)PD_ADC_UV_PER_COUNT_NUM) /
           ((float)PD_ADC_UV_PER_COUNT_DEN * 1000.0f);
        if (snapshot.sample_count == 0U) {
            snapshot.smooth_mv = mv;
            snapshot.noise_var_mv2 = 0.0f;
        } else {
            residual = mv - snapshot.smooth_mv;
            snapshot.smooth_mv += PD_NOISE_ALPHA * residual;
            snapshot.noise_var_mv2 += PD_NOISE_ALPHA *
                                       ((residual * residual) - snapshot.noise_var_mv2);
            if (snapshot.noise_var_mv2 < 0.0f) {
                snapshot.noise_var_mv2 = 0.0f;
            }
        }
        noise_rms = sqrtf(snapshot.noise_var_mv2);

        snapshot.valid = true;
        snapshot.raw = raw;
        snapshot.mv = mv;
        snapshot.net_mv = mv - settings->dark_mv;
        snapshot.power_uw = (float)photodiode_power_uw_from_mv(snapshot.net_mv, settings);
        snapshot.noise_rms_mv = noise_rms;
        pd_window_update(mv, &snapshot);
        snapshot.sample_count++;
    } else {
        snapshot.valid = false;
    }

    snapshot.last_error = rc;
    snapshot.updated_ms = now;
    pd_runtime[channel] = snapshot;
    pd_average_sample_locked(channel, rc, raw, mv, snapshot.net_mv);
    k_mutex_unlock(&pd_runtime_lock);

    if (rc == 0 && settings->noise_warn_rms_mv > 0.0f &&
        noise_rms > settings->noise_warn_rms_mv &&
        now >= snapshot.next_noise_warning_ms) {
        char context[128];

        snprintk(context, sizeof(context),
                 "channel=%s noise_rms_mv=%.3f threshold_mv=%.3f",
                 photodiode_channel_names[channel],
                 (double)noise_rms,
                 (double)settings->noise_warn_rms_mv);
        coo_cmd_runtime_warning_emit(command_runtime_get(), "photodiode_noise",
                         "photodiode residual noise exceeded warning threshold",
                         context);

        k_mutex_lock(&pd_runtime_lock, K_FOREVER);
        pd_runtime[channel].next_noise_warning_ms =
            now + PD_NOISE_WARNING_COOLDOWN_MS;
        k_mutex_unlock(&pd_runtime_lock);
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
        struct photodiode_average_status average_status;

        dst->valid = src->valid;
        dst->last_error = src->last_error;
        dst->raw = src->raw;
        dst->mv = src->mv;
        dst->net_mv = src->net_mv;
        dst->power_uw = src->power_uw;
        dst->noise_rms_mv = src->noise_rms_mv;
        dst->mean_mv_1s = src->mean_mv_1s;
        dst->rms_mv_0p5s = src->rms_mv_0p5s;
        dst->dark_mv = settings.channel[i].dark_mv;
        dst->lowest_dark_mv = settings.channel[i].lowest_dark_mv;
        dst->lowest_dark_valid = settings.channel[i].lowest_dark_valid;
        pd_average_copy_status_locked((enum photodiode_channel)i, &average_status);
        dst->average_state = average_status.state;
        dst->average_duration_ms = average_status.result.duration_ms;
        dst->average_samples = average_status.result.samples;
        dst->average_target_samples = average_status.result.target_samples;
        dst->average_last_error = average_status.last_error;
        dst->sample_count = src->sample_count;
        dst->age_ms = src->updated_ms > 0 ? (uint32_t)(now - src->updated_ms) : UINT32_MAX;
    }
    k_mutex_unlock(&pd_runtime_lock);

    out->uptime_ms = now;
}

int photodiode_start_dark_measurement(enum photodiode_channel channel,
                                      uint32_t duration_ms,
                                      bool store,
                                      struct photodiode_average_status *out)
{
    uint32_t sample_count;
    uint32_t requested_ms;

    if (channel < 0 || channel >= PHOTODIODE_CHANNEL_COUNT) {
        return -EINVAL;
    }
    if (adc_dev == NULL || !device_is_ready(adc_dev)) {
        return -ENODEV;
    }

    requested_ms = duration_ms == 0U ? PD_DARK_DEFAULT_DURATION_MS : duration_ms;
    sample_count = pd_average_duration_to_samples(requested_ms);

    k_mutex_lock(&pd_runtime_lock, K_FOREVER);
    if (pd_average[channel].state == PHOTODIODE_AVERAGE_MEASURING &&
        pd_average[channel].owner != PHOTODIODE_AVERAGE_OWNER_DARK) {
        k_mutex_unlock(&pd_runtime_lock);
        return -EBUSY;
    }
    pd_average_start_locked(channel, sample_count, PHOTODIODE_AVERAGE_OWNER_DARK,
                            store, out);
    k_mutex_unlock(&pd_runtime_lock);
    return 0;
}

int photodiode_start_average(enum photodiode_channel channel,
                             uint32_t duration_ms,
                             struct photodiode_average_status *out)
{
    uint32_t sample_count;

    if (channel < 0 || channel >= PHOTODIODE_CHANNEL_COUNT) {
        return -EINVAL;
    }
    if (adc_dev == NULL || !device_is_ready(adc_dev)) {
        return -ENODEV;
    }

    sample_count = pd_average_duration_to_samples(duration_ms);

    k_mutex_lock(&pd_runtime_lock, K_FOREVER);
    if (pd_average[channel].state == PHOTODIODE_AVERAGE_MEASURING &&
        pd_average[channel].owner == PHOTODIODE_AVERAGE_OWNER_DARK) {
        k_mutex_unlock(&pd_runtime_lock);
        return -EBUSY;
    }
    pd_average_start_locked(channel, sample_count, PHOTODIODE_AVERAGE_OWNER_USER,
                            false, out);
    k_mutex_unlock(&pd_runtime_lock);

    return 0;
}

int photodiode_get_average_status(enum photodiode_channel channel,
                                  struct photodiode_average_status *out)
{
    if (channel < 0 || channel >= PHOTODIODE_CHANNEL_COUNT || out == NULL) {
        return -EINVAL;
    }

    k_mutex_lock(&pd_runtime_lock, K_FOREVER);
    pd_average_copy_status_locked(channel, out);
    k_mutex_unlock(&pd_runtime_lock);
    return 0;
}

int photodiode_reset_lowest_dark(enum photodiode_channel channel, bool persist)
{
    struct app_photodiode_settings settings;

    if (channel < 0 || channel >= PHOTODIODE_CHANNEL_COUNT) {
        return -EINVAL;
    }

    app_settings_get_photodiode(&settings);
    settings.channel[channel].lowest_dark_mv = settings.channel[channel].dark_mv;
    settings.channel[channel].lowest_dark_valid = false;
    app_settings_update_photodiode_channel((uint8_t)channel,
                                           &settings.channel[channel],
                                           persist);
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
