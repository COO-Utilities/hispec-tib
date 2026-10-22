/*
 * Copyright (c) 2026 Caltech Optical Observatories
 * SPDX-License-Identifier: Apache-2.0
 */

#include "laser_command.h"
#include "laser_calibration.h"

#include <errno.h>
#include <float.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <string.h>

#include <zephyr/sys/util.h>

#include "app_settings.h"
#include "laserbank_tempcontrol.h"
#include "lasers.h"
#include "throughput_monitor.h"

#include <coo_commons/json_utils.h>

static const struct coo_json_string_choice laserbank_power_mode_choices[] = {
	{ "auto", HISPEC_LASER_BANK_POWER_AUTO },
	{ "override_on", HISPEC_LASER_BANK_POWER_OVERRIDE_ON },
	{ "override_off", HISPEC_LASER_BANK_POWER_OVERRIDE_OFF },
};

static const struct coo_json_string_choice heater_mode_choices[] = {
	{ "auto", LASERBANK_HEATER_MODE_AUTO },
	{ "override_on", LASERBANK_HEATER_MODE_OVERRIDE_ON },
	{ "override_off", LASERBANK_HEATER_MODE_OVERRIDE_OFF },
};

static int laser_cmd_error_rc(struct coo_cmd_response *out,
			      const struct coo_cmd_request *cmd,
			      const char *msg,
			      int rc)
{
	if (rc == -EBUSY) {
		return coo_cmd_busy_response(out, cmd);
	}
	if (rc == -EPERM) {
		return coo_cmd_error_rc(out, cmd, "laser bank power override_off", rc);
	}
	return coo_cmd_error_rc(out, cmd, msg, rc);
}

static int laser_cmd_failure(struct coo_cmd_response *out,
	const struct coo_cmd_request *cmd, enum hispec_laser_id id,
	const char *operation, int rc, const char *detail)
{
	char message[256];
	if (rc == -EBUSY) return coo_cmd_busy_response(out, cmd);
	/* Copy detail before response construction clears the reused payload buffer. */
	const char *reason = detail != NULL && detail[0] != '\0' ? detail :
		rc == -EPERM ? "laser bank power override_off" :
		rc == -EADDRNOTAVAIL ? "driver identity mismatch" :
		rc == -ERANGE || rc == -EINVAL ? "invalid setting or limit" :
		"operation failed; controller state uncertain";
	snprintk(message, sizeof(message), "%s %s: %s", hispec_laser_name(id), operation, reason);
	return coo_cmd_error_rc(out, cmd, message, rc);
}

static bool parse_laserbank_mode_request(const struct coo_cmd_request *cmd,
					 const char *key,
					 const struct coo_json_string_choice *choices,
					 size_t choice_count,
					 int *mode_value)
{
	const char *suffix;
	int payload_mode;
	int parse_rc;

	if (mode_value == NULL) {
		return false;
	}

	if (cmd == NULL) {
		return false;
	}
	parse_rc = coo_cmd_payload_empty(cmd) ? COO_JSON_EXTRACT_MISSING :
		coo_json_extract_string_choice(cmd->payload, "mode", choices,
					      choice_count, &payload_mode);
	if (parse_rc == COO_JSON_EXTRACT_ERR) {
		return false;
	}
	if (strcmp(cmd->key, key) == 0) {
		if (parse_rc != COO_JSON_EXTRACT_OK) return false;
		*mode_value = payload_mode;
		return true;
	}
	suffix = coo_cmd_key_suffix_after(cmd->key, key);
	return coo_json_match_string_choice(suffix, choices, choice_count, mode_value) == 0 &&
	       (parse_rc == COO_JSON_EXTRACT_MISSING || payload_mode == *mode_value);
}

int laserbank_power(const struct coo_cmd_request *cmd, struct coo_cmd_response *out)
{
	enum hispec_laser_bank_power_mode mode;
	char payload[128] = {0};
	int mode_value;
	int rc;

	if (cmd != NULL &&
	    (cmd->msg_type == COO_CMD_EFFECT ||
	     coo_cmd_key_suffix_after(cmd->key, "laser/bankpower")[0] != '\0')) {
		if (!parse_laserbank_mode_request(cmd, "laser/bankpower",
						  laserbank_power_mode_choices,
						  ARRAY_SIZE(laserbank_power_mode_choices),
						  &mode_value)) {
			return coo_cmd_error(out, cmd, "mode must be auto, override_on, or override_off");
		}
		mode = (enum hispec_laser_bank_power_mode)mode_value;
		/* Calibration owns bank state, but emergency bank-off must remain
		 * available even if its normal source STOP cannot be confirmed. */
		if (laser_calibration_active()) {
			if (mode != HISPEC_LASER_BANK_POWER_OVERRIDE_OFF) {
				return coo_cmd_busy_response(out, cmd);
			}
			(void)laser_calibration_stop(NULL); /* Bank-off remains available after a failed STOP. */
		}
		rc = hispec_laser_bank_power_mode_set(mode);
		if (rc != 0) {
			if (rc == -EBUSY) {
				return coo_cmd_busy_response(out, cmd);
			}
			mode = hispec_laser_bank_power_mode_get();
			snprintk(payload, sizeof(payload),
				 "{\"error\":\"laser bank power mode failed\","
				 "\"rc\":%d,\"mode\":\"%s\",\"powered\":%s}",
				 rc,
				 hispec_laser_bank_power_mode_name(mode),
				 hispec_laser_bank_power_is_enabled() ? "true" : "false");
			return coo_cmd_reply(out, cmd, COO_CMD_RESP_ERROR, payload);
		}
	}

	mode = hispec_laser_bank_power_mode_get();
	snprintk(payload, sizeof(payload),
		 "{\"mode\":\"%s\",\"powered\":%s}",
		 hispec_laser_bank_power_mode_name(mode),
		 hispec_laser_bank_power_is_enabled() ? "true" : "false");
	return coo_cmd_reply(out, cmd, COO_CMD_RESP_OK, payload);
}

