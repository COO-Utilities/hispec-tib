/**
 * @file housekeeping.c
 * @brief Slow background polling for ambient sensing and power policy.
 *
 * This actor owns slow periodic work that does not need a timing-critical
 * thread: ambient temperature sampling, laser-bank heater policy, laser
 * autooff service, and future auxiliary-power housekeeping.
 */

#include "housekeeping.h"

#include <errno.h>
#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/sensor.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/util.h>

#include "devices.h"
#include "laserbank_tempcontrol.h"

LOG_MODULE_REGISTER(housekeeping, LOG_LEVEL_INF);

#define HOUSEKEEPING_TEMP_INTERVAL_MS 1000U

static struct housekeeping_temperature_status temperature_status;
static K_MUTEX_DEFINE(temperature_status_lock);
static int64_t last_sample_ms;
static const struct device *temperature_dev;
static bool temperature_initialized;

static void temperature_update(float ambient_c, int error, bool valid)
{
	k_mutex_lock(&temperature_status_lock, K_FOREVER);
	temperature_status.ambient_c = ambient_c;
	temperature_status.last_error = error;
	temperature_status.valid = valid;
	last_sample_ms = valid ? k_uptime_get() : 0;
	temperature_status.age_ms = 0;
	k_mutex_unlock(&temperature_status_lock);
}

static void temperature_mark_error(int error)
{
	k_mutex_lock(&temperature_status_lock, K_FOREVER);
	temperature_status.last_error = error;
	temperature_status.valid = false;
	last_sample_ms = 0;
	temperature_status.age_ms = UINT32_MAX;
	k_mutex_unlock(&temperature_status_lock);
}

void housekeeping_get_temperature_status(struct housekeeping_temperature_status *out)
{
	int64_t now;

	if (out == NULL) {
		return;
	}

	k_mutex_lock(&temperature_status_lock, K_FOREVER);
	*out = temperature_status;
	now = k_uptime_get();
	out->age_ms = (out->valid && last_sample_ms > 0) ?
		      (uint32_t)(now - last_sample_ms) : UINT32_MAX;
	k_mutex_unlock(&temperature_status_lock);
}

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

static int temperature_init_once(void)
{
	int rc = 0;

	if (temperature_initialized) {
		return temperature_dev == NULL ? -ENODEV : 0;
	}

	temperature_dev = get_ds18b20_device();
	if (temperature_dev == NULL) {
		rc = -ENODEV;
	}
	temperature_update(0.0f, rc, false);
	temperature_initialized = true;
	return rc;
}

static int temperature_sample_once(void)
{
	struct sensor_value temp;
	float ambient_c;
	int rc;

	rc = temperature_init_once();
	if (rc != 0) {
		return rc;
	}

	/* Refresh the Zephyr sensor sample before reading the temperature channel. */
	rc = sensor_sample_fetch(temperature_dev);
	if (rc != 0) {
		LOG_ERR("DS18B20 sample fetch failed: %d", rc);
		temperature_mark_error(rc);
		return rc;
	}

	/* SENSOR_CHAN_AMBIENT_TEMP returns Celsius in integer + micro unit parts. */
	rc = sensor_channel_get(temperature_dev, SENSOR_CHAN_AMBIENT_TEMP, &temp);
	if (rc != 0) {
		LOG_ERR("DS18B20 channel read failed: %d", rc);
		temperature_mark_error(rc);
		return rc;
	}

	ambient_c = sensor_value_to_double(&temp);
	temperature_update(ambient_c, 0, true);
	return 0;
}

void housekeeping_thread(void *p1, void *p2, void *p3)
{
	int64_t next_temp_ms = 0;
	int64_t next_laserbank_ms = 0;
	bool laserbank_wake = false;

	ARG_UNUSED(p1);
	ARG_UNUSED(p2);
	ARG_UNUSED(p3);

	while (true) {
		int64_t now = k_uptime_get();

		if (now >= next_temp_ms) {
			(void)temperature_sample_once();
			next_temp_ms = now + HOUSEKEEPING_TEMP_INTERVAL_MS;
		}

		if (devices_board_type() == HISPEC_BOARD_TIB &&
		    (laserbank_wake || now >= next_laserbank_ms)) {
			laserbank_tempcontrol_run_once();
			next_laserbank_ms = k_uptime_get() +
					    LASERBANK_TEMPCONTROL_POLL_INTERVAL_MS;
			laserbank_wake = false;
		}

		now = k_uptime_get();
		int64_t wait_ms = next_temp_ms - now;

		if (devices_board_type() == HISPEC_BOARD_TIB) {
			wait_ms = MIN(wait_ms, next_laserbank_ms - now);
		}
		if (wait_ms < 0) {
			wait_ms = 0;
		}

		if (devices_board_type() == HISPEC_BOARD_TIB) {
			laserbank_wake =
				laserbank_tempcontrol_wait_for_wake(K_MSEC(wait_ms));
		} else {
			k_sleep(K_MSEC(wait_ms));
		}
	}
}
