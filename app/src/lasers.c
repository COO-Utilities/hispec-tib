/**
 * @file lasers.c
 * @brief Laser-bank power, Maiman status, and tuning helpers.
 *
 * A module mutex serializes shared RS-485/GPIO operations. Functions can block
 * on Modbus RTU, sleep for bank boot/fault-clear delays, and modify driver
 * state owned by the Maiman modules.
 *
 * Copyright (c) 2026 Caltech Optical Observatories
 * SPDX-License-Identifier: Apache-2.0
 */

#include "lasers.h"

#include "app_settings.h"
#include "command.h"
#include "devices.h"

#include <errno.h>
#include <math.h>
#include <stddef.h>
#include <string.h>

#include <zephyr/drivers/gpio.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/util.h>

LOG_MODULE_REGISTER(lasers, CONFIG_LASERS_LOG_LEVEL);

#define PLANCK_J_S 6.62607015e-34
#define LIGHT_M_PER_S 299792458.0

#define LASER_AUTOFF_NO_DEADLINE 0LL
#define LASER_COMMAND_LOCK_TIMEOUT_MS 250U
#define LASER_RESPONSE_CHECK_MS 1000U
#define LASER_RESPONSE_TIMEOUT_MS 5000U
#define LASER_COMM_WARNING_MS 5000U

/* Hardware writers serialize RS-485 sequences and bank GPIO under io_lock.
 * Confirmed state is published under state_lock, never held during I/O, sleeps,
 * persistence, or telemetry. Readers only take state_lock and keep seeing the
 * previous confirmed state while an operation is in flight. Lock order is
 * io_lock -> state_lock for hardware/settings publication. Deadline expiry
 * changes only operational health under state_lock.
 */
static K_MUTEX_DEFINE(laser_io_lock);
static K_MUTEX_DEFINE(laser_state_lock);

struct on_time_runtime {
	bool active;
	int64_t started_ms;
	int64_t accumulated_ms;
};

struct laser_output_estimate_state {
	double current_ma;
	double tec_temperature_c;
	bool valid; /* Control operation / observed controller condition, not read freshness. */
	int64_t response_deadline_ms;
	int64_t next_warning_ms;
	bool communication_fault;
	/* Identity and successfully written configuration survive STOP and transport
	 * errors. Only a bank power cycle or explicit configuration change resets them. */
	bool prepared;
	uint16_t device_id;
	uint16_t serial;
	bool started; /* Separate from nonzero-current emission accounting. */
};

// TODO remove all "hispec_" fromm this file, identify and carry out consolidation with overlapping non underscore names

static struct on_time_runtime laser_current_runtime[HISPEC_LASER_COUNT];
static struct on_time_runtime laser_tec_runtime[HISPEC_LASER_COUNT];
static struct app_laser_channel_settings laser_settings[HISPEC_LASER_COUNT];
static struct laser_output_estimate_state laser_output_estimate[HISPEC_LASER_COUNT];
static int64_t laser_autooff_deadline_ms[HISPEC_LASER_COUNT];
static struct k_work_q *laser_autooff_work_q;
static int64_t bank_power_started_ms;
static bool bank_power_requested_enabled;
static bool laser_runtime_initialized;
static enum hispec_laser_bank_power_mode bank_power_mode = HISPEC_LASER_BANK_POWER_AUTO; // HISPEC_LASER_BANK_POWER_OVERRIDE_OFF;

static const struct hispec_laser_driver_profile laser_profiles[] = {
	[HISPEC_LASER_1028_Y] = {
		.id = HISPEC_LASER_1028_Y,
		.name = "1028y",
		.modbus_name = "1028",
		.node_id = 5U,
		.expected_device_id = 0x1113,
		.properties = &LASER_1028,
	},
	[HISPEC_LASER_1270_J] = {
		.id = HISPEC_LASER_1270_J,
		.name = "1270j",
		.modbus_name = "1270",
		.node_id = 6U,
		.expected_device_id = 0x1113,
		.properties = &LASER_1270,
	},
	[HISPEC_LASER_1430_YJ] = {
		.id = HISPEC_LASER_1430_YJ,
		.name = "1430yj",
		.modbus_name = "yj1430",
		.node_id = 4U,
		.expected_device_id = 0x1113,
		.properties = &LASER_1430,
	},
	[HISPEC_LASER_1430_HK] = {
		.id = HISPEC_LASER_1430_HK,
		.name = "1430hk",
		.modbus_name = "hk1430",
		.node_id = 3U,
		.expected_device_id = 0x1113,
		.properties = &LASER_1430,
	},
	[HISPEC_LASER_1510_H] = {
		.id = HISPEC_LASER_1510_H,
		.name = "1510h",
		.modbus_name = "1510",
		.node_id = 1U,
		.expected_device_id = 0x1113,
		.properties = &LASER_1510,
	},
	[HISPEC_LASER_2330_K] = {
		.id = HISPEC_LASER_2330_K,
		.name = "2330k",
		.modbus_name = "2330",
		.node_id = 2U,
		.expected_device_id = 0x1113,
		.properties = &LASER_2330,
	},
};

BUILD_ASSERT(ARRAY_SIZE(laser_profiles) == HISPEC_LASER_COUNT,
	     "Laser profile table must match hispec_laser_id");

static int stop_output_locked(const struct hispec_laser_driver_profile *profile, bool stop_tec);
static int profile_for_id(enum hispec_laser_id id,
			  const struct hispec_laser_driver_profile **profile);
static int64_t next_autooff_deadline_locked(void);
static void hispec_laser_service_autooff(void);
static void laser_autooff_reschedule(void);
static void laser_autooff_work_handler(struct k_work *work);

static K_WORK_DELAYABLE_DEFINE(laser_autooff_work, laser_autooff_work_handler);

static void ensure_laser_runtime_settings_locked(void)
{
	struct app_laser_settings stored = {0};

	if (laser_runtime_initialized) {
		return;
	}

	/* app_settings_init() has loaded defaults/persistence. The existing autooff
	 * boot entry initializes the owner before worker threads start. Hardware
	 * writers also call here under io_lock; state-only getters never initialize.
	 */
	app_settings_get_laser(&stored);
	k_mutex_lock(&laser_state_lock, K_FOREVER);
	for (uint8_t i = 0U; i < HISPEC_LASER_COUNT; ++i) {
		laser_settings[i] = stored.channel[i];
		laser_output_estimate[i].current_ma = 0.0;
		laser_output_estimate[i].tec_temperature_c =
			stored.channel[i].properties.operating_temp_c;
		laser_output_estimate[i].valid = true;
		laser_autooff_deadline_ms[i] = LASER_AUTOFF_NO_DEADLINE;
	}

	laser_runtime_initialized = true;
	k_mutex_unlock(&laser_state_lock);
}

static void output_estimate_set_locked(enum hispec_laser_id id,
				       double current_ma,
				       double tec_temperature_c)
{
	if (id < 0 || id >= HISPEC_LASER_COUNT) {
		return;
	}

	k_mutex_lock(&laser_state_lock, K_FOREVER);
	laser_output_estimate[id].current_ma = current_ma;
	laser_output_estimate[id].tec_temperature_c = tec_temperature_c;
	laser_output_estimate[id].valid = true;
	/* This helper publishes a completed control operation. Install its grace
	 * atomically with emission state so a reader cannot see new current with
	 * an old/zero response deadline before transport accounting runs.
	 */
	if (current_ma > 0.0 || laser_output_estimate[id].started) laser_output_estimate[id].response_deadline_ms =
		k_uptime_get() + LASER_RESPONSE_TIMEOUT_MS;
	k_mutex_unlock(&laser_state_lock);
}

/* A failed operation leaves physical emission unknown, not necessarily off.
 * Keep its runtime/shutdown obligation; only a successful control operation
 * restores usable control state. Numerical estimates retain confirmed setpoints.
 * Caller holds laser_io_lock.
 */
static void laser_health_warning(enum hispec_laser_id id, const char *code,
                                 const char *message, int error);

static void invalidate_output_locked(enum hispec_laser_id id)
{
	k_mutex_lock(&laser_state_lock, K_FOREVER);
	bool newly_faulted = laser_output_estimate[id].valid;
	laser_output_estimate[id].valid = false;
	k_mutex_unlock(&laser_state_lock);
	if (newly_faulted) laser_health_warning(id, "laser_output_fault", "laser control operation or controller condition faulted", -EIO);
}

/* Queue owner-level diagnostics independently of maiman transaction verbosity.
 * Never called with state_lock held. The existing emitter logs and queues MQTT.
 */
static void laser_health_warning(enum hispec_laser_id id, const char *code,
                                 const char *message, int error)
{
	char context[96];
	snprintk(context, sizeof(context), "laser=%s node=%u rc=%d",
		laser_profiles[id].name, laser_profiles[id].node_id, error);
	coo_cmd_runtime_emit(command_runtime_get(), &(struct coo_cmd_runtime_emit_args){
		.type = COO_CMD_RUNTIME_EMIT_WARNING, .delivery = COO_CMD_RUNTIME_EMIT_BEST_EFFORT,
		.code = code, .msg = message, .context = context,
	});
}

/* Caller holds io_lock. A partial multi-register read may contain both a real
 * response and an error: retain the response time but report the failed read.
 * Local validation and GPIO activity provide no evidence of a driver response.
 */
static void laser_note_communication_locked(enum hispec_laser_id id,
                                           const maiman_driver_t *drv)
{
	bool recovered = false, warn = false;
	int64_t now = k_uptime_get();
	k_mutex_lock(&laser_state_lock, K_FOREVER);
	struct laser_output_estimate_state *state = &laser_output_estimate[id];
	if (drv->last_response_ms > 0) {
		state->response_deadline_ms = MAX(state->response_deadline_ms,
			drv->last_response_ms + LASER_RESPONSE_TIMEOUT_MS);
		recovered = state->communication_fault;
		state->communication_fault = false;
	}
	if (drv->io_failed && now >= state->next_warning_ms) {
		warn = true;
		state->next_warning_ms = now + LASER_COMM_WARNING_MS;
	} else if (!drv->io_failed && drv->last_response_ms > 0) {
		state->next_warning_ms = 0;
	}
	k_mutex_unlock(&laser_state_lock);
	if (warn) laser_health_warning(id, "laser_communication", "laser operation communication failed", drv->last_error);
	if (recovered) laser_health_warning(id, "laser_communication_recovered", "laser communication restored; measurements remain stopped", 0);
}

/* Health is separate from the numerical estimator. No I/O, initialization or
 * telemetry here; elapsed time still detects loss if background work is late.
 */
int hispec_laser_output_status(enum hispec_laser_id id, bool *emitting)
{
	if (id < 0 || id >= HISPEC_LASER_COUNT || emitting == NULL) return -EINVAL;
	k_mutex_lock(&laser_state_lock, K_FOREVER);
	const struct laser_output_estimate_state *state = &laser_output_estimate[id];
	*emitting = bank_power_requested_enabled && laser_current_runtime[id].active;
	int rc = !laser_runtime_initialized ? -EINVAL :
		(state->started && k_uptime_get() >= state->response_deadline_ms ? -ETIMEDOUT :
		 (!state->valid ? -EIO : 0));
	k_mutex_unlock(&laser_state_lock);
	return rc;
}

static const laserprops_t *runtime_props_locked(enum hispec_laser_id id)
{
	ensure_laser_runtime_settings_locked();
	if (id < 0 || id >= HISPEC_LASER_COUNT) {
		return NULL;
	}
	return &laser_settings[id].properties;
}

static bool float_is_valid(double value)
{
	return isfinite(value);
}

static bool float_is_positive(double value)
{
	return float_is_valid(value) && value > 0.0;
}

static bool float_is_nonzero(double value)
{
	return float_is_valid(value) && value != 0.0;
}

