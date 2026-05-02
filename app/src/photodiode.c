//
// Created by Jeb Bailey on 5/19/25.
//


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
#include "app_warning.h"
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

const char *const photodiode_channel_names[PHOTODIODE_CHANNEL_COUNT] = {
    "yj",
    "hk",
};

/* Hardware docs specify ADS1115 +/-6.144 V full scale at ADC_GAIN_1_3, which
 * gives 187.5 uV per signed 16-bit count.
 */
#define PD_ADC_UV_PER_COUNT_NUM 1875
#define PD_ADC_UV_PER_COUNT_DEN 10
#define PD_NOISE_ALPHA 0.02f
#define PD_NOISE_WARNING_COOLDOWN_MS 60000U
#define PD_DARK_DEFAULT_DURATION_MS (64U * PUBLISH_INTERVAL_MS)
#define PD_DARK_MAX_SAMPLES 512U
#define PD_DARK_TIMEOUT_MARGIN_MS 1000U

K_MSGQ_DEFINE(photodiode_queue, sizeof(struct OutMsg), 4, 4);

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
    uint32_t sample_count;
    int64_t updated_ms;
    int64_t next_noise_warning_ms;
};

struct photodiode_dark_request {
    bool active;
    bool done;
    bool store;
    enum photodiode_channel channel;
    uint16_t target_samples;
    uint16_t samples;
    uint32_t duration_ms;
    float sum_mv;
    float sum_sq_mv2;
    float min_mv;
    float max_mv;
    int result_rc;
    struct photodiode_dark_result result;
};

static struct photodiode_runtime_channel pd_runtime[PHOTODIODE_CHANNEL_COUNT];
static struct photodiode_dark_request pd_dark;
static K_MUTEX_DEFINE(pd_runtime_lock);
static K_MUTEX_DEFINE(pd_adc_lock);
static K_MUTEX_DEFINE(pd_dark_lock);
static K_SEM_DEFINE(pd_dark_done, 0, 1);

static int pd_read_raw(enum photodiode_channel channel, int16_t *raw)
{
    struct adc_sequence seq = {
        .channels = 0,
        .buffer = raw,
        .buffer_size = sizeof(*raw),
        .resolution = ADC_RESOLUTION,
        .oversampling = 0,
        .calibrate = false,
    };
    int rc;

    if (raw == NULL || channel < 0 || channel >= PHOTODIODE_CHANNEL_COUNT) {
        return -EINVAL;
    }
    if (devices_board_type() != HISPEC_BOARD_TIB ||
        adc_dev == NULL || !device_is_ready(adc_dev)) {
        return -ENODEV;
    }

    /* ADS1115 conversions are serialized because the chip uses a muxed ADC.
     * adc_read() blocks until the selected channel conversion completes.
     */
    k_mutex_lock(&pd_adc_lock, K_FOREVER);
    rc = adc_channel_setup(adc_dev, pd_adc_cfg[channel]);
    if (rc == 0) {
        rc = adc_read(adc_dev, &seq);
    }
    k_mutex_unlock(&pd_adc_lock);

    return rc;
}

