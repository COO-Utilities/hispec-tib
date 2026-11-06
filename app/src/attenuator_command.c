/*
 * Copyright (c) 2026 Caltech Optical Observatories
 * SPDX-License-Identifier: Apache-2.0
 */

#include "laser_calibration.h"
#include "attenuator_command.h"

#include <errno.h>
#include <float.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include <zephyr/sys/util.h>

#include "app_settings.h"
#include "attenuator.h"
#include "attenuator_calibration.h"
#include "devices.h"
#include "housekeeping.h"
#include "lasers.h"
#include "throughput_monitor.h"

#include <coo_commons/command_dispatch.h>
#include <coo_commons/json_utils.h>

enum attenuator_setting {
	ATTENUATOR_SETTING_COMPACT = 0,
	ATTENUATOR_SETTING_SETTINGS,
};

static const struct coo_json_string_choice attenuator_setting_choices[] = {
	{ "settings", ATTENUATOR_SETTING_SETTINGS },
};

static const struct coo_json_string_choice attenuator_cal_fiber_choices[] = {
	{ "m", 'M' },
	{ "s", 'S' },
};

struct atten_calibration_routes {
	const char *laser_input;
	enum photodiode_channel pd_channel;
	const char *pd_input[2];
	const char *pd_output;
};

static const struct atten_calibration_routes atten_calibration_routes[HISPEC_LASER_COUNT] = {
	[HISPEC_LASER_1028_Y] = {
		"yj_laser", PHOTODIODE_CHANNEL_YJ, { "yj_mm", "yj_sm" }, "yj_pd" },
	[HISPEC_LASER_1270_J] = {
		"yj_laser", PHOTODIODE_CHANNEL_YJ, { "yj_mm", "yj_sm" }, "yj_pd" },
	[HISPEC_LASER_1430_YJ] = {
		"yj_1430", PHOTODIODE_CHANNEL_YJ, { "yj_mm", "yj_sm" }, "yj_pd" },
	[HISPEC_LASER_1430_HK] = {
		"hk_1430", PHOTODIODE_CHANNEL_HK, { "hk_mm", "hk_sm" }, "hk_pd" },
	[HISPEC_LASER_1510_H] = {
		"hk_laser", PHOTODIODE_CHANNEL_HK, { "hk_mm", "hk_sm" }, "hk_pd" },
	[HISPEC_LASER_2330_K] = {
		"hk_laser", PHOTODIODE_CHANNEL_HK, { "hk_mm", "hk_sm" }, "hk_pd" },
};

static int atten_calibration_pd_error(const struct coo_cmd_request *cmd,
				      struct coo_cmd_response *out,
				      enum photodiode_channel channel,
				      const char *problem)
{
	char message[64] = {0};

	snprintk(message, sizeof(message), "photodiode %s %s",
		 photodiode_channel_names[channel], problem);
	return coo_cmd_error(out, cmd, message);
}

static int attenuator_index_from_name(const char *name, uint8_t *attenuator_index)
{
	enum hispec_laser_id laser_id;

	if (name == NULL || attenuator_index == NULL) {
		return -EINVAL;
	}
	if (strcmp(name, "lfc") == 0) {
		enum hispec_board_type board = devices_board_type();

		if (board != HISPEC_BOARD_CAL_YJ && board != HISPEC_BOARD_CAL_HK) {
			return -ENOENT;
		}
		*attenuator_index = HISPEC_ATTENUATOR_LFC_INDEX;
		return devices_attenuator_channel_available(*attenuator_index) ? 0 : -ENODEV;
	}
	if (devices_board_type() != HISPEC_BOARD_TIB) {
		return -ENOENT;
	}
	if (hispec_laser_id_from_name(name, &laser_id) != 0 ||
	    attenuator_index_from_laser_id(laser_id, attenuator_index) != 0) {
		return -ENOENT;
	}
	return devices_attenuator_channel_available(*attenuator_index) ? 0 : -ENODEV;
}