int laserbank_clearfaults(const struct coo_cmd_request *cmd, struct coo_cmd_response *out)
{
	uint32_t off_ms = 0U;
	char payload[COO_CMD_PAYLOAD_MAX] = {0};
	int rc;

	/* Fault clear powers the bank off. Cancel calibration first, but a failed
	 * normal STOP must not prevent that recovery power cycle. */
	(void)laser_calibration_stop(NULL);
	rc = hispec_laser_bank_clear_faults(HISPEC_LASER_BANK_FAULT_CLEAR_OFF_MS, &off_ms);
	if (rc != 0) {
		return laser_cmd_error_rc(out, cmd, "laser bank fault clear failed", rc);
	}

	snprintf(payload, sizeof(payload), "{\"off_ms\":%u}", off_ms);
	return coo_cmd_reply(out, cmd, COO_CMD_RESP_OK, payload);
}

static const char *laserbank_tempcontrol_auto_state(
	const struct laserbank_tempcontrol_status *status)
{
	if (status == NULL) {
		return "unknown";
	}
	if (status->heater_mode == LASERBANK_HEATER_MODE_OVERRIDE_ON) {
		return "override_on";
	}
	if (status->heater_mode == LASERBANK_HEATER_MODE_OVERRIDE_OFF) {
		return "override_off";
	}
	if (status->waiting_for_temps) {
		return "waiting_for_temps";
	}
	if (status->any_disabled_below_on_threshold) {
		return "warming_disabled_tec";
	}
	if (status->all_disabled_above_off_threshold) {
		return "disabled_tec_warm";
	}
	if (status->all_tecs_enabled) {
		return "tecs_running";
	}
	return "holding";
}

static int laserbank_tempcontrol_status_payload(char *payload, size_t payload_len)
{
	struct laserbank_tempcontrol_status status = {0};
	uint32_t poll_age_s;
	size_t off = 0U;

	laserbank_tempcontrol_get_status(&status);
	poll_age_s = status.last_poll_age_ms / 1000U;
	if (coo_json_append(payload, payload_len, &off,
			    "{\"mode\":\"%s\","
			    "\"auto_state\":\"%s\","
			    "\"heater_on\":%s,\"bank_power\":%s,"
			    "\"ambient_c\":",
			    laserbank_heater_mode_name(status.heater_mode),
			    laserbank_tempcontrol_auto_state(&status),
			    status.heater_on ? "true" : "false",
			    status.bank_powered ? "true" : "false") != 0 ||
	    coo_json_append_float_or_null(payload, payload_len, &off,
					  status.ambient_c, 2) != 0 ||
	    coo_json_append(payload, payload_len, &off,
			    ",\"idle_tec_temps\":%u,\"idle_tec_avg_c\":",
			    status.idle_tec_temp_count) != 0 ||
	    coo_json_append_float_or_null(payload, payload_len, &off,
					  status.idle_tec_temp_avg_c, 2) != 0 ||
	    coo_json_append(payload, payload_len, &off,
			    ",\"last_error\":%d,\"poll_age_s\":",
			    status.last_error) != 0) {
		return -ENOSPC;
	}
	if (status.last_poll_age_ms == UINT32_MAX) {
		if (coo_json_append(payload, payload_len, &off, "null}") != 0) {
			return -ENOSPC;
		}
	} else if (coo_json_append(payload, payload_len, &off, "%u}",
				   poll_age_s) != 0) {
		return -ENOSPC;
	}
	return 0;
}

int laserbank_heater(const struct coo_cmd_request *cmd, struct coo_cmd_response *out)
{
	enum laserbank_heater_mode mode;
	char payload[COO_CMD_PAYLOAD_MAX] = {0};
	int mode_value;

	if (cmd != NULL &&
	    (cmd->msg_type == COO_CMD_EFFECT ||
	     coo_cmd_key_suffix_after(cmd->key, "laser/bankheater")[0] != '\0')) {
		if (!parse_laserbank_mode_request(cmd, "laser/bankheater",
						  heater_mode_choices,
						  ARRAY_SIZE(heater_mode_choices),
						  &mode_value)) {
			return coo_cmd_reply(out, cmd, COO_CMD_RESP_ERROR,
					     "{\"error\":\"Use laser/bankheater mode=auto|override_on|override_off\"}");
		}
		mode = (enum laserbank_heater_mode)mode_value;
		int rc = laserbank_tempcontrol_set_heater_mode(mode, true);
		if (rc != 0) {
			return coo_cmd_error_rc(out, cmd, "laser bank heater relay unavailable", rc);
		}
	}

	if (laserbank_tempcontrol_status_payload(payload, sizeof(payload)) != 0) {
		return coo_cmd_error(out, cmd, "laser bank heater response too large");
	}
	return coo_cmd_reply(out, cmd, COO_CMD_RESP_OK, payload);
}

static int command_laser_id_from_payload(const struct coo_cmd_request *cmd,
					 enum hispec_laser_id *id,
					 char *name,
					 size_t name_len)
{
	int parse_rc;

	if (cmd == NULL || id == NULL || name == NULL || name_len == 0U) {
		return -EINVAL;
	}

	parse_rc = coo_json_extract_string(cmd->payload, "name", name, name_len);
	if (parse_rc != COO_JSON_EXTRACT_OK) {
		return -EINVAL;
	}
	return hispec_laser_id_from_name(name, id);
}