double hispec_laser_quantize_current_ma(double current_ma, double min_ma, double max_ma)
{
	if (!isfinite(current_ma) || !isfinite(min_ma) || !isfinite(max_ma) ||
	    min_ma < 0.0 || max_ma > UINT16_MAX / DIVIDER_CURRENT || min_ma > max_ma) {
		return NAN;
	}
	/* Bounds round inward; the request uses Maiman's nearest-step rounding.
	 * nextafter removes a single floating-point rounding error at a register
	 * boundary (for example, a computed threshold + one current step).
	 */
	double low = ceil(nextafter(min_ma * DIVIDER_CURRENT, -INFINITY));
	double high = floor(nextafter(max_ma * DIVIDER_CURRENT, INFINITY));
	if (low > high) {
		return NAN;
	}
	return CLAMP(floor(current_ma * DIVIDER_CURRENT + 0.5), low, high) / DIVIDER_CURRENT;
}

/* Keep wait policy visible at call sites; this helper only maps Zephyr mutex
 * timeout return codes to the domain -EBUSY response expected by commands.
 */
static int laser_io_lock_with_timeout(k_timeout_t timeout)
{
	int rc = k_mutex_lock(&laser_io_lock, timeout);

	return rc == 0 ? 0 : -EBUSY;
}

static void on_time_runtime_update_locked(struct on_time_runtime *runtimes,
					  size_t count,
					  int index,
					  bool active)
{
	struct on_time_runtime *runtime;
	int64_t now = k_uptime_get();

	if (index < 0 || (size_t)index >= count) {
		return;
	}

	k_mutex_lock(&laser_state_lock, K_FOREVER);
	runtime = &runtimes[index];
	if (active && !runtime->active) {
		runtime->active = true;
		runtime->started_ms = now;
	} else if (!active && runtime->active) {
		runtime->accumulated_ms += now - runtime->started_ms;
		runtime->active = false;
		runtime->started_ms = 0;
	}
	k_mutex_unlock(&laser_state_lock);
}

static double on_time_runtime_seconds_locked(const struct on_time_runtime *runtimes,
					    size_t count,
					    int index)
{
	const struct on_time_runtime *runtime;
	int64_t ms;

	if (index < 0 || (size_t)index >= count) {
		return LASERPROP_NA;
	}

	runtime = &runtimes[index];
	ms = runtime->accumulated_ms;
	if (runtime->active) {
		ms += k_uptime_get() - runtime->started_ms;
	}

	return (double)ms / 1000.0;
}

/* A confirmed stop publishes zero emission and accounts elapsed time before
 * any NVS write. The caller still holds io_lock, never state_lock, during flash.
 */
static void commit_current_runtime_locked(enum hispec_laser_id id, bool persist)
{
	double total;

	if (id < 0 || id >= HISPEC_LASER_COUNT) {
		return;
	}

	k_mutex_lock(&laser_state_lock, K_FOREVER);
	total = laser_settings[id].total_emitting_s +
		(double)on_time_runtime_seconds_locked(laser_current_runtime,
						       ARRAY_SIZE(laser_current_runtime),
						       id);
	laser_settings[id].total_emitting_s = total;
	laser_output_estimate[id].current_ma = 0.0;
	laser_output_estimate[id].valid = true;
	laser_output_estimate[id].started = false;
	laser_autooff_deadline_ms[id] = LASER_AUTOFF_NO_DEADLINE;
	laser_current_runtime[id].active = false;
	laser_current_runtime[id].started_ms = 0;
	laser_current_runtime[id].accumulated_ms = 0;
	k_mutex_unlock(&laser_state_lock);
	(void)app_settings_update_laser_total_emitting((uint8_t)id, total, persist);
}

static double clamp_with_flag(double value, double min_value, double max_value, bool *clamped)
{
	double out = value;

	if (out < min_value) {
		out = min_value;
		if (clamped != NULL) {
			*clamped = true;
		}
	}
	if (out > max_value) {
		out = max_value;
		if (clamped != NULL) {
			*clamped = true;
		}
	}
	return out;
}

static bool names_match(const char *a, const char *b)
{
	if (a == NULL || b == NULL) {
		return false;
	}

	while (*a != '\0' && *b != '\0') {
		char ca = *a;
		char cb = *b;

		if (ca >= 'A' && ca <= 'Z') {
			ca = (char)(ca - 'A' + 'a');
		}
		if (cb >= 'A' && cb <= 'Z') {
			cb = (char)(cb - 'A' + 'a');
		}
		if (ca != cb) {
			return false;
		}
		a++;
		b++;
	}

	return *a == '\0' && *b == '\0';
}

static int profile_for_id(enum hispec_laser_id id,
			  const struct hispec_laser_driver_profile **profile)
{
	if (profile == NULL || id < 0 || id >= HISPEC_LASER_COUNT) {
		return -EINVAL;
	}

	*profile = &laser_profiles[id];
	return 0;
}

int hispec_laser_id_from_name(const char *name, enum hispec_laser_id *out)
{
	if (name == NULL || out == NULL) {
		return -EINVAL;
	}

	for (uint8_t i = 0U; i < ARRAY_SIZE(laser_profiles); ++i) {
		const struct hispec_laser_driver_profile *profile = &laser_profiles[i];

		if (names_match(name, profile->name) ||
		    names_match(name, profile->modbus_name) ||
		    names_match(name, profile->properties->name)) {
			*out = profile->id;
			return 0;
		}
	}

	*out = HISPEC_LASER_UNKNOWN;
	return -EINVAL;
}

const char *hispec_laser_name(enum hispec_laser_id id)
{
	const struct hispec_laser_driver_profile *profile;

	if (profile_for_id(id, &profile) != 0) {
		return "unknown";
	}
	return profile->name;
}

int hispec_laser_get_driver_profile(enum hispec_laser_id id,
				    const struct hispec_laser_driver_profile **out)
{
	return profile_for_id(id, out);
}

int hispec_laser_make_driver(enum hispec_laser_id id, maiman_driver_t *drv)
{
	const struct hispec_laser_driver_profile *profile;
	int rc;

	if (drv == NULL) {
		return -EINVAL;
	}

	rc = profile_for_id(id, &profile);
	if (rc != 0) {
		return rc;
	}

	maiman_init(drv, profile->node_id);
	return 0;
}

static int zero_all_driver_currents_locked(bool stop_tecs)
{
	int first_rc = 0;

	for (uint8_t i = 0U; i < HISPEC_LASER_COUNT; ++i) {
		int rc = stop_output_locked(&laser_profiles[i], stop_tecs);

		if (first_rc == 0) {
			first_rc = rc;
		}
	}
	return first_rc;
}

bool hispec_laser_bank_power_is_enabled(void)
{
	bool enabled;

	k_mutex_lock(&laser_state_lock, K_FOREVER);
	enabled = bank_power_requested_enabled;
	k_mutex_unlock(&laser_state_lock);

	return enabled;
}

enum hispec_laser_bank_power_mode hispec_laser_bank_power_mode_get(void)
{
	enum hispec_laser_bank_power_mode mode;

	k_mutex_lock(&laser_state_lock, K_FOREVER);
	mode = bank_power_mode;
	k_mutex_unlock(&laser_state_lock);
	return mode;
}

const char *hispec_laser_bank_power_mode_name(enum hispec_laser_bank_power_mode mode)
{
	switch (mode) {
	case HISPEC_LASER_BANK_POWER_AUTO:
		return "auto";
	case HISPEC_LASER_BANK_POWER_OVERRIDE_ON:
		return "override_on";
	case HISPEC_LASER_BANK_POWER_OVERRIDE_OFF:
		return "override_off";
	default:
		return "unknown";
	}
}

static int bank_power_set_locked(bool enabled, bool *transitioned, bool force_write)
{
	bool was_enabled;
	int rc;
	int zero_rc = 0;

	if (transitioned != NULL) {
		*transitioned = false;
	}

	was_enabled = bank_power_requested_enabled;
	if (was_enabled == enabled && !force_write) {
		return 0;
	}

	if (!enabled && was_enabled) {
		zero_rc = zero_all_driver_currents_locked(true);
		if (zero_rc != 0) {
			LOG_WRN("Laser bank power-off driver cleanup failed rc=%d; continuing GPIO off",
				zero_rc);
		}
	}

	/* gpio_pin_set_dt() writes the logical active state described by
	 * laser_power_gpios in devicetree.
	 */
	rc = gpio_pin_set_dt(&laser_power_gpio, enabled ? 1 : 0);
	if (rc != 0) {
		return rc;
	}
	/* Reasserting a GPIO level is not a bank power transition. */
	if (was_enabled == enabled) {
		return 0;
	}
	ensure_laser_runtime_settings_locked();
	k_mutex_lock(&laser_state_lock, K_FOREVER);
	for (uint8_t i = 0U; i < HISPEC_LASER_COUNT; ++i) {
		laser_output_estimate[i].prepared = false;
		laser_output_estimate[i].device_id = 0;
		laser_output_estimate[i].serial = 0;
		laser_output_estimate[i].started = false;
		laser_output_estimate[i].valid = !enabled;
		if (!enabled) {
			output_estimate_set_locked((enum hispec_laser_id)i, 0.0,
				laser_output_estimate[i].tec_temperature_c);
			laser_autooff_deadline_ms[i] = LASER_AUTOFF_NO_DEADLINE;
		}
	}
	bank_power_requested_enabled = enabled;
	bank_power_started_ms = enabled ? k_uptime_get() : 0;
	k_mutex_unlock(&laser_state_lock);
	LOG_INF("Laser bank power %s; identity/configuration invalidated; GPIO %s",
		enabled ? "turned on" : "turned off",
		enabled ? "released" : "sinking low");

	if (transitioned != NULL) {
		*transitioned = was_enabled != enabled;
	}
	if (enabled && !was_enabled) {
		/* Let the Maiman modules boot before the caller starts Modbus IO. */
		k_msleep(HISPEC_LASER_BANK_BOOT_DELAY_MS);
	} else if (!enabled && was_enabled) {
		for (uint8_t i = 0U; i < HISPEC_LASER_COUNT; ++i) {
			commit_current_runtime_locked((enum hispec_laser_id)i, true);
			on_time_runtime_update_locked(laser_tec_runtime,
						      ARRAY_SIZE(laser_tec_runtime),
						      (enum hispec_laser_id)i, false);
		}
	}

	return 0;
}

int hispec_laser_bank_power_set(bool enabled, bool *transitioned)
{
	int rc;

	k_mutex_lock(&laser_io_lock, K_FOREVER);
	if (enabled && bank_power_mode == HISPEC_LASER_BANK_POWER_OVERRIDE_OFF) {
		rc = -EPERM;
	} else {
		rc = bank_power_set_locked(enabled, transitioned, false);
	}
	k_mutex_unlock(&laser_io_lock);
	if (rc == 0 && !enabled) {
		laser_autooff_reschedule();
	}

	return rc;
}

uint32_t hispec_laser_bank_power_on_duration_s(void)
{
	uint32_t duration_s = 0U;
	int64_t now_ms;

	k_mutex_lock(&laser_state_lock, K_FOREVER);
	now_ms = k_uptime_get();
	if (bank_power_requested_enabled && bank_power_started_ms > 0) {
		int64_t elapsed_s = (now_ms - bank_power_started_ms) / 1000;

		duration_s = elapsed_s > UINT32_MAX ? UINT32_MAX : (uint32_t)elapsed_s;
	}
	k_mutex_unlock(&laser_state_lock);

	return duration_s;
}