static int append_attenuator_physical_coeff_json(char *payload,
						 size_t payload_len,
						 size_t *off,
						 const char *name,
						 const struct attenuator_model_coeffs *coeffs,
						 double max_fit_residual_db)
{
	if (payload == NULL || off == NULL || name == NULL || coeffs == NULL) {
		return -EINVAL;
	}

	return coo_json_append(payload, payload_len, off,
			       "\"%s\":{\"fvoa_50pct_mv\":%.12g,"
			       "\"slope_inv_fvoa_mv\":%.12g,"
			       "\"max_atten_db\":%.12g,\"max_calibrated_db\":%.9g,\"gain\":%.12g,\"rms_db\":%.9g,"
			       "\"correction_coeff\":[%.9g,%.9g,%.9g,%.9g,%.9g,%.9g],\"max_fit_residual_db\":%.9g}",
			       name,
			       coeffs->fvoa_50pct_mv,
			       coeffs->slope_inv_fvoa_mv,
			       coeffs->max_atten_db, coeffs->max_calibrated_db,
			       coeffs->gain, coeffs->rms_db,
			       (double)coeffs->correction_coeff[0],
			       (double)coeffs->correction_coeff[1],
			       (double)coeffs->correction_coeff[2],
			       (double)coeffs->correction_coeff[3],
			       (double)coeffs->correction_coeff[4],
			       (double)coeffs->correction_coeff[5], max_fit_residual_db);
}

static int attenuator_index_from_command(const struct coo_cmd_request *cmd,
					 enum attenuator_setting *setting,
					 uint8_t *attenuator_index)
{
	char laser_name[16] = {0};
	char setting_name[16] = {0};
	int setting_value;

	if (cmd == NULL || setting == NULL || attenuator_index == NULL) {
		return -EINVAL;
	}

	if (coo_cmd_key_suffix_pair_copy(cmd->key, "atten",
					 laser_name, sizeof(laser_name),
					 setting_name, sizeof(setting_name)) == 0) {
		if (coo_json_match_string_choice(setting_name, attenuator_setting_choices,
						 ARRAY_SIZE(attenuator_setting_choices),
						 &setting_value) != 0) {
			return -ENOTSUP;
		}
		*setting = (enum attenuator_setting)setting_value;
	} else if (coo_cmd_key_suffix_segment_copy(cmd->key, "atten",
						   laser_name,
						   sizeof(laser_name)) == 0) {
		*setting = ATTENUATOR_SETTING_COMPACT;
	} else {
		return -EINVAL;
	}

	{
		int rc = attenuator_index_from_name(laser_name, attenuator_index);

		if (rc != 0) {
			return rc;
		}
	}
	if (attenuators[*attenuator_index].dac_cfg1.dev == NULL ||
	    attenuators[*attenuator_index].dac_cfg2.dev == NULL) {
		return -ENODEV;
	}
	return 0;
}

static int attenuator_status_reply(const struct coo_cmd_request *cmd,
				   struct coo_cmd_response *out,
				   uint8_t attenuator_index)
{
	struct attenuator_status status = {0};
	char payload[COO_CMD_PAYLOAD_MAX] = {0};
	double linear1;
	double linear2;

	if (!attenuator_get(&attenuators[attenuator_index], &status)) {
		return coo_cmd_reply(out, cmd, COO_CMD_RESP_ERROR,
				     "{\"error\":\"Failed to read attenuator\"}");
	}

	struct attenuator cached;
	attenuator_snapshot(&attenuators[attenuator_index], &cached);
	linear1 = pow(10.0, -status.attenuation_db1 / 10.0);
	linear2 = pow(10.0, -status.attenuation_db2 / 10.0);
	snprintk(payload, sizeof(payload),
		 "{\"loss_db\":%.6f,\"linear\":%.12g,"
		 "\"mv1\":%.6f,\"mv2\":%.6f,"
		 "\"loss1_db\":%.6f,\"loss2_db\":%.6f,"
		 "\"linear1\":%.12g,\"linear2\":%.12g,\"open1_mv\":%.6g,\"open2_mv\":%.6g}",
		 status.attenuation_db,
		 status.linear,
		 (double)status.voltage1,
		 (double)status.voltage2,
		 status.attenuation_db1,
		 status.attenuation_db2,
		 linear1,
		 linear2, (double)attenuator_open_voltage(&cached.dac_cfg1),
		 (double)attenuator_open_voltage(&cached.dac_cfg2));

	return coo_cmd_reply(out, cmd, COO_CMD_RESP_OK, payload);
}

