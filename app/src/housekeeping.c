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
#include "photodiode.h"

LOG_MODULE_REGISTER(housekeeping, LOG_LEVEL_INF);

#define HOUSEKEEPING_TEMP_INTERVAL_MS 1000U
#define HOUSEKEEPING_POWER_OUTPUT_COUNT (HOUSEKEEPING_POWER_BANK_HEATER + 1)
#define HOUSEKEEPING_PD_AUTOFF_NO_DEADLINE 0LL

struct power_on_time_runtime {
	bool active;
	int64_t started_ms;
};

static K_MUTEX_DEFINE(housekeeping_state_lock);
static struct housekeeping_temperature_status temperature_status;
static int64_t last_sample_ms;
static const struct device *temperature_dev;
static bool temperature_initialized;
static struct power_on_time_runtime power_on_time[HOUSEKEEPING_POWER_OUTPUT_COUNT];
static int64_t pd_autooff_deadline_ms[PHOTODIODE_CHANNEL_COUNT];
static bool pd_autooff_inhibited[PHOTODIODE_CHANNEL_COUNT];

static void temperature_work_handler(struct k_work *work);
static K_WORK_DELAYABLE_DEFINE(temperature_work, temperature_work_handler);
static void pd_autooff_work_handler(struct k_work *work);
static K_WORK_DELAYABLE_DEFINE(pd_autooff_work, pd_autooff_work_handler);
static struct k_work_q *housekeeping_work_q;

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

static bool power_output_to_pd_index(enum housekeeping_power_output output,
				     uint8_t *index)
{
	if (!power_output_is_photodiode(output) || index == NULL) {
		return false;
	}

	*index = (uint8_t)output;
	return true;
}

static int power_get_locked(enum housekeeping_power_output output, bool *enabled)
{
	const struct gpio_dt_spec *gpio = power_gpio(output);
	int val;

	if (gpio == NULL || enabled == NULL) {
		return -EINVAL;
	}
	if (!devices_relay_gpio_online()) {
		return -EIO;
	}

	val = gpio_pin_get_dt(gpio);
	if (val < 0) {
		return val;
	}

	*enabled = val > 0;
	return 0;
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
		runtime->active = false;
		runtime->started_ms = 0;
	}
}

static int power_set_locked(enum housekeeping_power_output output, bool enabled)
{
	const struct gpio_dt_spec *gpio = power_gpio(output);
	int rc;

	if (gpio == NULL) {
		return -EINVAL;
	}
	if (!devices_relay_gpio_online()) {
		if (power_output_is_photodiode(output)) {
			coo_cmd_runtime_emit(
				command_runtime_get(),
				&(const struct coo_cmd_runtime_emit_args){
					.type = COO_CMD_RUNTIME_EMIT_WARNING,
					.delivery = COO_CMD_RUNTIME_EMIT_BEST_EFFORT,
					.code = "relay_gpio_offline",
					.msg = "photodiode relay command ignored because relay GPIO expander is offline",
					.context = enabled ? "enable" : "disable",
				});
			return 0;
		}
		return -EIO;
	}

	/* Logical GPIO value; devicetree flags own DS2408 relay polarity. */
	rc = gpio_pin_set_dt(gpio, enabled ? 1 : 0);
	if (rc == 0) {
		power_on_time_update_locked(output, enabled);
	}
	return rc;
}

int housekeeping_power_set(enum housekeeping_power_output output, bool enabled)
{
	int rc;

	k_mutex_lock(&housekeeping_state_lock, K_FOREVER);
	rc = power_set_locked(output, enabled);
	k_mutex_unlock(&housekeeping_state_lock);
	return rc;
}

int housekeeping_power_get(enum housekeeping_power_output output, bool *enabled)
{
	int rc;

	k_mutex_lock(&housekeeping_state_lock, K_FOREVER);
	rc = power_get_locked(output, enabled);
	k_mutex_unlock(&housekeeping_state_lock);
	return rc;
}

double housekeeping_power_on_time_s(enum housekeeping_power_output output)
{
	struct power_on_time_runtime runtime;

	if (output < 0 || output >= HOUSEKEEPING_POWER_OUTPUT_COUNT) {
		return NAN;
	}

	k_mutex_lock(&housekeeping_state_lock, K_FOREVER);
	runtime = power_on_time[output];
	k_mutex_unlock(&housekeeping_state_lock);

	if (!runtime.active) {
		return 0.0;
	}

	return (double)(k_uptime_get() - runtime.started_ms) / 1000.0;
}

static int64_t pd_next_autooff_deadline_locked(void)
{
	int64_t next = HOUSEKEEPING_PD_AUTOFF_NO_DEADLINE;

	for (uint8_t i = 0U; i < PHOTODIODE_CHANNEL_COUNT; ++i) {
		const int64_t deadline = pd_autooff_deadline_ms[i];

		if (deadline == HOUSEKEEPING_PD_AUTOFF_NO_DEADLINE ||
		    pd_autooff_inhibited[i]) {
			continue;
		}
		if (next == HOUSEKEEPING_PD_AUTOFF_NO_DEADLINE || deadline < next) {
			next = deadline;
		}
	}

	return next;
}

