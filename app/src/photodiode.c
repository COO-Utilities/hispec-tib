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

static const char *const pd_channel_names[PHOTODIODE_CHANNEL_COUNT] = {
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
#define PD_DARK_DEFAULT_SAMPLES 64U
#define PD_DARK_MAX_SAMPLES 512U

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

static struct photodiode_runtime_channel pd_runtime[PHOTODIODE_CHANNEL_COUNT];
static K_MUTEX_DEFINE(pd_runtime_lock);
static K_MUTEX_DEFINE(pd_adc_lock);

int photodiode_channel_from_name(const char *name, enum photodiode_channel *channel)
{
    if (name == NULL || channel == NULL) {
        return -EINVAL;
    }

    for (uint8_t i = 0; i < PHOTODIODE_CHANNEL_COUNT; ++i) {
        if (strcmp(name, pd_channel_names[i]) == 0) {
            *channel = (enum photodiode_channel)i;
            return 0;
        }
    }

    return -ENOENT;
}

const char *photodiode_channel_name(enum photodiode_channel channel)
{
    if (channel < 0 || channel >= PHOTODIODE_CHANNEL_COUNT) {
        return "unknown";
    }

    return pd_channel_names[channel];
}

static float pd_sqrtf(float value)
{
    float x;

    if (value <= 0.0f) {
        return 0.0f;
    }

    x = value > 1.0f ? value : 1.0f;
    for (uint8_t i = 0; i < 6U; ++i) {
        x = 0.5f * (x + value / x);
    }

    return x;
}

static float pd_raw_to_mv(int16_t raw)
{
    return ((float)raw * (float)PD_ADC_UV_PER_COUNT_NUM) /
           ((float)PD_ADC_UV_PER_COUNT_DEN * 1000.0f);
}

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
    if (!devices_has_photodiodes() || adc_dev == NULL || !device_is_ready(adc_dev)) {
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

static float pd_power_uw(float net_mv, const struct app_pd_channel_settings *settings)
{
    if (settings == NULL || !(settings->gain_v_per_uw > 0.0f)) {
        return 0.0f;
    }

    return net_mv / (settings->gain_v_per_uw * 1000.0f);
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
        mv = pd_raw_to_mv(raw);
        if (snapshot.sample_count == 0U) {
            snapshot.smooth_mv = mv;
            snapshot.noise_var_mv2 = 0.0f;
        } else {
            residual = mv - snapshot.smooth_mv;
            snapshot.smooth_mv += PD_NOISE_ALPHA * residual;
            snapshot.noise_var_mv2 += PD_NOISE_ALPHA *
                                      ((residual * residual) - snapshot.noise_var_mv2);
        }
        noise_rms = pd_sqrtf(snapshot.noise_var_mv2);

        snapshot.valid = true;
        snapshot.raw = raw;
        snapshot.mv = mv;
        snapshot.net_mv = mv - settings->dark_mv;
        snapshot.power_uw = pd_power_uw(snapshot.net_mv, settings);
        snapshot.noise_rms_mv = noise_rms;
        snapshot.sample_count++;
    } else {
        snapshot.valid = false;
    }

    snapshot.last_error = rc;
    snapshot.updated_ms = now;
    pd_runtime[channel] = snapshot;
    k_mutex_unlock(&pd_runtime_lock);

    if (rc == 0 && settings->noise_warn_rms_mv > 0.0f &&
        noise_rms > settings->noise_warn_rms_mv &&
        now >= snapshot.next_noise_warning_ms) {
        char context[128];

        snprintk(context, sizeof(context),
                 "channel=%s noise_rms_mv=%.3f threshold_mv=%.3f",
                 photodiode_channel_name(channel),
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
                       photodiode_channel_name(channel),
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

int photodiode_measure_dark(enum photodiode_channel channel, uint16_t samples,
                            bool store, struct photodiode_dark_result *out)
{
    struct app_photodiode_settings settings;
    uint32_t n;
    float sum = 0.0f;
    float sum_sq = 0.0f;
    float min_mv = 0.0f;
    float max_mv = 0.0f;
    int rc = 0;

    if (channel < 0 || channel >= PHOTODIODE_CHANNEL_COUNT || out == NULL) {
        return -EINVAL;
    }

    if (samples == 0U) {
        samples = PD_DARK_DEFAULT_SAMPLES;
    }
    if (samples > PD_DARK_MAX_SAMPLES) {
        samples = PD_DARK_MAX_SAMPLES;
    }

    for (n = 0U; n < samples; ++n) {
        int16_t raw = 0;
        float mv;

        rc = pd_read_raw(channel, &raw);
        if (rc != 0) {
            return rc;
        }

        mv = pd_raw_to_mv(raw);
        if (n == 0U || mv < min_mv) {
            min_mv = mv;
        }
        if (n == 0U || mv > max_mv) {
            max_mv = mv;
        }
        sum += mv;
        sum_sq += mv * mv;
    }

    memset(out, 0, sizeof(*out));
    out->channel = channel;
    out->samples = (uint16_t)n;
    out->stored = store;
    out->mean_mv = sum / (float)n;
    out->rms_mv = pd_sqrtf((sum_sq / (float)n) - (out->mean_mv * out->mean_mv));
    out->min_mv = min_mv;
    out->max_mv = max_mv;

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
        app_settings_update_photodiode(&settings, true);
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
    app_settings_update_photodiode(&settings, persist);
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

    if (!devices_has_photodiodes()) {
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
                LOG_ERR("ADC %s read failed (%d)", photodiode_channel_name(i), rc);
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