int hispec_laser_bank_power_mode_set(enum hispec_laser_bank_power_mode mode)
{
	int rc = 0;

	if (mode < HISPEC_LASER_BANK_POWER_AUTO ||
	    mode > HISPEC_LASER_BANK_POWER_OVERRIDE_OFF) {
		return -EINVAL;
	}

	/* Power-mode changes may touch GPIO and drivers; report contention as busy. */
	rc = laser_io_lock_with_timeout(K_MSEC(LASER_COMMAND_LOCK_TIMEOUT_MS));
	if (rc != 0) {
		return rc;
	}
	if (mode == HISPEC_LASER_BANK_POWER_OVERRIDE_ON) {
		rc = bank_power_set_locked(true, NULL, true);
	} else if (mode == HISPEC_LASER_BANK_POWER_OVERRIDE_OFF) {
		rc = bank_power_set_locked(false, NULL, true);
	}
	if (rc == 0) {
		k_mutex_lock(&laser_state_lock, K_FOREVER);
		bank_power_mode = mode;
		k_mutex_unlock(&laser_state_lock);
	}
	k_mutex_unlock(&laser_io_lock);

	return rc;
}

static int ensure_bank_powered_locked(void)
{
	if (bank_power_mode == HISPEC_LASER_BANK_POWER_OVERRIDE_OFF) {
		return -EPERM;
	}
	return bank_power_set_locked(true, NULL, false);
}

int hispec_laser_bank_clear_faults(uint32_t off_ms, uint32_t *actual_off_ms)
{
	uint32_t delay_ms = (off_ms == 0U) ? HISPEC_LASER_BANK_FAULT_CLEAR_OFF_MS : off_ms;
	bool fault = false;
	int rc;

	if (actual_off_ms != NULL) {
		*actual_off_ms = 0U;
	}

	/* Fault clear can power-cycle the bank; do not queue forever behind Modbus. */
	rc = laser_io_lock_with_timeout(K_MSEC(LASER_COMMAND_LOCK_TIMEOUT_MS));
	if (rc != 0) {
		return rc;
	}

	if (!bank_power_requested_enabled) {
		rc = 0;
		goto out;
	}

	for (uint8_t i = 0U; i < HISPEC_LASER_COUNT; ++i) {
		maiman_driver_t drv = {0};
		uint16_t lock_status;

		maiman_init(&drv, laser_profiles[i].node_id);
		bool read_ok = maiman_read_u16(&drv, REG_LOCK_STATUS, &lock_status);
		laser_note_communication_locked((enum hispec_laser_id)i, &drv);
		if (!read_ok) {
			rc = -EIO;
			goto out;
		}
		if ((lock_status & LOCK_STATE_LD_OVERCURRENT) != 0U) {
			fault = true;
			break;
		}
	}

	if (!fault) {
		rc = 0;
		goto out;
	}

	rc = bank_power_set_locked(false, NULL, false);
	if (rc != 0) {
		goto out;
	}

	/* k_sleep() yields during the supply-off interval needed to clear the
	 * SF8025 overcurrent latch.
	 */
	k_sleep(K_MSEC(delay_ms));
	rc = bank_power_set_locked(true, NULL, false);
	if (rc == 0 && actual_off_ms != NULL) {
		*actual_off_ms = delay_ms;
	}

out:
	k_mutex_unlock(&laser_io_lock);
	return rc;
}

static double level_percent_for_current(const laserprops_t *props, double current_ma)
{
	double range;

	if (props == NULL || !float_is_valid(current_ma)) {
		return LASERPROP_NA;
	}
	range = props->nominal_current_ma - props->threshold_current_ma;
	if (range <= 0.0) {
		return LASERPROP_NA;
	}
	if (current_ma <= 0.0) {
		return 0.0;
	}
	return 100.0 * (current_ma - props->threshold_current_ma) / range;
}

static void init_temperature_channels(
	struct hispec_laser_channel_temperature channels[HISPEC_LASER_COUNT])
{
	memset(channels, 0,
	       sizeof(struct hispec_laser_channel_temperature) * HISPEC_LASER_COUNT);
	for (uint8_t i = 0U; i < HISPEC_LASER_COUNT; ++i) {
		channels[i].id = laser_profiles[i].id;
		channels[i].tec_temperature_c = LASERPROP_NA;
	}
}

static void read_temperature_channel_locked(uint8_t index,
					    struct hispec_laser_channel_temperature *channel)
{
	maiman_driver_t drv = {0};
	double tec_temp = LASERPROP_NA;
	bool tec_started = false;
	bool temp_ok;
	bool state_ok = false;

	maiman_init(&drv, laser_profiles[index].node_id);
	temp_ok = maiman_read_tec_temperature_measured(&drv, &tec_temp);
	if (temp_ok) {
		state_ok = maiman_read_tec_started(&drv, &tec_started);
	}

	laser_note_communication_locked((enum hispec_laser_id)index, &drv);
	if (state_ok && laser_current_runtime[index].active && !tec_started) {
		invalidate_output_locked((enum hispec_laser_id)index);
	}
	channel->valid = temp_ok && state_ok;
	channel->tec_temperature_c = tec_temp;
	channel->tec_enabled = state_ok && tec_started;
}

int hispec_laser_bank_read_temperatures(
	struct hispec_laser_channel_temperature channels[HISPEC_LASER_COUNT])
{
	int rc;

	if (channels == NULL) {
		return -EINVAL;
	}

	init_temperature_channels(channels);

	/* Foreground temp reads report bus contention as busy, not missing data. */
	rc = laser_io_lock_with_timeout(K_MSEC(LASER_COMMAND_LOCK_TIMEOUT_MS));
	if (rc != 0) {
		return rc;
	}
	if (bank_power_requested_enabled) {
		for (uint8_t i = 0U; i < HISPEC_LASER_COUNT; ++i) {
			read_temperature_channel_locked(i, &channels[i]);
		}
	}
	k_mutex_unlock(&laser_io_lock);
	return 0;
}

int hispec_laser_bank_poll_temperatures(
	struct hispec_laser_channel_temperature channels[HISPEC_LASER_COUNT])
{
	int rc;

	if (channels == NULL) {
		return -EINVAL;
	}

	init_temperature_channels(channels);

	for (uint8_t i = 0U; i < HISPEC_LASER_COUNT; ++i) {
		/* Background heater polling is opportunistic. Release the bus between
		 * drivers so a command can cut in instead of waiting behind a full
		 * six-node temperature sweep.
		 */
		rc = laser_io_lock_with_timeout(K_NO_WAIT);
		if (rc != 0) {
			return rc;
		}
		if (!bank_power_requested_enabled) {
			k_mutex_unlock(&laser_io_lock);
			return 0;
		}
		read_temperature_channel_locked(i, &channels[i]);
		k_mutex_unlock(&laser_io_lock);
	}

	return 0;
}

static int check_driver_serial_locked(const struct hispec_laser_driver_profile *profile,
				      uint16_t serial, uint16_t expected)
{
	if (profile == NULL || profile->id < 0 || profile->id >= HISPEC_LASER_COUNT ||
	    serial == 0U) {
		return -EINVAL;
	}

	if (serial == expected) {
		return 0;
	}

	LOG_ERR("Laser %s node %u serial mismatch: expected %u got %u",
		profile->name, profile->node_id, expected, serial);

	return -EADDRNOTAVAIL;
}

static int verify_driver_locked(const struct hispec_laser_driver_profile *profile,
				maiman_driver_t *drv,
				uint16_t *serial_out, uint16_t expected_serial)
{
	uint16_t device_id;
	uint16_t serial;

	if (profile == NULL || drv == NULL) {
		return -EINVAL;
	}

	struct laser_output_estimate_state *state = &laser_output_estimate[profile->id];
	device_id = state->device_id;
	if (device_id == 0U) device_id = maiman_get_device_id(drv);
	if (device_id == 0U) {
		return -EIO;
	}
	if (profile->expected_device_id != 0U && device_id != profile->expected_device_id) {
		LOG_ERR("Laser %s node %u device-id mismatch: expected 0x%04x got 0x%04x",
			profile->name, profile->node_id,
			profile->expected_device_id, device_id);
		return -EADDRNOTAVAIL;
	}

	serial = state->serial;
	if (serial == 0U) serial = maiman_get_serial_number(drv);
	if (serial == 0U) {
		return -EIO;
	}
	state->device_id = device_id;
	state->serial = serial;
	if (serial_out != NULL) {
		*serial_out = serial;
	}

	return check_driver_serial_locked(profile, serial, expected_serial);
}

int hispec_laser_verify_driver(enum hispec_laser_id id, uint16_t *serial_out)
{
	const struct hispec_laser_driver_profile *profile;
	maiman_driver_t drv = {0};
	int rc;

	rc = profile_for_id(id, &profile);
	if (rc != 0) {
		return rc;
	}

	k_mutex_lock(&laser_io_lock, K_FOREVER);
	rc = ensure_bank_powered_locked();
	if (rc == 0) {
		maiman_init(&drv, profile->node_id);
		rc = verify_driver_locked(profile, &drv, serial_out, laser_settings[profile->id].expected_serial);
	}
	k_mutex_unlock(&laser_io_lock);

	return rc;
}

static int check_ocp_limit_locked(const struct hispec_laser_driver_profile *profile,
				  maiman_driver_t *drv, const laserprops_t *props)
{
	double ocp_ma;

	ocp_ma = maiman_get_current_protection_threshold(drv);
	if (!float_is_valid(ocp_ma) || ocp_ma < 0.0) {
		return -EIO;
	}

	if (float_is_valid(props->dne_current_ma) && ocp_ma > props->dne_current_ma) {
		(void)maiman_set_current(drv, 0.0);
		(void)maiman_stop_device(drv);
		(void)maiman_stop_tec(drv);
		(void)maiman_allow_interlock(drv);
		LOG_ERR("Laser %s driver OCP exceeds diode DNE", profile->name);
		return -ERANGE;
	}

	return 0;
}

static uint16_t effective_blocking_lock_status(uint16_t lock_status,
					       uint16_t device_state)
{
	uint16_t blocking = lock_status & LOCK_STATE_BLOCKING_MASK;

	/* The command path explicitly writes DENY_INTERLOCK before emission. Once
	 * the driver reports that bit latched, the physical interlock lock bit is
	 * informational for this firmware-owned setup rather than a sequence
	 * blocker. Electrical/thermal/TEC lock bits remain hard blockers.
	 */
	if ((device_state & INTERLOCK_DENIED) != 0U) {
		blocking &= (uint16_t)~LOCK_STATE_INTERLOCK;
	}

	return blocking;
}

static const char *laser_blocked_reason(bool bank_powered,
					bool serial_matches,
					uint16_t device_state,
					uint16_t tec_state,
					uint16_t blocking_lock_status)
{
	if (!bank_powered) {
		return "bank_off";
	}
	if (!serial_matches) {
		return "driver_identity_mismatch";
	}
	if ((tec_state & TEC_OPERATION_STATE_STARTED) == 0U) {
		return "tec_not_started";
	}
	if ((blocking_lock_status & LOCK_STATE_LD_OVERCURRENT) != 0U) {
		return "ld_overcurrent";
	}
	if ((blocking_lock_status & LOCK_STATE_LD_OVERHEAT) != 0U) {
		return "ld_overheat";
	}
	if ((blocking_lock_status & LOCK_STATE_EXTERNAL_NTC_INTERLOCK) != 0U) {
		return "external_ntc_interlock";
	}
	if ((blocking_lock_status & LOCK_STATE_TEC_ERROR) != 0U) {
		return "tec_error";
	}
	if ((blocking_lock_status & LOCK_STATE_TEC_SELFHEAT) != 0U) {
		return "tec_selfheat";
	}
	if ((blocking_lock_status & LOCK_STATE_INTERLOCK) != 0U) {
		return "interlock";
	}

	return NULL;
}

