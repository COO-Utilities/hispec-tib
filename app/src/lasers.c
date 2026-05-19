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
#include "devices.h"

#include <errno.h>
#include <math.h>
#include <stddef.h>
#include <string.h>

#include <zephyr/drivers/gpio.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/util.h>

LOG_MODULE_REGISTER(lasers, LOG_LEVEL_INF);

#define PLANCK_J_S 6.62607015e-34
#define LIGHT_M_PER_S 299792458.0

#define LASER_AUTOFF_NO_DEADLINE 0LL

/* One mutex protects the shared RS-485 bus sequencing and bank-power GPIO.
 * k_mutex_lock() sleeps the calling thread instead of busy-waiting while another
 * command is talking to the Maiman modules.
 */
static K_MUTEX_DEFINE(laser_lock);

struct on_time_runtime {
	bool active;
	int64_t started_ms;
	int64_t accumulated_ms;
};

struct laser_output_estimate_state {
	float current_ma;
	float tec_temperature_c;
	bool valid;
};

static struct on_time_runtime laser_current_runtime[HISPEC_LASER_COUNT];
static struct on_time_runtime laser_tec_runtime[HISPEC_LASER_COUNT];
static struct app_laser_channel_settings laser_settings[HISPEC_LASER_COUNT];
static struct laser_output_estimate_state laser_output_estimate[HISPEC_LASER_COUNT];
static int64_t laser_autooff_deadline_ms[HISPEC_LASER_COUNT];
static int64_t bank_power_started_ms;
static bool laser_runtime_initialized;
static enum hispec_laser_bank_power_mode bank_power_mode = HISPEC_LASER_BANK_POWER_AUTO;

static const struct hispec_laser_driver_profile laser_profiles[] = {
	[HISPEC_LASER_1028_Y] = {
		.id = HISPEC_LASER_1028_Y,
		.name = "1028y",
		.modbus_name = "1028",
		.node_id = 5U,
		.expected_device_id = 0x1113,
		.expected_serial = 8229U,
		.properties = &LASER_1028,
	},
	[HISPEC_LASER_1270_J] = {
		.id = HISPEC_LASER_1270_J,
		.name = "1270j",
		.modbus_name = "1270",
		.node_id = 6U,
		.expected_device_id = 0x1113,
		.expected_serial = 8228U,
		.properties = &LASER_1270,
	},
	[HISPEC_LASER_1430_YJ] = {
		.id = HISPEC_LASER_1430_YJ,
		.name = "1430yj",
		.modbus_name = "yj1430",
		.node_id = 4U,
		.expected_device_id = 0x1113,
		.expected_serial = 8222U,
		.properties = &LASER_1430,
	},
	[HISPEC_LASER_1430_HK] = {
		.id = HISPEC_LASER_1430_HK,
		.name = "1430hk",
		.modbus_name = "hk1430",
		.node_id = 3U,
		.expected_device_id = 0x1113,
		.expected_serial = 8227U,
		.properties = &LASER_1430,
	},
	[HISPEC_LASER_1510_H] = {
		.id = HISPEC_LASER_1510_H,
		.name = "1510h",
		.modbus_name = "1510",
		.node_id = 1U,
		.expected_device_id = 0x1113,
		.expected_serial = 8225U,
		.properties = &LASER_1510,
	},
	[HISPEC_LASER_2330_K] = {
		.id = HISPEC_LASER_2330_K,
		.name = "2330k",
		.modbus_name = "2330",
		.node_id = 2U,
		.expected_device_id = 0x1113,
		.expected_serial = 8226U,
		.properties = &LASER_2330,
	},
};

BUILD_ASSERT(ARRAY_SIZE(laser_profiles) == HISPEC_LASER_COUNT,
	     "Laser profile table must match hispec_laser_id");

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

	/* app_settings_init() owns loading defaults plus persisted values. The
	 * laser module pulls the completed snapshot on first use so boot code does
	 * not need a laser-specific post-settings hook and app_settings.c stays a
	 * storage layer instead of calling into hardware/domain modules.
	 */
	app_settings_get_laser(&stored);
	for (uint8_t i = 0U; i < HISPEC_LASER_COUNT; ++i) {
		laser_settings[i] = stored.channel[i];
		laser_output_estimate[i].current_ma = 0.0f;
		laser_output_estimate[i].tec_temperature_c =
			stored.channel[i].properties.operating_temp_c;
		laser_output_estimate[i].valid = true;
		laser_autooff_deadline_ms[i] = LASER_AUTOFF_NO_DEADLINE;
	}

	laser_runtime_initialized = true;
}

static void output_estimate_set_locked(enum hispec_laser_id id,
				       float current_ma,
				       float tec_temperature_c)
{
	if (id < 0 || id >= HISPEC_LASER_COUNT) {
		return;
	}

	laser_output_estimate[id].current_ma = current_ma;
	laser_output_estimate[id].tec_temperature_c = tec_temperature_c;
	laser_output_estimate[id].valid = true;
}

static const laserprops_t *runtime_props_locked(enum hispec_laser_id id)
{
	ensure_laser_runtime_settings_locked();
	if (id < 0 || id >= HISPEC_LASER_COUNT) {
		return NULL;
	}
	return &laser_settings[id].properties;
}

static bool float_is_valid(float value)
{
	return value == value;
}

static bool float_is_positive(float value)
{
	return float_is_valid(value) && value > 0.0f;
}