int atten_setting_get(const struct coo_cmd_request *cmd, struct coo_cmd_response *out)
{
	enum attenuator_setting setting;
	uint8_t attenuator_index;
	int rc;
	char payload[COO_CMD_PAYLOAD_MAX] = {0};

	rc = attenuator_index_from_command(cmd, &setting, &attenuator_index);
	if (rc == -EINVAL) {
		return coo_cmd_reply(out, cmd, COO_CMD_RESP_ERROR,
					  "{\"error\":\"Failed to parse atten/setting\"}");
	}
	if (rc == -ENOTSUP) {
		return coo_cmd_reply(out, cmd, COO_CMD_RESP_ERROR, "{\"error\":\"Invalid setting\"}");
	}
	if (rc == -ENOENT) {
		return coo_cmd_reply(out, cmd, COO_CMD_RESP_ERROR, "{\"error\":\"Invalid attenuator\"}");
	}
	if (rc != 0) {
		return coo_cmd_reply(out, cmd, COO_CMD_RESP_ERROR,
					  "{\"error\":\"Attenuator unavailable on this board\"}");
	}

	switch (setting) {
	case ATTENUATOR_SETTING_SETTINGS: {
		struct app_attenuator_fit_policy policy;
		struct app_attenuator_open_settings open;
		app_settings_get_attenuator_fit(attenuator_index, &policy);
		app_settings_get_attenuator_open(attenuator_index, &open);
		size_t off = 0U;
		struct attenuator snapshot;
		attenuator_snapshot(&attenuators[attenuator_index], &snapshot);

		if (coo_json_append(payload, sizeof(payload), &off,
			"{\"open1_mv\":%.9g,\"open2_mv\":%.9g,",
			(double)open.mv[0], (double)open.mv[1]) != 0 ||
		    append_attenuator_physical_coeff_json(payload, sizeof(payload), &off,
							 "dac1",
							 &snapshot.coeff1, policy.max_fit_residual_db[0]) != 0 ||
		    coo_json_append(payload, sizeof(payload), &off, ",") != 0 ||
		    append_attenuator_physical_coeff_json(payload, sizeof(payload), &off,
							 "dac2",
							 &snapshot.coeff2, policy.max_fit_residual_db[1]) != 0 ||
		    coo_json_append(payload, sizeof(payload), &off, "}") != 0) {
			return coo_cmd_reply(out, cmd, COO_CMD_RESP_ERROR,
					     "{\"error\":\"Attenuator settings response too large\"}");
		}
		break;
	}
	case ATTENUATOR_SETTING_COMPACT:
		return attenuator_status_reply(cmd, out, attenuator_index);
	default:
		return coo_cmd_reply(out, cmd, COO_CMD_RESP_ERROR, "{\"error\":\"Invalid setting\"}");
	}

	return coo_cmd_reply(out, cmd, COO_CMD_RESP_OK, payload);
}

/* Merge one optional physical settings object into the current model/policy.
 * Validation is completed for the pair before any DAC, RAM or flash change.
 * Omitted coefficients, correction and RMS retain their current values.
 */
static int parse_attenuator_settings_object(const char *json, const char *key,
		struct attenuator_model_coeffs *out, double *max_fit_residual_db,
		bool *model_changed)
{
	char object_json[COO_CMD_PAYLOAD_MAX];
	int rc = coo_json_extract_object(json, key, object_json, sizeof(object_json));
	if (rc == COO_JSON_EXTRACT_MISSING) return 0;
	if (rc != COO_JSON_EXTRACT_OK ||
	    coo_json_validate_top_level_keys(object_json,
		"fvoa_50pct_mv,slope_inv_fvoa_mv,max_atten_db,max_calibrated_db,"
		"gain,rms_db,correction_coeff,max_fit_residual_db", NULL, 0U) != 0) {
		return -EINVAL;
	}

	const char *keys[] = {"fvoa_50pct_mv", "slope_inv_fvoa_mv", "max_atten_db",
		"max_calibrated_db", "gain", "rms_db"};
	double *values[] = {&out->fvoa_50pct_mv, &out->slope_inv_fvoa_mv, &out->max_atten_db,
		&out->max_calibrated_db, &out->gain, &out->rms_db};
	for (size_t i = 0; i < ARRAY_SIZE(keys); ++i) {
		double value;
		rc = coo_json_extract_double(object_json, keys[i], &value);
		if (rc == COO_JSON_EXTRACT_MISSING) continue;
		if (rc != COO_JSON_EXTRACT_OK || !isfinite(value)) return -EINVAL;
		*model_changed |= value != *values[i];
		*values[i] = value;
	}
	bool supplied = false;
	if (coo_json_extract_optional_double_range(object_json, "max_fit_residual_db",
		max_fit_residual_db, &supplied, 0.0, DBL_MAX) != 0 || *max_fit_residual_db <= 0.0) {
		return -EINVAL;
	}

	double correction[ATTENUATOR_MODEL_CORRECTION_TERMS];
	size_t parsed_len = 0U;
	rc = coo_json_extract_double_array(object_json, "correction_coeff",
		correction, ARRAY_SIZE(correction), &parsed_len);
	if (rc == COO_JSON_EXTRACT_MISSING) return 0;
	if (rc != COO_JSON_EXTRACT_OK || parsed_len != ARRAY_SIZE(correction)) return -EINVAL;
	for (size_t i = 0; i < ARRAY_SIZE(correction); ++i) {
		if (!isfinite(correction[i]) || fabs(correction[i]) > (double)FLT_MAX) return -EINVAL;
		*model_changed |= (float)correction[i] != out->correction_coeff[i];
		out->correction_coeff[i] = (float)correction[i];
	}
	return 0;
}

