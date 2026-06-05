/**
 * @file housekeeping.c
 * @brief Slow background polling for ambient sensing and power policy.
 *
 * This module owns slow relay-box power state and ambient temperature sampling.
 * Ambient sampling runs as delayable work because it is cache refresh, not a
 * timing-critical loop.
 */

#include "housekeeping.h"

#include <errno.h>
#include <math.h>
#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/sensor.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/util.h>

#include "command.h"
#include "devices.h"

LOG_MODULE_REGISTER(housekeeping, LOG_LEVEL_INF);

#define HOUSEKEEPING_TEMP_INTERVAL_MS 1000U
#define HOUSEKEEPING_POWER_OUTPUT_COUNT (HOUSEKEEPING_POWER_BANK_HEATER + 1)

struct power_on_time_runtime {
	bool active;
	int64_t started_ms;
	int64_t accumulated_ms;
};

static K_MUTEX_DEFINE(housekeeping_state_lock);
static struct housekeeping_temperature_status temperature_status;
static int64_t last_sample_ms;
static const struct device *temperature_dev;
static bool temperature_initialized;
static struct power_on_time_runtime power_on_time[HOUSEKEEPING_POWER_OUTPUT_COUNT];

static void temperature_work_handler(struct k_work *work);
static K_WORK_DELAYABLE_DEFINE(temperature_work, temperature_work_handler);
static struct k_work_q *temperature_work_q;

static void temperature_update(double ambient_c, int error, bool valid)
{
	k_mutex_lock(&housekeeping_state_lock, K_FOREVER);
	temperature_status.ambient_c = ambient_c;
	temperature_status.last_error = error;
	temperature_status.valid = valid;
	last_sample_ms = valid ? k_uptime_get() : 0;
	temperature_status.age_ms = 0;
	k_mutex_unlock(&housekeeping_state_lock);
}

static void temperature_mark_error(int error)
{
	k_mutex_lock(&housekeeping_state_lock, K_FOREVER);
	temperature_status.last_error = error;
	temperature_status.valid = false;
	last_sample_ms = 0;
	temperature_status.age_ms = UINT32_MAX;
	k_mutex_unlock(&housekeeping_state_lock);
}

void housekeeping_get_temperature_status(struct housekeeping_temperature_status *out)
{
	int64_t now;

	if (out == NULL) {
		return;
	}

	k_mutex_lock(&housekeeping_state_lock, K_FOREVER);
	*out = temperature_status;
	now = k_uptime_get();
	out->age_ms = (out->valid && last_sample_ms > 0) ?
		      (uint32_t)(now - last_sample_ms) : UINT32_MAX;
	k_mutex_unlock(&housekeeping_state_lock);
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
	temperature_update(0.0, rc, false);
	temperature_initialized = true;
	return rc;
}

static int temperature_sample_once(void)
{
	struct sensor_value temp;
	double ambient_c;
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

static const struct gpio_dt_spec *power_gpio(enum housekeeping_power_output output)
{
	switch (output) {
	case HOUSEKEEPING_POWER_YJ_PHOTODIODE:
		return &yj_power_gpio;
	case HOUSEKEEPING_POWER_HK_PHOTODIODE:
		return &hk_power_gpio;
	case HOUSEKEEPING_POWER_BANK_HEATER:
		return &heater_power_gpio;
	default:
		return NULL;
	}
}

static bool power_output_is_photodiode(enum housekeeping_power_output output)
{
	return output == HOUSEKEEPING_POWER_YJ_PHOTODIODE ||
	       output == HOUSEKEEPING_POWER_HK_PHOTODIODE;
}

static void power_on_time_update_locked(enum housekeeping_power_output output,
					bool active)
{
	struct power_on_time_runtime *runtime;
	int64_t now = k_uptime_get();

	if (output < 0 || output >= HOUSEKEEPING_POWER_OUTPUT_COUNT) {
		return;
	}

	runtime = &power_on_time[output];
	if (active && !runtime->active) {
		runtime->active = true;
		runtime->started_ms = now;
		return;
	}
	if (!active && runtime->active) {
		runtime->accumulated_ms += now - runtime->started_ms;
		runtime->active = false;
		runtime->started_ms = 0;
	}
}

int housekeeping_power_set(enum housekeeping_power_output output, bool enabled)
{
	const struct gpio_dt_spec *gpio = power_gpio(output);
	int rc;

	if (gpio == NULL) {
		return -EINVAL;
	}
	if (!devices_relay_gpio_online()) {
		if (power_output_is_photodiode(output)) {
			coo_cmd_runtime_warning_emit(command_runtime_get(), "relay_gpio_offline",
				"photodiode relay command ignored because relay GPIO expander is offline",
				enabled ? "enable" : "disable");
			return 0;
		}
		return -EIO;
	}

	k_mutex_lock(&housekeeping_state_lock, K_FOREVER);
	/* Logical GPIO value; devicetree flags own DS2408 relay polarity. */
	rc = gpio_pin_set_dt(gpio, enabled ? 1 : 0);
	if (rc == 0) {
		power_on_time_update_locked(output, enabled);
	}
	k_mutex_unlock(&housekeeping_state_lock);
	return rc;
}

int housekeeping_power_get(enum housekeeping_power_output output, bool *enabled)
{
	const struct gpio_dt_spec *gpio = power_gpio(output);
	int rc = 0;
	int val;

	if (gpio == NULL || enabled == NULL) {
		return -EINVAL;
	}
	if (!devices_relay_gpio_online()) {
		return -EIO;
	}

	k_mutex_lock(&housekeeping_state_lock, K_FOREVER);
	val = gpio_pin_get_dt(gpio);
	if (val < 0) {
		rc = val;
		goto out;
	}
	*enabled = val > 0;

out:
	k_mutex_unlock(&housekeeping_state_lock);
	return rc;
}

double housekeeping_power_on_time_s(enum housekeeping_power_output output)
{
	struct power_on_time_runtime runtime;
	int64_t ms;

	if (output < 0 || output >= HOUSEKEEPING_POWER_OUTPUT_COUNT) {
		return NAN;
	}

	k_mutex_lock(&housekeeping_state_lock, K_FOREVER);
	runtime = power_on_time[output];
	k_mutex_unlock(&housekeeping_state_lock);

	ms = runtime.accumulated_ms;
	if (runtime.active) {
		ms += k_uptime_get() - runtime.started_ms;
	}
	return (double)ms / 1000.0;
}

static void temperature_work_handler(struct k_work *work)
{
	ARG_UNUSED(work);

	(void)temperature_sample_once();
	(void)k_work_reschedule_for_queue(temperature_work_q, &temperature_work,
					  K_MSEC(HOUSEKEEPING_TEMP_INTERVAL_MS));
}

void housekeeping_start(struct k_work_q *work_q)
{
	__ASSERT_NO_MSG(work_q != NULL);
	if (work_q == NULL) {
		return;
	}

	temperature_work_q = work_q;
	(void)k_work_reschedule_for_queue(temperature_work_q, &temperature_work, K_NO_WAIT);
}
