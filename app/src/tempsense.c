//
// Created by Jeb Bailey on 4/28/26.
//

#include "tempsense.h"
#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/sensor.h>

#include <zephyr/kernel.h>             // k_sleep and uptime helpers.
#include <zephyr/logging/log.h>        // LOG_ERR and LOG_INF.
#include <errno.h>
#include <string.h>

LOG_MODULE_REGISTER(tempsense, LOG_LEVEL_INF);

struct tempsense_status tempsense;

static K_MUTEX_DEFINE(tempsense_lock);
static int64_t last_sample_ms;

static void tempsense_update(float ambient_c, int error, bool valid)
{
    /* One producer updates this cache; unrelated consumers read a snapshot. */
    k_mutex_lock(&tempsense_lock, K_FOREVER);
    tempsense.ambient_c = ambient_c;
    tempsense.last_error = error;
    tempsense.valid = valid;
    last_sample_ms = valid ? k_uptime_get() : 0;
    tempsense.age_ms = 0;
    k_mutex_unlock(&tempsense_lock);
}

static void tempsense_mark_error(int error)
{
    /* Keep the last good value but mark it unavailable until a fresh read works. */
    k_mutex_lock(&tempsense_lock, K_FOREVER);
    tempsense.last_error = error;
    tempsense.valid = false;
    last_sample_ms = 0;
    tempsense.age_ms = UINT32_MAX;
    k_mutex_unlock(&tempsense_lock);
}

void tempsense_get_status(struct tempsense_status *out)
{
    int64_t now;

    if (out == NULL) {
        return;
    }

    /* Copies a stable status so callers do not read while the sampler writes. */
    k_mutex_lock(&tempsense_lock, K_FOREVER);
    *out = tempsense;
    now = k_uptime_get();
    out->age_ms = (out->valid && last_sample_ms > 0) ? (uint32_t)(now - last_sample_ms) : UINT32_MAX;
    k_mutex_unlock(&tempsense_lock);
}


/*
 * Get a device structure from a devicetree node with compatible
 * "maxim,ds18b20". (If there are multiple, just pick one.)
 */
static const struct device *get_ds18b20_device(void)
{
    const struct device *const dev = DEVICE_DT_GET_ANY(maxim_ds18b20);

    if (dev == NULL) {
        LOG_ERR("No DS18B20 devicetree node with status okay");
        return NULL;
    }

    if (!device_is_ready(dev)) {
        LOG_ERR("DS18B20 device %s is not ready", dev->name);
        return NULL;
    }

    LOG_INF("Using DS18B20 device %s", dev->name);
    return dev;
}


void tempsensor_thread(void *p1, void *p2, void *p3)
{
    const struct device *dev = get_ds18b20_device();

    ARG_UNUSED(p1);
    ARG_UNUSED(p2);
    ARG_UNUSED(p3);

    tempsense_update(0.0f, dev == NULL ? -ENODEV : 0, false);

    if (dev == NULL) {
        return;
    }

    while (true) {
        struct sensor_value temp;
        float ambient_c;
        int res;

        /* Refresh the Zephyr sensor sample before reading the temperature channel. */
        res = sensor_sample_fetch(dev);
        if (res != 0) {
            LOG_ERR("DS18B20 sample fetch failed: %d", res);
            tempsense_mark_error(res);
            k_sleep(K_SECONDS(1));
            continue;
        }

        /* SENSOR_CHAN_AMBIENT_TEMP returns Celsius in integer + micro unit parts. */
        res = sensor_channel_get(dev, SENSOR_CHAN_AMBIENT_TEMP, &temp);
        if (res != 0) {
            LOG_ERR("DS18B20 channel read failed: %d", res);
            tempsense_mark_error(res);
            k_sleep(K_SECONDS(1));
            continue;
        }

        ambient_c = sensor_value_to_double(&temp);
        tempsense_update(ambient_c, 0, true);

        k_sleep(K_MSEC(1000));
    }
}