enum attenuator_physical_value_mode {
	ATTENUATOR_PHYSICAL_VALUE_NONE = 0,
	ATTENUATOR_PHYSICAL_VALUE_LINEAR,
	ATTENUATOR_PHYSICAL_VALUE_DB,
	ATTENUATOR_PHYSICAL_VALUE_MV,
};

struct attenuator_physical_value {
	enum attenuator_physical_value_mode mode;
	double value;
};

static int attenuator_extract_optional_double(const char *json,
					      const char *key,
					      double *value,
					      bool *present)
{
	double parsed;
	int rc;

	if (value == NULL || present == NULL) {
		return -EINVAL;
	}

	rc = coo_json_extract_double(json, key, &parsed);
	if (rc == COO_JSON_EXTRACT_MISSING) {
		return 0;
	}
	if (rc != COO_JSON_EXTRACT_OK || !isfinite(parsed)) {
		return -EINVAL;
	}

	*value = parsed;
	*present = true;
	return 0;
}

static int attenuator_extract_physical_value(
	const char *json,
	const char *linear_key,
	const char *db_key,
	const char *mv_key,
	struct attenuator_physical_value *out)
{
	bool linear_present = false;
	bool db_present = false;
	bool mv_present = false;
	double linear = 0.0;
	double db = 0.0;
	double mv = 0.0;
	uint8_t present_count;

	if (out == NULL) {
		return -EINVAL;
	}
	out->mode = ATTENUATOR_PHYSICAL_VALUE_NONE;
	out->value = 0.0;

	if (attenuator_extract_optional_double(json, linear_key,
					       &linear, &linear_present) != 0 ||
	    attenuator_extract_optional_double(json, db_key, &db, &db_present) != 0 ||
	    attenuator_extract_optional_double(json, mv_key, &mv, &mv_present) != 0) {
		return -EINVAL;
	}

	present_count = (linear_present ? 1U : 0U) +
			(db_present ? 1U : 0U) +
			(mv_present ? 1U : 0U);
	if (present_count > 1U) {
		return -EALREADY;
	}
	if (linear_present) {
		if (!(linear > 0.0 && linear <= 1.0)) {
			return -ERANGE;
		}
		out->mode = ATTENUATOR_PHYSICAL_VALUE_LINEAR;
		out->value = linear;
	} else if (db_present) {
		if (db < 0.0) {
			return -ERANGE;
		}
		out->mode = ATTENUATOR_PHYSICAL_VALUE_DB;
		out->value = db;
	} else if (mv_present) {
		if (mv < -(double)FLT_MAX || mv > (double)FLT_MAX) {
			return -ERANGE;
		}
		out->mode = ATTENUATOR_PHYSICAL_VALUE_MV;
		out->value = mv;
	}

	return 0;
}