static int64_t next_autooff_deadline_locked(void)
{
	int64_t next = LASER_AUTOFF_NO_DEADLINE;

	for (uint8_t i = 0U; i < HISPEC_LASER_COUNT; ++i) {
		const int64_t deadline = laser_autooff_deadline_ms[i];

		if (deadline <= LASER_AUTOFF_NO_DEADLINE) {
			continue;
		}
		if (next == LASER_AUTOFF_NO_DEADLINE || deadline < next) {
			next = deadline;
		}
	}

	return next;
}

static k_timeout_t laser_autooff_wait_timeout(void)
{
	int64_t next_deadline;
	int64_t wait_ms;

	k_mutex_lock(&laser_state_lock, K_FOREVER);
	next_deadline = next_autooff_deadline_locked();
	for (uint8_t i = 0; i < HISPEC_LASER_COUNT; ++i) {
		if (bank_power_requested_enabled && laser_output_estimate[i].started) {
			int64_t probe = k_uptime_get() + LASER_RESPONSE_CHECK_MS;
			if (next_deadline == 0 || probe < next_deadline) next_deadline = probe;
		}
	}
	k_mutex_unlock(&laser_state_lock);

	if (next_deadline == LASER_AUTOFF_NO_DEADLINE) {
		return K_FOREVER;
	}

	wait_ms = next_deadline - k_uptime_get();
	return wait_ms <= 0 ? K_NO_WAIT : K_MSEC(wait_ms);
}


static void laser_autooff_reschedule(void)
{
	const k_timeout_t timeout = laser_autooff_wait_timeout();

	/* Auto-off can run Modbus stop commands, so it uses the app blocking
	 * workqueue instead of Zephyr's Modbus-RX system workqueue.
	 */

	//TODO
	if (laser_autooff_work_q == NULL) {
		return;
	}
	if (K_TIMEOUT_EQ(timeout, K_FOREVER)) {
		(void)k_work_cancel_delayable(&laser_autooff_work);
	} else {
		(void)k_work_reschedule_for_queue(laser_autooff_work_q,
						  &laser_autooff_work, timeout);
	}
}

void hispec_laser_autooff_start(struct k_work_q *work_q)
{
	__ASSERT_NO_MSG(work_q != NULL);
	if (work_q == NULL) {
		return;
	}

	k_mutex_lock(&laser_io_lock, K_FOREVER);
	ensure_laser_runtime_settings_locked();
	k_mutex_unlock(&laser_io_lock);
	laser_autooff_work_q = work_q;
	laser_autooff_reschedule();
}

static int apply_runtime_profile_locked(const struct hispec_laser_driver_profile *profile,
					maiman_driver_t *drv,
					const struct app_laser_channel_settings *settings)
{
	const laserprops_t *props = &settings->properties;
	double old_min, old_max;
	int rc;

	laser_output_estimate[profile->id].prepared = false;
	LOG_DBG("Laser %s applying configuration", profile->name);
	rc = check_ocp_limit_locked(profile, drv, props);
	if (rc != 0) {
		return rc;
	}

	if (!maiman_set_current_max(drv,
		hispec_laser_quantize_current_ma(props->max_current_ma, 0.0, props->max_current_ma))) {
		return -EIO;
	}
	if (!maiman_set_current_set_calibration(drv,
						settings->current_set_calibration_pct)) {
		return -EIO;
	}
	if (!maiman_set_tec_current_limit(drv, props->tec_max_current_a)) {
		return -EIO;
	}
	/* Expand before moving the target, then narrow. This also handles disjoint
	 * old/new ranges without asking the controller to accept an invalid target.
	 * Absolute device limits are read-only; only the writable bounds change.
	 */
	if (!maiman_read_scaled(drv, REG_TEC_TEMPERATURE_MIN, DIVIDER_TEC_TEMPERATURE, true, &old_min) ||
	    !maiman_read_scaled(drv, REG_TEC_TEMPERATURE_MAX, DIVIDER_TEC_TEMPERATURE, true, &old_max) ||
	    (props->operating_temp_range_c.min_c < old_min &&
	     !maiman_write_scaled(drv, REG_TEC_TEMPERATURE_MIN, DIVIDER_TEC_TEMPERATURE, true,
		props->operating_temp_range_c.min_c)) ||
	    (props->operating_temp_range_c.max_c > old_max &&
	     !maiman_write_scaled(drv, REG_TEC_TEMPERATURE_MAX, DIVIDER_TEC_TEMPERATURE, true,
		props->operating_temp_range_c.max_c)) ||
	    !maiman_set_tec_temperature(drv, props->operating_temp_c) ||
	    (props->operating_temp_range_c.min_c > old_min &&
	     !maiman_write_scaled(drv, REG_TEC_TEMPERATURE_MIN, DIVIDER_TEC_TEMPERATURE, true,
		props->operating_temp_range_c.min_c)) ||
	    (props->operating_temp_range_c.max_c < old_max &&
	     !maiman_write_scaled(drv, REG_TEC_TEMPERATURE_MAX, DIVIDER_TEC_TEMPERATURE, true,
		props->operating_temp_range_c.max_c))) {
		return -EIO;
	}
	if (!maiman_set_tec_pid(drv, props->tec_pid)) {
		return -EIO;
	}
	/* All HISPEC laser operation is continuous-wave. Normalize installed
	 * drivers during profile programming so stale pulse-mode register values
	 * cannot survive a module replacement or manual reconfiguration.
	 */
	if (!maiman_set_frequency(drv, 0.0) ||
	    !maiman_set_duration(drv, 0.0)) {
		return -EIO;
	}

	if (!maiman_set_internal_current_control(drv, true) ||
	    !maiman_set_internal_enable_control(drv, true) ||
	    !maiman_set_internal_tec_temperature_control(drv, true) ||
	    !maiman_set_internal_tec_enable_control(drv, true) ||
	    !maiman_deny_interlock(drv)) return -EIO;
	k_mutex_lock(&laser_state_lock, K_FOREVER);
	laser_output_estimate[profile->id].tec_temperature_c = props->operating_temp_c;
	laser_output_estimate[profile->id].prepared = true;
	k_mutex_unlock(&laser_state_lock);
	return 0;
}

int hispec_laser_program_driver_profile(enum hispec_laser_id id, bool save_to_eeprom)
{
	const struct hispec_laser_driver_profile *profile;
	maiman_driver_t drv = {0};
	int rc;

	rc = profile_for_id(id, &profile);
	if (rc != 0) {
		return rc;
	}

	k_mutex_lock(&laser_io_lock, K_FOREVER);
	rc = ensure_bank_powered_locked();
	if (rc != 0) {
		goto out;
	}

	maiman_init(&drv, profile->node_id);
	rc = verify_driver_locked(profile, &drv, NULL, laser_settings[profile->id].expected_serial);
	if (rc != 0) {
		goto out;
	}

	/* Stop is checked before rewriting diode limits; failures preserve uncertainty. */
	rc = stop_output_locked(profile, false);
	if (rc != 0 || !maiman_allow_interlock(&drv)) {
		rc = rc != 0 ? rc : -EIO;
		goto out;
	}
	rc = apply_runtime_profile_locked(profile, &drv, &laser_settings[id]);
	if (rc != 0) {
		goto out;
	}

	if (save_to_eeprom && !maiman_save_parameters(&drv)) {
		rc = -EIO;
	}

out:
	if (rc != 0) {
		invalidate_output_locked(id);
	}
	laser_note_communication_locked(id, &drv);
	k_mutex_unlock(&laser_io_lock);
	return rc;
}

int hispec_laser_save_driver_settings(enum hispec_laser_id id)
{
	const struct hispec_laser_driver_profile *profile;
	maiman_driver_t drv = {0};
	int rc;

	rc = profile_for_id(id, &profile);
	if (rc != 0) {
		return rc;
	}

	k_mutex_lock(&laser_io_lock, K_FOREVER);
	rc = ensure_bank_powered_locked();
	if (rc == 0) {
		maiman_init(&drv, profile->node_id);
		rc = verify_driver_locked(profile, &drv, NULL, laser_settings[profile->id].expected_serial);
		if (rc == 0 && !maiman_save_parameters(&drv)) {
			rc = -EIO;
		}
	}
	if (rc != 0) {
		invalidate_output_locked(id);
	}
	laser_note_communication_locked(id, &drv);
	k_mutex_unlock(&laser_io_lock);

	return rc;
}

int hispec_laser_reset_driver_settings(enum hispec_laser_id id)
{
	const struct hispec_laser_driver_profile *profile;
	maiman_driver_t drv = {0};
	int rc;

	rc = profile_for_id(id, &profile);
	if (rc != 0) {
		return rc;
	}

	k_mutex_lock(&laser_io_lock, K_FOREVER);
	rc = ensure_bank_powered_locked();
	if (rc == 0) {
		maiman_init(&drv, profile->node_id);
		rc = verify_driver_locked(profile, &drv, NULL, laser_settings[profile->id].expected_serial);
		if (rc == 0 && !maiman_reset_parameters(&drv)) {
			rc = -EIO;
		}
	}
	/* Reset changes driver-owned registers even if its acknowledgement was lost. */
	laser_output_estimate[id].prepared = false;
	LOG_INF("Laser %s reset: configuration invalidated", profile->name);
	invalidate_output_locked(id);
	laser_note_communication_locked(id, &drv);
	k_mutex_unlock(&laser_io_lock);

	return rc;
}

/* No I/O: current and tune setters share the same fast-path qualification.
 * Configuration/power changes revoke preparation; control faults revoke readiness.
 */
static bool output_ready_locked(enum hispec_laser_id id)
{
	return bank_power_requested_enabled && laser_output_estimate[id].started &&
	       laser_output_estimate[id].valid && laser_output_estimate[id].prepared &&
	       k_uptime_get() < laser_output_estimate[id].response_deadline_ms;
}

static int prepare_to_operate_locked(const struct hispec_laser_driver_profile *profile,
				     maiman_driver_t *drv)
{
	struct laser_output_estimate_state *state = &laser_output_estimate[profile->id];
	uint16_t device_state, tec_state, lock_status;
	int rc = ensure_bank_powered_locked();

	if (rc != 0) return rc;
	maiman_init(drv, profile->node_id);
	rc = verify_driver_locked(profile, drv, NULL, laser_settings[profile->id].expected_serial);
	if (rc != 0) return rc;
	LOG_DBG("Laser %s prepare configuration_needed=%u", profile->name, !state->prepared);
	if (!state->prepared) {
		/* Establish app-owned limits/default temperature/CW mode once per bank
		 * power interval. Live tuning then survives ordinary STOP/start cycles.
		 * Failed programming leaves preparation false for the next explicit attempt.
		 */
		rc = apply_runtime_profile_locked(profile, drv, &laser_settings[profile->id]);
		if (rc != 0) return rc;
	}
	/* Configuration is stable, but controller lock and TEC state are dynamic. */
	if (!maiman_read_raw_tec_status(drv, &tec_state)) return -EIO;
	if (!(tec_state & TEC_OPERATION_STATE_STARTED) && !maiman_start_tec(drv)) return -EIO;
	on_time_runtime_update_locked(laser_tec_runtime, ARRAY_SIZE(laser_tec_runtime), profile->id, true);
	if (!maiman_read_u16(drv, REG_STATE_OF_DEVICE_COMMAND, &device_state) ||
	    !maiman_read_u16(drv, REG_LOCK_STATUS, &lock_status)) return -EIO;
	if (effective_blocking_lock_status(lock_status, device_state) != 0U) return -EIO;
	k_mutex_lock(&laser_state_lock, K_FOREVER);
	state->started = (device_state & OPERATION_STATE_STARTED) != 0U;
	k_mutex_unlock(&laser_state_lock);
	return 0;
}