static int laser_append_compact_status(char *payload, size_t payload_len,
				       const struct hispec_laser_status *status)
{
	size_t off = 0U;
	const laserprops_t *props = &status->properties;

	if (coo_json_append(payload, payload_len, &off,
			    "{\"name\":\"%s\",\"powered\":%s,"
			    "\"ready\":%s,\"output_state\":\"%s\",\"blocked_reason\":",
			    status->name,
			    status->bank_powered ? "true" : "false",
			    status->ready_to_operate ? "true" : "false", status->output_state) != 0 ||
	    coo_json_append_string_or_null(payload, payload_len, &off,
					   status->blocked_reason) != 0 ||
	    coo_json_append(payload, payload_len, &off,
			    ",\"tec_on_s\":") != 0 ||
	    coo_json_append_seconds_or_null(payload, payload_len, &off,
					    status->tec_runtime_active,
					    status->tec_on_time_s) != 0 ||
	    coo_json_append(payload, payload_len, &off,
			    ",\"emit_on_s\":") != 0 ||
	    coo_json_append_seconds_or_null(payload, payload_len, &off,
					    status->current_runtime_active,
					    status->current_on_time_s) != 0 ||
	    coo_json_append(payload, payload_len, &off,
			    ",\"emit_total_s\":") != 0 ||
	    coo_json_append_seconds_or_null(payload, payload_len, &off,
					    status->current_runtime_active,
					    status->total_emitting_s) != 0 ||
	    coo_json_append(payload, payload_len, &off,
			    ",\"temp_c\":") != 0 ||
	    coo_json_append_float_or_null(payload, payload_len, &off,
					  status->tec_temperature_measured_c, 2) != 0 ||
	    coo_json_append(payload, payload_len, &off,
			    ",\"i_mA\":") != 0 ||
	    coo_json_append_float_or_null(payload, payload_len, &off,
					  status->current_set_ma, 2) != 0 ||
	    coo_json_append(payload, payload_len, &off,
			    ",\"value\":") != 0 ||
	    coo_json_append_float_or_null(payload, payload_len, &off,
					  status->level_percent / 100.0, 4) != 0 ||
	    coo_json_append(payload, payload_len, &off,
			    ",\"power_mw\":") != 0 ||
	    coo_json_append_float_or_null(payload, payload_len, &off,
					  status->estimated_power_mw, 3) != 0 ||
	    coo_json_append(payload, payload_len, &off,
			    ",\"nominal_nm\":%.3f,\"tuned_nm\":",
			    props != NULL ? (double)props->wavelength_nm : 0.0) != 0 ||
	    coo_json_append_float_or_null(payload, payload_len, &off,
					  status->estimated_wavelength_nm, 3) != 0 ||
	    coo_json_append(payload, payload_len, &off,
			    ",\"tune_nm\":%.3f,\"tec_ma\":",
			    (double)status->tune_delta_nm) != 0 ||
	    coo_json_append_float_or_null(payload, payload_len, &off,
					  (double)status->tec_current_measured_a * 1000.0, 2) != 0 ||
	    coo_json_append(payload, payload_len, &off,
			    ",\"diode_v\":") != 0 ||
	    coo_json_append_float_or_null(payload, payload_len, &off,
					  status->voltage_v, 3) != 0 ||
	    coo_json_append(payload, payload_len, &off,
			    ",\"tec_v\":") != 0 ||
	    coo_json_append_float_or_null(payload, payload_len, &off,
					  status->tec_voltage_v, 3) != 0 ||
	    coo_json_append(payload, payload_len, &off,
			    ",\"off_in_s\":") != 0 ||
	    coo_json_append_i64_or_null(payload, payload_len, &off,
					status->autooff_active,
					status->off_in_s) != 0 ||
	    coo_json_append(payload, payload_len, &off,
			    ",\"oc_fault\":%s}",
			    status->lock_ld_overcurrent ? "true" : "false") != 0) {
		return -ENOSPC;
	}

	return 0;
}

int laser_get(const struct coo_cmd_request *cmd, struct coo_cmd_response *out)
{
	enum hispec_laser_id id;
	struct hispec_laser_status status = {0};
	char name[16] = {0};
	char payload[COO_CMD_PAYLOAD_MAX] = {0};
	int rc;

	if (command_laser_id_from_payload(cmd, &id, name, sizeof(name)) != 0) {
		return coo_cmd_error(out, cmd, "missing or invalid laser name");
	}

	rc = hispec_laser_get_status(id, false, &status);
	if (rc != 0) {
		return laser_cmd_failure(out, cmd, id, "status read", rc, NULL);
	}
	if (laser_append_compact_status(payload, sizeof(payload), &status) != 0) {
		return coo_cmd_error(out, cmd, "laser response too large");
	}
	return coo_cmd_reply(out, cmd, COO_CMD_RESP_OK, payload);
}

int laser_set(const struct coo_cmd_request *cmd, struct coo_cmd_response *out)
{
	enum hispec_laser_id id;
	struct app_laser_channel_settings settings;
	char name[16] = {0};
	double value = 0.0;
	uint32_t autooff_s;
	bool stop = false, autooff_present = false;
	int rc;

	if (command_laser_id_from_payload(cmd, &id, name, sizeof(name)) != 0) {
		return coo_cmd_error(out, cmd, "missing or invalid laser name");
	}
	if (coo_json_extract_optional_bool(cmd->payload, "stop", &stop, NULL) != 0) {
		return coo_cmd_error(out, cmd, "invalid stop");
	}
	int parsed = coo_json_extract_double(cmd->payload, "value", &value);
	if ((parsed != COO_JSON_EXTRACT_OK && !(stop && parsed == COO_JSON_EXTRACT_MISSING)) ||
	    !(value >= 0.0 && value <= 1.0) || (stop && value != 0.0)) {
		return coo_cmd_error(out, cmd, "value must be 0..1; stop requires zero or omitted value");
	}
	rc = hispec_laser_get_channel_settings(id, &settings);
	if (rc != 0) return laser_cmd_failure(out, cmd, id, "settings read", rc, NULL);
	autooff_s = settings.autooff_s;
	if (coo_json_extract_optional_u32(cmd->payload, "autooff_s", &autooff_s, &autooff_present) != 0 ||
	    (stop && autooff_present)) {
		return coo_cmd_error(out, cmd, "invalid autooff_s; not allowed with stop");
	}

	/* Preserve calibration's current/attenuation relationship. Zero or STOP
	 * is an explicit cancellation; positive manual drive waits for its owner. */
	if (laser_calibration_active()) {
		if (value > 0.0) {
			return coo_cmd_busy_response(out, cmd);
		}
		rc = laser_calibration_stop(NULL);
		if (rc != 0) {
			return laser_cmd_failure(out, cmd, id, "calibration stop", rc, NULL);
		}
	}
	throughput_monitor_note_laser_changed(id, false);
	if (stop) {
		rc = hispec_laser_stop_output(id, settings.disable_tec_at_autooff, out->payload, sizeof(out->payload));
	} else if (value == 0.0 && !autooff_present) {
		/* A momentary zero level preserves the existing shutdown deadline. */
		rc = hispec_laser_set_current_ma(id, 0.0, out->payload, sizeof(out->payload));
	} else {
		rc = hispec_laser_set_output_percent_autooff(id, value * 100.0, autooff_s, true, out->payload, sizeof(out->payload));
	}
	if (rc != 0) return laser_cmd_failure(out, cmd, id, stop ? "stop" : value == 0.0 ? "zero current" : "set level/start", rc, out->payload);
	return coo_cmd_ok(out, cmd);
}