static bool float_is_nonzero(float value)
{
	return float_is_valid(value) && value != 0.0f;
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

	runtime = &runtimes[index];
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

static float on_time_runtime_seconds_locked(const struct on_time_runtime *runtimes,
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

	return (float)ms / 1000.0f;
}

static void commit_current_runtime_locked(enum hispec_laser_id id, bool persist)
{
	double total;

	if (id < 0 || id >= HISPEC_LASER_COUNT) {
		return;
	}

	total = laser_settings[id].total_emitting_s +
		(double)on_time_runtime_seconds_locked(laser_current_runtime,
						       ARRAY_SIZE(laser_current_runtime),
						       id);
	laser_settings[id].total_emitting_s = total;
	laser_current_runtime[id].active = false;
	laser_current_runtime[id].started_ms = 0;
	laser_current_runtime[id].accumulated_ms = 0;
	(void)app_settings_update_laser_total_emitting((uint8_t)id, total, persist);
}

static float clampf_with_flag(float value, float min_value, float max_value, bool *clamped)
{
	float out = value;

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

const laserprops_t *hispec_laser_properties(enum hispec_laser_id id)
{
	const laserprops_t *props;

	k_mutex_lock(&laser_lock, K_FOREVER);
	props = runtime_props_locked(id);
	k_mutex_unlock(&laser_lock);
	return props;
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

static void bank_power_runtime_note_locked(bool enabled, int64_t now_ms)
{
	if (enabled && bank_power_started_ms <= 0) {
		bank_power_started_ms = now_ms;
	} else if (!enabled) {
		bank_power_started_ms = 0;
	}
}

static int bank_power_read_locked(bool *enabled)
{
	int val;

	if (enabled == NULL) {
		return -EINVAL;
	}

	val = gpio_pin_get_dt(&laser_power_gpio);
	if (val < 0) {
		return val;
	}

	*enabled = val > 0;
	bank_power_runtime_note_locked(*enabled, k_uptime_get());
	return 0;
}

static bool bank_power_is_enabled_locked(void)
{
	bool enabled = false;

	(void)bank_power_read_locked(&enabled);
	return enabled;
}

static int zero_all_driver_currents_locked(bool stop_tecs)
{
	int first_rc = 0;

	ensure_laser_runtime_settings_locked();
	if (!bank_power_is_enabled_locked()) {
		return 0;
	}

	for (uint8_t i = 0U; i < HISPEC_LASER_COUNT; ++i) {
		maiman_driver_t drv;

		maiman_init(&drv, laser_profiles[i].node_id);
		if (!maiman_set_current(&drv, 0.0f) || !maiman_stop_device(&drv)) {
			first_rc = first_rc == 0 ? -EIO : first_rc;
		}
		if (stop_tecs && !maiman_stop_tec(&drv)) {
			first_rc = first_rc == 0 ? -EIO : first_rc;
		}
			commit_current_runtime_locked((enum hispec_laser_id)i, true);
			output_estimate_set_locked((enum hispec_laser_id)i, 0.0f,
						   laser_output_estimate[i].tec_temperature_c);
			laser_autooff_deadline_ms[i] = LASER_AUTOFF_NO_DEADLINE;
			if (stop_tecs) {
				on_time_runtime_update_locked(laser_tec_runtime,
							      ARRAY_SIZE(laser_tec_runtime),
						      (enum hispec_laser_id)i, false);
		}
	}

	return first_rc;
}

bool hispec_laser_bank_power_is_enabled(void)
{
	bool enabled;

	k_mutex_lock(&laser_lock, K_FOREVER);
	enabled = bank_power_is_enabled_locked();
	k_mutex_unlock(&laser_lock);

	return enabled;
}

enum hispec_laser_bank_power_mode hispec_laser_bank_power_mode_get(void)
{
	enum hispec_laser_bank_power_mode mode;

	k_mutex_lock(&laser_lock, K_FOREVER);
	mode = bank_power_mode;
	k_mutex_unlock(&laser_lock);
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

static int bank_power_set_locked(bool enabled, bool *transitioned)
{
	bool was_enabled;
	int rc;
	int zero_rc = 0;

	if (transitioned != NULL) {
		*transitioned = false;
	}

	was_enabled = bank_power_is_enabled_locked();
	if (was_enabled == enabled) {
		return 0;
	}

	if (!enabled && was_enabled) {
		zero_rc = zero_all_driver_currents_locked(true);
	}

	/* gpio_pin_set_dt() writes the logical active state described by
	 * laser_power_gpios in devicetree.
	 */
	rc = gpio_pin_set_dt(&laser_power_gpio, enabled ? 1 : 0);
	if (rc != 0) {
		return rc;
	}
	bank_power_runtime_note_locked(enabled, k_uptime_get());

	if (transitioned != NULL) {
		*transitioned = true;
	}
	if (enabled) {
		/* Let the Maiman modules boot before the caller starts Modbus IO. */
		k_sleep(K_MSEC(HISPEC_LASER_BANK_BOOT_DELAY_MS));
	} else {
		for (uint8_t i = 0U; i < HISPEC_LASER_COUNT; ++i) {
			commit_current_runtime_locked((enum hispec_laser_id)i, true);
			on_time_runtime_update_locked(laser_tec_runtime,
						      ARRAY_SIZE(laser_tec_runtime),
						      (enum hispec_laser_id)i, false);
		}
	}

	return zero_rc;
}

int hispec_laser_bank_power_set(bool enabled, bool *transitioned)
{
	int rc;

	k_mutex_lock(&laser_lock, K_FOREVER);
	if (enabled && bank_power_mode == HISPEC_LASER_BANK_POWER_OVERRIDE_OFF) {
		rc = -EPERM;
	} else {
		rc = bank_power_set_locked(enabled, transitioned);
	}
	k_mutex_unlock(&laser_lock);
	if (rc == 0 && !enabled) {
		laser_autooff_reschedule();
	}

	return rc;
}

uint32_t hispec_laser_bank_power_on_duration_s(void)
{
	uint32_t duration_s = 0U;
	bool bank_powered = false;
	int64_t now_ms;

	k_mutex_lock(&laser_lock, K_FOREVER);
	now_ms = k_uptime_get();
	(void)bank_power_read_locked(&bank_powered);
	if (bank_powered && bank_power_started_ms > 0) {
		int64_t elapsed_s = (now_ms - bank_power_started_ms) / 1000;

		duration_s = elapsed_s > UINT32_MAX ? UINT32_MAX : (uint32_t)elapsed_s;
	}
	k_mutex_unlock(&laser_lock);

	return duration_s;
}

int hispec_laser_bank_power_mode_set(enum hispec_laser_bank_power_mode mode)
{
	int rc = 0;

	if (mode < HISPEC_LASER_BANK_POWER_AUTO ||
	    mode > HISPEC_LASER_BANK_POWER_OVERRIDE_OFF) {
		return -EINVAL;
	}

	k_mutex_lock(&laser_lock, K_FOREVER);
	bank_power_mode = mode;
	if (mode == HISPEC_LASER_BANK_POWER_OVERRIDE_ON) {
		rc = bank_power_set_locked(true, NULL);
	} else if (mode == HISPEC_LASER_BANK_POWER_OVERRIDE_OFF) {
		rc = bank_power_set_locked(false, NULL);
	}
	k_mutex_unlock(&laser_lock);

	return rc;
}

static int ensure_bank_powered_locked(void)
{
	if (bank_power_mode == HISPEC_LASER_BANK_POWER_OVERRIDE_OFF) {
		return -EPERM;
	}
	return bank_power_set_locked(true, NULL);
}

int hispec_laser_bank_clear_faults(uint32_t off_ms, uint32_t *actual_off_ms)
{
	uint32_t delay_ms = (off_ms == 0U) ? HISPEC_LASER_BANK_FAULT_CLEAR_OFF_MS : off_ms;
	bool fault = false;
	int rc;

	if (actual_off_ms != NULL) {
		*actual_off_ms = 0U;
	}

	k_mutex_lock(&laser_lock, K_FOREVER);

	if (!bank_power_is_enabled_locked()) {
		rc = 0;
		goto out;
	}

	for (uint8_t i = 0U; i < HISPEC_LASER_COUNT; ++i) {
		maiman_driver_t drv;
		uint16_t lock_status;

		maiman_init(&drv, laser_profiles[i].node_id);
		lock_status = maiman_get_raw_lock_status(&drv);
		if ((lock_status & LOCK_STATE_LD_OVERCURRENT) != 0U) {
			fault = true;
			break;
		}
	}

	if (!fault) {
		rc = 0;
		goto out;
	}

	rc = bank_power_set_locked(false, NULL);
	if (rc != 0) {
		goto out;
	}

	/* k_sleep() yields during the supply-off interval needed to clear the
	 * SF8025 overcurrent latch.
	 */
	k_sleep(K_MSEC(delay_ms));
	rc = bank_power_set_locked(true, NULL);
	if (rc == 0 && actual_off_ms != NULL) {
		*actual_off_ms = delay_ms;
	}

out:
	k_mutex_unlock(&laser_lock);
	return rc;
}

static float level_percent_for_current(const laserprops_t *props, float current_ma)
{
	float range;

	if (props == NULL || !float_is_valid(current_ma)) {
		return LASERPROP_NA;
	}
	range = props->nominal_current_ma - props->threshold_current_ma;
	if (range <= 0.0f) {
		return LASERPROP_NA;
	}
	if (current_ma <= 0.0f) {
		return 0.0f;
	}
	return 100.0f * (current_ma - props->threshold_current_ma) / range;
}

int hispec_laser_bank_read_temperatures(
	struct hispec_laser_channel_temperature channels[HISPEC_LASER_COUNT])
{
	int rc;

	if (channels == NULL) {
		return -EINVAL;
	}

	memset(channels, 0,
	       sizeof(struct hispec_laser_channel_temperature) * HISPEC_LASER_COUNT);
	for (uint8_t i = 0U; i < HISPEC_LASER_COUNT; ++i) {
		channels[i].id = laser_profiles[i].id;
		channels[i].tec_temperature_c = LASERPROP_NA;
	}

	k_mutex_lock(&laser_lock, K_FOREVER);
	if (!bank_power_is_enabled_locked()) {
		rc = 0;
		goto out_unlock;
	}

	for (uint8_t i = 0U; i < HISPEC_LASER_COUNT; ++i) {
		maiman_driver_t drv;
		float tec_temp = LASERPROP_NA;
		bool tec_started = false;
		bool temp_ok;
		bool state_ok;

		maiman_init(&drv, laser_profiles[i].node_id);
		temp_ok = maiman_read_tec_temperature_measured(&drv, &tec_temp);
		state_ok = maiman_read_tec_started(&drv, &tec_started);

		channels[i].valid = temp_ok && state_ok;
		channels[i].tec_temperature_c = tec_temp;
		channels[i].tec_enabled = state_ok && tec_started;
	}

	rc = 0;

out_unlock:
	k_mutex_unlock(&laser_lock);
	return rc;
}

static int verify_driver_locked(const struct hispec_laser_driver_profile *profile,
				maiman_driver_t *drv,
				uint16_t *serial_out)
{
	uint16_t device_id;
	uint16_t serial;

	if (profile == NULL || drv == NULL) {
		return -EINVAL;
	}

	device_id = maiman_get_device_id(drv);
	if (device_id == 0U) {
		return -EIO;
	}
	if (profile->expected_device_id != 0U && device_id != profile->expected_device_id) {
		LOG_ERR("Laser %s node %u device-id mismatch: expected 0x%04x got 0x%04x",
			profile->name, profile->node_id,
			profile->expected_device_id, device_id);
		return -EADDRNOTAVAIL;
	}

	serial = maiman_get_serial_number(drv);
	if (serial == 0U) {
		return -EIO;
	}
	if (serial_out != NULL) {
		*serial_out = serial;
	}
	if (profile->expected_serial != 0U && serial != profile->expected_serial) {
		LOG_ERR("Laser %s node %u serial mismatch: expected %u got %u",
			profile->name, profile->node_id,
			profile->expected_serial, serial);
		return -EADDRNOTAVAIL;
	}

	return 0;
}

int hispec_laser_verify_driver(enum hispec_laser_id id, uint16_t *serial_out)
{
	const struct hispec_laser_driver_profile *profile;
	maiman_driver_t drv;
	int rc;

	rc = profile_for_id(id, &profile);
	if (rc != 0) {
		return rc;
	}

	k_mutex_lock(&laser_lock, K_FOREVER);
	rc = ensure_bank_powered_locked();
	if (rc == 0) {
		maiman_init(&drv, profile->node_id);
		rc = verify_driver_locked(profile, &drv, serial_out);
	}
	k_mutex_unlock(&laser_lock);

	return rc;
}

static int check_ocp_limit_locked(const struct hispec_laser_driver_profile *profile,
				  maiman_driver_t *drv)
{
	float ocp_ma;
	const laserprops_t *props = runtime_props_locked(profile->id);

	ocp_ma = maiman_get_current_protection_threshold(drv);
	if (!float_is_valid(ocp_ma) || ocp_ma < 0.0f) {
		return -EIO;
	}

	if (float_is_valid(props->dne_current_ma) && ocp_ma > props->dne_current_ma) {
		(void)maiman_set_current(drv, 0.0f);
		(void)maiman_stop_device(drv);
		(void)maiman_stop_tec(drv);
		(void)maiman_allow_interlock(drv);
		LOG_ERR("Laser %s driver OCP exceeds diode DNE", profile->name);
		return -ERANGE;
	}

	return 0;
}

static int64_t next_autooff_deadline_locked(void)
{
	int64_t next = LASER_AUTOFF_NO_DEADLINE;

	ensure_laser_runtime_settings_locked();
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

	k_mutex_lock(&laser_lock, K_FOREVER);
	next_deadline = next_autooff_deadline_locked();
	k_mutex_unlock(&laser_lock);

	if (next_deadline == LASER_AUTOFF_NO_DEADLINE) {
		return K_FOREVER;
	}

	wait_ms = next_deadline - k_uptime_get();
	return wait_ms <= 0 ? K_NO_WAIT : K_MSEC(wait_ms);
}

static void laser_autooff_reschedule(void)
{
	const k_timeout_t timeout = laser_autooff_wait_timeout();

	/* This delayable work runs on Zephyr's system workqueue. Auto-off is slow
	 * best-effort cleanup, not a timing path, and the only blocking work here is
	 * the Modbus stop sequence for an expired output.
	 */
	if (K_TIMEOUT_EQ(timeout, K_FOREVER)) {
		(void)k_work_cancel_delayable(&laser_autooff_work);
	} else {
		(void)k_work_reschedule(&laser_autooff_work, timeout);
	}
}

static int apply_runtime_profile_locked(const struct hispec_laser_driver_profile *profile,
					maiman_driver_t *drv)
{
	const laserprops_t *props = runtime_props_locked(profile->id);
	int rc;

	rc = check_ocp_limit_locked(profile, drv);
	if (rc != 0) {
		return rc;
	}

	if (!maiman_set_current_max(drv, props->max_current_ma)) {
		return -EIO;
	}
	if (!maiman_set_current_set_calibration(drv,
						laser_settings[profile->id].current_set_calibration_pct)) {
		return -EIO;
	}
	if (!maiman_set_tec_current_limit(drv, props->tec_max_current_a)) {
		return -EIO;
	}
	if (!maiman_set_tec_pid(drv, props->tec_pid)) {
		return -EIO;
	}
	/* All HISPEC laser operation is continuous-wave. Normalize installed
	 * drivers during profile programming so stale pulse-mode register values
	 * cannot survive a module replacement or manual reconfiguration.
	 */
	if (!maiman_set_frequency(drv, 0.0f) ||
	    !maiman_set_duration(drv, 0.0f)) {
		return -EIO;
	}

	return 0;
}

int hispec_laser_program_driver_profile(enum hispec_laser_id id, bool save_to_eeprom)
{
	const struct hispec_laser_driver_profile *profile;
	maiman_driver_t drv;
	int rc;

	rc = profile_for_id(id, &profile);
	if (rc != 0) {
		return rc;
	}

	k_mutex_lock(&laser_lock, K_FOREVER);
	rc = ensure_bank_powered_locked();
	if (rc != 0) {
		goto out;
	}

	maiman_init(&drv, profile->node_id);
	rc = verify_driver_locked(profile, &drv, NULL);
	if (rc != 0) {
		goto out;
	}

	/* Put the driver in a non-emitting state before rewriting diode limits. */
	(void)maiman_allow_interlock(&drv);
	(void)maiman_set_current(&drv, 0.0f);
	(void)maiman_stop_device(&drv);
	rc = apply_runtime_profile_locked(profile, &drv);
	if (rc != 0) {
		goto out;
	}

	if (save_to_eeprom && !maiman_save_parameters(&drv)) {
		rc = -EIO;
	}

out:
	k_mutex_unlock(&laser_lock);
	return rc;
}

int hispec_laser_save_driver_settings(enum hispec_laser_id id)
{
	const struct hispec_laser_driver_profile *profile;
	maiman_driver_t drv;
	int rc;

	rc = profile_for_id(id, &profile);
	if (rc != 0) {
		return rc;
	}

	k_mutex_lock(&laser_lock, K_FOREVER);
	rc = ensure_bank_powered_locked();
	if (rc == 0) {
		maiman_init(&drv, profile->node_id);
		rc = verify_driver_locked(profile, &drv, NULL);
		if (rc == 0 && !maiman_save_parameters(&drv)) {
			rc = -EIO;
		}
	}
	k_mutex_unlock(&laser_lock);

	return rc;
}

int hispec_laser_reset_driver_settings(enum hispec_laser_id id)
{
	const struct hispec_laser_driver_profile *profile;
	maiman_driver_t drv;
	int rc;

	rc = profile_for_id(id, &profile);
	if (rc != 0) {
		return rc;
	}

	k_mutex_lock(&laser_lock, K_FOREVER);
	rc = ensure_bank_powered_locked();
	if (rc == 0) {
		maiman_init(&drv, profile->node_id);
		rc = verify_driver_locked(profile, &drv, NULL);
		if (rc == 0 && !maiman_reset_parameters(&drv)) {
			rc = -EIO;
		}
	}
	k_mutex_unlock(&laser_lock);

	return rc;
}

static int prepare_to_operate_locked(const struct hispec_laser_driver_profile *profile,
				     maiman_driver_t *drv)
{
	const laserprops_t *props = runtime_props_locked(profile->id);
	float current_temp;
	uint16_t lock_status;
	int rc;

	rc = ensure_bank_powered_locked();
	if (rc != 0) {
		return rc;
	}

	maiman_init(drv, profile->node_id);
	rc = verify_driver_locked(profile, drv, NULL);
	if (rc != 0) {
		return rc;
	}

	rc = apply_runtime_profile_locked(profile, drv);
	if (rc != 0) {
		return rc;
	}

	if (!maiman_set_internal_current_control(drv, true) ||
	    !maiman_set_internal_enable_control(drv, true) ||
	    !maiman_set_internal_tec_temperature_control(drv, true) ||
	    !maiman_set_internal_tec_enable_control(drv, true) ||
	    !maiman_deny_interlock(drv)) {
		return -EIO;
	}

	if (!maiman_is_tec_started(drv)) {
		current_temp = maiman_get_tec_temperature_measured(drv);
		if (!float_is_valid(current_temp) || current_temp < -100.0f) {
			current_temp = props->operating_temp_c;
		}
		if (!maiman_set_tec_temperature(drv, current_temp) ||
		    !maiman_start_tec(drv)) {
			return -EIO;
		}
	}

	lock_status = maiman_get_raw_lock_status(drv);
	if ((lock_status & LOCK_STATE_BLOCKING_MASK) != 0U) {
		return -EIO;
	}

	return 0;
}

static void status_defaults(const struct hispec_laser_driver_profile *profile,
			    struct hispec_laser_status *out)
{
	memset(out, 0, sizeof(*out));
	out->id = profile->id;
	out->name = profile->name;
	out->properties = profile->properties;
	out->expected_device_id = profile->expected_device_id;
	out->expected_serial = profile->expected_serial;
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
	out->tune_delta_nm = 0.0f;
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

int hispec_laser_get_status(enum hispec_laser_id id, struct hispec_laser_status *out)
{
	const struct hispec_laser_driver_profile *profile;
	maiman_driver_t drv;
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

	k_mutex_lock(&laser_lock, K_FOREVER);
	out->properties = runtime_props_locked(id);
	out->current_on_time_s =
		on_time_runtime_seconds_locked(laser_current_runtime,
					       ARRAY_SIZE(laser_current_runtime), id);
	out->tec_on_time_s =
		on_time_runtime_seconds_locked(laser_tec_runtime,
					       ARRAY_SIZE(laser_tec_runtime), id);
	out->total_emitting_s = laser_settings[id].total_emitting_s +
				(double)out->current_on_time_s;
	out->tune_delta_nm = laser_settings[id].tune_delta_nm;
	out->autooff_s = laser_settings[id].autooff_s;
	out->current_set_calibration_pct = laser_settings[id].current_set_calibration_pct;
	out->ntc_t_coefficient_per_c = out->properties->ntc_t_coefficient_per_c;
	if (laser_autooff_deadline_ms[id] > 0) {
		int64_t remaining_ms = laser_autooff_deadline_ms[id] - k_uptime_get();

		out->off_in_s = remaining_ms > 0 ? (remaining_ms + 999) / 1000 : 0;
	}
	out->bank_powered = bank_power_is_enabled_locked();
	if (!out->bank_powered) {
		rc = 0;
		goto out_unlock;
	}

	maiman_init(&drv, profile->node_id);
	out->device_id = maiman_get_device_id(&drv);
	out->serial_number = maiman_get_serial_number(&drv);
	out->serial_matches = (profile->expected_serial == 0U ||
			       out->serial_number == profile->expected_serial);
	if (out->device_id == 0U || out->serial_number == 0U) {
		read_ok = false;
	}

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
	out->ready_to_operate = out->tec_started &&
				((out->lock_status & LOCK_STATE_BLOCKING_MASK) == 0U);

	if (!maiman_get_current(&drv, &out->current_set_ma)) {
		read_ok = false;
	}
	out->level_percent = level_percent_for_current(out->properties, out->current_set_ma);
	out->current_measured_ma = maiman_get_current_measured(&drv);
	out->current_min_ma = maiman_get_current_min(&drv);
	out->current_max_ma = maiman_get_current_max(&drv);
	out->current_max_limit_ma = maiman_get_current_max_limit(&drv);
	out->current_protection_threshold_ma = maiman_get_current_protection_threshold(&drv);
	out->current_set_calibration_pct = maiman_get_current_set_calibration(&drv);
	out->voltage_v = maiman_get_voltage_measured(&drv);
	out->tec_temperature_set_c = maiman_get_tec_temperature_value(&drv);
	out->tec_temperature_measured_c = maiman_get_tec_temperature_measured(&drv);
	out->pcb_temperature_c = maiman_get_pcb_temperature_measured(&drv);
	out->tec_current_measured_a = maiman_get_tec_current_measured(&drv);
	out->tec_current_limit_a = maiman_get_tec_current_limit(&drv);
	out->tec_voltage_v = maiman_get_tec_voltage(&drv);
	out->ntc_t_coefficient_per_c = maiman_get_ntc_b25_100_coefficient(&drv);
	if (!maiman_get_tec_pid(&drv, &out->tec_pid)) {
		read_ok = false;
	}

	out->estimated_power_mw =
		hispec_laser_estimate_power_mw(out->properties, out->current_set_ma);
	out->estimated_wavelength_nm =
		hispec_laser_estimate_wavelength_nm(out->properties,
						    out->tec_temperature_measured_c,
						    out->current_set_ma);
	rc = read_ok ? 0 : -EIO;

out_unlock:
	k_mutex_unlock(&laser_lock);
	return rc;
}

static int stop_output_locked(const struct hispec_laser_driver_profile *profile, bool stop_tec)
{
	maiman_driver_t drv;
	int rc;

	ensure_laser_runtime_settings_locked();
	if (!bank_power_is_enabled_locked()) {
		return 0;
	}

	maiman_init(&drv, profile->node_id);
	rc = verify_driver_locked(profile, &drv, NULL);
	if (rc != 0) {
		return rc;
	}

	if (!maiman_set_current(&drv, 0.0f) || !maiman_stop_device(&drv)) {
		return -EIO;
	}
	if (stop_tec && !maiman_stop_tec(&drv)) {
		return -EIO;
	}

	commit_current_runtime_locked(profile->id, true);
	output_estimate_set_locked(profile->id, 0.0f,
				   laser_output_estimate[profile->id].tec_temperature_c);
	laser_autooff_deadline_ms[profile->id] = LASER_AUTOFF_NO_DEADLINE;
	if (stop_tec) {
		on_time_runtime_update_locked(laser_tec_runtime, ARRAY_SIZE(laser_tec_runtime),
					      profile->id, false);
	}
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

	k_mutex_lock(&laser_lock, K_FOREVER);
	rc = stop_output_locked(profile, stop_tec);
	k_mutex_unlock(&laser_lock);
	if (rc == 0) {
		laser_autooff_reschedule();
	}
	return rc;
}

int hispec_laser_stop_all_outputs(bool stop_tecs)
{
	int rc;

	k_mutex_lock(&laser_lock, K_FOREVER);
	rc = zero_all_driver_currents_locked(stop_tecs);
	k_mutex_unlock(&laser_lock);
	if (rc == 0) {
		laser_autooff_reschedule();
	}
	return rc;
}

int hispec_laser_set_current_ma(enum hispec_laser_id id, float current_ma)
{
	const struct hispec_laser_driver_profile *profile;
	const laserprops_t *props;
	maiman_driver_t drv;
	int rc;

	rc = profile_for_id(id, &profile);
	if (rc != 0) {
		return rc;
	}
	k_mutex_lock(&laser_lock, K_FOREVER);
	props = runtime_props_locked(id);
	k_mutex_unlock(&laser_lock);

	if (!float_is_valid(current_ma) || current_ma < 0.0f ||
	    current_ma > props->max_current_ma) {
		return -ERANGE;
	}

	k_mutex_lock(&laser_lock, K_FOREVER);
	if (current_ma == 0.0f) {
		rc = stop_output_locked(profile, false);
		goto out;
	}

	rc = prepare_to_operate_locked(profile, &drv);
	if (rc != 0) {
		goto out;
	}

	if (!maiman_set_current(&drv, current_ma) || !maiman_start_device(&drv)) {
		rc = -EIO;
	} else {
		on_time_runtime_update_locked(laser_current_runtime,
					      ARRAY_SIZE(laser_current_runtime), id, true);
		on_time_runtime_update_locked(laser_tec_runtime,
					      ARRAY_SIZE(laser_tec_runtime), id, true);
		output_estimate_set_locked(id, current_ma, props->operating_temp_c);
	}

out:
	k_mutex_unlock(&laser_lock);
	return rc;
}

int hispec_laser_set_output_mw(enum hispec_laser_id id, float power_mw)
{
	const laserprops_t *props = hispec_laser_properties(id);
	float current_ma;

	if (props == NULL || !float_is_valid(power_mw) || power_mw < 0.0f) {
		return -EINVAL;
	}

	if (power_mw == 0.0f) {
		return hispec_laser_set_current_ma(id, 0.0f);
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

int hispec_laser_set_output_percent(enum hispec_laser_id id, float percent)
{
	const laserprops_t *props = hispec_laser_properties(id);
	float current_range_ma;
	float current_ma;

	if (props == NULL || !float_is_valid(percent) || percent < 0.0f || percent > 100.0f) {
		return -ERANGE;
	}

	if (percent == 0.0f) {
		return hispec_laser_set_current_ma(id, 0.0f);
	}

	current_range_ma = props->nominal_current_ma - props->threshold_current_ma;
	if (!float_is_positive(current_range_ma)) {
		return -EINVAL;
	}

	current_ma = props->threshold_current_ma + current_range_ma * (percent / 100.0f);
	if (current_ma > props->max_current_ma) {
		current_ma = props->max_current_ma;
	}

	return hispec_laser_set_current_ma(id, current_ma);
}

int hispec_laser_set_output_percent_autooff(enum hispec_laser_id id,
					    float percent,
					    uint32_t autooff_s)
{
	struct app_laser_channel_settings settings;
	const laserprops_t *props;
	int rc;

	if (id < 0 || id >= HISPEC_LASER_COUNT) {
		return -EINVAL;
	}

	k_mutex_lock(&laser_lock, K_FOREVER);
	ensure_laser_runtime_settings_locked();
	settings = laser_settings[id];
	props = &laser_settings[id].properties;
	k_mutex_unlock(&laser_lock);

	if (percent > 0.0f && settings.tune_delta_nm != 0.0f) {
		struct hispec_laser_tune_request request = {
			.desired_power_percent = percent,
			.wavelength_nm = props->wavelength_nm + settings.tune_delta_nm,
			.use_current = true,
			.use_temperature = true,
			.maximum_power_shift_percent = 100.0f,
			.apply = true,
		};
		struct hispec_laser_tune_result result = {0};

		rc = hispec_laser_tune_wavelength(id, &request, &result);
	} else {
		rc = hispec_laser_set_output_percent(id, percent);
	}

	if (rc == 0) {
		k_mutex_lock(&laser_lock, K_FOREVER);
		if (percent > 0.0f) {
			laser_autooff_deadline_ms[id] =
				autooff_s == 0U ? LASER_AUTOFF_NO_DEADLINE :
				k_uptime_get() + (int64_t)autooff_s * 1000LL;
		} else {
			laser_autooff_deadline_ms[id] = LASER_AUTOFF_NO_DEADLINE;
		}
		k_mutex_unlock(&laser_lock);
		laser_autooff_reschedule();
	}

	return rc;
}

int hispec_laser_set_tec_temperature_c(enum hispec_laser_id id, float temperature_c)
{
	const struct hispec_laser_driver_profile *profile;
	const laserprops_t *props;
	maiman_driver_t drv;
	int rc;

	rc = profile_for_id(id, &profile);
	if (rc != 0) {
		return rc;
	}
	k_mutex_lock(&laser_lock, K_FOREVER);
	props = runtime_props_locked(id);
	k_mutex_unlock(&laser_lock);

	if (!float_is_valid(temperature_c) ||
	    temperature_c < props->operating_temp_range_c.min_c ||
	    temperature_c > props->operating_temp_range_c.max_c) {
		return -ERANGE;
	}

	k_mutex_lock(&laser_lock, K_FOREVER);
	rc = prepare_to_operate_locked(profile, &drv);
	if (rc == 0 && !maiman_set_tec_temperature(&drv, temperature_c)) {
		rc = -EIO;
	} else if (rc == 0) {
		output_estimate_set_locked(id, laser_output_estimate[id].current_ma,
					   temperature_c);
	}
	k_mutex_unlock(&laser_lock);

	return rc;
}

int hispec_laser_set_tec_pid(enum hispec_laser_id id, tec_pid_t pid)
{
	const struct hispec_laser_driver_profile *profile;
	maiman_driver_t drv;
	int rc;

	rc = profile_for_id(id, &profile);
	if (rc != 0) {
		return rc;
	}

	k_mutex_lock(&laser_lock, K_FOREVER);
	rc = ensure_bank_powered_locked();
	if (rc == 0) {
		maiman_init(&drv, profile->node_id);
		rc = verify_driver_locked(profile, &drv, NULL);
	}
	if (rc == 0 && !maiman_set_tec_pid(&drv, pid)) {
		rc = -EIO;
	}
	k_mutex_unlock(&laser_lock);

	return rc;
}

static int validate_laser_settings(const struct hispec_laser_driver_profile *profile,
				   const struct app_laser_channel_settings *settings)
{
	const laserprops_t *props;

	if (profile == NULL || profile->properties == NULL || settings == NULL) {
		return -EINVAL;
	}

	props = &settings->properties;
	if (!float_is_valid(props->nominal_current_ma) ||
	    !float_is_valid(props->max_current_ma) ||
	    !float_is_valid(props->threshold_current_ma) ||
	    !float_is_valid(props->efficiency_mw_per_ma) ||
	    !float_is_valid(props->wavelength_nm) ||
	    !float_is_valid(settings->current_set_calibration_pct) ||
	    !float_is_valid(props->tec_max_current_a) ||
	    !float_is_valid(props->dlambda_dT_nm_per_k) ||
	    !float_is_valid(props->dlambda_dA_nm_per_ma) ||
	    props->threshold_current_ma < 0.0f ||
	    props->threshold_current_ma > 1000.0f ||
	    props->nominal_current_ma <= props->threshold_current_ma ||
	    props->nominal_current_ma > 1000.0f ||
	    props->max_current_ma < props->nominal_current_ma ||
	    props->max_current_ma > 1000.0f ||
	    props->efficiency_mw_per_ma < 0.0f ||
	    props->efficiency_mw_per_ma > 100.0f ||
	    props->wavelength_nm < 1.0f ||
	    props->wavelength_nm > 10000.0f ||
	    settings->current_set_calibration_pct < 95.0f ||
	    settings->current_set_calibration_pct > 105.0f ||
	    props->tec_max_current_a <= 0.0f ||
	    props->tec_max_current_a > profile->properties->tec_max_current_a ||
	    props->dlambda_dT_nm_per_k < -10.0f ||
	    props->dlambda_dT_nm_per_k > 10.0f ||
	    props->dlambda_dA_nm_per_ma < -10.0f ||
	    props->dlambda_dA_nm_per_ma > 10.0f) {
		return -ERANGE;
	}

	if (!float_is_valid(props->operating_temp_range_c.min_c) ||
	    !float_is_valid(props->operating_temp_range_c.max_c) ||
	    !float_is_valid(props->operating_temp_c) ||
	    props->operating_temp_range_c.min_c < 15.0f ||
	    props->operating_temp_range_c.max_c > 40.0f ||
	    props->operating_temp_range_c.min_c > props->operating_temp_range_c.max_c ||
	    props->operating_temp_c < props->operating_temp_range_c.min_c ||
	    props->operating_temp_c > props->operating_temp_range_c.max_c) {
		return -ERANGE;
	}

	return 0;
}

static bool laser_driver_settings_differ(const struct app_laser_channel_settings *a,
					 const struct app_laser_channel_settings *b)
{
	return a->properties.max_current_ma != b->properties.max_current_ma ||
	       a->current_set_calibration_pct != b->current_set_calibration_pct ||
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

	k_mutex_lock(&laser_lock, K_FOREVER);
	ensure_laser_runtime_settings_locked();
	*out = laser_settings[id];
	k_mutex_unlock(&laser_lock);
	return 0;
}

int hispec_laser_update_channel_settings(enum hispec_laser_id id,
					 const struct app_laser_channel_settings *settings,
					 bool persist)
{
	const struct hispec_laser_driver_profile *profile;
	struct app_laser_channel_settings previous;
	maiman_driver_t drv;
	bool apply_driver;
	bool was_powered = false;
	int rc;
	int power_restore_rc = 0;

	rc = profile_for_id(id, &profile);
	if (rc != 0) {
		return rc;
	}

	rc = validate_laser_settings(profile, settings);
	if (rc != 0) {
		return rc;
	}

	k_mutex_lock(&laser_lock, K_FOREVER);
	ensure_laser_runtime_settings_locked();
	previous = laser_settings[id];
	apply_driver = laser_driver_settings_differ(&previous, settings);

	if (apply_driver) {
		if (bank_power_mode == HISPEC_LASER_BANK_POWER_OVERRIDE_OFF) {
			rc = -EPERM;
			goto out_unlock;
		}

		was_powered = bank_power_is_enabled_locked();
		rc = bank_power_set_locked(true, NULL);
		if (rc != 0) {
			goto out_unlock;
		}

		rc = stop_output_locked(profile, settings->disable_tec_at_autooff);
		if (rc != 0) {
			goto restore_power;
		}

		laser_settings[id] = *settings;
		maiman_init(&drv, profile->node_id);
		rc = verify_driver_locked(profile, &drv, NULL);
		if (rc == 0) {
			rc = apply_runtime_profile_locked(profile, &drv);
		}
		if (rc != 0) {
			laser_settings[id] = previous;
		}

restore_power:
		if (!was_powered) {
			power_restore_rc = bank_power_set_locked(false, NULL);
			if (rc == 0) {
				rc = power_restore_rc;
			}
		}
	} else {
		laser_settings[id] = *settings;
	}

out_unlock:
	k_mutex_unlock(&laser_lock);

	if (rc == 0) {
		(void)app_settings_update_laser_channel((uint8_t)id, settings, persist);
	}
	return rc;
}

int hispec_laser_set_tune_delta_nm(enum hispec_laser_id id, float delta_nm,
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
	settings.tune_delta_nm = delta_nm;
	return hispec_laser_update_channel_settings(id, &settings, persist);
}

float hispec_laser_get_tune_delta_nm(enum hispec_laser_id id)
{
	float value = LASERPROP_NA;

	k_mutex_lock(&laser_lock, K_FOREVER);
	ensure_laser_runtime_settings_locked();
	if (id >= 0 && id < HISPEC_LASER_COUNT) {
		value = laser_settings[id].tune_delta_nm;
	}
	k_mutex_unlock(&laser_lock);
	return value;
}

static void hispec_laser_service_autooff(void)
{
	int64_t now = k_uptime_get();

	for (uint8_t i = 0U; i < HISPEC_LASER_COUNT; ++i) {
		bool expired = false;
		bool stop_tec = false;

		k_mutex_lock(&laser_lock, K_FOREVER);
		ensure_laser_runtime_settings_locked();
		expired = laser_autooff_deadline_ms[i] > 0 &&
			  now >= laser_autooff_deadline_ms[i];
		stop_tec = laser_settings[i].disable_tec_at_autooff;
		k_mutex_unlock(&laser_lock);

		if (expired) {
			(void)hispec_laser_stop_output((enum hispec_laser_id)i, stop_tec);
		}
	}
}

static void laser_autooff_work_handler(struct k_work *work)
{
	ARG_UNUSED(work);

	hispec_laser_service_autooff();
	laser_autooff_reschedule();
}

float hispec_laser_estimate_power_mw(const laserprops_t *properties, float current_ma)
{
	float power_mw;

	if (properties == NULL || !float_is_valid(current_ma) ||
	    !float_is_positive(properties->efficiency_mw_per_ma)) {
		return LASERPROP_NA;
	}

	power_mw = (current_ma - properties->threshold_current_ma) *
		   properties->efficiency_mw_per_ma;
	return (power_mw > 0.0f) ? power_mw : 0.0f;
}

int laser_estimate_flux(enum hispec_laser_id id,
			float fractional_noise,
			float constant_noise_mw,
			struct hispec_laser_flux_estimate *out)
{
	laserprops_t properties;
	float current_ma;
	float tec_temperature_c;
	double power_mw;
	double power_w;
	double photon_j;
	double wavelength_m;
	double power_err_mw;

	if (out == NULL || id < 0 || id >= HISPEC_LASER_COUNT) {
		return -EINVAL;
	}

	k_mutex_lock(&laser_lock, K_FOREVER);
	ensure_laser_runtime_settings_locked();
	if (!laser_output_estimate[id].valid) {
		k_mutex_unlock(&laser_lock);
		return -EAGAIN;
	}
	properties = laser_settings[id].properties;
	current_ma = laser_output_estimate[id].current_ma;
	tec_temperature_c = laser_output_estimate[id].tec_temperature_c;
	k_mutex_unlock(&laser_lock);

	if (!float_is_valid(current_ma) ||
	    !float_is_valid(tec_temperature_c) ||
	    !float_is_valid(fractional_noise) ||
	    !float_is_valid(constant_noise_mw) ||
	    fractional_noise < 0.0f || constant_noise_mw < 0.0f ||
	    !float_is_valid(properties.wavelength_nm) ||
	    properties.wavelength_nm <= 0.0f) {
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
	power_err_mw = sqrt((power_mw * (double)fractional_noise) *
			    (power_mw * (double)fractional_noise) +
			    (double)constant_noise_mw * (double)constant_noise_mw);

	out->power_mw = power_mw;
	out->power_err_mw = power_err_mw;
	out->flux_ph_s = power_w / photon_j;
	out->flux_err_ph_s = (power_err_mw * 1.0e-3) / photon_j;
	return 0;
}

float hispec_laser_current_on_time_s(enum hispec_laser_id id)
{
	float value;

	k_mutex_lock(&laser_lock, K_FOREVER);
	value = on_time_runtime_seconds_locked(laser_current_runtime,
					       ARRAY_SIZE(laser_current_runtime), id);
	k_mutex_unlock(&laser_lock);

	return value;
}

float hispec_laser_estimate_wavelength_nm(const laserprops_t *properties,
					  float tec_temperature_c,
					  float current_ma)
{
	float delta_i_ma;
	float delta_t_c;

	if (properties == NULL || !float_is_valid(tec_temperature_c) ||
	    !float_is_valid(current_ma) ||
	    !float_is_valid(properties->dlambda_dT_nm_per_k) ||
	    !float_is_valid(properties->dlambda_dA_nm_per_ma)) {
		return LASERPROP_NA;
	}

	if (current_ma == 0.0f) {
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
	const laserprops_t *props;
	float brightness;
	float current_range_ma;
	float desired_i_ma;
	float initial_wavelength_nm;
	float delta_lambda_nm;
	float target_temp_c;
	float delta_from_temp_nm;
	float delta_remaining_nm;
	float delta_i_ma;
	float allowed_delta_i_ma;
	float target_current_ma;
	float delta_from_current_nm;
	float estimated_wavelength_nm;
	float estimated_power_mw;
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
	k_mutex_lock(&laser_lock, K_FOREVER);
	props = runtime_props_locked(id);
	k_mutex_unlock(&laser_lock);

	memset(result, 0, sizeof(*result));
	result->requested_wavelength_nm = request->wavelength_nm;

	if (!float_is_valid(request->desired_power_percent) ||
	    !float_is_valid(request->wavelength_nm) ||
	    !float_is_valid(request->maximum_power_shift_percent) ||
	    request->desired_power_percent < 0.0f ||
	    request->desired_power_percent > 100.0f ||
	    request->maximum_power_shift_percent < 0.0f) {
		return -ERANGE;
	}

	if (request->desired_power_percent == 0.0f) {
		result->target_current_ma = 0.0f;
		result->target_temperature_c = props->operating_temp_c;
		result->estimated_power_mw = 0.0f;
		result->estimated_wavelength_nm = props->wavelength_nm;
		result->wavelength_error_nm = request->wavelength_nm - props->wavelength_nm;
		if (request->apply) {
			return hispec_laser_set_current_ma(id, 0.0f);
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

	brightness = request->desired_power_percent / 100.0f;
	desired_i_ma = props->threshold_current_ma + current_range_ma * brightness;
	desired_i_ma = clampf_with_flag(desired_i_ma, props->threshold_current_ma,
					props->max_current_ma, &current_clamped);

	initial_wavelength_nm =
		(desired_i_ma - props->nominal_current_ma) *
		props->dlambda_dA_nm_per_ma + props->wavelength_nm;

	delta_lambda_nm = request->wavelength_nm - initial_wavelength_nm;
	if (request->use_temperature) {
		target_temp_c = props->operating_temp_c +
				delta_lambda_nm / props->dlambda_dT_nm_per_k;
		target_temp_c = clampf_with_flag(target_temp_c,
						 props->operating_temp_range_c.min_c,
						 props->operating_temp_range_c.max_c,
						 &temp_clamped);
		delta_from_temp_nm =
			props->dlambda_dT_nm_per_k *
			(target_temp_c - props->operating_temp_c);
	} else {
		target_temp_c = props->operating_temp_c;
		delta_from_temp_nm = 0.0f;
	}

	delta_remaining_nm = delta_lambda_nm - delta_from_temp_nm;
	if (request->use_current) {
		delta_i_ma = delta_remaining_nm / props->dlambda_dA_nm_per_ma;
		allowed_delta_i_ma =
			(request->maximum_power_shift_percent / 100.0f) * current_range_ma;
		if (delta_i_ma > allowed_delta_i_ma) {
			delta_i_ma = allowed_delta_i_ma;
			current_clamped = true;
		}
		if (delta_i_ma < -allowed_delta_i_ma) {
			delta_i_ma = -allowed_delta_i_ma;
			current_clamped = true;
		}
		target_current_ma = desired_i_ma + delta_i_ma;
		target_current_ma = clampf_with_flag(target_current_ma,
						    props->threshold_current_ma,
						    props->max_current_ma,
						    &current_clamped);
		delta_from_current_nm =
			(target_current_ma - props->nominal_current_ma) *
			props->dlambda_dA_nm_per_ma;
	} else {
		target_current_ma = desired_i_ma;
		delta_from_current_nm =
			(desired_i_ma - props->nominal_current_ma) *
			props->dlambda_dA_nm_per_ma;
	}

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

	if (request->apply) {
		maiman_driver_t drv;

		k_mutex_lock(&laser_lock, K_FOREVER);
		rc = prepare_to_operate_locked(profile, &drv);
		if (rc == 0 &&
		    (!maiman_set_tec_temperature(&drv, target_temp_c) ||
		     !maiman_set_current(&drv, target_current_ma) ||
		     !maiman_start_device(&drv))) {
			rc = -EIO;
		}
		if (rc == 0) {
			on_time_runtime_update_locked(laser_current_runtime,
						      ARRAY_SIZE(laser_current_runtime),
						      id, true);
			on_time_runtime_update_locked(laser_tec_runtime,
						      ARRAY_SIZE(laser_tec_runtime),
						      id, true);
			output_estimate_set_locked(id, target_current_ma, target_temp_c);
		}
		k_mutex_unlock(&laser_lock);
		return rc;
	}

	return 0;
}