static void status_defaults(const struct hispec_laser_driver_profile *profile,
			    struct hispec_laser_status *out)
{
	memset(out, 0, sizeof(*out));
	out->id = profile->id;
	out->name = profile->name;
	out->properties = *profile->properties;
	out->expected_device_id = profile->expected_device_id;
	out->current_set_ma = LASERPROP_NA;
	out->level_percent = LASERPROP_NA;
	out->current_measured_ma = LASERPROP_NA;
	out->current_min_ma = LASERPROP_NA;
	out->current_max_ma = LASERPROP_NA;
	out->current_max_limit_ma = LASERPROP_NA;
	out->current_protection_threshold_ma = LASERPROP_NA;
	out->voltage_v = LASERPROP_NA;
	out->current_on_time_s = LASERPROP_NA;
	out->tec_on_time_s = LASERPROP_NA;
	out->total_emitting_s = 0.0;
	out->tune_delta_nm = 0.0;
	out->autooff_s = 0U;
	out->off_in_s = 0;
	out->tec_temperature_set_c = LASERPROP_NA;
	out->tec_temperature_measured_c = LASERPROP_NA;
	out->pcb_temperature_c = LASERPROP_NA;
	out->tec_current_measured_a = LASERPROP_NA;
	out->tec_current_limit_a = LASERPROP_NA;
	out->tec_voltage_v = LASERPROP_NA;
	out->current_set_calibration_pct = LASERPROP_NA;
	out->ntc_t_coefficient_per_c = LASERPROP_NA;
	out->estimated_power_mw = LASERPROP_NA;
	out->estimated_wavelength_nm = LASERPROP_NA;
}

int hispec_laser_get_status(enum hispec_laser_id id, bool engineering, struct hispec_laser_status *out)
{
	const struct hispec_laser_driver_profile *profile;
	maiman_driver_t drv = {0};
	bool read_ok = true;
	int rc;

	if (out == NULL) {
		return -EINVAL;
	}

	rc = profile_for_id(id, &profile);
	if (rc != 0) {
		return rc;
	}

	status_defaults(profile, out);

	/* Status is retryable; avoid waiting forever behind driver timeouts. */
	rc = laser_io_lock_with_timeout(K_MSEC(LASER_COMMAND_LOCK_TIMEOUT_MS));
	if (rc != 0) {
		return rc;
	}
	ensure_laser_runtime_settings_locked();
	out->properties = *runtime_props_locked(id);
	out->expected_serial = laser_settings[id].expected_serial;
	out->current_on_time_s =
		on_time_runtime_seconds_locked(laser_current_runtime,
					       ARRAY_SIZE(laser_current_runtime), id);
	out->tec_on_time_s =
		on_time_runtime_seconds_locked(laser_tec_runtime,
					       ARRAY_SIZE(laser_tec_runtime), id);
	out->current_runtime_active = laser_current_runtime[id].active;
	out->tec_runtime_active = laser_tec_runtime[id].active;
	out->total_emitting_s = laser_settings[id].total_emitting_s +
				(double)out->current_on_time_s;
	out->tune_delta_nm = laser_settings[id].tune_delta_nm;
	out->autooff_s = laser_settings[id].autooff_s;
	out->current_set_calibration_pct = laser_settings[id].current_set_calibration_pct;
	out->ntc_t_coefficient_per_c = out->properties.ntc_t_coefficient_per_c;
	if (laser_autooff_deadline_ms[id] > 0) {
		int64_t remaining_ms = laser_autooff_deadline_ms[id] - k_uptime_get();

		out->autooff_active = remaining_ms > 0;
		out->off_in_s = out->autooff_active ? (remaining_ms + 999) / 1000 : 0;
	}
	out->bank_powered = bank_power_requested_enabled;
	if (!out->bank_powered) {
		out->blocked_reason = laser_blocked_reason(false, true, 0U, 0U, 0U);
		rc = 0;
		goto out_unlock;
	}

	maiman_init(&drv, profile->node_id);
	if (engineering) {
		out->device_id = maiman_get_device_id(&drv);
		if (drv.io_failed) { rc = -EIO; goto out_unlock; }
		out->serial_number = maiman_get_serial_number(&drv);
		if (drv.io_failed) { rc = -EIO; goto out_unlock; }
		/* An explicit diagnostic observation supersedes cached identity. */
		if (laser_output_estimate[id].device_id != out->device_id ||
		    laser_output_estimate[id].serial != out->serial_number) {
			laser_output_estimate[id].prepared = false;
		}
		laser_output_estimate[id].device_id = out->device_id;
		laser_output_estimate[id].serial = out->serial_number;
		rc = check_driver_serial_locked(profile, out->serial_number, laser_settings[id].expected_serial);
		if (out->device_id != profile->expected_device_id) rc = -EADDRNOTAVAIL;
	} else {
		rc = verify_driver_locked(profile, &drv, &out->serial_number, laser_settings[id].expected_serial);
		out->device_id = laser_output_estimate[id].device_id;
	}
	if (rc != 0) goto out_unlock;
	out->expected_serial = laser_settings[id].expected_serial;
	out->serial_matches = out->expected_serial != 0U &&
			      out->serial_number == out->expected_serial;

	out->device_state = maiman_get_raw_status(&drv);
	out->tec_state = maiman_get_raw_tec_status(&drv);
	out->lock_status = maiman_get_raw_lock_status(&drv);

	out->operation_started = (out->device_state & OPERATION_STATE_STARTED) != 0U;
	out->current_set_internal = (out->device_state & CURRENT_SET_INTERNAL) != 0U;
	out->enable_internal = (out->device_state & ENABLE_INTERNAL) != 0U;
	out->external_ntc_denied = (out->device_state & EXTERNAL_NTC_INTERLOCK_DENIED) != 0U;
	out->interlock_denied = (out->device_state & INTERLOCK_DENIED) != 0U;
	out->tec_started = (out->tec_state & TEC_OPERATION_STATE_STARTED) != 0U;
	out->tec_set_internal = (out->tec_state & TEC_SET_INTERNAL) != 0U;
	out->tec_enable_internal = (out->tec_state & TEC_ENABLE_INTERNAL) != 0U;
	out->lock_interlock = (out->lock_status & LOCK_STATE_INTERLOCK) != 0U;
	out->lock_ld_overcurrent = (out->lock_status & LOCK_STATE_LD_OVERCURRENT) != 0U;
	out->lock_ld_overheat = (out->lock_status & LOCK_STATE_LD_OVERHEAT) != 0U;
	out->lock_external_ntc_interlock =
		(out->lock_status & LOCK_STATE_EXTERNAL_NTC_INTERLOCK) != 0U;
	out->lock_tec_error = (out->lock_status & LOCK_STATE_TEC_ERROR) != 0U;
	out->lock_tec_selfheat = (out->lock_status & LOCK_STATE_TEC_SELFHEAT) != 0U;
	out->blocking_lock_status = effective_blocking_lock_status(out->lock_status,
								   out->device_state);
	if (!drv.io_failed && (!out->serial_matches || out->blocking_lock_status != 0U ||
	    (laser_output_estimate[id].started &&
	     ((out->device_state & OPERATION_STATE_STARTED) == 0U || !out->tec_started)))) {
		invalidate_output_locked(id);
	}
	out->ready_to_operate = !drv.io_failed && out->serial_matches && out->tec_started &&
				out->blocking_lock_status == 0U;
	out->blocked_reason = laser_blocked_reason(out->bank_powered,
						   out->serial_matches,
						   out->device_state,
						   out->tec_state,
						   out->blocking_lock_status);

	out->current_set_ma = laser_output_estimate[id].current_ma;
	out->tec_temperature_set_c = laser_output_estimate[id].tec_temperature_c;
	if (engineering) {
		if (!maiman_get_current(&drv, &out->current_set_ma)) read_ok = false;
		out->current_measured_ma = maiman_get_current_measured(&drv);
		out->current_min_ma = maiman_get_current_min(&drv);
		out->current_max_ma = maiman_get_current_max(&drv);
		out->current_max_limit_ma = maiman_get_current_max_limit(&drv);
		out->current_protection_threshold_ma = maiman_get_current_protection_threshold(&drv);
		out->current_set_calibration_pct = maiman_get_current_set_calibration(&drv);
		out->tec_temperature_set_c = maiman_get_tec_temperature_value(&drv);
		out->pcb_temperature_c = maiman_get_pcb_temperature_measured(&drv);
		out->tec_current_limit_a = maiman_get_tec_current_limit(&drv);
		out->ntc_t_coefficient_per_c = maiman_get_ntc_b25_100_coefficient(&drv);
		if (!maiman_get_tec_pid(&drv, &out->tec_pid)) read_ok = false;
	}
	out->level_percent = level_percent_for_current(&out->properties, out->current_set_ma);
	out->voltage_v = maiman_get_voltage_measured(&drv);
	out->tec_temperature_measured_c = maiman_get_tec_temperature_measured(&drv);
	out->tec_current_measured_a = maiman_get_tec_current_measured(&drv);
	out->tec_voltage_v = maiman_get_tec_voltage(&drv);

	out->estimated_power_mw =
		hispec_laser_estimate_power_mw(&out->properties, out->current_set_ma);
	out->estimated_wavelength_nm =
		hispec_laser_estimate_wavelength_nm(&out->properties,
						    out->tec_temperature_measured_c,
						    out->current_set_ma);
	rc = read_ok ? 0 : -EIO;

out_unlock:
	if (drv.io_failed) {
		rc = -EIO;
	}
	if (rc == -EADDRNOTAVAIL) {
		invalidate_output_locked(id);
	}
	laser_note_communication_locked(id, &drv);
	k_mutex_unlock(&laser_io_lock);
	return rc;
}

static int stop_output_locked(const struct hispec_laser_driver_profile *profile, bool stop_tec)
{
	maiman_driver_t drv = {0};

	ensure_laser_runtime_settings_locked();
	if (!bank_power_requested_enabled) {
		commit_current_runtime_locked(profile->id, true);
		return 0;
	}

	maiman_init(&drv, profile->node_id);
	struct laser_output_estimate_state *state = &laser_output_estimate[profile->id];
	LOG_DBG("Laser %s stop started=%u valid=%u stop_tec=%u", profile->name, state->started, state->valid, stop_tec);
	if (state->started || !state->valid || state->current_ma != 0.0) {
		bool zeroed = maiman_set_current(&drv, 0.0);
		if (zeroed) {
			on_time_runtime_update_locked(laser_current_runtime, ARRAY_SIZE(laser_current_runtime), profile->id, false);
			k_mutex_lock(&laser_state_lock, K_FOREVER);
			state->current_ma = 0.0;
			k_mutex_unlock(&laser_state_lock);
		}
		bool stopped = maiman_stop_device(&drv);
		if (!zeroed || !stopped) {
			laser_note_communication_locked(profile->id, &drv);
			invalidate_output_locked(profile->id);
			return -EIO;
		}
	}

	if (stop_tec && laser_tec_runtime[profile->id].active) {
		if (!maiman_stop_tec(&drv)) {
			laser_note_communication_locked(profile->id, &drv);
			invalidate_output_locked(profile->id);
			return -EIO;
		}
		on_time_runtime_update_locked(laser_tec_runtime, ARRAY_SIZE(laser_tec_runtime),
					      profile->id, false);
	}
	laser_note_communication_locked(profile->id, &drv);
	commit_current_runtime_locked(profile->id, true);
	LOG_INF("Laser %s stopped", profile->name);
	return 0;
}

int hispec_laser_stop_output(enum hispec_laser_id id, bool stop_tec)
{
	const struct hispec_laser_driver_profile *profile;
	int rc;

	rc = profile_for_id(id, &profile);
	if (rc != 0) {
		return rc;
	}

	k_mutex_lock(&laser_io_lock, K_FOREVER);
	rc = stop_output_locked(profile, stop_tec);
	k_mutex_unlock(&laser_io_lock);
	if (rc == 0) {
		laser_autooff_reschedule();
	}
	return rc;
}