int laser_tune_get(const struct coo_cmd_request *cmd, struct coo_cmd_response *out)
{
	enum hispec_laser_id id;
	char name[16] = {0};
	char payload[COO_CMD_PAYLOAD_MAX];

	if (command_laser_id_from_payload(cmd, &id, name, sizeof(name)) != 0) {
		return coo_cmd_error(out, cmd, "missing or invalid laser name");
	}
	snprintk(payload, sizeof(payload),
		 "{\"name\":\"%s\",\"tune_nm\":%.4f}",
		 hispec_laser_name(id),
		 (double)hispec_laser_get_tune_delta_nm(id));
	return coo_cmd_reply(out, cmd, COO_CMD_RESP_OK, payload);
}

int laser_tune_set(const struct coo_cmd_request *cmd, struct coo_cmd_response *out)
{
	/* Tuning would invalidate the reference temperature/wavelength snapshot. */
	if (laser_calibration_active()) {
		return coo_cmd_busy_response(out, cmd);
	}

	enum hispec_laser_id id;
	char name[16] = {0};
	double delta_nm = 0.0;
	int parse_rc;
	int rc;

	if (command_laser_id_from_payload(cmd, &id, name, sizeof(name)) != 0) {
		return coo_cmd_error(out, cmd, "missing or invalid laser name");
	}
	parse_rc = coo_json_extract_double(cmd->payload, "tune_nm", &delta_nm);
	if (parse_rc != COO_JSON_EXTRACT_OK) {
		return coo_cmd_error(out, cmd, "missing or invalid tune_nm");
	}
	throughput_monitor_note_laser_changed(id, true);
	rc = hispec_laser_set_tune_delta_nm(id, delta_nm, true, out->payload, sizeof(out->payload));
	if (rc != 0) {
		return laser_cmd_failure(out, cmd, id, "set tune", rc, out->payload);
	}
	return coo_cmd_ok(out, cmd);
}

/* Format a settings snapshot; ld_serial is compiled diode identity, not driver readback.
 * Unavailable datasheet values remain JSON null. No I/O or state changes.
 */
static int laser_settings_payload(char *payload, size_t payload_len,
				  enum hispec_laser_id id,
				  const struct app_laser_channel_settings *settings)
{
	const laserprops_t *p = &settings->properties;
	size_t off = 0U;

	if (coo_json_append(payload, payload_len, &off,
		"{\"name\":\"%s\",\"settings\":{"
		"\"model\":\"%s\",\"ld_serial\":\"%s\",\"expected_serial\":%u,"
		"\"nominal_current_ma\":%.3f,"
		"\"max_current_ma\":%.3f,\"current_cal_pct\":%.3f,"
		"\"fractional_noise\":%.9g,\"constant_noise_mw\":%.9g,"
		"\"threshold_current_ma\":%.3f,\"efficiency_mw_per_ma\":%.6f,"
		"\"ref_mw\":%.9g,\"ref_ma\":%.1f,\"ref_temp_c\":%.2f,"
		"\"min_autolevel_current_ma\":%.1f,"
		"\"wavelength_nm\":%.3f,\"operating_temp_range_c\":[%.2f,%.2f],"
		"\"default_operating_temp_c\":%.2f,\"thermistor_kohm\":%.2f,"
		"\"isolation_db\":%.2f,\"tec_curr_lim_a\":%.3f,"
		"\"tec_pid\":{\"p\":%u,\"i\":%u,\"d\":%u},"
		"\"disable_tec_at_autooff\":%s,"
		"\"ntc_coeff_pct_per_c\":",
		hispec_laser_name(id), p->model_number, p->ld_serial,
		settings->expected_serial,
		(double)p->nominal_current_ma,
		(double)p->max_current_ma,
		(double)settings->current_cal_pct,
		settings->fractional_noise, settings->constant_noise_mw,
		(double)p->threshold_current_ma,
		(double)p->efficiency_mw_per_ma,
		p->ref_mw, p->ref_ma, p->ref_temp_c,
		settings->min_autolevel_current_ma,
		(double)p->wavelength_nm,
		(double)p->operating_temp_range_c.min_c,
		(double)p->operating_temp_range_c.max_c,
		(double)p->operating_temp_c,
		(double)p->thermistor_kohm,
		(double)p->isolation_db,
		(double)p->tec_curr_lim_a,
		p->tec_pid.kp, p->tec_pid.ki, p->tec_pid.kd,
		settings->disable_tec_at_autooff ? "true" : "false") != 0 ||
	    coo_json_append_float_or_null(payload, payload_len, &off,
					  p->ntc_coeff_pct_per_c, 6) != 0 ||
	    coo_json_append(payload, payload_len, &off,
		",\"dlambda_dT_nm_per_k\":%.6f,"
		"\"dlambda_dA_nm_per_ma\":%.6f,"
		"\"autooff_s\":%u,\"tune_nm\":%.4f,"
		"\"emit_total_s\":%llu}}",
		(double)p->dlambda_dT_nm_per_k,
		(double)p->dlambda_dA_nm_per_ma,
		settings->autooff_s,
		(double)settings->tune_delta_nm,
		(unsigned long long)settings->total_emitting_s) != 0) {
		return -ENOSPC;
	}

	return 0;
}