static void pd_dark_sample(enum photodiode_channel channel, int rc, float mv)
{
    float mean;
    float variance;

    /* Dark calibration is synchronized by a semaphore: the command path
     * requests a window, and the sampler thread gives the semaphore when the
     * requested number of regular ADC samples has been latched.
     */
    k_mutex_lock(&pd_dark_lock, K_FOREVER);
    if (!pd_dark.active || pd_dark.channel != channel) {
        k_mutex_unlock(&pd_dark_lock);
        return;
    }

    if (rc != 0) {
        memset(&pd_dark.result, 0, sizeof(pd_dark.result));
        pd_dark.result.channel = channel;
        pd_dark.result.duration_ms = pd_dark.duration_ms;
        pd_dark.result.samples = pd_dark.samples;
        pd_dark.result.stored = pd_dark.store;
        pd_dark.result_rc = rc;
        pd_dark.active = false;
        pd_dark.done = true;
        k_sem_give(&pd_dark_done);
        k_mutex_unlock(&pd_dark_lock);
        return;
    }

    if (pd_dark.samples == 0U) {
        pd_dark.min_mv = mv;
        pd_dark.max_mv = mv;
    } else {
        if (mv < pd_dark.min_mv) {
            pd_dark.min_mv = mv;
        }
        if (mv > pd_dark.max_mv) {
            pd_dark.max_mv = mv;
        }
    }

    pd_dark.sum_mv += mv;
    pd_dark.sum_sq_mv2 += mv * mv;
    pd_dark.samples++;

    if (pd_dark.samples < pd_dark.target_samples) {
        k_mutex_unlock(&pd_dark_lock);
        return;
    }

    mean = pd_dark.sum_mv / (float)pd_dark.samples;
    variance = (pd_dark.sum_sq_mv2 / (float)pd_dark.samples) - (mean * mean);
    if (variance < 0.0f) {
        variance = 0.0f;
    }

    memset(&pd_dark.result, 0, sizeof(pd_dark.result));
    pd_dark.result.channel = channel;
    pd_dark.result.duration_ms = pd_dark.duration_ms;
    pd_dark.result.samples = pd_dark.samples;
    pd_dark.result.stored = pd_dark.store;
    pd_dark.result.mean_mv = mean;
    pd_dark.result.rms_mv = sqrtf(variance);
    pd_dark.result.min_mv = pd_dark.min_mv;
    pd_dark.result.max_mv = pd_dark.max_mv;
    pd_dark.result_rc = 0;
    pd_dark.active = false;
    pd_dark.done = true;
    k_sem_give(&pd_dark_done);
    k_mutex_unlock(&pd_dark_lock);
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
        snapshot.power_uw = (settings->gain_v_per_uw > 0.0f) ?
                            snapshot.net_mv / (settings->gain_v_per_uw * 1000.0f) :
                            0.0f;
        snapshot.noise_rms_mv = noise_rms;
        snapshot.sample_count++;
    } else {
        snapshot.valid = false;
    }

    snapshot.last_error = rc;
    snapshot.updated_ms = now;
    pd_runtime[channel] = snapshot;
    k_mutex_unlock(&pd_runtime_lock);

    pd_dark_sample(channel, rc, mv);

    if (rc == 0 && settings->noise_warn_rms_mv > 0.0f &&
        noise_rms > settings->noise_warn_rms_mv &&
        now >= snapshot.next_noise_warning_ms) {
        char context[128];

        snprintk(context, sizeof(context),
                 "channel=%s noise_rms_mv=%.3f threshold_mv=%.3f",
                 photodiode_channel_names[channel],
                 (double)noise_rms,
                 (double)settings->noise_warn_rms_mv);
        app_warning_emit("photodiode_noise",
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
        dst->dark_mv = settings.channel[i].dark_mv;
        dst->lowest_dark_mv = settings.channel[i].lowest_dark_mv;
        dst->lowest_dark_valid = settings.channel[i].lowest_dark_valid;
        dst->sample_count = src->sample_count;
        dst->age_ms = src->updated_ms > 0 ? (uint32_t)(now - src->updated_ms) : UINT32_MAX;
    }
    k_mutex_unlock(&pd_runtime_lock);

    out->uptime_ms = now;
}

