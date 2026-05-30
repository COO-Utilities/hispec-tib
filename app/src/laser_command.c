/*
 * Copyright (c) 2026 Caltech Optical Observatories
 * SPDX-License-Identifier: Apache-2.0
 */

#include "laser_command.h"

#include <errno.h>
#include <float.h>
#include <stdint.h>
#include <stdio.h>

#include <zephyr/sys/util.h>

#include "app_settings.h"
#include "laserbank_tempcontrol.h"
#include "lasers.h"
#include "throughput_monitor.h"

#include <coo_commons/json_utils.h>

#define LASERBANK_FAULT_CLEAR_OFF_MS 250U

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

static bool parse_laserbank_override_request(const struct coo_cmd_request *cmd,
					     const char *key,
					     const struct coo_json_string_choice *choices,
					     size_t choice_count,
					     int *mode_value)
{
	const char *suffix;

	if (mode_value == NULL) {
		return false;
	}

	suffix = coo_cmd_key_suffix_after(cmd != NULL ? cmd->key : NULL, key);
	if (coo_json_match_string_choice(suffix, choices, choice_count,
					 mode_value) == 0) {
		return true;
	}
	if (coo_cmd_payload_empty(cmd)) {
		return false;
	}
	return coo_json_extract_string_choice(cmd->payload, "override",
					      choices, choice_count,
					      mode_value) == COO_JSON_EXTRACT_OK;
}

struct coo_cmd_response laserbank_power(const struct coo_cmd_request *cmd)
{
	enum hispec_laser_bank_power_mode mode;
	char payload[128] = {0};
	int mode_value;
	int rc;

	if (cmd != NULL &&
	    (cmd->msg_type == COO_CMD_EFFECT ||
	     coo_cmd_key_suffix_after(cmd->key, "laserbank/power")[0] != '\0')) {
		if (!parse_laserbank_override_request(cmd, "laserbank/power",
						      laserbank_power_mode_choices,
						      ARRAY_SIZE(laserbank_power_mode_choices),
						      &mode_value)) {
			return coo_cmd_error(cmd, "override must be auto, override_on, or override_off");
		}
		mode = (enum hispec_laser_bank_power_mode)mode_value;
		rc = hispec_laser_bank_power_mode_set(mode);
		if (rc != 0) {
			mode = hispec_laser_bank_power_mode_get();
			snprintk(payload, sizeof(payload),
				 "{\"error\":\"laser bank power mode failed\","
				 "\"rc\":%d,\"mode\":\"%s\",\"powered\":%s}",
				 rc,
				 hispec_laser_bank_power_mode_name(mode),
				 hispec_laser_bank_power_is_enabled() ? "true" : "false");
			return coo_cmd_reply(cmd, COO_CMD_RESP_ERROR, payload);
		}
	}

	mode = hispec_laser_bank_power_mode_get();
	snprintk(payload, sizeof(payload),
		 "{\"mode\":\"%s\",\"powered\":%s}",
		 hispec_laser_bank_power_mode_name(mode),
		 hispec_laser_bank_power_is_enabled() ? "true" : "false");
	return coo_cmd_reply(cmd, COO_CMD_RESP_OK, payload);
}

struct coo_cmd_response laserbank_clearfaults(const struct coo_cmd_request *cmd)
{
	uint32_t off_ms = 0U;
	char payload[MAX_PAYLOAD_LEN] = {0};
	int rc;

	rc = hispec_laser_bank_clear_faults(LASERBANK_FAULT_CLEAR_OFF_MS, &off_ms);
	if (rc != 0) {
		return coo_cmd_error_rc(cmd, "laser bank fault clear failed", rc);
	}

	snprintf(payload, sizeof(payload), "{\"off_ms\":%u}", off_ms);
	return coo_cmd_reply(cmd, COO_CMD_RESP_OK, payload);
}