int laser_settings_get(const struct coo_cmd_request *cmd, struct coo_cmd_response *out)
{
	enum hispec_laser_id id;
	struct app_laser_channel_settings settings;
	char name[16] = {0};
	char payload[COO_CMD_PAYLOAD_MAX] = {0};
	int rc;

	if (command_laser_id_from_payload(cmd, &id, name, sizeof(name)) != 0) {
		return coo_cmd_error(out, cmd, "missing or invalid laser name");
	}
	rc = hispec_laser_get_channel_settings(id, &settings);
	if (rc != 0) {
		return laser_cmd_failure(out, cmd, id, "settings read", rc, NULL);
	}
	if (laser_settings_payload(payload, sizeof(payload), id, &settings) != 0) {
		return coo_cmd_error(out, cmd, "laser settings response too large");
	}
	return coo_cmd_reply(out, cmd, COO_CMD_RESP_OK, payload);
}

static int laser_parse_settings_update(const char *json,
				       struct app_laser_channel_settings *settings,
				       bool *changed, char invalid[64])
{
	double range[2] = {0};
	size_t range_len = 0U;
	char pid_json[128] = {0};
	uint16_t parsed_serial;
	bool serial_changed = false;
	int rc;

	if (json == NULL || settings == NULL || changed == NULL) {
		return -EINVAL;
	}
	if (coo_json_validate_top_level_keys(json,
		"nominal_current_ma,max_current_ma,threshold_current_ma,min_autolevel_current_ma,efficiency_mw_per_ma,"
		"wavelength_nm,current_cal_pct,fractional_noise,constant_noise_mw,ref_mw,ref_ma,ref_temp_c,"
		"default_operating_temp_c,operating_temp_range_c,tec_curr_lim_a,tec_pid,ntc_coeff_pct_per_c,"
		"dlambda_dT_nm_per_k,dlambda_dA_nm_per_ma,disable_tec_at_autooff,autooff_s,expected_serial",
		invalid, 64U) != 0) {
		return -EINVAL;
	}

#define LASER_PARSE_FLOAT(key, field) do { \
		strcpy(invalid, key); \
		if (coo_json_extract_optional_double_range(json, key, &(field), \
							  changed, -DBL_MAX, \
							  DBL_MAX) != 0) \
			return -EINVAL; \
	} while (0)

	LASER_PARSE_FLOAT("nominal_current_ma", settings->properties.nominal_current_ma);
	LASER_PARSE_FLOAT("max_current_ma", settings->properties.max_current_ma);
	LASER_PARSE_FLOAT("threshold_current_ma", settings->properties.threshold_current_ma);
	LASER_PARSE_FLOAT("min_autolevel_current_ma", settings->min_autolevel_current_ma);
	LASER_PARSE_FLOAT("efficiency_mw_per_ma", settings->properties.efficiency_mw_per_ma);
	LASER_PARSE_FLOAT("ref_mw", settings->properties.ref_mw);
	LASER_PARSE_FLOAT("ref_ma", settings->properties.ref_ma);
	LASER_PARSE_FLOAT("ref_temp_c", settings->properties.ref_temp_c);
	LASER_PARSE_FLOAT("wavelength_nm", settings->properties.wavelength_nm);
	LASER_PARSE_FLOAT("fractional_noise", settings->fractional_noise);
	LASER_PARSE_FLOAT("constant_noise_mw", settings->constant_noise_mw);
	LASER_PARSE_FLOAT("current_cal_pct",
			  settings->current_cal_pct);
	LASER_PARSE_FLOAT("default_operating_temp_c", settings->properties.operating_temp_c);
	LASER_PARSE_FLOAT("tec_curr_lim_a", settings->properties.tec_curr_lim_a);
	LASER_PARSE_FLOAT("ntc_coeff_pct_per_c", settings->properties.ntc_coeff_pct_per_c);
	LASER_PARSE_FLOAT("dlambda_dT_nm_per_k", settings->properties.dlambda_dT_nm_per_k);
	LASER_PARSE_FLOAT("dlambda_dA_nm_per_ma", settings->properties.dlambda_dA_nm_per_ma);

#undef LASER_PARSE_FLOAT

	strcpy(invalid, "operating_temp_range_c");
	rc = coo_json_extract_double_array(json, "operating_temp_range_c",
					   range, ARRAY_SIZE(range), &range_len);
	if (rc == COO_JSON_EXTRACT_ERR) {
		return -EINVAL;
	}
	if (rc == COO_JSON_EXTRACT_OK) {
		if (range_len != 2U) {
			return -ERANGE;
		}
		settings->properties.operating_temp_range_c.min_c = (double)range[0];
		settings->properties.operating_temp_range_c.max_c = (double)range[1];
		*changed = true;
	}

	strcpy(invalid, "tec_pid");
	rc = coo_json_extract_object(json, "tec_pid", pid_json, sizeof(pid_json));
	if (rc == COO_JSON_EXTRACT_ERR) {
		return -EINVAL;
	}
	if (rc == COO_JSON_EXTRACT_OK) {
		if (coo_json_validate_top_level_keys(pid_json, "p,i,d", NULL, 0U) != 0) {
			return -EINVAL;
		}
		if (coo_json_extract_optional_u16(pid_json, "p",
						  &settings->properties.tec_pid.kp,
						  changed) != 0 ||
		    coo_json_extract_optional_u16(pid_json, "i",
						  &settings->properties.tec_pid.ki,
						  changed) != 0 ||
		    coo_json_extract_optional_u16(pid_json, "d",
						  &settings->properties.tec_pid.kd,
						  changed) != 0) {
			return -EINVAL;
		}
	}

	strcpy(invalid, "disable_tec_at_autooff");
	if (coo_json_extract_optional_bool(json, "disable_tec_at_autooff",
					   &settings->disable_tec_at_autooff,
					   changed) != 0) {
		return -EINVAL;
	}

	strcpy(invalid, "autooff_s");
	if (coo_json_extract_optional_u32(json, "autooff_s",
					  &settings->autooff_s, changed) != 0) {
		return -EINVAL;
	}

	strcpy(invalid, "expected_serial");
	parsed_serial = settings->expected_serial;
	if (coo_json_extract_optional_u16(json, "expected_serial",
					  &parsed_serial, &serial_changed) != 0) {
		return -EINVAL;
	}
	if (serial_changed) {
		if (parsed_serial == 0U) {
			return -ERANGE;
		}
		settings->expected_serial = parsed_serial;
		*changed = true;
	}

	return 0;
}

