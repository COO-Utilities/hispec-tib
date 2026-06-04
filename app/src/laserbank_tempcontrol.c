/**
 * @file laserbank_tempcontrol.c
 * @brief Laser-bank temperature-control policy loop.
 *
 * The delayable work owns automatic bank pre-warm decisions for TIB. It never
 * publishes MQTT directly; warnings are best-effort messages queued through the
 * command runtime.
 */

#include "laserbank_tempcontrol.h"

#include <errno.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include "app_settings.h"
#include "command.h"
#include "devices.h"
#include "housekeeping.h"
#include "lasers.h"

LOG_MODULE_REGISTER(laserbank_tempcontrol, LOG_LEVEL_INF);

#define LASERBANK_WARM_MIN_C 15.0f
#define LASERBANK_COLD_OFF_C 20.0f

struct laserbank_cached_channel {
	bool valid;
	bool tec_enabled;
	float tec_temperature_c;
	int64_t last_valid_ms;
};

struct laserbank_tempcontrol_runtime {
	struct laserbank_cached_channel channel[HISPEC_LASER_COUNT];
	struct laserbank_tempcontrol_status status;
	int64_t last_poll_ms;
	int64_t all_tecs_enabled_since_ms;
	int64_t last_override_warning_ms;
	enum laserbank_heater_mode previous_mode;
	bool have_previous_mode;
};

static struct laserbank_tempcontrol_runtime control;
static K_MUTEX_DEFINE(control_lock);

static void tempcontrol_work_handler(struct k_work *work);
static K_WORK_DELAYABLE_DEFINE(tempcontrol_work, tempcontrol_work_handler);

const char *laserbank_heater_mode_name(enum laserbank_heater_mode mode)
{
	switch (mode) {
	case LASERBANK_HEATER_MODE_AUTO:
		return "auto";
	case LASERBANK_HEATER_MODE_OVERRIDE_ON:
		return "override_on";
	case LASERBANK_HEATER_MODE_OVERRIDE_OFF:
		return "override_off";
	default:
		return "unknown";
	}
}

static bool heater_mode_is_valid(enum laserbank_heater_mode mode)
{
	return mode == LASERBANK_HEATER_MODE_AUTO ||
	       mode == LASERBANK_HEATER_MODE_OVERRIDE_ON ||
	       mode == LASERBANK_HEATER_MODE_OVERRIDE_OFF;
}

static bool channel_is_stale(const struct laserbank_cached_channel *channel,
			     int64_t now_ms)
{
	return channel == NULL || !channel->valid ||
	       channel->last_valid_ms <= 0 ||
	       now_ms - channel->last_valid_ms > LASERBANK_TEMPCONTROL_TEMP_STALE_MS;
}

static void copy_status_locked(struct laserbank_tempcontrol_status *out)
{
	if (out == NULL) {
		return;
	}

	*out = control.status;
	if (control.last_poll_ms > 0) {
		out->last_poll_age_ms = (uint32_t)(k_uptime_get() - control.last_poll_ms);
	} else {
		out->last_poll_age_ms = UINT32_MAX;
	}
}

void laserbank_tempcontrol_get_status(struct laserbank_tempcontrol_status *out)
{
	k_mutex_lock(&control_lock, K_FOREVER);
	control.status.bank_powered = hispec_laser_bank_power_is_enabled();
	copy_status_locked(out);
	k_mutex_unlock(&control_lock);
}

int laserbank_tempcontrol_set_heater_mode(enum laserbank_heater_mode mode,
				      bool persist)
{
	struct app_laserbank_settings settings;

	if (!heater_mode_is_valid(mode)) {
		return -EINVAL;
	}
	if (!devices_relay_gpio_online()) {
		return -EIO;
	}

	app_settings_get_laserbank(&settings);
	settings.heater_mode = mode;
	app_settings_update_laserbank(&settings, persist);
	(void)k_work_reschedule(&tempcontrol_work, K_NO_WAIT);
	return 0;
}

static void summarize_temperature_state(const struct housekeeping_temperature_status *ambient,
					int64_t now_ms)
{
	bool all_enabled = true;
	uint8_t valid_count = 0U;
	uint8_t stale_count = 0U;
	float off_threshold = LASERBANK_COLD_OFF_C;

	control.status.any_disabled_below_15c = false;
	control.status.any_disabled_above_off_threshold = false;

	if (ambient != NULL && ambient->valid && ambient->ambient_c > LASERBANK_WARM_MIN_C) {
		off_threshold = LASERBANK_WARM_MIN_C;
	}

	for (uint8_t i = 0U; i < HISPEC_LASER_COUNT; ++i) {
		struct laserbank_cached_channel *channel = &control.channel[i];

		if (channel_is_stale(channel, now_ms)) {
			stale_count++;
			all_enabled = false;
			continue;
		}

		valid_count++;
		if (!channel->tec_enabled) {
			all_enabled = false;
			if (channel->tec_temperature_c < LASERBANK_WARM_MIN_C) {
				control.status.any_disabled_below_15c = true;
			}
			if (channel->tec_temperature_c > off_threshold) {
				control.status.any_disabled_above_off_threshold = true;
			}
		}
	}

	if (all_enabled && valid_count == HISPEC_LASER_COUNT) {
		if (control.all_tecs_enabled_since_ms <= 0) {
			control.all_tecs_enabled_since_ms = now_ms;
		}
		control.status.all_tecs_enabled = true;
		control.status.all_tecs_enabled_ms =
			(uint32_t)(now_ms - control.all_tecs_enabled_since_ms);
	} else {
		control.all_tecs_enabled_since_ms = 0;
		control.status.all_tecs_enabled = false;
		control.status.all_tecs_enabled_ms = 0U;
	}

	control.status.valid_temp_count = valid_count;
	control.status.stale_temp_count = stale_count;
}

