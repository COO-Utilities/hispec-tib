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

BUILD_ASSERT(DT_PROP(PD_HK_ADC_CHANNEL_NODE, zephyr_resolution) == ADS1115_DT_RESOLUTION,
             "photodiode ADS1115 channels must use the same resolution");

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
#define PD_NOISE_WARNING_COOLDOWN_MS 60000U
#define PD_DARK_DEFAULT_DURATION_MS (64U * PUBLISH_INTERVAL_MS)
#define PD_DARK_MAX_DURATION_MS (60U * 60U * 1000U)
#define PD_DARK_MAX_SAMPLES (PD_DARK_MAX_DURATION_MS / PUBLISH_INTERVAL_MS)
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

struct photodiode_dark_request {
    enum photodiode_dark_state state;
    bool store;
    uint32_t target_samples;
    uint32_t samples;
    uint32_t duration_ms;
    float sum_mv;
    float sum_sq_mv2;
    float min_mv;
    float max_mv;
    int last_error;
    struct photodiode_dark_result result;
};

static struct photodiode_runtime_channel pd_runtime[PHOTODIODE_CHANNEL_COUNT];
static struct photodiode_dark_request pd_dark[PHOTODIODE_CHANNEL_COUNT];
static K_MUTEX_DEFINE(pd_runtime_lock);

static int pd_read_raw(enum photodiode_channel channel, int16_t *raw)
{
    struct adc_channel_cfg cfg = *pd_adc_cfg[channel];
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

    cfg.channel_id = ADS1115_ZEPHYR_CHANNEL_ID;
    rc = adc_channel_setup(adc_dev, &cfg);
    if (rc == 0) {
        rc = adc_read(adc_dev, &seq);
    }

    return rc;
}