static void pd_format_channel_json(char *payload, size_t payload_len, size_t *off,
                                   enum photodiode_channel channel,
                                   const struct photodiode_channel_status *status)
{
    int written;

    written = snprintk(payload + *off, payload_len - *off,
                       "\"%s\":{\"valid\":%s,\"raw\":%d,\"mv\":%.3f,"
                       "\"net_mv\":%.3f,\"power_uw\":%.6f,"
                       "\"noise_rms_mv\":%.3f,\"dark_mv\":%.3f,"
                       "\"lowest_dark_mv\":%.3f,\"lowest_dark_valid\":%s,"
                       "\"age_ms\":%u,\"samples\":%u}",
                       photodiode_channel_names[channel],
                       status->valid ? "true" : "false",
                       status->raw,
                       (double)status->mv,
                       (double)status->net_mv,
                       (double)status->power_uw,
                       (double)status->noise_rms_mv,
                       (double)status->dark_mv,
                       (double)status->lowest_dark_mv,
                       status->lowest_dark_valid ? "true" : "false",
                       status->age_ms,
                       status->sample_count);
    if (written > 0 && written < (int)(payload_len - *off)) {
        *off += (size_t)written;
    }
}

static void pd_build_telemetry_payload(char *payload, size_t payload_len)
{
    struct photodiode_status status;
    size_t off = 0;
    int written;

    photodiode_get_status(&status);

    written = snprintk(payload, payload_len, "{");
    if (written < 0 || written >= (int)payload_len) {
        return;
    }
    off = (size_t)written;
    pd_format_channel_json(payload, payload_len, &off, PHOTODIODE_CHANNEL_YJ,
                           &status.channel[PHOTODIODE_CHANNEL_YJ]);
    written = snprintk(payload + off, payload_len - off, ",");
    if (written < 0 || written >= (int)(payload_len - off)) {
        return;
    }
    off += (size_t)written;
    pd_format_channel_json(payload, payload_len, &off, PHOTODIODE_CHANNEL_HK,
                           &status.channel[PHOTODIODE_CHANNEL_HK]);
    (void)snprintk(payload + off, payload_len - off,
                   ",\"uptime_ms\":%lld}", status.uptime_ms);
}