static void maybe_emit_override_warning(enum laserbank_heater_mode mode,
					int64_t now_ms)
{
	if (mode == LASERBANK_HEATER_MODE_AUTO) {
		return;
	}

	if (control.last_override_warning_ms > 0 &&
	    now_ms - control.last_override_warning_ms <
		    LASERBANK_TEMPCONTROL_OVERRIDE_WARNING_MS) {
		return;
	}

	coo_cmd_runtime_warning_emit(command_runtime_get(), "laserbank_heater_override",
			 "laser bank heater override is active; automatic warmup is disabled",
			 laserbank_heater_mode_name(mode));
	control.last_override_warning_ms = now_ms;
}

static void apply_heater(bool enable)
{
	int rc;

	rc = housekeeping_power_set(HOUSEKEEPING_POWER_BANK_HEATER, enable);
	if (rc != 0) {
		k_mutex_lock(&control_lock, K_FOREVER);
		control.status.last_error = rc;
		k_mutex_unlock(&control_lock);
		LOG_WRN("Failed to set laser bank heater %s (%d)",
			enable ? "on" : "off", rc);
		return;
	}

	k_mutex_lock(&control_lock, K_FOREVER);
	control.status.heater_on = enable;
	k_mutex_unlock(&control_lock);
}

static void run_heater_control_cycle(void)
{
	struct app_laserbank_settings settings;
	struct hispec_laser_channel_temperature poll[HISPEC_LASER_COUNT] = {0};
	struct housekeeping_temperature_status ambient = {0};
	bool entered_auto;
	bool all_stale;
	int64_t now_ms = k_uptime_get();
	int rc;

	app_settings_get_laserbank(&settings);
	housekeeping_get_temperature_status(&ambient);

	k_mutex_lock(&control_lock, K_FOREVER);
	control.status.available = true;
	control.status.heater_mode = settings.heater_mode;
	control.status.bank_powered = hispec_laser_bank_power_is_enabled();
	control.status.ambient_valid = ambient.valid;
	control.status.ambient_c = ambient.ambient_c;
	control.status.ambient_age_ms = ambient.age_ms;
	control.status.last_error = 0;
	entered_auto = settings.heater_mode == LASERBANK_HEATER_MODE_AUTO &&
		       (!control.have_previous_mode ||
			control.previous_mode != LASERBANK_HEATER_MODE_AUTO);
	control.previous_mode = settings.heater_mode;
	control.have_previous_mode = true;
	k_mutex_unlock(&control_lock);

	if (settings.heater_mode != LASERBANK_HEATER_MODE_AUTO) {
		bool force_on = settings.heater_mode == LASERBANK_HEATER_MODE_OVERRIDE_ON;

		apply_heater(force_on);
		k_mutex_lock(&control_lock, K_FOREVER);
		maybe_emit_override_warning(settings.heater_mode, now_ms);
		k_mutex_unlock(&control_lock);
		return;
	}

	if (entered_auto && !hispec_laser_bank_power_is_enabled()) {
		bool transitioned;

		rc = hispec_laser_bank_power_set(true, &transitioned);
		if (rc != 0) {
			k_mutex_lock(&control_lock, K_FOREVER);
			control.status.last_error = rc;
			k_mutex_unlock(&control_lock);
			return;
		}
	}

	rc = hispec_laser_bank_read_temperatures(poll);
	now_ms = k_uptime_get();

	k_mutex_lock(&control_lock, K_FOREVER);
	control.last_poll_ms = now_ms;
	control.status.bank_powered = hispec_laser_bank_power_is_enabled();
	if (rc == 0) {
		for (uint8_t i = 0U; i < HISPEC_LASER_COUNT; ++i) {
			if (!poll[i].valid) {
				continue;
			}

			control.channel[i].valid = true;
			control.channel[i].tec_enabled = poll[i].tec_enabled;
			control.channel[i].tec_temperature_c = poll[i].tec_temperature_c;
			control.channel[i].last_valid_ms = now_ms;
		}
	} else {
		control.status.last_error = rc;
	}
	(void)housekeeping_power_get(HOUSEKEEPING_POWER_BANK_HEATER,
				     &control.status.heater_on);

	summarize_temperature_state(&ambient, now_ms);
	all_stale = control.status.stale_temp_count == HISPEC_LASER_COUNT;
	k_mutex_unlock(&control_lock);

	if (all_stale) {
		if (ambient.valid && ambient.ambient_c < LASERBANK_WARM_MIN_C) {
			(void)hispec_laser_bank_power_set(true, NULL);
		} else {
			apply_heater(false);
		}
		return;
	}

	k_mutex_lock(&control_lock, K_FOREVER);
	if (control.status.any_disabled_below_15c) {
		k_mutex_unlock(&control_lock);
		apply_heater(true);
	} else if (control.status.any_disabled_above_off_threshold ||
		   (control.status.all_tecs_enabled &&
		    control.status.all_tecs_enabled_ms >=
			    LASERBANK_TEMPCONTROL_POLL_INTERVAL_MS)) {
		k_mutex_unlock(&control_lock);
		apply_heater(false);
	} else {
		k_mutex_unlock(&control_lock);
	}
}

static void tempcontrol_work_handler(struct k_work *work)
{
	ARG_UNUSED(work);

	run_heater_control_cycle();
	(void)k_work_reschedule(&tempcontrol_work,
				 K_MSEC(LASERBANK_TEMPCONTROL_POLL_INTERVAL_MS));
}

void laserbank_tempcontrol_start(void)
{
	(void)k_work_reschedule(&tempcontrol_work, K_NO_WAIT);
}
