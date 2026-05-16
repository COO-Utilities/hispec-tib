/*
 * Copyright (c) 2026 Caltech Optical Observatories
 * SPDX-License-Identifier: Apache-2.0
 */

#include "laser_command.h"

#include <errno.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>

#include <zephyr/sys/util.h>

#include "app_settings.h"
#include "devices.h"
#include "laserbank_control.h"
#include "lasers.h"
#include "throughput_monitor.h"

#include <coo_commons/json_utils.h>

#define LASERBANK_FAULT_CLEAR_OFF_MS 250U

static const char *command_suffix_after(const struct Command *cmd, const char *prefix)
{
	const char *suffix;
	size_t prefix_len;

	if (cmd == NULL || prefix == NULL) {
		return "";
	}

	prefix_len = strlen(prefix);
	if (strncmp(cmd->key, prefix, prefix_len) != 0) {
		return "";
	}

	suffix = cmd->key + prefix_len;
	return suffix[0] == '/' ? suffix + 1 : suffix;
}

static bool parse_laserbank_power_mode_text(const char *text,
					    enum hispec_laser_bank_power_mode *mode)
{
	if (text == NULL || mode == NULL) {
		return false;
	}
	if (strcasecmp(text, "auto") == 0) {
		*mode = HISPEC_LASER_BANK_POWER_AUTO;
		return true;
	}
	if (strcasecmp(text, "override_on") == 0 || strcasecmp(text, "on") == 0) {
		*mode = HISPEC_LASER_BANK_POWER_OVERRIDE_ON;
		return true;
	}
	if (strcasecmp(text, "override_off") == 0 || strcasecmp(text, "off") == 0) {
		*mode = HISPEC_LASER_BANK_POWER_OVERRIDE_OFF;
		return true;
	}
	return false;
}

static bool parse_laserbank_power_request(const struct Command *cmd,
					  enum hispec_laser_bank_power_mode *mode)
{
	const char *suffix = command_suffix_after(cmd, "laserbank/power");
	char text[20] = {0};

	if (parse_laserbank_power_mode_text(suffix, mode)) {
		return true;
	}
	if (cmd == NULL || cmd->payload_len == 0U || strcmp(cmd->payload, "{}") == 0) {
		return false;
	}
	if (coo_json_extract_string(cmd->payload, "override", text, sizeof(text)) ==
	    COO_JSON_EXTRACT_OK ||
	    coo_json_extract_string(cmd->payload, "mode", text, sizeof(text)) ==
	    COO_JSON_EXTRACT_OK) {
		return parse_laserbank_power_mode_text(text, mode);
	}
	return parse_laserbank_power_mode_text(cmd->payload, mode);
}

struct OutMsg laserbank_power(const struct Command *cmd)
{
	enum hispec_laser_bank_power_mode mode;
	char payload[MAX_PAYLOAD_LEN] = {0};
	int rc;

	if (devices_board_type() != HISPEC_BOARD_TIB) {
		return coo_cmd_error(cmd, "laser bank unavailable on this board");
	}

	if (cmd != NULL &&
	    (cmd->msg_type == MSG_SET ||
	     command_suffix_after(cmd, "laserbank/power")[0] != '\0')) {
		if (!parse_laserbank_power_request(cmd, &mode)) {
			return coo_cmd_error(cmd, "override must be auto, override_on, or override_off");
		}
		rc = hispec_laser_bank_power_mode_set(mode);
		if (rc != 0) {
			return coo_cmd_error_rc(cmd, "laser bank power mode failed", rc);
		}
	}

	mode = hispec_laser_bank_power_mode_get();
	snprintk(payload, sizeof(payload),
		 "{\"mode\":\"%s\",\"powered\":%s}",
		 hispec_laser_bank_power_mode_name(mode),
		 hispec_laser_bank_power_is_enabled() ? "true" : "false");
	return coo_cmd_reply(cmd, RESP_OK, payload);
}

struct OutMsg laserbank_clearfaults(const struct Command *cmd)
{
	bool fault = false;
	uint32_t off_ms = 0U;
	char payload[MAX_PAYLOAD_LEN] = {0};
	int rc;

	if (devices_board_type() != HISPEC_BOARD_TIB) {
		return coo_cmd_error(cmd, "laser bank unavailable on this board");
	}

