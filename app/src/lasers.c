/**
 * @file lasers.c
 * @brief Laser-bank power, relay outputs, Maiman status, and tuning helpers.
 *
 * A module mutex serializes shared RS-485/GPIO operations. Functions can block
 * on Modbus RTU, sleep for bank boot/fault-clear delays, and modify driver
 * state owned by the Maiman modules.
 *
 * Copyright (c) 2026 Caltech Optical Observatories
 * SPDX-License-Identifier: Apache-2.0
 */

#include "lasers.h"

#include "devices.h"

#include <errno.h>
#include <stddef.h>
#include <string.h>

#include <zephyr/drivers/gpio.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/util.h>

LOG_MODULE_REGISTER(lasers, LOG_LEVEL_INF);

/* One mutex protects the shared RS-485 bus sequencing and the bank/relay GPIOs.
 * k_mutex_lock() sleeps the calling thread instead of busy-waiting while another
 * command is talking to the Maiman modules.
 */
static K_MUTEX_DEFINE(laser_lock);

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

static int require_tib(void)
{
	return (devices_board_type() == HISPEC_BOARD_TIB) ? 0 : -ENODEV;
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
	const struct hispec_laser_driver_profile *profile;

	if (profile_for_id(id, &profile) != 0) {
		return NULL;
	}
	return profile->properties;
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

static bool bank_power_is_enabled_locked(void)
{
	int val;

	if (devices_board_type() != HISPEC_BOARD_TIB || !gpio_is_ready_dt(&laser_power_gpio)) {
		return false;
	}

	val = gpio_pin_get_dt(&laser_power_gpio);
	return val > 0;
}

bool hispec_laser_bank_power_is_enabled(void)
{
	bool enabled;

	k_mutex_lock(&laser_lock, K_FOREVER);
	enabled = bank_power_is_enabled_locked();
	k_mutex_unlock(&laser_lock);

	return enabled;
}

static int bank_power_set_locked(bool enabled, bool *transitioned)
{
	bool was_enabled;
	int rc;

	if (transitioned != NULL) {
		*transitioned = false;
	}

	rc = require_tib();
	if (rc != 0) {
		return rc;
	}

	if (!gpio_is_ready_dt(&laser_power_gpio)) {
		return -ENODEV;
	}

	was_enabled = bank_power_is_enabled_locked();
	if (was_enabled == enabled) {
		return 0;
	}

	/* gpio_pin_set_dt() writes the logical active state described by
	 * laser_power_gpios in devicetree.
	 */
	rc = gpio_pin_set_dt(&laser_power_gpio, enabled ? 1 : 0);
	if (rc != 0) {
		return rc;
	}

	if (transitioned != NULL) {
		*transitioned = true;
	}
	if (enabled) {
		/* Let the Maiman modules boot before the caller starts Modbus IO. */
		k_sleep(K_MSEC(HISPEC_LASER_BANK_BOOT_DELAY_MS));
	}

	return 0;
}

int hispec_laser_bank_power_set(bool enabled, bool *transitioned)
{
	int rc;

	k_mutex_lock(&laser_lock, K_FOREVER);
	rc = bank_power_set_locked(enabled, transitioned);
	k_mutex_unlock(&laser_lock);

	return rc;
}

static int ensure_bank_powered_locked(void)
{
	return bank_power_set_locked(true, NULL);
}

int hispec_laser_bank_clear_faults(uint32_t off_ms)
{
	uint32_t delay_ms = (off_ms == 0U) ? HISPEC_LASER_BANK_FAULT_CLEAR_OFF_MS : off_ms;
	int rc;

	k_mutex_lock(&laser_lock, K_FOREVER);

	rc = bank_power_set_locked(false, NULL);
	if (rc != 0) {
		goto out;
	}

	/* k_sleep() yields during the supply-off interval needed to clear the
	 * SF8025 overcurrent latch.
	 */
	k_sleep(K_MSEC(delay_ms));
	rc = bank_power_set_locked(true, NULL);

out:
	k_mutex_unlock(&laser_lock);
	return rc;
}

static const struct gpio_dt_spec *aux_gpio(enum hispec_laser_aux_output output)
{
	switch (output) {
	case HISPEC_LASER_AUX_YJ_PHOTODIODE:
		return &yj_power_gpio;
	case HISPEC_LASER_AUX_HK_PHOTODIODE:
		return &hk_power_gpio;
	case HISPEC_LASER_AUX_BANK_HEATER:
		return &heater_power_gpio;
	default:
		return NULL;
	}
}

int hispec_laser_aux_power_set(enum hispec_laser_aux_output output, bool enabled)
{
	const struct gpio_dt_spec *gpio = aux_gpio(output);
	int rc;

	if (gpio == NULL) {
		return -EINVAL;
	}

	k_mutex_lock(&laser_lock, K_FOREVER);
	rc = require_tib();
	if (rc != 0) {
		goto out;
	}
	if (!gpio_is_ready_dt(gpio)) {
		rc = -ENODEV;
		goto out;
	}

	/* Relay GPIOs are logical Zephyr GPIOs; devicetree active flags handle
	 * the DS2408's open-drain electrical behavior.
	 */
	rc = gpio_pin_set_dt(gpio, enabled ? 1 : 0);

out:
	k_mutex_unlock(&laser_lock);
	return rc;
}

int hispec_laser_aux_power_get(enum hispec_laser_aux_output output, bool *enabled)
{
	const struct gpio_dt_spec *gpio = aux_gpio(output);
	int rc = 0;
	int val;

	if (gpio == NULL || enabled == NULL) {
		return -EINVAL;
	}

	k_mutex_lock(&laser_lock, K_FOREVER);
	rc = require_tib();
	if (rc != 0) {
		goto out;
	}
	if (!gpio_is_ready_dt(gpio)) {
		rc = -ENODEV;
		goto out;
	}

	val = gpio_pin_get_dt(gpio);
	if (val < 0) {
		rc = val;
		goto out;
	}
	*enabled = val > 0;

out:
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
	const laserprops_t *props = profile->properties;

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

static int apply_runtime_profile_locked(const struct hispec_laser_driver_profile *profile,
					maiman_driver_t *drv)
{
	const laserprops_t *props = profile->properties;
	int rc;

	rc = check_ocp_limit_locked(profile, drv);
	if (rc != 0) {
		return rc;
	}

	if (!maiman_set_current_max(drv, props->max_current_ma)) {
		return -EIO;
	}
	if (!maiman_set_tec_current_limit(drv, props->tec_max_current_a)) {
		return -EIO;
	}
	if (!maiman_set_tec_pid(drv, props->tec_pid)) {
		return -EIO;
	}
	if (!maiman_set_frequency(drv, 0.0f)) {
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
			current_temp = profile->properties->operating_temp_c;
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
	out->current_measured_ma = LASERPROP_NA;
	out->current_min_ma = LASERPROP_NA;
	out->current_max_ma = LASERPROP_NA;
	out->current_max_limit_ma = LASERPROP_NA;
	out->current_protection_threshold_ma = LASERPROP_NA;
	out->voltage_v = LASERPROP_NA;
	out->tec_temperature_set_c = LASERPROP_NA;
	out->tec_temperature_measured_c = LASERPROP_NA;
	out->pcb_temperature_c = LASERPROP_NA;
	out->tec_current_measured_a = LASERPROP_NA;
	out->tec_current_limit_a = LASERPROP_NA;
	out->tec_voltage_v = LASERPROP_NA;
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
	rc = require_tib();
	if (rc != 0) {
		goto out_unlock;
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
	out->current_measured_ma = maiman_get_current_measured(&drv);
	out->current_min_ma = maiman_get_current_min(&drv);
	out->current_max_ma = maiman_get_current_max(&drv);
	out->current_max_limit_ma = maiman_get_current_max_limit(&drv);
	out->current_protection_threshold_ma = maiman_get_current_protection_threshold(&drv);
	out->voltage_v = maiman_get_voltage_measured(&drv);
	out->tec_temperature_set_c = maiman_get_tec_temperature_value(&drv);
	out->tec_temperature_measured_c = maiman_get_tec_temperature_measured(&drv);
	out->pcb_temperature_c = maiman_get_pcb_temperature_measured(&drv);
	out->tec_current_measured_a = maiman_get_tec_current_measured(&drv);
	out->tec_current_limit_a = maiman_get_tec_current_limit(&drv);
	out->tec_voltage_v = maiman_get_tec_voltage(&drv);
	if (!maiman_get_tec_pid(&drv, &out->tec_pid)) {
		read_ok = false;
	}

	out->estimated_power_mw =
		hispec_laser_estimate_power_mw(profile->properties, out->current_set_ma);
	out->estimated_wavelength_nm =
		hispec_laser_estimate_wavelength_nm(profile->properties,
						    out->tec_temperature_measured_c,
						    out->current_set_ma);
	rc = read_ok ? 0 : -EIO;

out_unlock:
	k_mutex_unlock(&laser_lock);
	return rc;
}

static int stop_output_locked(const struct hispec_laser_driver_profile *profile)
{
	maiman_driver_t drv;
	int rc;

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

	return 0;
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
	props = profile->properties;

	if (!float_is_valid(current_ma) || current_ma < 0.0f ||
	    current_ma > props->max_current_ma) {
		return -ERANGE;
	}

	k_mutex_lock(&laser_lock, K_FOREVER);
	if (current_ma == 0.0f) {
		rc = stop_output_locked(profile);
		goto out;
	}

	rc = prepare_to_operate_locked(profile, &drv);
	if (rc != 0) {
		goto out;
	}

	if (!maiman_set_current(&drv, current_ma) || !maiman_start_device(&drv)) {
		rc = -EIO;
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
	props = profile->properties;

	if (!float_is_valid(temperature_c) ||
	    temperature_c < props->operating_temp_range_c.min_c ||
	    temperature_c > props->operating_temp_range_c.max_c) {
		return -ERANGE;
	}

	k_mutex_lock(&laser_lock, K_FOREVER);
	rc = prepare_to_operate_locked(profile, &drv);
	if (rc == 0 && !maiman_set_tec_temperature(&drv, temperature_c)) {
		rc = -EIO;
	}
	k_mutex_unlock(&laser_lock);

	return rc;
}

int hispec_laser_set_pulse(enum hispec_laser_id id, float frequency_hz, float duration_ms)
{
	const struct hispec_laser_driver_profile *profile;
	maiman_driver_t drv;
	int rc;

	rc = profile_for_id(id, &profile);
	if (rc != 0) {
		return rc;
	}
	if (!float_is_valid(frequency_hz) || !float_is_valid(duration_ms) ||
	    frequency_hz < 0.0f || duration_ms < 0.0f) {
		return -ERANGE;
	}

	k_mutex_lock(&laser_lock, K_FOREVER);
	rc = ensure_bank_powered_locked();
	if (rc == 0) {
		maiman_init(&drv, profile->node_id);
		rc = verify_driver_locked(profile, &drv, NULL);
	}
	if (rc == 0 &&
	    (!maiman_set_frequency(&drv, frequency_hz) ||
	     !maiman_set_duration(&drv, duration_ms))) {
		rc = -EIO;
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
	props = profile->properties;

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
		k_mutex_unlock(&laser_lock);
		return rc;
	}

	return 0;
}