int hispec_laser_stop_all_outputs(bool stop_tecs)
{
	int rc;

	k_mutex_lock(&laser_io_lock, K_FOREVER);
	rc = zero_all_driver_currents_locked(stop_tecs);
	k_mutex_unlock(&laser_io_lock);
	if (rc == 0) {
		laser_autooff_reschedule();
	}
	return rc;
}

int hispec_laser_set_current_ma(enum hispec_laser_id id, double current_ma)
{
	const struct hispec_laser_driver_profile *profile;
	const laserprops_t *props;
	maiman_driver_t drv = {0};
	bool running;
	int rc;

	rc = profile_for_id(id, &profile);
	if (rc != 0) {
		return rc;
	}
	rc = laser_io_lock_with_timeout(K_MSEC(LASER_COMMAND_LOCK_TIMEOUT_MS));
	if (rc != 0) {
		return rc;
	}
	props = runtime_props_locked(id);

	if (!float_is_valid(current_ma) || current_ma < 0.0 ||
	    current_ma > props->max_current_ma) {
		k_mutex_unlock(&laser_io_lock);
		return -ERANGE;
	}
	current_ma = hispec_laser_quantize_current_ma(current_ma, 0.0, props->max_current_ma);
	running = output_ready_locked(id);
	/* A no-op is not communication and must not extend the response deadline.
	 * An unprepared/faulted driver still needs the ordinary recovery path.
	 */
	if (running && current_ma == laser_output_estimate[id].current_ma) {
		rc = 0;
		goto out;
	}

	LOG_DBG("Laser %s level current_ma=%.3f started=%u configured=%u", profile->name,
		current_ma, laser_output_estimate[id].started, laser_output_estimate[id].prepared);
	if (current_ma == 0.0) {
		maiman_init(&drv, profile->node_id);
		if (bank_power_requested_enabled && !maiman_set_current(&drv, 0.0)) {
			rc = -EIO;
		} else {
			on_time_runtime_update_locked(laser_current_runtime, ARRAY_SIZE(laser_current_runtime), id, false);
			k_mutex_lock(&laser_state_lock, K_FOREVER);
			bool was_valid = laser_output_estimate[id].valid;
			output_estimate_set_locked(id, 0.0, laser_output_estimate[id].tec_temperature_c);
			/* Zeroing current cannot establish whether a previously failed STOP
			 * actually stopped the driver. Preserve that uncertainty. */
			laser_output_estimate[id].valid = was_valid || !bank_power_requested_enabled;
			k_mutex_unlock(&laser_state_lock);
			rc = 0;
		}
		goto out;
	}

	if (running) {
		maiman_init(&drv, profile->node_id);
	} else {
		rc = prepare_to_operate_locked(profile, &drv);
		if (rc != 0) {
			goto out;
		}
	}

	/* A running bank needs one current write, for increasing or decreasing
	 * current. No profile writes, repeated enable, or invented settling delay.
	 */
	if (!maiman_set_current(&drv, current_ma) ||
	    ((!laser_output_estimate[id].started || !laser_output_estimate[id].valid) && !maiman_start_device(&drv))) {
		LOG_WRN("Laser %s current/enable write failed current=%.3fmA",
			profile->name, (double)current_ma);
		rc = -EIO;
	} else {
		LOG_DBG("Laser %s current=%.3fmA", profile->name, current_ma);
		k_mutex_lock(&laser_state_lock, K_FOREVER);
		on_time_runtime_update_locked(laser_current_runtime,
					      ARRAY_SIZE(laser_current_runtime), id, true);
		on_time_runtime_update_locked(laser_tec_runtime,
					      ARRAY_SIZE(laser_tec_runtime), id, true);
		output_estimate_set_locked(id, current_ma, laser_output_estimate[id].tec_temperature_c);
		laser_output_estimate[id].started = true;
		k_mutex_unlock(&laser_state_lock);
	}

out:
	if (rc != 0) {
		invalidate_output_locked(id);
	}
	laser_note_communication_locked(id, &drv);
	LOG_DBG("Laser %s level result rc=%d started=%u current_ma=%.3f valid=%u", profile->name, rc,
		laser_output_estimate[id].started, laser_output_estimate[id].current_ma, laser_output_estimate[id].valid);
	k_mutex_unlock(&laser_io_lock);
	laser_autooff_reschedule();
	return rc;
}

int hispec_laser_set_output_mw(enum hispec_laser_id id, double power_mw)
{
	struct app_laser_channel_settings settings;
	const laserprops_t *props = &settings.properties;
	double current_ma;

	if (hispec_laser_get_channel_settings(id, &settings) != 0 || !float_is_valid(power_mw) || power_mw < 0.0) {
		return -EINVAL;
	}

	if (power_mw == 0.0) {
		return hispec_laser_set_current_ma(id, 0.0);
	}
	if (!float_is_positive(props->efficiency_mw_per_ma)) {
		return -EINVAL;
	}

	current_ma = props->threshold_current_ma + (power_mw / props->efficiency_mw_per_ma);
	if (current_ma > props->max_current_ma) {
		return -ERANGE;
	}

	return hispec_laser_set_current_ma(id, current_ma);
}

int hispec_laser_set_output_percent(enum hispec_laser_id id, double percent)
{
	struct app_laser_channel_settings settings;
	const laserprops_t *props = &settings.properties;
	double current_range_ma;
	double current_ma;

	if (hispec_laser_get_channel_settings(id, &settings) != 0 || !float_is_valid(percent) || percent < 0.0 || percent > 100.0) {
		return -ERANGE;
	}

	if (percent == 0.0) {
		return hispec_laser_set_current_ma(id, 0.0);
	}

	current_range_ma = props->nominal_current_ma - props->threshold_current_ma;
	if (!float_is_positive(current_range_ma)) {
		return -EINVAL;
	}

	current_ma = hispec_laser_quantize_current_ma(
		props->threshold_current_ma + current_range_ma * (percent / 100.0),
		0.0, MIN(props->nominal_current_ma, props->max_current_ma));

	return hispec_laser_set_current_ma(id, current_ma);
}

int hispec_laser_set_output_percent_autooff(enum hispec_laser_id id,
					    double percent,
					    uint32_t autooff_s, bool apply_tune)
{
	struct app_laser_channel_settings settings;
	const laserprops_t *props;
	int rc;

	if (id < 0 || id >= HISPEC_LASER_COUNT) {
		return -EINVAL;
	}

	rc = hispec_laser_get_channel_settings(id, &settings);
	if (rc != 0) {
		return rc;
	}
	props = &settings.properties;

	if (apply_tune && percent > 0.0 && settings.tune_delta_nm != 0.0) {
		/* Positive level commands apply the stored tune request. Setting
		 * level 0 writes zero current without STOP and preserves tune_delta_nm
		 * stored for the next start.
		 */
		struct hispec_laser_tune_request request = {
			.desired_power_percent = percent,
			.wavelength_nm = props->wavelength_nm + settings.tune_delta_nm,
			.use_current = true,
			.use_temperature = true,
			.maximum_power_shift_percent = 100.0,
			.apply = true,
		};
		struct hispec_laser_tune_result result = {0};

		rc = hispec_laser_tune_wavelength(id, &request, &result);
	} else {
		rc = hispec_laser_set_output_percent(id, percent);
	}

	if (rc == 0) {
		k_mutex_lock(&laser_io_lock, K_FOREVER);
		k_mutex_lock(&laser_state_lock, K_FOREVER);
		if (laser_output_estimate[id].started) {
			laser_autooff_deadline_ms[id] =
				autooff_s == 0U ? LASER_AUTOFF_NO_DEADLINE :
				k_uptime_get() + (int64_t)autooff_s * 1000LL;
		} else {
			laser_autooff_deadline_ms[id] = LASER_AUTOFF_NO_DEADLINE;
		}
		k_mutex_unlock(&laser_state_lock);
		k_mutex_unlock(&laser_io_lock);
		laser_autooff_reschedule();
	}

	return rc;
}

int hispec_laser_set_tec_temperature_c(enum hispec_laser_id id, double temperature_c)
{
	const struct hispec_laser_driver_profile *profile;
	const laserprops_t *props;
	maiman_driver_t drv = {0};
	int rc;

	rc = profile_for_id(id, &profile);
	if (rc != 0) {
		return rc;
	}
	rc = laser_io_lock_with_timeout(K_MSEC(LASER_COMMAND_LOCK_TIMEOUT_MS));
	if (rc != 0) {
		return rc;
	}
	props = runtime_props_locked(id);

	if (!float_is_valid(temperature_c) ||
	    temperature_c < props->operating_temp_range_c.min_c ||
	    temperature_c > props->operating_temp_range_c.max_c) {
		k_mutex_unlock(&laser_io_lock);
		return -ERANGE;
	}


	rc = prepare_to_operate_locked(profile, &drv);
	if (rc == 0 && !maiman_set_tec_temperature(&drv, temperature_c)) {
		LOG_WRN("Laser %s TEC setpoint write failed temp=%.3fC",
			profile->name, (double)temperature_c);
		rc = -EIO;
	} else if (rc == 0) {
		LOG_INF("Laser %s TEC setpoint updated temp=%.3fC",
			profile->name, (double)temperature_c);
		k_mutex_lock(&laser_state_lock, K_FOREVER);
		laser_output_estimate[id].tec_temperature_c = temperature_c;
		k_mutex_unlock(&laser_state_lock);
	}
	if (rc != 0) {
		invalidate_output_locked(id);
	}
	laser_note_communication_locked(id, &drv);
	k_mutex_unlock(&laser_io_lock);

	return rc;
}

int hispec_laser_set_tec_pid(enum hispec_laser_id id, tec_pid_t pid)
{
	const struct hispec_laser_driver_profile *profile;
	maiman_driver_t drv = {0};
	int rc;

	rc = profile_for_id(id, &profile);
	if (rc != 0) {
		return rc;
	}

	k_mutex_lock(&laser_io_lock, K_FOREVER);
	rc = ensure_bank_powered_locked();
	if (rc == 0) {
		maiman_init(&drv, profile->node_id);
		rc = verify_driver_locked(profile, &drv, NULL, laser_settings[profile->id].expected_serial);
	}
	if (rc == 0 && !maiman_set_tec_pid(&drv, pid)) {
		laser_output_estimate[id].prepared = false;
		rc = -EIO;
	}
	if (rc != 0) {
		invalidate_output_locked(id);
	}
	laser_note_communication_locked(id, &drv);
	k_mutex_unlock(&laser_io_lock);

	return rc;
}

static int validate_laser_settings(const struct hispec_laser_driver_profile *profile,
				   struct app_laser_channel_settings *settings)
{
	const laserprops_t *props;

	if (profile == NULL || profile->properties == NULL || settings == NULL) {
		return -EINVAL;
	}

	props = &settings->properties;
	if (!float_is_valid(props->nominal_current_ma) ||
	    !float_is_valid(props->max_current_ma) ||
	    !float_is_valid(props->threshold_current_ma) ||
	    !float_is_valid(settings->min_autolevel_current_ma) || settings->min_autolevel_current_ma < 0.0 ||
	    !float_is_valid(props->efficiency_mw_per_ma) ||
	    !float_is_valid(props->wavelength_nm) ||
	    !float_is_valid(settings->current_set_calibration_pct) ||
	    !float_is_valid(settings->fractional_noise) || settings->fractional_noise < 0.0 ||
	    !float_is_valid(settings->constant_noise_mw) || settings->constant_noise_mw < 0.0 ||
	    !float_is_valid(props->tec_max_current_a) ||
	    !float_is_valid(props->dlambda_dT_nm_per_k) ||
	    !float_is_valid(props->dlambda_dA_nm_per_ma) ||
	    props->threshold_current_ma < 0.0 ||
	    props->threshold_current_ma > 1000.0 ||
	    props->nominal_current_ma <= props->threshold_current_ma ||
	    props->nominal_current_ma > 1000.0 ||
	    props->max_current_ma < props->nominal_current_ma ||
	    props->max_current_ma > 1000.0 ||
	    props->efficiency_mw_per_ma < 0.0 ||
	    props->efficiency_mw_per_ma > 100.0 ||
	    props->wavelength_nm < 1.0 ||
	    props->wavelength_nm > 10000.0 ||
	    settings->current_set_calibration_pct < 95.0 ||
	    settings->current_set_calibration_pct > 105.0 ||
	    props->tec_max_current_a <= 0.0 ||
	    props->tec_max_current_a > profile->properties->tec_max_current_a ||
	    props->dlambda_dT_nm_per_k < -10.0 ||
	    props->dlambda_dT_nm_per_k > 10.0 ||
	    props->dlambda_dA_nm_per_ma < -10.0 ||
	    props->dlambda_dA_nm_per_ma > 10.0 ||
	    settings->expected_serial == 0U) {
		return -ERANGE;
	}