static int attenuator_set_compact_value(const struct coo_cmd_request *cmd,
					struct coo_cmd_response *out,
					uint8_t attenuator_index)
{
	struct attenuator_physical_value physical[ATTENUATOR_PHYSICAL_COUNT];
	bool total_linear_present = false;
	bool total_db_present = false;
	double total_linear = 0.0;
	double total_db = 0.0;
	bool has_physical;
	int rc;

	if (attenuator_extract_optional_double(cmd->payload, "value",
					       &total_linear,
					       &total_linear_present) != 0 ||
	    attenuator_extract_optional_double(cmd->payload, "value_db",
					       &total_db, &total_db_present) != 0) {
		return coo_cmd_reply(out, cmd, COO_CMD_RESP_ERROR,
				     "{\"error\":\"Invalid attenuator value\"}");
	}
	if (total_linear_present && total_db_present) {
		return coo_cmd_reply(out, cmd, COO_CMD_RESP_ERROR,
				     "{\"error\":\"Mixed total attenuation units\"}");
	}
	if ((total_linear_present && !(total_linear > 0.0 && total_linear <= 1.0)) ||
	    (total_db_present && total_db < 0.0)) {
		return coo_cmd_error(out, cmd, "value must be 0 < value <= 1; value_db must be >= 0");
	}

	rc = attenuator_extract_physical_value(cmd->payload,
					       "value1", "value1_db", "value1_mv",
					       &physical[0]);
	if (rc == -EALREADY) {
		return coo_cmd_reply(out, cmd, COO_CMD_RESP_ERROR,
				     "{\"error\":\"Mixed attenuator 1 units\"}");
	}
	if (rc != 0) {
		return coo_cmd_reply(out, cmd, COO_CMD_RESP_ERROR,
				     "{\"error\":\"Invalid attenuator 1 value\"}");
	}
	rc = attenuator_extract_physical_value(cmd->payload,
					       "value2", "value2_db", "value2_mv",
					       &physical[1]);
	if (rc == -EALREADY) {
		return coo_cmd_reply(out, cmd, COO_CMD_RESP_ERROR,
				     "{\"error\":\"Mixed attenuator 2 units\"}");
	}
	if (rc != 0) {
		return coo_cmd_reply(out, cmd, COO_CMD_RESP_ERROR,
				     "{\"error\":\"Invalid attenuator 2 value\"}");
	}

	has_physical = physical[0].mode != ATTENUATOR_PHYSICAL_VALUE_NONE ||
		       physical[1].mode != ATTENUATOR_PHYSICAL_VALUE_NONE;
	if ((total_linear_present || total_db_present) && has_physical) {
		return coo_cmd_reply(out, cmd, COO_CMD_RESP_ERROR,
				     "{\"error\":\"Mixed total and physical attenuator values\"}");
	}
	if (!total_linear_present && !total_db_present && !has_physical) {
		return coo_cmd_reply(out, cmd, COO_CMD_RESP_ERROR,
				     "{\"error\":\"Missing attenuator value\"}");
	}

	if (total_linear_present) {
		if (!attenuator_set_linear(&attenuators[attenuator_index],
					   total_linear, false)) {
			return coo_cmd_reply(out, cmd, COO_CMD_RESP_ERROR,
					     "{\"error\":\"attenuator apply failed\"}");
		}
	} else if (total_db_present) {
		if (!attenuator_set_db(&attenuators[attenuator_index], total_db, false)) {
			return coo_cmd_reply(out, cmd, COO_CMD_RESP_ERROR,
					     "{\"error\":\"attenuator apply failed\"}");
		}
	} else {
		struct attenuator_target target[ATTENUATOR_PHYSICAL_COUNT];
		for (uint8_t i = 0; i < ATTENUATOR_PHYSICAL_COUNT; ++i) {
			target[i] = (struct attenuator_target){
				.update = physical[i].mode != ATTENUATOR_PHYSICAL_VALUE_NONE,
				.voltage_mv = physical[i].mode == ATTENUATOR_PHYSICAL_VALUE_MV,
				.value = physical[i].mode == ATTENUATOR_PHYSICAL_VALUE_LINEAR ?
					-10.0 * log10(physical[i].value) : physical[i].value,
			};
		}
		if (!attenuator_set_pair(&attenuators[attenuator_index], target)) {
			return coo_cmd_error(out, cmd, "physical attenuator apply failed; earlier writes may have succeeded");
		}
	}

	throughput_monitor_note_attenuator_changed(attenuator_index);
	return attenuator_status_reply(cmd, out, attenuator_index);
}