static void laserbank_tempcontrol_status_payload(char *payload, size_t payload_len)
{
	struct laserbank_tempcontrol_status status = {0};

	laserbank_tempcontrol_get_status(&status);
	snprintk(payload, payload_len,
		 "{\"heater_mode\":\"%s\","
		 "\"heater_on\":%s,\"bank_power\":%s,"
		 "\"ambient_valid\":%s,\"ambient_c\":%.2f,"
		 "\"valid_temps\":%u,\"stale_temps\":%u,"
		 "\"any_disabled_below_15c\":%s,"
		 "\"any_disabled_above_off_threshold\":%s,"
		 "\"all_tecs_enabled\":%s,\"all_tecs_enabled_ms\":%u,"
		 "\"last_error\":%d,\"last_poll_age_ms\":%u}",
		 laserbank_heater_mode_name(status.heater_mode),
		 status.heater_on ? "true" : "false",
		 status.bank_powered ? "true" : "false",
		 status.ambient_valid ? "true" : "false",
		 (double)status.ambient_c,
		 status.valid_temp_count,
		 status.stale_temp_count,
		 status.any_disabled_below_15c ? "true" : "false",
		 status.any_disabled_above_off_threshold ? "true" : "false",
		 status.all_tecs_enabled ? "true" : "false",
		 status.all_tecs_enabled_ms,
		 status.last_error,
		 status.last_poll_age_ms);
}

struct coo_cmd_response laserbank_heater(const struct coo_cmd_request *cmd)
{
	enum laserbank_heater_mode mode;
	char payload[MAX_PAYLOAD_LEN] = {0};
	int mode_value;

	if (cmd != NULL &&
	    (cmd->msg_type == COO_CMD_EFFECT ||
	     coo_cmd_key_suffix_after(cmd->key, "laserbank/heater")[0] != '\0')) {
		if (!parse_laserbank_override_request(cmd, "laserbank/heater",
						      heater_mode_choices,
						      ARRAY_SIZE(heater_mode_choices),
						      &mode_value)) {
			return coo_cmd_reply(cmd, COO_CMD_RESP_ERROR,
					     "{\"error\":\"Use laserbank/heater auto|override_on|override_off\"}");
		}
		mode = (enum laserbank_heater_mode)mode_value;
		int rc = laserbank_tempcontrol_set_heater_mode(mode, true);
		if (rc != 0) {
			return coo_cmd_error_rc(cmd, "laser bank heater relay unavailable", rc);
		}
	}

	laserbank_tempcontrol_status_payload(payload, sizeof(payload));
	return coo_cmd_reply(cmd, COO_CMD_RESP_OK, payload);
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
	const laserprops_t *props = status->properties;

	if (coo_json_append(payload, payload_len, &off,
			    "{\"name\":\"%s\",\"powered\":%s,"
			    "\"tec_on_s\":%.1f,\"emit_on_s\":%.1f,"
			    "\"emit_total_s\":%.1f,\"temp_c\":",
			    status->name,
			    status->bank_powered ? "true" : "false",
			    (double)status->tec_on_time_s,
			    (double)status->current_on_time_s,
			    status->total_emitting_s) != 0 ||
	    coo_json_append_float_or_null(payload, payload_len, &off,
					  status->tec_temperature_measured_c, 2) != 0 ||
	    coo_json_append(payload, payload_len, &off,
			    ",\"current_ma\":") != 0 ||
	    coo_json_append_float_or_null(payload, payload_len, &off,
					  status->current_set_ma, 2) != 0 ||
	    coo_json_append(payload, payload_len, &off,
			    ",\"level\":") != 0 ||
	    coo_json_append_float_or_null(payload, payload_len, &off,
					  status->level_percent, 2) != 0 ||
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
			    ",\"offin_s\":%lld,\"oc_fault\":%s}",
			    (long long)status->off_in_s,
			    status->lock_ld_overcurrent ? "true" : "false") != 0) {
		return -ENOSPC;
	}

	return 0;
}

struct coo_cmd_response laser_get(const struct coo_cmd_request *cmd)
{
	enum hispec_laser_id id;
	struct hispec_laser_status status = {0};
	char name[16] = {0};
	char payload[MAX_PAYLOAD_LEN] = {0};
	int rc;