	if (!float_is_valid(props->operating_temp_range_c.min_c) ||
	    !float_is_valid(props->operating_temp_range_c.max_c) ||
	    !float_is_valid(props->operating_temp_c) ||
	    props->operating_temp_range_c.min_c < 15.0 ||
	    props->operating_temp_range_c.max_c > 40.0 ||
	    props->operating_temp_range_c.min_c > props->operating_temp_range_c.max_c ||
	    props->operating_temp_c < props->operating_temp_range_c.min_c ||
	    props->operating_temp_c > props->operating_temp_range_c.max_c) {
		return -ERANGE;
	}

	/* Raising threshold may consume a previously accepted autolevel floor.
	 * Normalize the candidate only; rejected updates never alter live settings.
	 */
	double minimum = MAX(settings->min_autolevel_current_ma,
		props->threshold_current_ma + 1.0 / DIVIDER_CURRENT);
	minimum = hispec_laser_quantize_current_ma(minimum, minimum, props->nominal_current_ma);
	if (!isfinite(minimum)) {
		return -ERANGE;
	}
	settings->min_autolevel_current_ma = minimum;
	return 0;
}

int hispec_laser_validate_channel_settings(enum hispec_laser_id id,
					   struct app_laser_channel_settings *settings)
{
	const struct hispec_laser_driver_profile *profile;
	int rc;

	rc = profile_for_id(id, &profile);
	if (rc != 0) {
		return rc;
	}

	return validate_laser_settings(profile, settings);
}

static bool laser_driver_settings_differ(const struct app_laser_channel_settings *a,
					 const struct app_laser_channel_settings *b)
{
	return a->properties.max_current_ma != b->properties.max_current_ma ||
	       a->current_set_calibration_pct != b->current_set_calibration_pct ||
	       a->properties.operating_temp_c != b->properties.operating_temp_c ||
	       a->properties.tec_max_current_a != b->properties.tec_max_current_a ||
	       a->properties.tec_pid.kp != b->properties.tec_pid.kp ||
	       a->properties.tec_pid.ki != b->properties.tec_pid.ki ||
	       a->properties.tec_pid.kd != b->properties.tec_pid.kd;
}

int hispec_laser_get_channel_settings(enum hispec_laser_id id,
				      struct app_laser_channel_settings *out)
{
	if (out == NULL || id < 0 || id >= HISPEC_LASER_COUNT) {
		return -EINVAL;
	}

	k_mutex_lock(&laser_state_lock, K_FOREVER);
	if (!laser_runtime_initialized) {
		k_mutex_unlock(&laser_state_lock);
		return -EINVAL;
	}
	*out = laser_settings[id];
	k_mutex_unlock(&laser_state_lock);
	return 0;
}

int hispec_laser_update_channel_settings(enum hispec_laser_id id,
					 const struct app_laser_channel_settings *requested_settings,
					 bool persist)
{
	const struct hispec_laser_driver_profile *profile;
	struct app_laser_channel_settings normalized = *requested_settings;
	const struct app_laser_channel_settings *settings = &normalized;
	struct app_laser_channel_settings previous;
	maiman_driver_t drv = {0};
	bool apply_driver;
	bool range_changed;
	bool stop_emission;
	bool settings_applied = false;
	bool was_powered = false;
	int rc;
	int power_restore_rc = 0;

	rc = profile_for_id(id, &profile);
	if (rc != 0) {
		return rc;
	}

	rc = validate_laser_settings(profile, &normalized);
	if (rc != 0) {
		return rc;
	}

	/* Settings updates may reprogram drivers; keep command wait bounded. */
	rc = laser_io_lock_with_timeout(K_MSEC(LASER_COMMAND_LOCK_TIMEOUT_MS));
	if (rc != 0) {
		return rc;
	}
	ensure_laser_runtime_settings_locked();
	previous = laser_settings[id];
	apply_driver = laser_driver_settings_differ(&previous, settings) ||
		previous.expected_serial != settings->expected_serial;
	range_changed = previous.properties.operating_temp_range_c.min_c != settings->properties.operating_temp_range_c.min_c ||
		previous.properties.operating_temp_range_c.max_c != settings->properties.operating_temp_range_c.max_c;
	stop_emission = apply_driver || range_changed ||
		previous.properties.threshold_current_ma != settings->properties.threshold_current_ma ||
		previous.properties.nominal_current_ma != settings->properties.nominal_current_ma ||
		previous.properties.efficiency_mw_per_ma != settings->properties.efficiency_mw_per_ma ||
		previous.properties.wavelength_nm != settings->properties.wavelength_nm ||
		previous.properties.dlambda_dT_nm_per_k != settings->properties.dlambda_dT_nm_per_k ||
		previous.properties.dlambda_dA_nm_per_ma != settings->properties.dlambda_dA_nm_per_ma ||
		previous.min_autolevel_current_ma != settings->min_autolevel_current_ma;

	/* Stop before changing the model or operating envelope. For app-only/range
	 * updates this never powers an idle bank merely to program settings.
	 */
	if (stop_emission) {
		rc = stop_output_locked(profile, settings->disable_tec_at_autooff);
		if (rc != 0) {
			goto out_unlock;
		}
	}

	if (apply_driver) {
		if (bank_power_mode == HISPEC_LASER_BANK_POWER_OVERRIDE_OFF) {
			rc = -EPERM;
			goto out_unlock;
		}

		was_powered = bank_power_requested_enabled;
		rc = bank_power_set_locked(true, NULL, false);
		if (rc != 0) {
			goto out_unlock;
		}

		rc = stop_output_locked(profile, settings->disable_tec_at_autooff);
		if (rc != 0) {
			goto restore_power;
		}

		maiman_init(&drv, profile->node_id);
		rc = verify_driver_locked(profile, &drv, NULL, settings->expected_serial);
		if (rc == 0) {
			rc = apply_runtime_profile_locked(profile, &drv, settings);
		}
		settings_applied = rc == 0;

restore_power:
		if (!was_powered) {
			power_restore_rc = bank_power_set_locked(false, NULL, false);
			if (rc == 0) {
				rc = power_restore_rc;
			}
		}
	} else {
		settings_applied = true;
	}

out_unlock:
	if (settings_applied) {
		k_mutex_lock(&laser_state_lock, K_FOREVER);
		/* STOP may have just committed emission time after the caller's copy. */
		normalized.total_emitting_s = laser_settings[id].total_emitting_s;
		laser_settings[id] = *settings;
		if (range_changed && !apply_driver) {
			/* Apply new bounds/default target at the next preparation/start. */
			laser_output_estimate[id].prepared = false;
		}
		k_mutex_unlock(&laser_state_lock);
	}
	if (apply_driver) {
		if (rc != 0) {
			invalidate_output_locked(id);
		}
	}

	if (settings_applied) {
		/* A failed restore-to-off is reported because the bank state needs
		 * attention, but it does not undo successfully programmed settings.
		 */
		(void)app_settings_update_laser_channel((uint8_t)id, settings, persist);
	}
	laser_note_communication_locked(id, &drv);
	k_mutex_unlock(&laser_io_lock);
	return rc;
}

int hispec_laser_set_tune_delta_nm(enum hispec_laser_id id, double delta_nm,
				   bool persist)
{
	struct app_laser_channel_settings settings;
	int rc;

	if (!float_is_valid(delta_nm)) {
		return -EINVAL;
	}
	rc = hispec_laser_get_channel_settings(id, &settings);
	if (rc != 0) {
		return rc;
	}
	/* Store the tuning request. It is applied later by positive laser-level
	 * commands, not immediately written to TEC/current registers here.
	 */
	settings.tune_delta_nm = delta_nm;
	return hispec_laser_update_channel_settings(id, &settings, persist);
}

double hispec_laser_get_tune_delta_nm(enum hispec_laser_id id)
{
	double value = LASERPROP_NA;

	k_mutex_lock(&laser_state_lock, K_FOREVER);
	if (id >= 0 && id < HISPEC_LASER_COUNT) {
		value = laser_settings[id].tune_delta_nm;
	}
	k_mutex_unlock(&laser_state_lock);
	return value;
}

static void hispec_laser_service_autooff(void)
{
	int64_t now = k_uptime_get();

	for (uint8_t i = 0U; i < HISPEC_LASER_COUNT; ++i) {
		bool expired = false;
		bool stop_tec = false;

		k_mutex_lock(&laser_state_lock, K_FOREVER);
		expired = laser_autooff_deadline_ms[i] > 0 &&
			  now >= laser_autooff_deadline_ms[i];
		stop_tec = laser_settings[i].disable_tec_at_autooff;
		k_mutex_unlock(&laser_state_lock);

		if (expired) {
			(void)hispec_laser_stop_output((enum hispec_laser_id)i, stop_tec);
		}
	}
}

static void laser_autooff_work_handler(struct k_work *work)
{
	ARG_UNUSED(work);

	hispec_laser_service_autooff();
	for (uint8_t i = 0; i < HISPEC_LASER_COUNT; ++i) {
		bool active, fault = false;
		k_mutex_lock(&laser_state_lock, K_FOREVER);
		struct laser_output_estimate_state *state = &laser_output_estimate[i];
		active = bank_power_requested_enabled && laser_output_estimate[i].started;
		if (active && k_uptime_get() >= state->response_deadline_ms && !state->communication_fault) {
			state->communication_fault = true;
			fault = true;
		}
		k_mutex_unlock(&laser_state_lock);
		if (fault) laser_health_warning(i, "laser_communication_fault", "laser response timeout", -ETIMEDOUT);
		if (!active || laser_io_lock_with_timeout(K_NO_WAIT) != 0) continue;
		/* Recheck after serialization: a foreground stop may have won the bus. */
		if (bank_power_requested_enabled && laser_output_estimate[i].started) {
			maiman_driver_t drv;
			bool started;
			maiman_init(&drv, laser_profiles[i].node_id);
			bool ok = maiman_read_tec_started(&drv, &started);
			laser_note_communication_locked(i, &drv);
			if (ok && !started) invalidate_output_locked(i);
		}
		k_mutex_unlock(&laser_io_lock);
	}
	laser_autooff_reschedule();
}

double hispec_laser_estimate_power_mw(const laserprops_t *properties, double current_ma)
{
	double power_mw;

	if (properties == NULL || !float_is_valid(current_ma) ||
	    !float_is_positive(properties->efficiency_mw_per_ma)) {
		return LASERPROP_NA;
	}

	power_mw = (current_ma - properties->threshold_current_ma) *
		   properties->efficiency_mw_per_ma;
	return (power_mw > 0.0) ? power_mw : 0.0;
}

int laser_estimate_flux(enum hispec_laser_id id,
			struct hispec_laser_flux_estimate *out)
{
	laserprops_t properties;
	double current_ma;
	double tec_temperature_c;
	double power_mw;
	double power_w;
	double photon_j;
	double wavelength_m;
	double power_err_mw;
	double fractional_noise;
	double constant_noise_mw;