static void pd_autooff_reschedule_locked(void)
{
	int64_t next_deadline;
	int64_t delay_ms;

	if (housekeeping_work_q == NULL) {
		return;
	}

	next_deadline = pd_next_autooff_deadline_locked();
	if (next_deadline == HOUSEKEEPING_PD_AUTOFF_NO_DEADLINE) {
		(void)k_work_cancel_delayable(&pd_autooff_work);
		return;
	}

	delay_ms = next_deadline - k_uptime_get();
	if (delay_ms < 0) {
		delay_ms = 0;
	}
	(void)k_work_reschedule_for_queue(housekeeping_work_q, &pd_autooff_work,
					  K_MSEC(delay_ms));
}

int housekeeping_photodiode_auto_enable(enum housekeeping_power_output output,
					uint32_t autooff_s,
					bool *was_off)
{
	uint8_t index;
	bool enabled = false;
	int64_t now;
	int rc;

	if (!power_output_to_pd_index(output, &index)) {
		return -EINVAL;
	}

	k_mutex_lock(&housekeeping_state_lock, K_FOREVER);
	rc = power_get_locked(output, &enabled);
	if (rc == 0 && !enabled) {
		rc = power_set_locked(output, true);
	}
	if (rc == 0) {
		now = k_uptime_get();
		pd_autooff_deadline_ms[index] =
			autooff_s == 0U ? HOUSEKEEPING_PD_AUTOFF_NO_DEADLINE :
			now + (int64_t)autooff_s * 1000LL;
		pd_autooff_reschedule_locked();
	}
	k_mutex_unlock(&housekeeping_state_lock);

	if (was_off != NULL) {
		*was_off = rc == 0 && !enabled;
	}
	return rc;
}

void housekeeping_photodiode_autooff_cancel(enum housekeeping_power_output output)
{
	uint8_t index;

	if (!power_output_to_pd_index(output, &index)) {
		return;
	}

	k_mutex_lock(&housekeeping_state_lock, K_FOREVER);
	pd_autooff_deadline_ms[index] = HOUSEKEEPING_PD_AUTOFF_NO_DEADLINE;
	pd_autooff_reschedule_locked();
	k_mutex_unlock(&housekeeping_state_lock);
}

void housekeeping_photodiode_autooff_inhibit(enum housekeeping_power_output output,
					     bool inhibited)
{
	uint8_t index;

	if (!power_output_to_pd_index(output, &index)) {
		return;
	}

	k_mutex_lock(&housekeeping_state_lock, K_FOREVER);
	pd_autooff_inhibited[index] = inhibited;
	pd_autooff_reschedule_locked();
	k_mutex_unlock(&housekeeping_state_lock);
}

int64_t housekeeping_photodiode_autooff_remaining_s(enum housekeeping_power_output output)
{
	uint8_t index;
	int64_t deadline;
	int64_t remaining_ms;

	if (!power_output_to_pd_index(output, &index)) {
		return -1;
	}

	k_mutex_lock(&housekeeping_state_lock, K_FOREVER);
	deadline = pd_autooff_inhibited[index] ?
		   HOUSEKEEPING_PD_AUTOFF_NO_DEADLINE :
		   pd_autooff_deadline_ms[index];
	k_mutex_unlock(&housekeeping_state_lock);

	if (deadline == HOUSEKEEPING_PD_AUTOFF_NO_DEADLINE) {
		return -1;
	}

	remaining_ms = deadline - k_uptime_get();
	if (remaining_ms <= 0) {
		return 0;
	}
	return (remaining_ms + 999LL) / 1000LL;
}

static void temperature_work_handler(struct k_work *work)
{
	ARG_UNUSED(work);

	(void)temperature_sample_once();
	(void)k_work_reschedule_for_queue(housekeeping_work_q, &temperature_work,
					  K_MSEC(HOUSEKEEPING_TEMP_INTERVAL_MS));
}

static void pd_autooff_work_handler(struct k_work *work)
{
	int64_t now = k_uptime_get();

	ARG_UNUSED(work);

	k_mutex_lock(&housekeeping_state_lock, K_FOREVER);
	for (uint8_t i = 0U; i < PHOTODIODE_CHANNEL_COUNT; ++i) {
		int rc;

		if (pd_autooff_deadline_ms[i] == HOUSEKEEPING_PD_AUTOFF_NO_DEADLINE ||
		    pd_autooff_inhibited[i] ||
		    now < pd_autooff_deadline_ms[i]) {
			continue;
		}

		pd_autooff_deadline_ms[i] = HOUSEKEEPING_PD_AUTOFF_NO_DEADLINE;
		rc = power_set_locked((enum housekeeping_power_output)i, false);
		if (rc != 0) {
			LOG_WRN("Failed to auto-off photodiode relay %u (%d)", i, rc);
		}
	}
	pd_autooff_reschedule_locked();
	k_mutex_unlock(&housekeeping_state_lock);
}

void housekeeping_start(struct k_work_q *work_q)
{
	__ASSERT_NO_MSG(work_q != NULL);
	if (work_q == NULL) {
		return;
	}

	housekeeping_work_q = work_q;
	(void)k_work_reschedule_for_queue(housekeeping_work_q, &temperature_work, K_NO_WAIT);
}