int photodiode_measure_dark(enum photodiode_channel channel, uint32_t duration_ms,
                            bool store, struct photodiode_dark_result *out)
{
    struct app_photodiode_settings settings;
    uint32_t requested_ms;
    uint32_t sample_count;
    uint32_t max_duration_ms;
    uint32_t timeout_ms;
    int rc;

    if (channel < 0 || channel >= PHOTODIODE_CHANNEL_COUNT || out == NULL) {
        return -EINVAL;
    }
    if (devices_board_type() != HISPEC_BOARD_TIB ||
        adc_dev == NULL || !device_is_ready(adc_dev)) {
        return -ENODEV;
    }

    requested_ms = duration_ms == 0U ? PD_DARK_DEFAULT_DURATION_MS : duration_ms;
    max_duration_ms = PD_DARK_MAX_SAMPLES * PUBLISH_INTERVAL_MS;
    if (requested_ms >= max_duration_ms) {
        sample_count = PD_DARK_MAX_SAMPLES;
    } else {
        sample_count = (requested_ms + (PUBLISH_INTERVAL_MS / 2U)) / PUBLISH_INTERVAL_MS;
        if (sample_count == 0U) {
            sample_count = 1U;
        }
    }

    k_mutex_lock(&pd_dark_lock, K_FOREVER);
    if (pd_dark.active) {
        k_mutex_unlock(&pd_dark_lock);
        return -EBUSY;
    }
    memset(&pd_dark, 0, sizeof(pd_dark));
    pd_dark.active = true;
    pd_dark.store = store;
    pd_dark.channel = channel;
    pd_dark.target_samples = (uint16_t)sample_count;
    pd_dark.duration_ms = sample_count * PUBLISH_INTERVAL_MS;
    k_sem_reset(&pd_dark_done);
    k_mutex_unlock(&pd_dark_lock);

    /* Wait for the sampler thread to latch the requested window. This avoids
     * direct ADC reads from command handling and keeps dark calibration on the
     * same cadence as normal photodiode monitoring.
     */
    timeout_ms = (sample_count * PUBLISH_INTERVAL_MS) + PD_DARK_TIMEOUT_MARGIN_MS;
    rc = k_sem_take(&pd_dark_done, K_MSEC(timeout_ms));
    if (rc != 0) {
        k_mutex_lock(&pd_dark_lock, K_FOREVER);
        pd_dark.active = false;
        pd_dark.done = false;
        k_mutex_unlock(&pd_dark_lock);
        return -ETIMEDOUT;
    }

    k_mutex_lock(&pd_dark_lock, K_FOREVER);
    if (!pd_dark.done) {
        k_mutex_unlock(&pd_dark_lock);
        return -EIO;
    }
    rc = pd_dark.result_rc;
    *out = pd_dark.result;
    pd_dark.done = false;
    k_mutex_unlock(&pd_dark_lock);

    if (rc != 0) {
        return rc;
    }

    app_settings_get_photodiode(&settings);
    out->previous_dark_mv = settings.channel[channel].dark_mv;
    out->configured_dark_mv = settings.channel[channel].dark_mv;
    out->lowest_dark_mv = settings.channel[channel].lowest_dark_mv;
    out->lowest_dark_valid = settings.channel[channel].lowest_dark_valid;

    if (store) {
        settings.channel[channel].dark_mv = out->mean_mv;
        if (!settings.channel[channel].lowest_dark_valid ||
            out->mean_mv < settings.channel[channel].lowest_dark_mv) {
            settings.channel[channel].lowest_dark_mv = out->mean_mv;
            settings.channel[channel].lowest_dark_valid = true;
        }
        out->configured_dark_mv = settings.channel[channel].dark_mv;
        out->lowest_dark_mv = settings.channel[channel].lowest_dark_mv;
        out->lowest_dark_valid = settings.channel[channel].lowest_dark_valid;
        app_settings_update_photodiode_channel((uint8_t)channel,
                                               &settings.channel[channel],
                                               true);
    }

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

    while (!devices_board_type_checked()) {
        k_sleep(K_MSEC(10));
    }

    if (devices_board_type() != HISPEC_BOARD_TIB) {
        LOG_INF("Photodiode monitor disabled for board type %s",
                devices_board_type_name());
        while (1) {
            k_sleep(K_HOURS(1));
        }
    }

    while (adc_dev == NULL || !device_is_ready(adc_dev)) {
        LOG_ERR("ADS1115 not ready");
        k_sleep(K_MSEC(100));
    }

    while (1) {
        struct app_photodiode_settings settings;
        struct OutMsg msg = {0};
        int64_t start = k_uptime_get();

        app_settings_get_photodiode(&settings);

        for (uint8_t i = 0; i < PHOTODIODE_CHANNEL_COUNT; ++i) {
            int16_t raw = 0;
            int rc = pd_read_raw((enum photodiode_channel)i, &raw);

            if (rc != 0) {
                LOG_ERR("ADC %s read failed (%d)", photodiode_channel_names[i], rc);
            }
            pd_update_channel((enum photodiode_channel)i, rc, raw, &settings.channel[i]);
        }

        msg.target = OUT_TARGET_MQTT;
        msg.qos = 0;
        snprintk(msg.topic, sizeof(msg.topic), "dt/hsfib-tib/photodiode");
        pd_build_telemetry_payload(msg.payload, sizeof(msg.payload));
        msg.payload_len = strlen(msg.payload);

        while (k_msgq_put(&photodiode_queue, &msg, K_NO_WAIT) != 0) {
            /* photodiode_queue is full: purge old data & try again */
            LOG_WRN("ADC msgq full, purging");
            k_msgq_purge(&photodiode_queue);
        }

        int64_t elapsed = k_uptime_get() - start;  // overflow every 300M years
        int64_t remaining = PUBLISH_INTERVAL_MS - elapsed;

        if (remaining > 0) {
            k_sleep(K_MSEC(remaining));
        } else {
            LOG_WRN("ADC loop overran interval by %lld ms", -remaining);
        }
    }
}