int laser_settings_set(const struct coo_cmd_request *cmd, struct coo_cmd_response *out)
{
	/* Keep driver/model settings fixed for acquisition and later application. */
	if (laser_calibration_active()) {
		return coo_cmd_busy_response(out, cmd);
	}

	enum hispec_laser_id id;
	struct app_laser_channel_settings settings;
	char name[16] = {0};
	char settings_json[COO_CMD_PAYLOAD_MAX] = {0};
	char invalid[64] = "settings";
	bool changed = false;
	bool persist = false;
	int rc;

	if (command_laser_id_from_payload(cmd, &id, name, sizeof(name)) != 0) {
		return coo_cmd_error(out, cmd, "missing or invalid laser name");
	}
	rc = hispec_laser_get_channel_settings(id, &settings);
	if (rc != 0) {
		return laser_cmd_failure(out, cmd, id, "settings read", rc, NULL);
	}

	rc = coo_json_extract_object(cmd->payload, "settings", settings_json, sizeof(settings_json));
	if (rc != COO_JSON_EXTRACT_OK) {
		return coo_cmd_error(out, cmd, "missing or invalid settings object");
	}
	if (coo_json_extract_optional_bool(cmd->payload, "persist",
					   &persist, NULL) != 0) {
		return coo_cmd_error(out, cmd, "invalid persist");
	}

	rc = laser_parse_settings_update(settings_json, &settings, &changed, invalid);
	if (rc != 0) {
		char message[112];

		snprintk(message, sizeof(message), "settings.%s invalid or read-only, see catalog", invalid);
		return coo_cmd_error(out, cmd, message);
	}
	if (!changed) {
		return coo_cmd_error(out, cmd, "no laser settings fields supplied");
	}
	if (hispec_laser_validate_channel_settings(id, &settings) != 0) {
		return coo_cmd_error(out, cmd, "laser settings out of range, see catalog");
	}

	rc = throughput_monitor_update_laser_settings(id, &settings, persist, out->payload, sizeof(out->payload));
	if (rc != 0) {
		return laser_cmd_failure(out, cmd, id, "update settings", rc, out->payload);
	}
	return coo_cmd_ok(out, cmd);
}

static int json_append_named_float(char *payload, size_t payload_len,
				   size_t *off, const char *name,
				   double value, int precision)
{
	if (coo_json_append(payload, payload_len, off, ",\"%s\":", name) != 0) {
		return -ENOSPC;
	}
	return coo_json_append_float_or_null(payload, payload_len, off,
					     value, precision);
}