int atten_setting_set(const struct coo_cmd_request *cmd, struct coo_cmd_response *out)
{
	/* A manual DAC/model change would invalidate the run's fixed-gain segments. */
	if (laser_calibration_active() || attenuator_calibration_active()) {
		return coo_cmd_busy_response(out, cmd);
	}

	enum attenuator_setting setting;
	uint8_t attenuator_index;
	char invalid[64] = "argument";
	int rc;

	rc = attenuator_index_from_command(cmd, &setting, &attenuator_index);
	if (rc == -EINVAL) {
		return coo_cmd_reply(out, cmd, COO_CMD_RESP_ERROR,
					  "{\"error\":\"atten key invalid, see catalog\"}");
	}
	if (rc == -ENOTSUP) {
		return coo_cmd_reply(out, cmd, COO_CMD_RESP_ERROR, "{\"error\":\"Invalid setting\"}");
	}
	if (rc == -ENOENT) {
		return coo_cmd_reply(out, cmd, COO_CMD_RESP_ERROR, "{\"error\":\"Invalid attenuator\"}");
	}
	if (rc != 0) {
		return coo_cmd_reply(out, cmd, COO_CMD_RESP_ERROR,
					  "{\"error\":\"Attenuator unavailable on this board\"}");
	}

	rc = coo_json_validate_top_level_keys(cmd->payload,
		setting == ATTENUATOR_SETTING_SETTINGS ? "dac1,dac2,open1_mv,open2_mv,persist" :
		"value,value_db,value1,value2,value1_db,value2_db,value1_mv,value2_mv",
		invalid, sizeof(invalid));
	if (rc != 0) {
		char message[112];

		snprintk(message, sizeof(message), "%s invalid for atten setting, see catalog", invalid);
		return coo_cmd_error(out, cmd, message);
	}

	switch (setting) {
	case ATTENUATOR_SETTING_SETTINGS: {
		struct app_attenuator_fit_policy policy;
		struct app_attenuator_open_settings open;
		struct app_attenuator_channel_settings stored = {0};
		struct attenuator snapshot;
		bool persist = false;
		bool model_changed = false;
		app_settings_get_attenuator_fit(attenuator_index, &policy);
		app_settings_get_attenuator_open(attenuator_index, &open);
		attenuator_snapshot(&attenuators[attenuator_index], &snapshot);
		struct attenuator_model_coeffs physical[] = {snapshot.coeff1, snapshot.coeff2};
		if (coo_json_extract_optional_bool(cmd->payload, "persist", &persist, NULL) != 0)
			return coo_cmd_error(out, cmd, "invalid persist");
		for (uint8_t i = 0; i < ATTENUATOR_PHYSICAL_COUNT; ++i) {
			double mv = open.mv[i];
			bool supplied = false;
			if (coo_json_extract_optional_double_range(cmd->payload,
				i == 0 ? "open1_mv" : "open2_mv", &mv, &supplied,
				0.0, ATTENUATOR_DRIVE_MAX_MV) != 0)
				return coo_cmd_error(out, cmd, "open1_mv/open2_mv must be 0..3300");
			open.mv[i] = (float)mv;
			if (parse_attenuator_settings_object(cmd->payload, i == 0 ? "dac1" : "dac2",
				&physical[i], &policy.max_fit_residual_db[i], &model_changed) != 0)
				return coo_cmd_error(out, cmd, "dac1/dac2 settings invalid, see catalog");
		}
		if (!attenuator_model_coefficients_valid(physical)) {
			return coo_cmd_error(out, cmd, "invalid resulting attenuator model");
		}

		/* Policy/open edits and save-only requests do not touch DACs. Model
		 * changes retain the existing total-dB application (closed stays closed).
		 */
		if (model_changed) {
			rc = attenuator_apply_coefficients_preserve_db(&attenuators[attenuator_index], physical);
			/* Even a failed pair write can have changed the first DAC. */
			throughput_monitor_note_attenuator_changed(attenuator_index);
			if (rc != 0) return coo_cmd_error_rc(out, cmd,
				"model apply failed; earlier DAC writes may have succeeded", rc);
		}
		(void)attenuator_set_open_voltages(&attenuators[attenuator_index], open.mv);
		for (uint8_t i = 0; i < ATTENUATOR_PHYSICAL_COUNT; ++i) {
			stored.physical[i].fvoa_50pct_mv = physical[i].fvoa_50pct_mv;
			stored.physical[i].slope_inv_fvoa_mv = physical[i].slope_inv_fvoa_mv;
			stored.physical[i].max_atten_db = physical[i].max_atten_db;
			stored.physical[i].max_calibrated_db = physical[i].max_calibrated_db;
			stored.physical[i].gain = physical[i].gain;
			stored.physical[i].rms_db = physical[i].rms_db;
			memcpy(stored.physical[i].correction_coeff, physical[i].correction_coeff,
			       sizeof(stored.physical[i].correction_coeff));
		}
		/* Save the complete logical settings, including fields omitted from
		 * this request. Independent records may fail; attempt each and report.
		 */
		int coeff_rc = app_settings_update_attenuator_channel(attenuator_index, &stored, persist);
		int open_rc = app_settings_update_attenuator_open(attenuator_index, &open, persist);
		int fit_rc = app_settings_update_attenuator_fit(attenuator_index, &policy, persist);
		if (coeff_rc != 0 || open_rc != 0 || fit_rc != 0) {
			char message[144];
			snprintk(message, sizeof(message),
				 "settings applied; persistence failed: coefficients_rc=%d opens_rc=%d fit_policy_rc=%d",
				 coeff_rc, open_rc, fit_rc);
			return coo_cmd_error_rc(out, cmd, message,
				coeff_rc != 0 ? coeff_rc : open_rc != 0 ? open_rc : fit_rc);
		}
		return atten_setting_get(cmd, out);
	}
	case ATTENUATOR_SETTING_COMPACT:
		/* An error response also returns zero when it was built successfully. */
		return attenuator_set_compact_value(cmd, out, attenuator_index);
	default:
		return coo_cmd_reply(out, cmd, COO_CMD_RESP_ERROR, "{\"error\":\"Invalid setting\"}");
	}

}

static int atten_calibration_status_reply(
	const struct coo_cmd_request *cmd,
	const struct attenuator_calibration_status *status,
	enum coo_cmd_msg_type type,
	struct coo_cmd_response *out)
{
	char payload[COO_CMD_PAYLOAD_MAX] = {0};