	if (command_laser_id_from_payload(cmd, &id, name, sizeof(name)) != 0) {
		return coo_cmd_error(cmd, "missing or invalid laser name");
	}

	rc = hispec_laser_get_status(id, &status);
	if (rc != 0 && !status.bank_powered) {
		return coo_cmd_error_rc(cmd, "laser status failed", rc);
	}
	if (laser_append_compact_status(payload, sizeof(payload), &status) != 0) {
		return coo_cmd_error(cmd, "laser response too large");
	}
	return rc == 0 ? coo_cmd_reply(cmd, COO_CMD_RESP_OK, payload) :
	       coo_cmd_error_rc(cmd, "laser status failed", rc);
}

struct coo_cmd_response laser_set(const struct coo_cmd_request *cmd)
{
	enum hispec_laser_id id;
	struct app_laser_channel_settings settings;
	char name[16] = {0};
	float level = 0.0f;
	uint32_t autooff_s;
	int parse_rc;
	int rc;

	if (command_laser_id_from_payload(cmd, &id, name, sizeof(name)) != 0) {
		return coo_cmd_error(cmd, "missing or invalid laser name");
	}
	parse_rc = coo_json_extract_float(cmd->payload, "level", &level);
	if (parse_rc != COO_JSON_EXTRACT_OK || level < 0.0f || level > 100.0f) {
		return coo_cmd_error(cmd, "level must be 0..100");
	}
	rc = hispec_laser_get_channel_settings(id, &settings);
	if (rc != 0) {
		return coo_cmd_error_rc(cmd, "laser settings unavailable", rc);
	}
	autooff_s = settings.autooff_s;
	if (coo_json_extract_optional_u32(cmd->payload, "autooff_s",
					  &autooff_s, NULL) != 0) {
		return coo_cmd_error(cmd, "invalid autooff_s");
	}

	throughput_monitor_note_laser_changed(id);
	rc = hispec_laser_set_output_percent_autooff(id, level, autooff_s);
	if (rc != 0) {
		return coo_cmd_error_rc(cmd, "laser level failed", rc);
	}
	return coo_cmd_ok(cmd);
}

struct coo_cmd_response laser_tune_get(const struct coo_cmd_request *cmd)
{
	enum hispec_laser_id id;
	char name[16] = {0};
	char payload[MAX_PAYLOAD_LEN];

	if (command_laser_id_from_payload(cmd, &id, name, sizeof(name)) != 0) {
		return coo_cmd_error(cmd, "missing or invalid laser name");
	}
	snprintk(payload, sizeof(payload),
		 "{\"name\":\"%s\",\"tune_nm\":%.4f}",
		 hispec_laser_name(id),
		 (double)hispec_laser_get_tune_delta_nm(id));
	return coo_cmd_reply(cmd, COO_CMD_RESP_OK, payload);
}

struct coo_cmd_response laser_tune_set(const struct coo_cmd_request *cmd)
{
	enum hispec_laser_id id;
	char name[16] = {0};
	float delta_nm = 0.0f;
	int parse_rc;
	int rc;

	if (command_laser_id_from_payload(cmd, &id, name, sizeof(name)) != 0) {
		return coo_cmd_error(cmd, "missing or invalid laser name");
	}
	parse_rc = coo_json_extract_float(cmd->payload, "tune_nm", &delta_nm);
	if (parse_rc == COO_JSON_EXTRACT_MISSING) {
		parse_rc = coo_json_extract_float(cmd->payload, "delta_nm", &delta_nm);
	}
	if (parse_rc != COO_JSON_EXTRACT_OK) {
		return coo_cmd_error(cmd, "missing tune_nm");
	}
	throughput_monitor_note_laser_changed(id);
	rc = hispec_laser_set_tune_delta_nm(id, delta_nm, true);
	if (rc != 0) {
		return coo_cmd_error_rc(cmd, "laser tune failed", rc);
	}
	return coo_cmd_ok(cmd);
}