int laser_status_get(const struct coo_cmd_request *cmd, struct coo_cmd_response *out)
{
	enum hispec_laser_id id;
	struct hispec_laser_status s = {0};
	char name[16] = {0};
	char payload[COO_CMD_PAYLOAD_MAX] = {0};
	size_t off = 0U;
	int rc;

	if (command_laser_id_from_payload(cmd, &id, name, sizeof(name)) != 0) {
		return coo_cmd_error(out, cmd, "missing or invalid laser name");
	}

	rc = hispec_laser_get_status(id, true, &s);
	if (rc != 0) {
		return laser_cmd_failure(out, cmd, id, "engineering status read", rc, NULL);
	}
	if (coo_json_append(payload, sizeof(payload), &off,
			    "{\"name\":\"%s\",\"read_rc\":%d,\"powered\":%s,"
			    "\"dev_id\":%u,\"serial\":%u,\"expected_serial\":%u,"
			    "\"serial_ok\":%s,"
			    "\"raw_state\":%u,\"raw_lock\":%u,\"raw_tec\":%u,"
			    "\"blocking_lock\":%u,\"blocked_reason\":",
			    s.name, rc, s.bank_powered ? "true" : "false",
			    s.device_id, s.serial_number, s.expected_serial,
			    s.serial_matches ? "true" : "false",
			    s.device_state, s.lock_status, s.tec_state,
			    s.blocking_lock_status) != 0 ||
	    coo_json_append_string_or_null(payload, sizeof(payload), &off,
					   s.blocked_reason) != 0 ||
	    coo_json_append(payload, sizeof(payload), &off,
			    ",\"op_started\":%s,\"ready\":%s,"
			    "\"curr_set_internal\":%s,\"enable_internal\":%s,"
			    "\"ext_ntc_denied\":%s,\"interlock_denied\":%s,"
			    "\"interlock\":%s,\"ext_ntc_interlock\":%s,"
			    "\"ld_overcurrent\":%s,\"ld_overheat\":%s,"
			    "\"tec_started\":%s,\"tec_set_internal\":%s,"
			    "\"tec_enable_internal\":%s,\"tec_error\":%s,"
			    "\"tec_selfheat\":%s",
			    s.operation_started ? "true" : "false",
			    s.ready_to_operate ? "true" : "false",
			    s.current_set_internal ? "true" : "false",
			    s.enable_internal ? "true" : "false",
			    s.external_ntc_denied ? "true" : "false",
			    s.interlock_denied ? "true" : "false",
			    s.lock_interlock ? "true" : "false",
			    s.lock_external_ntc_interlock ? "true" : "false",
			    s.lock_ld_overcurrent ? "true" : "false",
			    s.lock_ld_overheat ? "true" : "false",
			    s.tec_started ? "true" : "false",
			    s.tec_set_internal ? "true" : "false",
			    s.tec_enable_internal ? "true" : "false",
			    s.lock_tec_error ? "true" : "false",
			    s.lock_tec_selfheat ? "true" : "false") != 0 ||
	    json_append_named_float(payload, sizeof(payload), &off,
				    "i_mA", s.current_set_ma, 3) != 0 ||
	    json_append_named_float(payload, sizeof(payload), &off,
				    "curr_meas_ma", s.current_measured_ma, 3) != 0 ||
	    json_append_named_float(payload, sizeof(payload), &off,
				    "curr_min_ma", s.current_min_ma, 3) != 0 ||
	    json_append_named_float(payload, sizeof(payload), &off,
				    "max_current_ma", s.max_current_ma, 3) != 0 ||
	    json_append_named_float(payload, sizeof(payload), &off,
				    "drv_max_ma", s.current_max_limit_ma, 3) != 0 ||
	    json_append_named_float(payload, sizeof(payload), &off,
				    "ocp_ma", s.current_protection_threshold_ma, 3) != 0 ||
	    json_append_named_float(payload, sizeof(payload), &off,
				    "current_cal_pct", s.current_cal_pct, 3) != 0 ||
	    json_append_named_float(payload, sizeof(payload), &off,
				    "diode_v", s.voltage_v, 3) != 0 ||
	    json_append_named_float(payload, sizeof(payload), &off,
				    "tec_temp_set_c", s.tec_temperature_set_c, 3) != 0 ||
	    json_append_named_float(payload, sizeof(payload), &off,
				    "tec_temp_c", s.tec_temperature_measured_c, 3) != 0 ||
	    json_append_named_float(payload, sizeof(payload), &off,
				    "pcb_temp_c", s.pcb_temperature_c, 3) != 0 ||
	    json_append_named_float(payload, sizeof(payload), &off,
				    "tec_ma", (double)s.tec_current_measured_a * 1000.0, 3) != 0 ||
	    json_append_named_float(payload, sizeof(payload), &off,
				    "tec_curr_lim_a", s.tec_curr_lim_a, 3) != 0 ||
	    json_append_named_float(payload, sizeof(payload), &off,
				    "tec_v", s.tec_voltage_v, 3) != 0 ||
	    (s.bank_powered ?
	     coo_json_append(payload, sizeof(payload), &off, ",\"pid\":[%u,%u,%u]",
				     s.tec_pid.kp, s.tec_pid.ki, s.tec_pid.kd) :
	     coo_json_append(payload, sizeof(payload), &off, ",\"pid\":null")) != 0 ||
	    json_append_named_float(payload, sizeof(payload), &off,
				    "ntc_coeff_pct_per_c", s.ntc_coeff_pct_per_c, 6) != 0 ||
	    coo_json_append(payload, sizeof(payload), &off, "}") != 0) {
		return coo_cmd_error(out, cmd, "laser engineering status response too large");
	}
	return coo_cmd_reply(out, cmd, COO_CMD_RESP_OK, payload);
}

/* Commands only parse and shape replies; the calibration owner performs all
 * acquisition, hardware sequencing, fitting, settings and optional NVS work. */

/* Build a reply from the last coherent calibration snapshot. May wait on the
 * status mutex; the command runtime owns transport publication. */
static int laser_calibration_reply(const struct coo_cmd_request *cmd,
				   struct coo_cmd_response *out, int rc)
{
	struct laser_calibration_status status;
	laser_calibration_get_status(&status);

	int reply = coo_cmd_reply(out, cmd, rc == 0 ? COO_CMD_RESP_OK : COO_CMD_RESP_ERROR,
		NULL);
	if (reply != 0) {
		return reply;
	}
	if (laser_calibration_format_status(out->payload, sizeof(out->payload), &status) != 0) {
		return coo_cmd_error(out, cmd, "laser calibration status too large");
	}
	out->payload_len = strlen(out->payload);
	return 0;
}

/** Query calibration progress/results without changing acquisition or hardware. */
int laser_calibration_get(const struct coo_cmd_request *cmd, struct coo_cmd_response *out)
{
	return laser_calibration_reply(cmd, out, 0);
}

/** Parse one calibration action and delegate to its domain owner. Start/continue,
 * stop and defaults restoration can block on hardware or a fit join; requested
 * persistence belongs to the owner. This handler only constructs the reply. */