	if (attenuator_calibration_format_status(payload, sizeof(payload), status) != 0) {
		return coo_cmd_reply(out, cmd, COO_CMD_RESP_ERROR,
				     "{\"error\":\"Calibration status payload too large\"}");
	}
	return coo_cmd_reply(out, cmd, type, payload);
}

static int parse_calibration_records_suffix(const char *key,
					    uint8_t *physical_index,
					    bool *chunk_requested,
					    uint8_t *chunk_index)
{
	const char *suffix;
	const char *slash;
	char physical[8] = {0};
	char chunk_text[8] = {0};
	size_t physical_len;
	size_t chunk_len;
	char *end = NULL;
	unsigned long parsed;

	if (key == NULL || physical_index == NULL ||
	    chunk_requested == NULL || chunk_index == NULL) {
		return -EINVAL;
	}
	*chunk_requested = false;
	*chunk_index = 0U;

	suffix = coo_cmd_key_suffix_after(key, "atten/calibrate/records");
	if (suffix[0] == '\0') {
		return -EINVAL;
	}
	slash = strchr(suffix, '/');
	physical_len = slash == NULL ? strlen(suffix) : (size_t)(slash - suffix);
	if (physical_len == 0U || physical_len >= sizeof(physical)) {
		return -EINVAL;
	}
	memcpy(physical, suffix, physical_len);
	physical[physical_len] = '\0';

	if (slash != NULL) {
		chunk_len = strlen(slash + 1U);
		if (chunk_len == 0U || chunk_len >= sizeof(chunk_text) ||
		    strchr(slash + 1U, '/') != NULL) {
			return -EINVAL;
		}
		memcpy(chunk_text, slash + 1U, chunk_len + 1U);
		*chunk_requested = true;
	}

	if (strcmp(physical, "dac1") == 0) {
		*physical_index = 0U;
	} else if (strcmp(physical, "dac2") == 0) {
		*physical_index = 1U;
	} else {
		return -EINVAL;
	}

	if (*chunk_requested) {
		errno = 0;
		parsed = strtoul(chunk_text, &end, 10);
		if (errno != 0 || end == NULL || *end != '\0' ||
		    parsed > UINT8_MAX) {
			return -EINVAL;
		}
		*chunk_index = (uint8_t)parsed;
	}
	return 0;
}

int atten_calibration_records_get(const struct coo_cmd_request *cmd,
				  struct coo_cmd_response *out)
{
	uint8_t physical_index;
	bool chunk_requested;
	uint8_t chunk_index;
	size_t written = 0U;
	int rc;

	if (cmd != NULL && cmd->source == COO_CMD_SOURCE_SERIAL) {
		return coo_cmd_error(out, cmd,
				     "calibration records are binary MQTT payloads");
	}

	rc = parse_calibration_records_suffix(cmd != NULL ? cmd->key : NULL,
					      &physical_index,
					      &chunk_requested, &chunk_index);
	if (rc != 0) {
		return coo_cmd_error(out, cmd,
				     "use atten/calibrate/records/<dac1|dac2>[/<chunk>]");
	}

	rc = coo_cmd_reply(out, cmd, COO_CMD_RESP_OK, NULL);
	if (rc != 0) {
		return rc;
	}

	if (chunk_requested) {
		rc = attenuator_calibration_write_record_chunk(
			out->payload, sizeof(out->payload),
			physical_index, chunk_index, &written);
	} else {
		rc = attenuator_calibration_write_data_metadata(
			out->payload, sizeof(out->payload),
			physical_index, &written);
	}
	if (rc != 0) {
		return coo_cmd_error(out, cmd, rc == -ERANGE ?
			"chunk index out of range" : "calibration records unavailable");
	}
	out->payload_len = written;
	return 0;
}

int atten_calibration_get(const struct coo_cmd_request *cmd, struct coo_cmd_response *out)
{
	struct attenuator_calibration_status status = {0};

	attenuator_calibration_get_status(&status);
	return atten_calibration_status_reply(cmd, &status, COO_CMD_RESP_OK, out);
}