static int laser_settings_payload(char *payload, size_t payload_len,
				  enum hispec_laser_id id,
				  const struct app_laser_channel_settings *settings)
{
	const laserprops_t *p = &settings->properties;
	int written;

	written = snprintk(payload, payload_len,
		"{\"name\":\"%s\",\"settings\":{"
		"\"model\":\"%s\",\"nominal_current_ma\":%.3f,"
		"\"max_current_ma\":%.3f,\"current_set_calibration_pct\":%.3f,"
		"\"threshold_current_ma\":%.3f,\"efficiency_mw_per_ma\":%.6f,"
		"\"wavelength_nm\":%.3f,\"operating_temp_range_c\":[%.2f,%.2f],"
		"\"default_operating_temp_c\":%.2f,\"thermistor_kohm\":%.2f,"
		"\"isolation_db\":%.2f,\"tec_max_current_a\":%.3f,"
		"\"tec_pid\":{\"p\":%u,\"i\":%u,\"d\":%u},"
		"\"disable_tec_at_autooff\":%s,"
		"\"ntc_t_coefficient_per_c\":%.6f,"
		"\"dlambda_dT_nm_per_k\":%.6f,"
		"\"dlambda_dA_nm_per_ma\":%.6f,"
		"\"autooff_s\":%u,\"tune_nm\":%.4f,"
		"\"emit_total_s\":%.1f}}",
		hispec_laser_name(id), p->model_number,
		(double)p->nominal_current_ma,
		(double)p->max_current_ma,
		(double)settings->current_set_calibration_pct,
		(double)p->threshold_current_ma,
		(double)p->efficiency_mw_per_ma,
		(double)p->wavelength_nm,
		(double)p->operating_temp_range_c.min_c,
		(double)p->operating_temp_range_c.max_c,
		(double)p->operating_temp_c,
		(double)p->thermistor_kohm,
		(double)p->isolation_db,
		(double)p->tec_max_current_a,
		p->tec_pid.kp, p->tec_pid.ki, p->tec_pid.kd,
		settings->disable_tec_at_autooff ? "true" : "false",
		(double)p->ntc_t_coefficient_per_c,
		(double)p->dlambda_dT_nm_per_k,
		(double)p->dlambda_dA_nm_per_ma,
		settings->autooff_s,
		(double)settings->tune_delta_nm,
		settings->total_emitting_s);

	return written >= 0 && written < (int)payload_len ? 0 : -ENOSPC;
}

struct coo_cmd_response laser_settings_get(const struct coo_cmd_request *cmd)
{
	enum hispec_laser_id id;
	struct app_laser_channel_settings settings;
	char name[16] = {0};
	char payload[MAX_PAYLOAD_LEN] = {0};
	int rc;

	if (command_laser_id_from_payload(cmd, &id, name, sizeof(name)) != 0) {
		return coo_cmd_error(cmd, "missing or invalid laser name");
	}
	rc = hispec_laser_get_channel_settings(id, &settings);
	if (rc != 0) {
		return coo_cmd_error_rc(cmd, "laser settings unavailable", rc);
	}
	if (laser_settings_payload(payload, sizeof(payload), id, &settings) != 0) {
		return coo_cmd_error(cmd, "laser settings response too large");
	}
	return coo_cmd_reply(cmd, COO_CMD_RESP_OK, payload);
}