	if (out == NULL || id < 0 || id >= HISPEC_LASER_COUNT) {
		return -EINVAL;
	}

	k_mutex_lock(&laser_state_lock, K_FOREVER);
	if (!laser_runtime_initialized) {
		k_mutex_unlock(&laser_state_lock);
		return -EINVAL;
	}
	properties = laser_settings[id].properties;
	fractional_noise = laser_settings[id].fractional_noise;
	constant_noise_mw = laser_settings[id].constant_noise_mw;
	current_ma = laser_output_estimate[id].current_ma;
	tec_temperature_c = laser_output_estimate[id].tec_temperature_c;
	k_mutex_unlock(&laser_state_lock);

	if (!float_is_valid(current_ma) ||
	    !float_is_valid(tec_temperature_c) ||
	    !float_is_valid(fractional_noise) ||
	    !float_is_valid(constant_noise_mw) ||
	    fractional_noise < 0.0 || constant_noise_mw < 0.0 ||
	    !float_is_valid(properties.wavelength_nm) ||
	    properties.wavelength_nm <= 0.0) {
		return -EINVAL;
	}

	memset(out, 0, sizeof(*out));
	power_mw = hispec_laser_estimate_power_mw(&properties, current_ma);
	out->current_ma = current_ma;
	out->tec_temperature_c = tec_temperature_c;
	out->wavelength_nm = hispec_laser_estimate_wavelength_nm(&properties,
								 tec_temperature_c,
								 current_ma);
	if (!(out->wavelength_nm > 0.0)) {
		return -EINVAL;
	}
	wavelength_m = out->wavelength_nm * 1.0e-9;
	photon_j = PLANCK_J_S * LIGHT_M_PER_S / wavelength_m;
	power_w = power_mw * 1.0e-3;
	power_err_mw = hypot(power_mw * fractional_noise, constant_noise_mw);

	out->power_mw = power_mw;
	out->power_err_mw = power_err_mw;
	out->flux_ph_s = power_w / photon_j;
	out->flux_err_ph_s = (power_err_mw * 1.0e-3) / photon_j;
	return 0;
}

double hispec_laser_current_on_time_s(enum hispec_laser_id id)
{
	double value;

	k_mutex_lock(&laser_state_lock, K_FOREVER);
	value = on_time_runtime_seconds_locked(laser_current_runtime,
					       ARRAY_SIZE(laser_current_runtime), id);
	k_mutex_unlock(&laser_state_lock);

	return value;
}

double hispec_laser_estimate_wavelength_nm(const laserprops_t *properties,
					  double tec_temperature_c,
					  double current_ma)
{
	double delta_i_ma;
	double delta_t_c;

	if (properties == NULL || !float_is_valid(tec_temperature_c) ||
	    !float_is_valid(current_ma) ||
	    !float_is_valid(properties->dlambda_dT_nm_per_k) ||
	    !float_is_valid(properties->dlambda_dA_nm_per_ma)) {
		return LASERPROP_NA;
	}
	if (current_ma == 0.0) {
		return properties->wavelength_nm;
	}

	delta_i_ma = current_ma - properties->nominal_current_ma;
	delta_t_c = tec_temperature_c - properties->operating_temp_c;
	return properties->wavelength_nm +
	       delta_t_c * properties->dlambda_dT_nm_per_k +
	       delta_i_ma * properties->dlambda_dA_nm_per_ma;
}

int hispec_laser_tune_wavelength(enum hispec_laser_id id,
				 const struct hispec_laser_tune_request *request,
				 struct hispec_laser_tune_result *result)
{
	const struct hispec_laser_driver_profile *profile;
	struct app_laser_channel_settings settings;
	const laserprops_t *props = &settings.properties;
	double brightness;
	double current_range_ma;
	double desired_i_ma;
	double initial_wavelength_nm;
	double delta_lambda_nm;
	double target_temp_c;
	double delta_from_temp_nm;
	double delta_remaining_nm;
	double delta_i_ma;
	double allowed_delta_i_ma;
	double target_current_ma;
	double delta_from_current_nm;
	double estimated_wavelength_nm;
	double estimated_power_mw;
	bool temp_clamped = false;
	bool current_clamped = false;
	int rc;

	if (request == NULL || result == NULL) {
		return -EINVAL;
	}

	rc = profile_for_id(id, &profile);
	if (rc != 0) {
		return rc;
	}
	rc = hispec_laser_get_channel_settings(id, &settings);
	if (rc != 0) {
		return rc;
	}

	memset(result, 0, sizeof(*result));
	result->requested_wavelength_nm = request->wavelength_nm;

	if (!float_is_valid(request->desired_power_percent) ||
	    !float_is_valid(request->wavelength_nm) ||
	    !float_is_valid(request->maximum_power_shift_percent) ||
	    request->desired_power_percent < 0.0 ||
	    request->desired_power_percent > 100.0 ||
	    request->maximum_power_shift_percent < 0.0) {
		return -ERANGE;
	}

	if (request->desired_power_percent == 0.0) {
		result->target_current_ma = 0.0;
		result->target_temperature_c = props->operating_temp_c;
		result->estimated_power_mw = 0.0;
		result->estimated_wavelength_nm = props->wavelength_nm;
		result->wavelength_error_nm = request->wavelength_nm - props->wavelength_nm;
		if (request->apply) {
			return hispec_laser_set_current_ma(id, 0.0);
		}
		return 0;
	}

	if ((request->use_temperature && !float_is_nonzero(props->dlambda_dT_nm_per_k)) ||
	    (request->use_current && !float_is_nonzero(props->dlambda_dA_nm_per_ma))) {
		return -EINVAL;
	}

	current_range_ma = props->nominal_current_ma - props->threshold_current_ma;
	if (!float_is_positive(current_range_ma)) {
		return -EINVAL;
	}

	brightness = request->desired_power_percent / 100.0;
	desired_i_ma = props->threshold_current_ma + current_range_ma * brightness;
	desired_i_ma = clamp_with_flag(desired_i_ma, props->threshold_current_ma,
					props->max_current_ma, &current_clamped);

	initial_wavelength_nm =
		(desired_i_ma - props->nominal_current_ma) *
		props->dlambda_dA_nm_per_ma + props->wavelength_nm;

	delta_lambda_nm = request->wavelength_nm - initial_wavelength_nm;
	if (request->use_temperature) {
		target_temp_c = props->operating_temp_c +
				delta_lambda_nm / props->dlambda_dT_nm_per_k;
		target_temp_c = clamp_with_flag(target_temp_c,
						 props->operating_temp_range_c.min_c,
						 props->operating_temp_range_c.max_c,
						 &temp_clamped);
		delta_from_temp_nm =
			props->dlambda_dT_nm_per_k *
			(target_temp_c - props->operating_temp_c);
	} else {
		target_temp_c = props->operating_temp_c;
		delta_from_temp_nm = 0.0;
	}

	delta_remaining_nm = delta_lambda_nm - delta_from_temp_nm;
	if (request->use_current) {
		delta_i_ma = delta_remaining_nm / props->dlambda_dA_nm_per_ma;
		allowed_delta_i_ma =
			(request->maximum_power_shift_percent / 100.0) * current_range_ma;
		if (delta_i_ma > allowed_delta_i_ma) {
			delta_i_ma = allowed_delta_i_ma;
			current_clamped = true;
		}
		if (delta_i_ma < -allowed_delta_i_ma) {
			delta_i_ma = -allowed_delta_i_ma;
			current_clamped = true;
		}
		target_current_ma = desired_i_ma + delta_i_ma;
		target_current_ma = clamp_with_flag(target_current_ma,
						    props->threshold_current_ma,
						    props->max_current_ma,
						    &current_clamped);
	} else {
		target_current_ma = desired_i_ma;
	}

	/* Report and apply the same register-representable current, including in
	 * a dry-run tune result. Limits must survive the driver's rounding.
	 */
	target_current_ma = hispec_laser_quantize_current_ma(target_current_ma,
		props->threshold_current_ma, props->max_current_ma);
	if (!isfinite(target_current_ma)) {
		return -ERANGE;
	}
	delta_from_current_nm = (target_current_ma - props->nominal_current_ma) *
		props->dlambda_dA_nm_per_ma;
	estimated_wavelength_nm = props->wavelength_nm +
				  delta_from_temp_nm + delta_from_current_nm;
	estimated_power_mw =
		hispec_laser_estimate_power_mw(props, target_current_ma);

	result->target_current_ma = target_current_ma;
	result->target_temperature_c = target_temp_c;
	result->estimated_power_mw = estimated_power_mw;
	result->estimated_wavelength_nm = estimated_wavelength_nm;
	result->wavelength_error_nm = request->wavelength_nm - estimated_wavelength_nm;
	result->power_error_mw =
		hispec_laser_estimate_power_mw(props, desired_i_ma) - estimated_power_mw;
	result->temperature_clamped = temp_clamped;
	result->current_clamped = current_clamped;

	if (request->apply && target_current_ma == 0.0) {
		return hispec_laser_set_current_ma(id, 0.0);
	}
	if (request->apply) {
		maiman_driver_t drv = {0};

		/* Applying a tune point writes TEC/current registers; use bounded wait. */
		rc = laser_io_lock_with_timeout(K_MSEC(LASER_COMMAND_LOCK_TIMEOUT_MS));
		if (rc != 0) {
			return rc;
		}
		const laserprops_t *current = runtime_props_locked(id);
		if (target_current_ma > current->max_current_ma ||
		    target_temp_c < current->operating_temp_range_c.min_c ||
		    target_temp_c > current->operating_temp_range_c.max_c) {
			k_mutex_unlock(&laser_io_lock);
			return -ERANGE;
		}
		bool running = output_ready_locked(id);
		bool current_changed = target_current_ma != laser_output_estimate[id].current_ma;
		bool temperature_changed = target_temp_c != laser_output_estimate[id].tec_temperature_c;
		if (running && !current_changed && !temperature_changed) {
			/* Auto-off rearming belongs to the calling level operation. */
			k_mutex_unlock(&laser_io_lock);
			return 0;
		}
		if (running) {
			maiman_init(&drv, profile->node_id);
		} else {
			rc = prepare_to_operate_locked(profile, &drv);
		}
		if (rc == 0 &&
		    (((!running || temperature_changed) &&
		      !maiman_set_tec_temperature(&drv, target_temp_c)) ||
		     ((!running || current_changed) && !maiman_set_current(&drv, target_current_ma)) ||
		     ((!laser_output_estimate[id].started || !laser_output_estimate[id].valid) && !maiman_start_device(&drv)))) {
			LOG_WRN("Laser %s tune apply failed temp=%.3fC current=%.3fmA",
				profile->name, (double)target_temp_c,
				(double)target_current_ma);
			rc = -EIO;
		}
		if (rc == 0) {
			LOG_DBG("Laser %s tune applied temp=%.3fC current=%.3fmA",
				profile->name, (double)target_temp_c,
				(double)target_current_ma);
			k_mutex_lock(&laser_state_lock, K_FOREVER);
			on_time_runtime_update_locked(laser_current_runtime,
						      ARRAY_SIZE(laser_current_runtime),
						      id, true);
			on_time_runtime_update_locked(laser_tec_runtime,
						      ARRAY_SIZE(laser_tec_runtime),
						      id, true);
			output_estimate_set_locked(id, target_current_ma, target_temp_c);
			laser_output_estimate[id].started = true;
			k_mutex_unlock(&laser_state_lock);
		} else {
			invalidate_output_locked(id);
		}
		laser_note_communication_locked(id, &drv);
		k_mutex_unlock(&laser_io_lock);
		laser_autooff_reschedule();
		return rc;
	}

	return 0;
}