int atten_calibration_set(const struct coo_cmd_request *cmd, struct coo_cmd_response *out)
{
	struct attenuator_calibration_status status = {0};
	struct app_photodiode_settings pd_settings;
	struct photodiode_status pd_status;
	char laser_name[16] = {0};
	char output[MEMS_SOURCEDEST_MAX_LEN] = {0};
	struct attenuator_calibration_auto_request request = {
		.output = output,
	};
	const struct atten_calibration_routes *routes;
	char fiber = 'M';
	uint32_t dwell_ms = 0U;
	uint8_t attenuator_index;
	double dark_mv;
	bool pd_power = false;
	bool persist = false;
	bool stop = false;
	int rc;
	int parse_rc;
	int choice_value;

	if (coo_json_extract_optional_bool(cmd->payload, "stop", &stop, NULL) != 0) {
		return coo_cmd_error(out, cmd, "invalid stop");
	}
	if (stop) {
		/* Stop takes priority over accompanying start options. */
		rc = attenuator_calibration_stop(&status);
		return atten_calibration_status_reply(cmd, &status,
			rc == 0 ? COO_CMD_RESP_OK : COO_CMD_RESP_ERROR, out);
	}

	parse_rc = coo_json_extract_string(cmd->payload, "laser",
					   laser_name, sizeof(laser_name));
	if (parse_rc != COO_JSON_EXTRACT_OK ||
	    hispec_laser_id_from_name(laser_name, &request.laser) != 0) {
		return coo_cmd_error(out, cmd, "missing or invalid laser");
	}
	parse_rc = coo_json_extract_string(cmd->payload, "output",
					   output, sizeof(output));
	if (parse_rc != COO_JSON_EXTRACT_OK) {
		return coo_cmd_error(out, cmd, "missing or invalid output");
	}
	parse_rc = coo_json_extract_string_choice(cmd->payload, "fiber",
						  attenuator_cal_fiber_choices,
						  ARRAY_SIZE(attenuator_cal_fiber_choices),
						  &choice_value);
	if (parse_rc == COO_JSON_EXTRACT_ERR) {
		return coo_cmd_error(out, cmd, "fiber must be M or S");
	}
	if (parse_rc == COO_JSON_EXTRACT_OK) {
		fiber = (char)choice_value;
	}
	if (coo_json_extract_optional_u32(cmd->payload, "dwell_ms", &dwell_ms, NULL) != 0) {
		return coo_cmd_error(out, cmd, "invalid dwell_ms");
	}
	if (coo_json_extract_optional_bool(cmd->payload, "persist", &persist, NULL) != 0) {
		return coo_cmd_error(out, cmd, "invalid persist");
	}
	request.dwell_ms = dwell_ms;
	request.persist = persist;

	if (attenuator_index_from_laser_id(request.laser, &attenuator_index) != 0 ||
	    !devices_attenuator_channel_available(attenuator_index)) {
		return coo_cmd_error(out, cmd, "attenuator unavailable for laser");
	}
	request.attenuator_index = attenuator_index;
	routes = &atten_calibration_routes[request.laser];
	if (mems_router_get_route(&router, routes->laser_input, request.output) == NULL) {
		return coo_cmd_error(out, cmd, "invalid output route for laser");
	}
	request.route_input = routes->laser_input;
	request.channel = routes->pd_channel;
	request.pd_input = routes->pd_input[fiber == 'M' ? 0U : 1U];
	request.pd_output = routes->pd_output;
	if (mems_router_get_route(&router, request.pd_input,
				  routes->pd_output) == NULL) {
		return coo_cmd_error(out, cmd, "invalid photodiode route for laser/fiber");
	}

	app_settings_get_photodiode(&pd_settings);
	dark_mv = pd_settings.channel[request.channel].dark.mean_mv;
	if (!isfinite(dark_mv) ||
	    dark_mv < PHOTODIODE_DARK_MIN_MV ||
	    dark_mv > PHOTODIODE_DARK_MAX_MV) {
		return atten_calibration_pd_error(cmd, out, request.channel, "dark invalid");
	}
	rc = housekeeping_power_get((enum housekeeping_power_output)request.channel, &pd_power);
	if (rc != 0) {
		return coo_cmd_error_rc(out, cmd, "photodiode power read failed", rc);
	}
	if (!pd_power) {
		return atten_calibration_pd_error(cmd, out, request.channel, "power is off");
	}
	photodiode_get_status(&pd_status);
	if (pd_status.channel[request.channel].dark_pending) {
		return atten_calibration_pd_error(cmd, out, request.channel, "dark capture active");
	}
	if (!isfinite(pd_status.channel[request.channel].mv)) {
		return atten_calibration_pd_error(cmd, out, request.channel, "sample unavailable");
	}

	rc = attenuator_calibration_start_auto(&request, &status);
	if (rc != 0 && status.state == NULL) {
		return coo_cmd_error_rc(out, cmd, "attenuator calibration hardware setup failed", rc);
	}
	return atten_calibration_status_reply(cmd, &status, rc == 0 ? COO_CMD_RESP_OK : COO_CMD_RESP_ERROR, out);
}
