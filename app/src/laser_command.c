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

static bool parse_laserbank_mode_request(const struct coo_cmd_request *cmd,
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
	return coo_json_extract_string_choice(cmd->payload, "mode",
					      choices, choice_count,
					      mode_value) == COO_JSON_EXTRACT_OK;
}

int laserbank_power(const struct coo_cmd_request *cmd, struct coo_cmd_response *out)
{
	enum hispec_laser_bank_power_mode mode;
	char payload[128] = {0};
	int mode_value;
	int rc;

	if (cmd != NULL &&
	    (cmd->msg_type == COO_CMD_EFFECT ||
	     coo_cmd_key_suffix_after(cmd->key, "laserbank/power")[0] != '\0')) {
		if (!parse_laserbank_mode_request(cmd, "laserbank/power",
						  laserbank_power_mode_choices,
						  ARRAY_SIZE(laserbank_power_mode_choices),
						  &mode_value)) {
			return coo_cmd_error(out, cmd, "mode must be auto, override_on, or override_off");
		}
		mode = (enum hispec_laser_bank_power_mode)mode_value;
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
	char payload[MAX_PAYLOAD_LEN] = {0};
	int rc;

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
	if (status->any_disabled_below_15c) {
		return "warming_disabled_tec";
	}
	if (status->any_disabled_above_off_threshold) {
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
	char payload[MAX_PAYLOAD_LEN] = {0};
	int mode_value;

	if (cmd != NULL &&
	    (cmd->msg_type == COO_CMD_EFFECT ||
	     coo_cmd_key_suffix_after(cmd->key, "laserbank/heater")[0] != '\0')) {
		if (!parse_laserbank_mode_request(cmd, "laserbank/heater",
						  heater_mode_choices,
						  ARRAY_SIZE(heater_mode_choices),
						  &mode_value)) {
			return coo_cmd_reply(out, cmd, COO_CMD_RESP_ERROR,
					     "{\"error\":\"Use laserbank/heater mode=auto|override_on|override_off\"}");
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

static int json_append_seconds_or_null(char *payload, size_t payload_len,
				       size_t *off, bool active, double seconds)
{
	if (!active || seconds < 0.0 || seconds != seconds) {
		return coo_json_append(payload, payload_len, off, "null");
	}

	return coo_json_append(payload, payload_len, off, "%lld",
			       (long long)seconds);
}

static int json_append_i64_or_null(char *payload, size_t payload_len,
				   size_t *off, bool active, int64_t seconds)
{
	if (!active) {
		return coo_json_append(payload, payload_len, off, "null");
	}

	return coo_json_append(payload, payload_len, off, "%lld",
			       (long long)seconds);
}

static int json_append_string_or_null(char *payload, size_t payload_len,
				      size_t *off, const char *value)
{
	if (value == NULL || value[0] == '\0') {
		return coo_json_append(payload, payload_len, off, "null");
	}

	return coo_json_append(payload, payload_len, off, "\"%s\"", value);
}

static int laser_append_compact_status(char *payload, size_t payload_len,
				       const struct hispec_laser_status *status)
{
	size_t off = 0U;
	const laserprops_t *props = status->properties;

	if (coo_json_append(payload, payload_len, &off,
			    "{\"name\":\"%s\",\"powered\":%s,"
			    "\"ready\":%s,\"blocked_reason\":",
			    status->name,
			    status->bank_powered ? "true" : "false",
			    status->ready_to_operate ? "true" : "false") != 0 ||
	    json_append_string_or_null(payload, payload_len, &off,
				       status->blocked_reason) != 0 ||
	    coo_json_append(payload, payload_len, &off,
			    ",\"tec_on_s\":") != 0 ||
	    json_append_seconds_or_null(payload, payload_len, &off,
					status->tec_runtime_active,
					status->tec_on_time_s) != 0 ||
	    coo_json_append(payload, payload_len, &off,
			    ",\"emit_on_s\":") != 0 ||
	    json_append_seconds_or_null(payload, payload_len, &off,
					status->current_runtime_active,
					status->current_on_time_s) != 0 ||
	    coo_json_append(payload, payload_len, &off,
			    ",\"emit_total_s\":") != 0 ||
	    json_append_seconds_or_null(payload, payload_len, &off,
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
			    ",\"off_in_s\":") != 0 ||
	    json_append_i64_or_null(payload, payload_len, &off,
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
	char payload[MAX_PAYLOAD_LEN] = {0};
	int rc;

	if (command_laser_id_from_payload(cmd, &id, name, sizeof(name)) != 0) {
		return coo_cmd_error(out, cmd, "missing or invalid laser name");
	}

	rc = hispec_laser_get_status(id, &status);
	if (rc != 0) {
		return laser_cmd_error_rc(out, cmd, "laser status failed", rc);
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
	double level = 0.0;
	uint32_t autooff_s;
	int parse_rc;
	int rc;

	if (command_laser_id_from_payload(cmd, &id, name, sizeof(name)) != 0) {
		return coo_cmd_error(out, cmd, "missing or invalid laser name");
	}
	parse_rc = coo_json_extract_double(cmd->payload, "level", &level);
	if (parse_rc != COO_JSON_EXTRACT_OK || level < 0.0 || level > 100.0) {
		return coo_cmd_error(out, cmd, "level must be 0..100");
	}
	rc = hispec_laser_get_channel_settings(id, &settings);
	if (rc != 0) {
		return laser_cmd_error_rc(out, cmd, "laser settings unavailable", rc);
	}
	autooff_s = settings.autooff_s;
	if (coo_json_extract_optional_u32(cmd->payload, "autooff_s",
					  &autooff_s, NULL) != 0) {
		return coo_cmd_error(out, cmd, "invalid autooff_s");
	}

	throughput_monitor_note_laser_changed(id);
	rc = hispec_laser_set_output_percent_autooff(id, level, autooff_s);
	if (rc != 0) {
		return laser_cmd_error_rc(out, cmd, "laser level failed", rc);
	}
	return coo_cmd_ok(out, cmd);
}

int laser_tune_get(const struct coo_cmd_request *cmd, struct coo_cmd_response *out)
{
	enum hispec_laser_id id;
	char name[16] = {0};
	char payload[MAX_PAYLOAD_LEN];

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
	enum hispec_laser_id id;
	char name[16] = {0};
	double delta_nm = 0.0;
	int parse_rc;
	int rc;

	if (command_laser_id_from_payload(cmd, &id, name, sizeof(name)) != 0) {
		return coo_cmd_error(out, cmd, "missing or invalid laser name");
	}
	parse_rc = coo_json_extract_double(cmd->payload, "tune_nm", &delta_nm);
	if (parse_rc == COO_JSON_EXTRACT_MISSING) {
		parse_rc = coo_json_extract_double(cmd->payload, "delta_nm", &delta_nm);
	}
	if (parse_rc != COO_JSON_EXTRACT_OK) {
		return coo_cmd_error(out, cmd, "missing tune_nm");
	}
	throughput_monitor_note_laser_changed(id);
	rc = hispec_laser_set_tune_delta_nm(id, delta_nm, true);
	if (rc != 0) {
		return laser_cmd_error_rc(out, cmd, "laser tune failed", rc);
	}
	return coo_cmd_ok(out, cmd);
}

static int laser_settings_payload(char *payload, size_t payload_len,
				  enum hispec_laser_id id,
				  const struct app_laser_channel_settings *settings)
{
	const laserprops_t *p = &settings->properties;
	int written;

	written = snprintk(payload, payload_len,
		"{\"name\":\"%s\",\"settings\":{"
		"\"model\":\"%s\",\"expected_serial\":%u,"
		"\"nominal_current_ma\":%.3f,"
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
		settings->expected_serial,
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

int laser_settings_get(const struct coo_cmd_request *cmd, struct coo_cmd_response *out)
{
	enum hispec_laser_id id;
	struct app_laser_channel_settings settings;
	char name[16] = {0};
	char payload[MAX_PAYLOAD_LEN] = {0};
	int rc;

	if (command_laser_id_from_payload(cmd, &id, name, sizeof(name)) != 0) {
		return coo_cmd_error(out, cmd, "missing or invalid laser name");
	}
	rc = hispec_laser_get_channel_settings(id, &settings);
	if (rc != 0) {
		return laser_cmd_error_rc(out, cmd, "laser settings unavailable", rc);
	}
	if (laser_settings_payload(payload, sizeof(payload), id, &settings) != 0) {
		return coo_cmd_error(out, cmd, "laser settings response too large");
	}
	return coo_cmd_reply(out, cmd, COO_CMD_RESP_OK, payload);
}

static int laser_parse_settings_update(const char *json,
				       struct app_laser_channel_settings *settings,
				       bool *changed)
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

#define LASER_PARSE_FLOAT(key, field) do { \
		if (coo_json_extract_optional_double_range(json, key, &(field), \
							  changed, -DBL_MAX, \
							  DBL_MAX) != 0) \
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
		settings->properties.operating_temp_range_c.min_c = (double)range[0];
		settings->properties.operating_temp_range_c.max_c = (double)range[1];
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
	enum hispec_laser_id id;
	struct app_laser_channel_settings settings;
	char name[16] = {0};
	char settings_json[MAX_PAYLOAD_LEN] = {0};
	const char *json;
	bool changed = false;
	bool persist = false;
	int rc;

	if (command_laser_id_from_payload(cmd, &id, name, sizeof(name)) != 0) {
		return coo_cmd_error(out, cmd, "missing or invalid laser name");
	}
	rc = hispec_laser_get_channel_settings(id, &settings);
	if (rc != 0) {
		return laser_cmd_error_rc(out, cmd, "laser settings unavailable", rc);
	}

	rc = coo_json_extract_object(cmd->payload, "settings", settings_json, sizeof(settings_json));
	if (rc == COO_JSON_EXTRACT_ERR) {
		return coo_cmd_error(out, cmd, "invalid settings object");
	}
	json = rc == COO_JSON_EXTRACT_OK ? settings_json : cmd->payload;
	if (coo_json_extract_optional_bool(cmd->payload, "persist",
					   &persist, NULL) != 0) {
		return coo_cmd_error(out, cmd, "invalid persist");
	}

	rc = laser_parse_settings_update(json, &settings, &changed);
	if (rc != 0) {
		return laser_cmd_error_rc(out, cmd, "invalid laser settings", rc);
	}
	if (!changed) {
		return coo_cmd_error(out, cmd, "no laser settings fields supplied");
	}

	throughput_monitor_note_laser_changed(id);
	rc = hispec_laser_update_channel_settings(id, &settings, persist);
	if (rc != 0) {
		return laser_cmd_error_rc(out, cmd, "laser settings update failed", rc);
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
	char payload[MAX_PAYLOAD_LEN] = {0};
	size_t off = 0U;
	int rc;

	if (command_laser_id_from_payload(cmd, &id, name, sizeof(name)) != 0) {
		return coo_cmd_error(out, cmd, "missing or invalid laser name");
	}

	rc = hispec_laser_get_status(id, &s);
	if (rc != 0) {
		return laser_cmd_error_rc(out, cmd, "laser engineering status failed", rc);
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
	    json_append_string_or_null(payload, sizeof(payload), &off,
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
		return coo_cmd_error(out, cmd, "laser engineering status response too large");
	}
	return coo_cmd_reply(out, cmd, COO_CMD_RESP_OK, payload);
}