const char *photodiode_dark_state_name(enum photodiode_dark_state state)
{
    switch (state) {
    case PHOTODIODE_DARK_IDLE:
        return "idle";
    case PHOTODIODE_DARK_MEASURING:
        return "measuring";
    case PHOTODIODE_DARK_COMPLETE:
        return "complete";
    case PHOTODIODE_DARK_ERROR:
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

static uint32_t pd_dark_duration_to_samples(uint32_t duration_ms)
{
    uint32_t requested_ms = duration_ms == 0U ?
                            PD_DARK_DEFAULT_DURATION_MS : duration_ms;

    if (requested_ms >= PD_DARK_MAX_DURATION_MS) {
        return PD_DARK_MAX_SAMPLES;
    }

    requested_ms += PUBLISH_INTERVAL_MS / 2U;
    requested_ms /= PUBLISH_INTERVAL_MS;

    return requested_ms == 0U ? 1U : requested_ms;
}

static void pd_dark_copy_status_locked(enum photodiode_channel channel,
                                       struct photodiode_dark_status *out)
{
    const struct photodiode_dark_request *dark = &pd_dark[channel];

    if (out == NULL) {
        return;
    }

    memset(out, 0, sizeof(*out));
    out->channel = channel;
    out->state = dark->state;
    out->store = dark->store;
    out->duration_ms = dark->duration_ms;
    out->samples = dark->samples;
    out->target_samples = dark->target_samples;
    out->last_error = dark->last_error;
    out->result = dark->result;
}

static void pd_dark_finish_store_locked(enum photodiode_channel channel,
                                        struct photodiode_dark_request *dark)
{
    struct app_photodiode_settings settings;

    app_settings_get_photodiode(&settings);
    settings.channel[channel].dark_mv = dark->result.mean_mv;
    if (!settings.channel[channel].lowest_dark_valid ||
        dark->result.mean_mv < settings.channel[channel].lowest_dark_mv) {
        settings.channel[channel].lowest_dark_mv = dark->result.mean_mv;
        settings.channel[channel].lowest_dark_valid = true;
    }

    dark->result.configured_dark_mv = settings.channel[channel].dark_mv;
    dark->result.lowest_dark_mv = settings.channel[channel].lowest_dark_mv;
    dark->result.lowest_dark_valid = settings.channel[channel].lowest_dark_valid;

    /* This settings write can briefly extend one sampler iteration, but it
     * happens only when a user-requested dark window completes.
     */
    app_settings_update_photodiode_channel((uint8_t)channel,
                                           &settings.channel[channel],
                                           true);
}

static void pd_dark_sample_locked(enum photodiode_channel channel, int rc, float mv)
{
    struct photodiode_dark_request *dark = &pd_dark[channel];
    float mean;
    float variance;

    if (dark->state != PHOTODIODE_DARK_MEASURING ||
        dark->samples >= dark->target_samples) {
        return;
    }

    if (rc != 0) {
        dark->state = PHOTODIODE_DARK_ERROR;
        dark->last_error = rc;
        dark->result.samples = dark->samples;
        return;
    }

    if (dark->samples == 0U) {
        dark->min_mv = mv;
        dark->max_mv = mv;
    } else {
        if (mv < dark->min_mv) {
            dark->min_mv = mv;
        }
        if (mv > dark->max_mv) {
            dark->max_mv = mv;
        }
    }

    dark->sum_mv += mv;
    dark->sum_sq_mv2 += mv * mv;
    dark->samples++;

    if (dark->samples < dark->target_samples) {
        return;
    }

    mean = dark->sum_mv / (float)dark->samples;
    variance = (dark->sum_sq_mv2 / (float)dark->samples) - (mean * mean);
    if (variance < 0.0f) {
        variance = 0.0f;
    }

    dark->result.samples = dark->samples;
    dark->result.mean_mv = mean;
    dark->result.rms_mv = sqrtf(variance);
    dark->result.min_mv = dark->min_mv;
    dark->result.max_mv = dark->max_mv;
    dark->last_error = 0;

    if (dark->store) {
        pd_dark_finish_store_locked(channel, dark);
    }

    dark->state = PHOTODIODE_DARK_COMPLETE;
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
    pd_dark_sample_locked(channel, rc, mv);
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
        dst->dark_state = pd_dark[i].state;
        dst->dark_duration_ms = pd_dark[i].duration_ms;
        dst->dark_samples = pd_dark[i].samples;
        dst->dark_target_samples = pd_dark[i].target_samples;
        dst->dark_last_error = pd_dark[i].last_error;
        dst->sample_count = src->sample_count;
        dst->age_ms = src->updated_ms > 0 ? (uint32_t)(now - src->updated_ms) : UINT32_MAX;
    }
    k_mutex_unlock(&pd_runtime_lock);

    out->uptime_ms = now;
}

int photodiode_start_dark_measurement(enum photodiode_channel channel,
                                      uint32_t duration_ms,
                                      bool store,
                                      struct photodiode_dark_status *out)
{
    struct app_photodiode_settings settings;
    uint32_t sample_count;
    struct photodiode_dark_request *dark;

    if (channel < 0 || channel >= PHOTODIODE_CHANNEL_COUNT) {
        return -EINVAL;
    }
    if (adc_dev == NULL || !device_is_ready(adc_dev)) {
        return -ENODEV;
    }

    sample_count = pd_dark_duration_to_samples(duration_ms);
    app_settings_get_photodiode(&settings);

    k_mutex_lock(&pd_runtime_lock, K_FOREVER);
    dark = &pd_dark[channel];
    memset(dark, 0, sizeof(*dark));
    dark->state = PHOTODIODE_DARK_MEASURING;
    dark->store = store;
    dark->target_samples = sample_count;
    dark->duration_ms = sample_count * PUBLISH_INTERVAL_MS;
    dark->result.channel = channel;
    dark->result.duration_ms = dark->duration_ms;
    dark->result.stored = store;
    dark->result.previous_dark_mv = settings.channel[channel].dark_mv;
    dark->result.configured_dark_mv = settings.channel[channel].dark_mv;
    dark->result.lowest_dark_mv = settings.channel[channel].lowest_dark_mv;
    dark->result.lowest_dark_valid = settings.channel[channel].lowest_dark_valid;
    pd_dark_copy_status_locked(channel, out);
    k_mutex_unlock(&pd_runtime_lock);

    return 0;
}

int photodiode_get_dark_status(enum photodiode_channel channel,
                               struct photodiode_dark_status *out)
{
    if (channel < 0 || channel >= PHOTODIODE_CHANNEL_COUNT || out == NULL) {
        return -EINVAL;
    }

    k_mutex_lock(&pd_runtime_lock, K_FOREVER);
    pd_dark_copy_status_locked(channel, out);
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
    ARG_UNUSED(p1);
    ARG_UNUSED(p2);
    ARG_UNUSED(p3);

    k_sleep(K_MSEC(10));

    while (adc_dev == NULL || !device_is_ready(adc_dev)) {
        LOG_ERR("ADS1115 not ready");
        k_sleep(K_MSEC(100));
    }

    k_timer_start(&pd_sample_timer, K_NO_WAIT, K_MSEC(PUBLISH_INTERVAL_MS));

    while (1) {
        struct app_photodiode_settings settings;
        int64_t start = k_uptime_get();
        uint32_t elapsed_samples;

        k_sem_take(&pd_sample_sem, K_FOREVER);
        elapsed_samples = k_timer_status_get(&pd_sample_timer);
        if (elapsed_samples > 1U) {
            LOG_WRN("ADC sample timer missed %u intervals",
                    (unsigned int)(elapsed_samples - 1U));
        }

        app_settings_get_photodiode(&settings);

        for (uint8_t i = 0; i < PHOTODIODE_CHANNEL_COUNT; ++i) {
            int16_t raw = 0;
            int rc = pd_read_raw((enum photodiode_channel)i, &raw);

            if (rc != 0) {
                LOG_ERR("ADC %s read failed (%d)", photodiode_channel_names[i], rc);
            }
            pd_update_channel((enum photodiode_channel)i, rc, raw, &settings.channel[i]);
        }

        int64_t elapsed = k_uptime_get() - start;  // overflow every 300M years
        if (elapsed > PUBLISH_INTERVAL_MS) {
            LOG_WRN("ADC loop overran interval by %lld ms",
                    elapsed - PUBLISH_INTERVAL_MS);
        }
    }
}