int laser_calibration_set(const struct coo_cmd_request *cmd, struct coo_cmd_response *out)
{
	bool stop = false;
	bool resume = false;
	bool restore = false;
	bool persist = false;

	/* Select the action first. A stop does not need a valid route or optical
	 * assumptions, so dispatch it before parsing acquisition-only arguments. */
	if (coo_json_extract_optional_bool(cmd->payload, "stop", &stop, NULL) != 0 ||
	    coo_json_extract_optional_bool(cmd->payload, "continue", &resume, NULL) != 0 ||
	    coo_json_extract_optional_bool(cmd->payload, "restore_defaults", &restore, NULL) != 0 ||
	    coo_json_extract_optional_bool(cmd->payload, "persist", &persist, NULL) != 0 ||
	    (unsigned)stop + (unsigned)resume + (unsigned)restore > 1) {
		return coo_cmd_error(out, cmd, "invalid or conflicting calibration action");
	}
	if (stop) {
		return laser_calibration_reply(cmd, out, laser_calibration_stop(NULL));
	}

	/* Missing loss stays NAN so a loss-fitting start cannot silently assume
	 * a lossless cable. Continue may retain its existing value when omitted. */
	double loopback = NAN;
	int parsed = coo_json_extract_double(cmd->payload, "loopback_loss_db", &loopback);
	if (parsed == COO_JSON_EXTRACT_ERR ||
	    (parsed == COO_JSON_EXTRACT_OK && (!isfinite(loopback) || loopback < 0.0))) {
		return coo_cmd_error(out, cmd, "loopback_loss_db must be finite and nonnegative");
	}

	if (resume) {
		/* Resume uses the original run's settings; only its second cable loss
		 * may change after recabling. Reject arguments that would imply a restart. */
		if (coo_json_validate_top_level_keys(cmd->payload, "continue,loopback_loss_db", NULL, 0) != 0) {
			return coo_cmd_error(out, cmd, "continue accepts only loopback_loss_db");
		}
		int rc = laser_calibration_continue(parsed == COO_JSON_EXTRACT_OK, loopback, NULL);
		if (rc == -EINVAL) {
			return coo_cmd_error(out, cmd, "calibration is not awaiting the other output");
		}
		return laser_calibration_reply(cmd, out, rc);
	}

	/* Both start and scoped restore need an explicit source/output identity. */
	char name[16];
	char output[16];
	char fiber[4] = "S";
	struct laser_calibration_request req = {
		.output = output,
		.fiber = 'S',
		.dwell_ms = 1000,
		.loopback_loss_db = loopback,
		.persist = persist,
	};
	if (coo_json_extract_string(cmd->payload, "laser", name, sizeof(name)) != COO_JSON_EXTRACT_OK ||
	    hispec_laser_id_from_name(name, &req.laser) != 0 ||
	    coo_json_extract_string(cmd->payload, "output", output, sizeof(output)) != COO_JSON_EXTRACT_OK) {
		return coo_cmd_error(out, cmd, "laser and output are required");
	}

	if (restore) {
		if (coo_json_validate_top_level_keys(cmd->payload, "restore_defaults,laser,output,persist", NULL, 0) != 0) {
			return coo_cmd_error(out, cmd, "restore accepts laser, output, persist");
		}

		struct laser_calibration_restore_result r;
		int rc = laser_calibration_restore_defaults(req.laser, output, persist, &r);
		/* A failed part does not undo another successful part. Return all
		 * application/persistence outcomes even when the overall rc is nonzero. */
		char payload[320];
		snprintk(payload, sizeof(payload), "{\"rc\":%d,\"laser_applied\":%s,\"route_applied\":%s,"
			"\"laser_persisted\":%s,\"route_persisted\":%s,\"laser_error\":%d,\"route_error\":%d,"
			"\"laser_persist_error\":%d,\"route_persist_error\":%d}", rc,
			r.laser_applied ? "true" : "false", r.route_applied ? "true" : "false",
			r.laser_persisted ? "true" : "false", r.route_persisted ? "true" : "false",
			r.laser_error, r.route_error, r.laser_persist_error, r.route_persist_error);
		return coo_cmd_reply(out, cmd, rc == 0 ? COO_CMD_RESP_OK : COO_CMD_RESP_ERROR, payload);
	}

	/* Remaining arguments belong only to a new acquisition. Normalize the
	 * return-fiber selector here; physical limits are checked by the owner. */
	parsed = coo_json_extract_string(cmd->payload, "fiber", fiber, sizeof(fiber));
	if (parsed == COO_JSON_EXTRACT_ERR || fiber[1] != '\0' ||
	    (fiber[0] != 'S' && fiber[0] != 'M' && fiber[0] != 's' && fiber[0] != 'm')) {
		return coo_cmd_error(out, cmd, "fiber must be S or M");
	}
	req.fiber = (fiber[0] == 'S' || fiber[0] == 's') ? 'S' : 'M';

	if (coo_json_extract_optional_u32(cmd->payload, "dwell_ms", &req.dwell_ms, NULL) != 0 ||
	    coo_json_extract_optional_bool(cmd->payload, "determine_static_loss", &req.determine_static_loss, NULL) != 0 ||
	    coo_json_extract_optional_bool(cmd->payload, "update_noise", &req.update_noise, NULL) != 0 ||
	    coo_json_extract_optional_bool(cmd->payload, "other_output", &req.other_output, NULL) != 0) {
		return coo_cmd_error(out, cmd, "invalid calibration options");
	}

	int rc = laser_calibration_start(&req, NULL);
	if (rc != 0) {
		return coo_cmd_error_rc(out, cmd,
			"laser calibration start failed; check PD power/dark, reference, route and options", rc);
	}
	return laser_calibration_reply(cmd, out, 0);
}

/** Return binary metadata or a record chunk through the command response.
 * May wait for a locked data copy; no hardware I/O. Serial replies are textual,
 * so this raw wire representation is restricted to MQTT. */
int laser_calibration_records_get(const struct coo_cmd_request *cmd, struct coo_cmd_response *out)
{
	if (cmd->source == COO_CMD_SOURCE_SERIAL) {
		return coo_cmd_error(out, cmd, "calibration records are binary MQTT payloads");
	}

	/* An empty suffix selects metadata; a numeric suffix selects raw records. */
	const char *suffix = coo_cmd_key_suffix_after(cmd->key, "laser/calibrate/records");
	int chunk = -1;
	if (*suffix != '\0') {
		char *end;
		if (*suffix < '0' || *suffix > '9') {
			return coo_cmd_error(out, cmd, "invalid record chunk");
		}
		unsigned long value = strtoul(suffix, &end, 10);
		if (*end != '\0' || value > 127) {
			return coo_cmd_error(out, cmd, "invalid record chunk");
		}
		chunk = (int)value;
	}

	int rc = coo_cmd_reply(out, cmd, COO_CMD_RESP_OK, NULL);
	if (rc != 0) {
		return rc;
	}
	size_t written;
	rc = laser_calibration_write_data(out->payload, sizeof(out->payload), chunk, &written);
	if (rc != 0) {
		return coo_cmd_error_rc(out, cmd, "calibration records unavailable or chunk out of range", rc);
	}
	out->payload_len = written;
	return 0;
}