static int laser_parse_settings_update(const char *json,
				       struct app_laser_channel_settings *settings,
				       bool *changed)
{
	double range[2] = {0};
	size_t range_len = 0U;
	char pid_json[128] = {0};
	int rc;

	if (json == NULL || settings == NULL || changed == NULL) {
		return -EINVAL;
	}

#define LASER_PARSE_FLOAT(key, field) do { \
		if (coo_json_extract_optional_float_range(json, key, &(field), \
							  changed, -FLT_MAX, \
							  FLT_MAX) != 0) \
			return -EINVAL; \
	} while (0)

	LASER_PARSE_FLOAT("nominal_current_ma", settings->properties.nominal_current_ma);
	LASER_PARSE_FLOAT("max_current_ma", settings->properties.max_current_ma);
	LASER_PARSE_FLOAT("threshold_current_ma", settings->properties.threshold_current_ma);
	LASER_PARSE_FLOAT("efficiency_mw_per_ma", settings->properties.efficiency_mw_per_ma);
	LASER_PARSE_FLOAT("wavelength_nm", settings->properties.wavelength_nm);
	LASER_PARSE_FLOAT("current_set_calibration_pct",
			  settings->current_set_calibration_pct);
	LASER_PARSE_FLOAT("current_set_calibration_%",
			  settings->current_set_calibration_pct);
	LASER_PARSE_FLOAT("default_operating_temp_c", settings->properties.operating_temp_c);
	LASER_PARSE_FLOAT("tec_max_current_a", settings->properties.tec_max_current_a);
	LASER_PARSE_FLOAT("dlambda_dT_nm_per_k", settings->properties.dlambda_dT_nm_per_k);
	LASER_PARSE_FLOAT("dlambda_dA_nm_per_ma", settings->properties.dlambda_dA_nm_per_ma);

#undef LASER_PARSE_FLOAT

	rc = coo_json_extract_double_array(json, "operating_temp_range_c",
					   range, ARRAY_SIZE(range), &range_len);
	if (rc == COO_JSON_EXTRACT_ERR) {
		return -EINVAL;
	}
	if (rc == COO_JSON_EXTRACT_OK) {
		if (range_len != 2U) {
			return -ERANGE;
		}
		settings->properties.operating_temp_range_c.min_c = (float)range[0];
		settings->properties.operating_temp_range_c.max_c = (float)range[1];
		*changed = true;
	}

	rc = coo_json_extract_object(json, "tec_pid", pid_json, sizeof(pid_json));
	if (rc == COO_JSON_EXTRACT_ERR) {
		return -EINVAL;
	}
	if (rc == COO_JSON_EXTRACT_OK) {
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

	if (coo_json_extract_optional_bool(json, "disable_tec_at_autooff",
					   &settings->disable_tec_at_autooff,
					   changed) != 0) {
		return -EINVAL;
	}

	if (coo_json_extract_optional_u32(json, "autooff_s",
					  &settings->autooff_s, changed) != 0) {
		return -EINVAL;
	}

	return 0;
}

struct coo_cmd_response laser_settings_set(const struct coo_cmd_request *cmd)
{
	enum hispec_laser_id id;
	struct app_laser_channel_settings settings;
	char name[16] = {0};
	char settings_json[MAX_PAYLOAD_LEN] = {0};
	const char *json;
	bool changed = false;
	int rc;

	if (command_laser_id_from_payload(cmd, &id, name, sizeof(name)) != 0) {
		return coo_cmd_error(cmd, "missing or invalid laser name");
	}
	rc = hispec_laser_get_channel_settings(id, &settings);
	if (rc != 0) {
		return coo_cmd_error_rc(cmd, "laser settings unavailable", rc);
	}

	rc = coo_json_extract_object(cmd->payload, "settings", settings_json, sizeof(settings_json));
	if (rc == COO_JSON_EXTRACT_ERR) {
		return coo_cmd_error(cmd, "invalid settings object");
	}
	json = rc == COO_JSON_EXTRACT_OK ? settings_json : cmd->payload;

	rc = laser_parse_settings_update(json, &settings, &changed);
	if (rc != 0) {
		return coo_cmd_error_rc(cmd, "invalid laser settings", rc);
	}
	if (!changed) {
		return coo_cmd_error(cmd, "no laser settings fields supplied");
	}

	throughput_monitor_note_laser_changed(id);
	rc = hispec_laser_update_channel_settings(id, &settings, true);
	if (rc != 0) {
		return coo_cmd_error_rc(cmd, "laser settings update failed", rc);
	}
	return coo_cmd_ok(cmd);
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

struct coo_cmd_response laser_engstatus_get(const struct coo_cmd_request *cmd)
{
	enum hispec_laser_id id;
	struct hispec_laser_status s = {0};
	char name[16] = {0};
	char payload[MAX_PAYLOAD_LEN] = {0};
	size_t off = 0U;
	int rc;

