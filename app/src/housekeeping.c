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
#include "tempsense.h"

LOG_MODULE_REGISTER(housekeeping, LOG_LEVEL_INF);

#define HOUSEKEEPING_TEMP_INTERVAL_MS 1000U

static struct tempsense_status tempsense;
static K_MUTEX_DEFINE(tempsense_lock);
static int64_t last_sample_ms;
static const struct device *tempsense_dev;
static bool tempsense_initialized;

static void tempsense_update(float ambient_c, int error, bool valid)
{
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

	k_mutex_lock(&tempsense_lock, K_FOREVER);
	*out = tempsense;
	now = k_uptime_get();
	out->age_ms = (out->valid && last_sample_ms > 0) ?
		      (uint32_t)(now - last_sample_ms) : UINT32_MAX;
	k_mutex_unlock(&tempsense_lock);
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

static int tempsense_init_once(void)
{
	int rc = 0;

	if (tempsense_initialized) {
		return tempsense_dev == NULL ? -ENODEV : 0;
	}

	tempsense_dev = get_ds18b20_device();
	if (tempsense_dev == NULL) {
		rc = -ENODEV;
	}
	tempsense_update(0.0f, rc, false);
	tempsense_initialized = true;
	return rc;
}

static int tempsense_sample_once(void)
{
	struct sensor_value temp;
	float ambient_c;
	int rc;

	rc = tempsense_init_once();
	if (rc != 0) {
		return rc;
	}

	/* Refresh the Zephyr sensor sample before reading the temperature channel. */
	rc = sensor_sample_fetch(tempsense_dev);
	if (rc != 0) {
		LOG_ERR("DS18B20 sample fetch failed: %d", rc);
		tempsense_mark_error(rc);
		return rc;
	}

	/* SENSOR_CHAN_AMBIENT_TEMP returns Celsius in integer + micro unit parts. */
	rc = sensor_channel_get(tempsense_dev, SENSOR_CHAN_AMBIENT_TEMP, &temp);
	if (rc != 0) {
		LOG_ERR("DS18B20 channel read failed: %d", rc);
		tempsense_mark_error(rc);
		return rc;
	}

	ambient_c = sensor_value_to_double(&temp);
	tempsense_update(ambient_c, 0, true);
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
			(void)tempsense_sample_once();
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