	if (!hispec_laser_bank_power_is_enabled()) {
		return coo_cmd_reply(cmd, RESP_OK, "{\"off_ms\":0}");
	}
	rc = hispec_laser_bank_any_overcurrent_fault(&fault);
	if (rc != 0) {
		return coo_cmd_error_rc(cmd, "overcurrent status unavailable", rc);
	}
	if (!fault) {
		return coo_cmd_reply(cmd, RESP_OK, "{\"off_ms\":0}");
	}
	if (hispec_laser_bank_clear_faults(LASERBANK_FAULT_CLEAR_OFF_MS) != 0) {
		return coo_cmd_error(cmd, "laser bank power cycle failed");
	}
	off_ms = LASERBANK_FAULT_CLEAR_OFF_MS;

	if (!hispec_laser_bank_power_is_enabled()) {
		return coo_cmd_error(cmd, "laser bank power cycle could not turn on");
	}

	snprintf(payload, sizeof(payload), "{\"off_ms\":%u}", off_ms);
	return coo_cmd_reply(cmd, RESP_OK, payload);
}

static void laserbank_control_status_payload(char *payload, size_t payload_len)
{
	struct laserbank_control_status status = {0};

	laserbank_control_get_status(&status);
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

static bool parse_heater_mode_text(const char *text,
				   enum laserbank_heater_mode *mode)
{
	if (text == NULL || mode == NULL) {
		return false;
	}

	if (strcasecmp(text, "auto") == 0) {
		*mode = LASERBANK_HEATER_MODE_AUTO;
		return true;
	}
	if (strcasecmp(text, "override_on") == 0 ||
	    strcasecmp(text, "overide_on") == 0 ||
	    strcasecmp(text, "on") == 0) {
		*mode = LASERBANK_HEATER_MODE_OVERRIDE_ON;
		return true;
	}
	if (strcasecmp(text, "override_off") == 0 ||
	    strcasecmp(text, "overide_off") == 0 ||
	    strcasecmp(text, "off") == 0) {
		*mode = LASERBANK_HEATER_MODE_OVERRIDE_OFF;
		return true;
	}

	return false;
}

static bool parse_heater_request(const struct Command *cmd,
				 enum laserbank_heater_mode *mode)
{
	const char *suffix = command_suffix_after(cmd, "laserbank/heater");
	char state[20] = {0};
	bool flag;
	int rc;

	if (parse_heater_mode_text(suffix, mode)) {
		return true;
	}

	if (cmd == NULL || cmd->payload_len == 0U || strcmp(cmd->payload, "{}") == 0) {
		return false;
	}

	if (coo_json_extract_string(cmd->payload, "override", state, sizeof(state)) ==
	    COO_JSON_EXTRACT_OK ||
	    coo_json_extract_string(cmd->payload, "state", state, sizeof(state)) ==
	    COO_JSON_EXTRACT_OK) {
		return parse_heater_mode_text(state, mode);
	}

	rc = coo_json_extract_bool(cmd->payload, "override_on", &flag);
	if (rc == COO_JSON_EXTRACT_OK && flag) {
		*mode = LASERBANK_HEATER_MODE_OVERRIDE_ON;
		return true;
	}
	rc = coo_json_extract_bool(cmd->payload, "override_off", &flag);
	if (rc == COO_JSON_EXTRACT_OK && flag) {
		*mode = LASERBANK_HEATER_MODE_OVERRIDE_OFF;
		return true;
	}

	return parse_heater_mode_text(cmd->payload, mode);
}

struct OutMsg laserbank_heater(const struct Command *cmd)
{
	enum laserbank_heater_mode mode;
	char payload[MAX_PAYLOAD_LEN] = {0};

	if (devices_board_type() != HISPEC_BOARD_TIB) {
		return coo_cmd_error(cmd, "laser bank unavailable on this board");
	}

	if (cmd != NULL &&
	    (cmd->msg_type == MSG_SET ||
	     command_suffix_after(cmd, "laserbank/heater")[0] != '\0')) {
		if (!parse_heater_request(cmd, &mode)) {
			return coo_cmd_reply(cmd, RESP_ERROR,
					     "{\"error\":\"Use laserbank/heater auto|override_on|override_off\"}");
		}
		int rc = laserbank_control_set_heater_mode(mode, true);
		if (rc != 0) {
			return coo_cmd_error_rc(cmd, "laser bank heater relay unavailable", rc);
		}
	}

	laserbank_control_status_payload(payload, sizeof(payload));
	return coo_cmd_reply(cmd, RESP_OK, payload);
}

static int command_laser_id_from_payload(const struct Command *cmd,
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

static struct OutMsg laser_unavailable(const struct Command *cmd)
{
	return coo_cmd_error(cmd, "laser bank unavailable on this board");
}

static struct OutMsg laser_error_response(const struct Command *cmd,
					  const char *msg,
					  int rc)
{
	return coo_cmd_error_rc(cmd, msg, rc);
}

static float laser_status_level(const struct hispec_laser_status *status)
{
	const laserprops_t *props;
	float range;

	if (status == NULL || status->properties == NULL ||
	    status->current_set_ma != status->current_set_ma) {
		return LASERPROP_NA;
	}
	props = status->properties;
	range = props->nominal_current_ma - props->threshold_current_ma;
	if (range <= 0.0f) {
		return LASERPROP_NA;
	}
	if (status->current_set_ma <= 0.0f) {
		return 0.0f;
	}
	return 100.0f * (status->current_set_ma - props->threshold_current_ma) / range;
}

static int laser_append_compact_status(char *payload, size_t payload_len,
				       const struct hispec_laser_status *status)
{
	size_t off = 0U;
	float level = laser_status_level(status);
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
	    coo_json_append_float_or_null(payload, payload_len, &off, level, 2) != 0 ||
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

struct OutMsg laser_get(const struct Command *cmd)
{
	enum hispec_laser_id id;
	struct hispec_laser_status status = {0};
	char name[16] = {0};
	char payload[MAX_PAYLOAD_LEN] = {0};
	int rc;

	if (devices_board_type() != HISPEC_BOARD_TIB) {
		return laser_unavailable(cmd);
	}
	if (command_laser_id_from_payload(cmd, &id, name, sizeof(name)) != 0) {
		return coo_cmd_error(cmd, "missing or invalid laser name");
	}

	rc = hispec_laser_get_status(id, &status);
	if (rc != 0 && !status.bank_powered) {
		return laser_error_response(cmd, "laser status failed", rc);
	}
	if (laser_append_compact_status(payload, sizeof(payload), &status) != 0) {
		return coo_cmd_error(cmd, "laser response too large");
	}
	return rc == 0 ? coo_cmd_reply(cmd, RESP_OK, payload) :
	       laser_error_response(cmd, "laser status failed", rc);
}

struct OutMsg laser_set(const struct Command *cmd)
{
	enum hispec_laser_id id;
	struct app_laser_channel_settings settings;
	char name[16] = {0};
	float level = 0.0f;
	uint32_t autooff_s;
	int parse_rc;
	int rc;

	if (devices_board_type() != HISPEC_BOARD_TIB) {
		return laser_unavailable(cmd);
	}
	if (command_laser_id_from_payload(cmd, &id, name, sizeof(name)) != 0) {
		return coo_cmd_error(cmd, "missing or invalid laser name");
	}
	parse_rc = coo_json_extract_float(cmd->payload, "level", &level);
	if (parse_rc != COO_JSON_EXTRACT_OK || level < 0.0f || level > 100.0f) {
		return coo_cmd_error(cmd, "level must be 0..100");
	}
	rc = hispec_laser_get_channel_settings(id, &settings);
	if (rc != 0) {
		return laser_error_response(cmd, "laser settings unavailable", rc);
	}
	autooff_s = settings.autooff_s;
	parse_rc = coo_json_extract_u32(cmd->payload, "autooff_s", &autooff_s);
	if (parse_rc == COO_JSON_EXTRACT_ERR) {
		return coo_cmd_error(cmd, "invalid autooff_s");
	}

	throughput_monitor_note_laser_changed(id);
	rc = hispec_laser_set_output_percent_autooff(id, level, autooff_s);
	if (rc != 0) {
		return laser_error_response(cmd, "laser level failed", rc);
	}
	return coo_cmd_ok(cmd);
}

struct OutMsg laser_tune_get(const struct Command *cmd)
{
	enum hispec_laser_id id;
	char name[16] = {0};
	char payload[MAX_PAYLOAD_LEN];

	if (devices_board_type() != HISPEC_BOARD_TIB) {
		return laser_unavailable(cmd);
	}
	if (command_laser_id_from_payload(cmd, &id, name, sizeof(name)) != 0) {
		return coo_cmd_error(cmd, "missing or invalid laser name");
	}
	snprintk(payload, sizeof(payload),
		 "{\"name\":\"%s\",\"tune_nm\":%.4f}",
		 hispec_laser_name(id),
		 (double)hispec_laser_get_tune_delta_nm(id));
	return coo_cmd_reply(cmd, RESP_OK, payload);
}

struct OutMsg laser_tune_set(const struct Command *cmd)
{
	enum hispec_laser_id id;
	char name[16] = {0};
	float delta_nm = 0.0f;
	int parse_rc;
	int rc;

	if (devices_board_type() != HISPEC_BOARD_TIB) {
		return laser_unavailable(cmd);
	}
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
		return laser_error_response(cmd, "laser tune failed", rc);
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

struct OutMsg laser_settings_get(const struct Command *cmd)
{
	enum hispec_laser_id id;
	struct app_laser_channel_settings settings;
	char name[16] = {0};
	char payload[MAX_PAYLOAD_LEN] = {0};
	int rc;

	if (devices_board_type() != HISPEC_BOARD_TIB) {
		return laser_unavailable(cmd);
	}
	if (command_laser_id_from_payload(cmd, &id, name, sizeof(name)) != 0) {
		return coo_cmd_error(cmd, "missing or invalid laser name");
	}
	rc = hispec_laser_get_channel_settings(id, &settings);
	if (rc != 0) {
		return laser_error_response(cmd, "laser settings unavailable", rc);
	}
	if (laser_settings_payload(payload, sizeof(payload), id, &settings) != 0) {
		return coo_cmd_error(cmd, "laser settings response too large");
	}
	return coo_cmd_reply(cmd, RESP_OK, payload);
}

static int laser_parse_settings_update(const char *json,
				       struct app_laser_channel_settings *settings,
				       bool *driver_changed,
				       bool *changed)
{
	double range[2] = {0};
	size_t range_len = 0U;
	char pid_json[128] = {0};
	float fval;
	uint32_t uval;
	bool bval;
	int rc;

	if (json == NULL || settings == NULL || driver_changed == NULL || changed == NULL) {
		return -EINVAL;
	}

#define LASER_PARSE_FLOAT(key, field, minv, maxv, driver) do { \
		rc = coo_json_extract_float(json, key, &fval); \
		if (rc == COO_JSON_EXTRACT_ERR) return -EINVAL; \
		if (rc == COO_JSON_EXTRACT_OK) { \
			if (!(fval >= (minv) && fval <= (maxv))) return -ERANGE; \
			(field) = fval; \
			*changed = true; \
			if (driver) *driver_changed = true; \
		} \
	} while (0)

	LASER_PARSE_FLOAT("nominal_current_ma", settings->properties.nominal_current_ma,
			  0.0f, 1000.0f, false);
	LASER_PARSE_FLOAT("max_current_ma", settings->properties.max_current_ma,
			  0.0f, 1000.0f, true);
	LASER_PARSE_FLOAT("threshold_current_ma", settings->properties.threshold_current_ma,
			  0.0f, 1000.0f, false);
	LASER_PARSE_FLOAT("efficiency_mw_per_ma", settings->properties.efficiency_mw_per_ma,
			  0.0f, 100.0f, false);
	LASER_PARSE_FLOAT("wavelength_nm", settings->properties.wavelength_nm,
			  1.0f, 10000.0f, false);
	LASER_PARSE_FLOAT("current_set_calibration_pct",
			  settings->current_set_calibration_pct,
			  95.0f, 105.0f, true);
	LASER_PARSE_FLOAT("current_set_calibration_%",
			  settings->current_set_calibration_pct,
			  95.0f, 105.0f, true);
	LASER_PARSE_FLOAT("default_operating_temp_c", settings->properties.operating_temp_c,
			  15.0f, 40.0f, false);
	LASER_PARSE_FLOAT("dlambda_dT_nm_per_k", settings->properties.dlambda_dT_nm_per_k,
			  -10.0f, 10.0f, false);
	LASER_PARSE_FLOAT("dlambda_dA_nm_per_ma", settings->properties.dlambda_dA_nm_per_ma,
			  -10.0f, 10.0f, false);

#undef LASER_PARSE_FLOAT

	rc = coo_json_extract_double_array(json, "operating_temp_range_c",
					   range, ARRAY_SIZE(range), &range_len);
	if (rc == COO_JSON_EXTRACT_ERR) {
		return -EINVAL;
	}
	if (rc == COO_JSON_EXTRACT_OK) {
		if (range_len != 2U || range[0] < 15.0 || range[1] > 40.0 || range[0] > range[1]) {
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
		if (coo_json_extract_u32(pid_json, "p", &uval) == COO_JSON_EXTRACT_OK &&
		    uval <= UINT16_MAX) {
			settings->properties.tec_pid.kp = (uint16_t)uval;
			*changed = true;
			*driver_changed = true;
		}
		if (coo_json_extract_u32(pid_json, "i", &uval) == COO_JSON_EXTRACT_OK &&
		    uval <= UINT16_MAX) {
			settings->properties.tec_pid.ki = (uint16_t)uval;
			*changed = true;
			*driver_changed = true;
		}
		if (coo_json_extract_u32(pid_json, "d", &uval) == COO_JSON_EXTRACT_OK &&
		    uval <= UINT16_MAX) {
			settings->properties.tec_pid.kd = (uint16_t)uval;
			*changed = true;
			*driver_changed = true;
		}
	}

	rc = coo_json_extract_bool(json, "disable_tec_at_autooff", &bval);
	if (rc == COO_JSON_EXTRACT_ERR) {
		return -EINVAL;
	}
	if (rc == COO_JSON_EXTRACT_OK) {
		settings->disable_tec_at_autooff = bval;
		*changed = true;
	}

	rc = coo_json_extract_u32(json, "autooff_s", &uval);
	if (rc == COO_JSON_EXTRACT_ERR) {
		return -EINVAL;
	}
	if (rc == COO_JSON_EXTRACT_OK) {
		settings->autooff_s = uval;
		*changed = true;
	}

	return 0;
}

struct OutMsg laser_settings_set(const struct Command *cmd)
{
	enum hispec_laser_id id;
	struct app_laser_channel_settings settings;
	char name[16] = {0};
	char settings_json[MAX_PAYLOAD_LEN] = {0};
	const char *json;
	bool changed = false;
	bool driver_changed = false;
	int rc;

	if (devices_board_type() != HISPEC_BOARD_TIB) {
		return laser_unavailable(cmd);
	}
	if (command_laser_id_from_payload(cmd, &id, name, sizeof(name)) != 0) {
		return coo_cmd_error(cmd, "missing or invalid laser name");
	}
	rc = hispec_laser_get_channel_settings(id, &settings);
	if (rc != 0) {
		return laser_error_response(cmd, "laser settings unavailable", rc);
	}

	rc = coo_json_extract_object(cmd->payload, "settings", settings_json, sizeof(settings_json));
	if (rc == COO_JSON_EXTRACT_ERR) {
		return coo_cmd_error(cmd, "invalid settings object");
	}
	json = rc == COO_JSON_EXTRACT_OK ? settings_json : cmd->payload;

	rc = laser_parse_settings_update(json, &settings, &driver_changed, &changed);
	if (rc != 0) {
		return laser_error_response(cmd, "invalid laser settings", rc);
	}
	if (!changed) {
		return coo_cmd_error(cmd, "no laser settings fields supplied");
	}

	throughput_monitor_note_laser_changed(id);
	rc = hispec_laser_update_channel_settings(id, &settings, driver_changed, true);
	if (rc != 0) {
		return laser_error_response(cmd, "laser settings update failed", rc);
	}
	return coo_cmd_ok(cmd);
}

struct OutMsg laser_status_get(const struct Command *cmd)
{
	return laser_get(cmd);
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

struct OutMsg laser_engstatus_get(const struct Command *cmd)
{
	enum hispec_laser_id id;
	struct hispec_laser_status s = {0};
	char name[16] = {0};
	char payload[MAX_PAYLOAD_LEN] = {0};
	size_t off = 0U;
	int rc;

	if (devices_board_type() != HISPEC_BOARD_TIB) {
		return laser_unavailable(cmd);
	}
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
	return rc == 0 ? coo_cmd_reply(cmd, RESP_OK, payload) :
	       laser_error_response(cmd, "laser engineering status failed", rc);
}