	if (command_laser_id_from_payload(cmd, &id, name, sizeof(name)) != 0) {
		return coo_cmd_error(cmd, "missing or invalid laser name");
	}

	rc = hispec_laser_get_status(id, &s);
	if (coo_json_append(payload, sizeof(payload), &off,
			    "{\"name\":\"%s\",\"read_rc\":%d,\"powered\":%s,"
			    "\"dev_id\":%u,\"serial\":%u,\"serial_ok\":%s,"
			    "\"raw_state\":%u,\"raw_lock\":%u,\"raw_tec\":%u,"
			    "\"op_started\":%s,\"ready\":%s,"
			    "\"curr_set_internal\":%s,\"enable_internal\":%s,"
			    "\"ext_ntc_denied\":%s,\"interlock_denied\":%s,"
			    "\"interlock\":%s,\"ext_ntc_interlock\":%s,"
			    "\"ld_overcurrent\":%s,\"ld_overheat\":%s,"
			    "\"tec_started\":%s,\"tec_set_internal\":%s,"
			    "\"tec_enable_internal\":%s,\"tec_error\":%s,"
			    "\"tec_selfheat\":%s",
			    s.name, rc, s.bank_powered ? "true" : "false",
			    s.device_id, s.serial_number,
			    s.serial_matches ? "true" : "false",
			    s.device_state, s.lock_status, s.tec_state,
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
				    "curr_ma", s.current_set_ma, 3) != 0 ||
	    json_append_named_float(payload, sizeof(payload), &off,
				    "curr_meas_ma", s.current_measured_ma, 3) != 0 ||
	    json_append_named_float(payload, sizeof(payload), &off,
				    "curr_min_ma", s.current_min_ma, 3) != 0 ||
	    json_append_named_float(payload, sizeof(payload), &off,
				    "curr_max_ma", s.current_max_ma, 3) != 0 ||
	    json_append_named_float(payload, sizeof(payload), &off,
				    "drv_max_ma", s.current_max_limit_ma, 3) != 0 ||
	    json_append_named_float(payload, sizeof(payload), &off,
				    "ocp_ma", s.current_protection_threshold_ma, 3) != 0 ||
	    json_append_named_float(payload, sizeof(payload), &off,
				    "curr_cal_pct", s.current_set_calibration_pct, 3) != 0 ||
	    json_append_named_float(payload, sizeof(payload), &off,
				    "diode_v", s.voltage_v, 3) != 0 ||
	    json_append_named_float(payload, sizeof(payload), &off,
				    "tec_temp_set_c", s.tec_temperature_set_c, 3) != 0 ||
	    json_append_named_float(payload, sizeof(payload), &off,
				    "tec_temp_c", s.tec_temperature_measured_c, 3) != 0 ||
	    json_append_named_float(payload, sizeof(payload), &off,
				    "pcb_temp_c", s.pcb_temperature_c, 3) != 0 ||
	    json_append_named_float(payload, sizeof(payload), &off,
				    "tec_curr_a", s.tec_current_measured_a, 3) != 0 ||
	    json_append_named_float(payload, sizeof(payload), &off,
				    "tec_curr_lim_a", s.tec_current_limit_a, 3) != 0 ||
	    json_append_named_float(payload, sizeof(payload), &off,
				    "tec_v", s.tec_voltage_v, 3) != 0 ||
	    coo_json_append(payload, sizeof(payload), &off,
			    ",\"pid\":[%u,%u,%u]",
			    s.tec_pid.kp, s.tec_pid.ki, s.tec_pid.kd) != 0 ||
	    json_append_named_float(payload, sizeof(payload), &off,
				    "ntc_t_coeff", s.ntc_t_coefficient_per_c, 6) != 0 ||
	    coo_json_append(payload, sizeof(payload), &off, "}") != 0) {
		return coo_cmd_error(cmd, "laser engineering status response too large");
	}
	return rc == 0 ? coo_cmd_reply(cmd, COO_CMD_RESP_OK, payload) :
	       coo_cmd_error_rc(cmd, "laser engineering status failed", rc);
}
