/*
 * Copyright (c) 2026 Caltech Optical Observatories
 * SPDX-License-Identifier: Apache-2.0
 */

#include "attenuator_command.h"

#include <errno.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include <zephyr/sys/util.h>

#include "app_settings.h"
#include "attenuator.h"
#include "attenuator_calibration.h"
#include "devices.h"
#include "lasers.h"
#include "throughput_monitor.h"

#include <coo_commons/command_dispatch.h>
#include <coo_commons/json_utils.h>

enum attenuator_setting {
	ATTENUATOR_SETTING_COEFF = 0,
	ATTENUATOR_SETTING_COMPACT,
};

static const struct coo_json_string_choice attenuator_setting_choices[] = {
	{ "coeff", ATTENUATOR_SETTING_COEFF },
};

static const struct coo_json_string_choice attenuator_cal_fiber_choices[] = {
	{ "m", 'M' },
	{ "s", 'S' },
};

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
	return 0;
}

static int attenuator_status_reply(const struct coo_cmd_request *cmd,
				   struct coo_cmd_response *out,
				   uint8_t attenuator_index)
{
	struct attenuator_status status = {0};
	char payload[MAX_PAYLOAD_LEN] = {0};
	double linear1;
	double linear2;

	if (!attenuator_get(&attenuators[attenuator_index], &status)) {
		return coo_cmd_reply(out, cmd, COO_CMD_RESP_ERROR,
				     "{\"error\":\"Failed to read attenuator\"}");
	}

	linear1 = pow(10.0, -status.attenuation_db1 / 10.0);
	linear2 = pow(10.0, -status.attenuation_db2 / 10.0);
	snprintk(payload, sizeof(payload),
		 "{\"db\":%.6f,\"linear\":%.12g,"
		 "\"v1_mv\":%.6f,\"v2_mv\":%.6f,"
		 "\"db1\":%.6f,\"db2\":%.6f,"
		 "\"linear1\":%.12g,\"linear2\":%.12g}",
		 status.attenuation_db,
		 status.linear,
		 status.voltage1,
		 status.voltage2,
		 status.attenuation_db1,
		 status.attenuation_db2,
		 linear1,
		 linear2);

	return coo_cmd_reply(out, cmd, COO_CMD_RESP_OK, payload);
}

int atten_setting_get(const struct coo_cmd_request *cmd, struct coo_cmd_response *out)
{
	enum attenuator_setting setting;
	uint8_t attenuator_index;
	int rc;
	char payload[MAX_PAYLOAD_LEN] = {0};

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
	case ATTENUATOR_SETTING_COEFF:
		snprintk(payload, sizeof(payload),
			 "{\"dac1\":[%.12g,%.12g],\"dac2\":[%.12g,%.12g],"
			 "\"gain1\":%.12g,\"gain2\":%.12g}",
			 attenuators[attenuator_index].coeff1.slope,
			 attenuators[attenuator_index].coeff1.offset,
			 attenuators[attenuator_index].coeff2.slope,
			 attenuators[attenuator_index].coeff2.offset,
			 attenuators[attenuator_index].coeff1.gain,
			 attenuators[attenuator_index].coeff2.gain);
		break;
	case ATTENUATOR_SETTING_COMPACT:
		return attenuator_status_reply(cmd, out, attenuator_index);
	default:
		return coo_cmd_reply(out, cmd, COO_CMD_RESP_ERROR, "{\"error\":\"Invalid setting\"}");
	}

	return coo_cmd_reply(out, cmd, COO_CMD_RESP_OK, payload);
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
		out->mode = ATTENUATOR_PHYSICAL_VALUE_LINEAR;
		out->value = linear;
	} else if (db_present) {
		out->mode = ATTENUATOR_PHYSICAL_VALUE_DB;
		out->value = db;
	} else if (mv_present) {
		out->mode = ATTENUATOR_PHYSICAL_VALUE_MV;
		out->value = mv;
	}

	return 0;
}

static bool attenuator_set_physical_value(struct attenuator *drv,
					  uint8_t physical_index,
					  const struct attenuator_physical_value *request)
{
	double db;

	if (drv == NULL || request == NULL) {
		return false;
	}

	switch (request->mode) {
	case ATTENUATOR_PHYSICAL_VALUE_NONE:
		return true;
	case ATTENUATOR_PHYSICAL_VALUE_LINEAR:
		if (request->value <= 0.0 || request->value > 1.0) {
			return false;
		}
		db = -10.0 * log10(request->value);
		return physical_index == 0U ?
		       attenuator_set_dac1_db(drv, db) :
		       attenuator_set_dac2_db(drv, db);
	case ATTENUATOR_PHYSICAL_VALUE_DB:
		return physical_index == 0U ?
		       attenuator_set_dac1_db(drv, request->value) :
		       attenuator_set_dac2_db(drv, request->value);
	case ATTENUATOR_PHYSICAL_VALUE_MV:
		return attenuator_set_physical_voltage(drv, physical_index,
						       request->value);
	default:
		return false;
	}
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
					   total_linear)) {
			return coo_cmd_reply(out, cmd, COO_CMD_RESP_ERROR,
					     "{\"error\":\"Invalid linear transmission\"}");
		}
	} else if (total_db_present) {
		if (!attenuator_set_db(&attenuators[attenuator_index], total_db)) {
			return coo_cmd_reply(out, cmd, COO_CMD_RESP_ERROR,
					     "{\"error\":\"Invalid dB attenuation\"}");
		}
	} else {
		for (uint8_t i = 0U; i < ATTENUATOR_PHYSICAL_COUNT; ++i) {
			if (!attenuator_set_physical_value(&attenuators[attenuator_index],
							   i, &physical[i])) {
				return coo_cmd_reply(out, cmd, COO_CMD_RESP_ERROR,
						     "{\"error\":\"Invalid physical attenuator value\"}");
			}
		}
	}

	return 0;
}

int atten_setting_set(const struct coo_cmd_request *cmd, struct coo_cmd_response *out)
{
	enum attenuator_setting setting;
	uint8_t attenuator_index;
	int rc;

	rc = attenuator_index_from_command(cmd, &setting, &attenuator_index);
	if (rc == -EINVAL) {
		return coo_cmd_reply(out, cmd, COO_CMD_RESP_ERROR,
					  "{\"error\":\"Failed to parse laser/setting\"}");
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
	case ATTENUATOR_SETTING_COEFF: {
		double dac1_coeffs[ATTENUATOR_COEFF_COUNT] = {0};
		double dac2_coeffs[ATTENUATOR_COEFF_COUNT] = {0};
		size_t dac1_len = 0U;
		size_t dac2_len = 0U;
		struct app_attenuator_channel_settings stored_coeffs = {0};
		struct attenuator_model_coeffs physical[ATTENUATOR_PHYSICAL_COUNT];
		bool persist = false;
		double gain1;
		double gain2;
		int parse_rc;

		parse_rc = coo_json_extract_double_array(cmd->payload, "dac1",
							 dac1_coeffs,
							 ATTENUATOR_COEFF_COUNT,
							 &dac1_len);
		if (parse_rc != COO_JSON_EXTRACT_OK || dac1_len != ATTENUATOR_COEFF_COUNT) {
			return coo_cmd_reply(out, cmd, COO_CMD_RESP_ERROR,
						  "{\"error\":\"Improper arguments\"}");
		}

		parse_rc = coo_json_extract_double_array(cmd->payload, "dac2",
							 dac2_coeffs,
							 ATTENUATOR_COEFF_COUNT,
							 &dac2_len);
		if (parse_rc != COO_JSON_EXTRACT_OK || dac2_len != ATTENUATOR_COEFF_COUNT) {
			return coo_cmd_reply(out, cmd, COO_CMD_RESP_ERROR,
						  "{\"error\":\"Improper arguments\"}");
		}

		if (coo_json_extract_double(cmd->payload, "gain1", &gain1) !=
		    COO_JSON_EXTRACT_OK ||
		    coo_json_extract_double(cmd->payload, "gain2", &gain2) !=
		    COO_JSON_EXTRACT_OK) {
			return coo_cmd_reply(out, cmd, COO_CMD_RESP_ERROR,
					     "{\"error\":\"Missing gain\"}");
		}

		if (coo_json_extract_optional_bool(cmd->payload, "persist",
						   &persist, NULL) != 0) {
			return coo_cmd_reply(out, cmd, COO_CMD_RESP_ERROR,
						  "{\"error\":\"Invalid persist flag\"}");
		}

		physical[0].slope = dac1_coeffs[0];
		physical[0].offset = dac1_coeffs[1];
		physical[0].gain = gain1;
		physical[1].slope = dac2_coeffs[0];
		physical[1].offset = dac2_coeffs[1];
		physical[1].gain = gain2;
		stored_coeffs.physical[0].slope = dac1_coeffs[0];
		stored_coeffs.physical[0].offset = dac1_coeffs[1];
		stored_coeffs.physical[0].gain = gain1;
		stored_coeffs.physical[1].slope = dac2_coeffs[0];
		stored_coeffs.physical[1].offset = dac2_coeffs[1];
		stored_coeffs.physical[1].gain = gain2;

		if (attenuator_apply_coefficients_preserve_db(
			    &attenuators[attenuator_index], physical) != 0) {
			return coo_cmd_reply(out, cmd, COO_CMD_RESP_ERROR,
						  "{\"error\":\"Failed to apply coefficients\"}");
		}
		app_settings_update_attenuator_channel(attenuator_index,
						       &stored_coeffs,
						       persist);
		break;
	}
	case ATTENUATOR_SETTING_COMPACT:
		rc = attenuator_set_compact_value(cmd, out, attenuator_index);
		if (rc != 0) {
			return rc;
		}
		break;
	default:
		return coo_cmd_reply(out, cmd, COO_CMD_RESP_ERROR, "{\"error\":\"Invalid setting\"}");
	}

	throughput_monitor_note_attenuator_changed(attenuator_index);

	if (setting == ATTENUATOR_SETTING_COEFF) {
		return coo_cmd_ok(out, cmd);
	}

	return attenuator_status_reply(cmd, out, attenuator_index);
}

static int atten_calibration_status_reply(
	const struct coo_cmd_request *cmd,
	const struct attenuator_calibration_status *status,
	enum coo_cmd_msg_type type,
	struct coo_cmd_response *out)
{
	char payload[MAX_PAYLOAD_LEN] = {0};

	if (attenuator_calibration_format_status(payload, sizeof(payload), status) != 0) {
		return coo_cmd_reply(out, cmd, COO_CMD_RESP_ERROR,
				     "{\"error\":\"Calibration status payload too large\"}");
	}
	return coo_cmd_reply(out, cmd, type, payload);
}

static int parse_calibration_data_suffix(const char *key,
					 uint8_t *physical_index,
					 uint8_t *start_index)
{
	const char *suffix;
	const char *slash;
	char physical[8] = {0};
	char start_text[8] = "0";
	size_t physical_len;
	size_t start_len;
	char *end = NULL;
	unsigned long parsed;

	if (key == NULL || physical_index == NULL || start_index == NULL) {
		return -EINVAL;
	}

	suffix = coo_cmd_key_suffix_after(key, "atten/calibrate/data");
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
		start_len = strlen(slash + 1U);
		if (start_len == 0U || start_len >= sizeof(start_text) ||
		    strchr(slash + 1U, '/') != NULL) {
			return -EINVAL;
		}
		memcpy(start_text, slash + 1U, start_len + 1U);
	}

	if (strcmp(physical, "dac1") == 0) {
		*physical_index = 0U;
	} else if (strcmp(physical, "dac2") == 0) {
		*physical_index = 1U;
	} else {
		return -EINVAL;
	}

	errno = 0;
	parsed = strtoul(start_text, &end, 10);
	if (errno != 0 || end == NULL || *end != '\0' ||
	    parsed >= ATTENUATOR_CAL_RECORD_COUNT) {
		return -EINVAL;
	}
	*start_index = (uint8_t)parsed;
	return 0;
}

int atten_calibration_data_get(const struct coo_cmd_request *cmd,
			       struct coo_cmd_response *out)
{
	char payload[MAX_PAYLOAD_LEN] = {0};
	uint8_t physical_index;
	uint8_t start_index;
	int rc;

	rc = parse_calibration_data_suffix(cmd != NULL ? cmd->key : NULL,
					   &physical_index, &start_index);
	if (rc != 0) {
		return coo_cmd_error(out, cmd,
				     "use atten/calibrate/data/<dac1|dac2>[/<start>]");
	}

	rc = attenuator_calibration_format_data_page(payload, sizeof(payload),
						     physical_index, start_index);
	if (rc != 0) {
		return coo_cmd_error(out, cmd, "calibration data page unavailable");
	}
	return coo_cmd_reply(out, cmd, COO_CMD_RESP_OK, payload);
}

static int parse_calibration_batch_object(
	const char *json,
	const char *key,
	struct attenuator_calibration_batch *batch)
{
	char object_json[MAX_PAYLOAD_LEN] = {0};
	size_t voltage_len = 0U;
	size_t flux_len = 0U;
	int rc;

	if (batch == NULL) {
		return -EINVAL;
	}

	memset(batch, 0, sizeof(*batch));
	rc = coo_json_extract_object(json, key, object_json, sizeof(object_json));
	if (rc == COO_JSON_EXTRACT_MISSING) {
		return COO_JSON_EXTRACT_MISSING;
	}
	if (rc != COO_JSON_EXTRACT_OK) {
		return COO_JSON_EXTRACT_ERR;
	}

	rc = coo_json_extract_double_array(object_json, "v_mV",
					   batch->voltage_mv,
					   ATTENUATOR_CAL_POINT_COUNT,
					   &voltage_len);
	if (rc != COO_JSON_EXTRACT_OK) {
		return COO_JSON_EXTRACT_ERR;
	}
	rc = coo_json_extract_double_array(object_json, "flux",
					   batch->flux,
					   ATTENUATOR_CAL_POINT_COUNT,
					   &flux_len);
	if (rc != COO_JSON_EXTRACT_OK ||
	    voltage_len != flux_len ||
	    voltage_len < ATTENUATOR_CAL_MIN_BATCH_POINTS) {
		return COO_JSON_EXTRACT_ERR;
	}

	batch->len = voltage_len;
	return COO_JSON_EXTRACT_OK;
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
	struct attenuator_calibration_batch batch[2];
	char mode[16] = {0};
	char atten_name[16] = "lfc";
	char laser_name[16] = {0};
	char output[MEMS_SOURCEDEST_MAX_LEN] = {0};
	struct attenuator_calibration_auto_request request = {
		.output = output,
		.fiber = 'M',
	};
	uint32_t dwell_ms = 0U;
	bool persistent = false;
	bool stop = false;
	bool cont = false;
	bool cont_present = false;
	bool other_present = false;
	double other_mv = ATTENUATOR_DRIVE_MAX_MV;
	int rc;
	int parse_rc;
	int choice_value;
	uint8_t attenuator_index;

	if (coo_json_extract_optional_bool(cmd->payload, "stop", &stop, NULL) != 0) {
		return coo_cmd_error(out, cmd, "invalid stop");
	}
	if (stop) {
		(void)attenuator_calibration_stop(&status);
		return atten_calibration_status_reply(cmd, &status, COO_CMD_RESP_OK, out);
	}

	if (coo_json_extract_optional_bool(cmd->payload, "continue",
					   &cont, &cont_present) != 0) {
		return coo_cmd_error(out, cmd, "invalid continue");
	}
	if (cont_present) {
		if (coo_json_extract_optional_double_range(cmd->payload, "other_mv",
							   &other_mv,
							   &other_present,
							   0.0,
							   ATTENUATOR_DRIVE_MAX_MV) != 0) {
			return coo_cmd_error(out, cmd, "invalid other_mv");
		}
		if (cont) {
			rc = attenuator_calibration_manual_continue(other_present,
								   other_mv,
								   &status);
			if (rc != 0) {
					return atten_calibration_status_reply(cmd, &status,
									     COO_CMD_RESP_ERROR,
									     out);
			}
		} else {
			attenuator_calibration_get_status(&status);
		}
		return atten_calibration_status_reply(cmd, &status, COO_CMD_RESP_OK, out);
	}

	parse_rc = parse_calibration_batch_object(cmd->payload, "dac1", &batch[0]);
	if (parse_rc == COO_JSON_EXTRACT_OK) {
		if (parse_calibration_batch_object(cmd->payload, "dac2", &batch[1]) !=
		    COO_JSON_EXTRACT_OK) {
			return coo_cmd_error(out, cmd, "manual fit requires dac1 and dac2 batches");
		}
		(void)coo_json_extract_string(cmd->payload, "attenuator",
					      atten_name, sizeof(atten_name));
		if (attenuator_index_from_name(atten_name, &attenuator_index) != 0) {
			return coo_cmd_error(out, cmd, "invalid attenuator");
		}
		if (coo_json_extract_optional_bool(cmd->payload, "persist",
						   &persistent, NULL) != 0) {
			return coo_cmd_error(out, cmd, "invalid persist");
		}
		rc = attenuator_calibration_fit_manual(attenuator_index, batch,
						       persistent, &status);
			return atten_calibration_status_reply(
				cmd, &status, rc == 0 ? COO_CMD_RESP_OK : COO_CMD_RESP_ERROR,
				out);
	}
	if (parse_rc == COO_JSON_EXTRACT_ERR) {
		return coo_cmd_error(out, cmd, "invalid manual fit batch");
	}

	(void)coo_json_extract_string(cmd->payload, "mode", mode, sizeof(mode));
	if (strcmp(mode, "manual") == 0) {
		(void)coo_json_extract_string(cmd->payload, "attenuator",
					      atten_name, sizeof(atten_name));
		if (attenuator_index_from_name(atten_name, &attenuator_index) != 0) {
			return coo_cmd_error(out, cmd, "invalid attenuator");
		}
		if (coo_json_extract_optional_u32(cmd->payload, "dwell_ms",
						  &dwell_ms, NULL) != 0) {
			return coo_cmd_error(out, cmd, "invalid dwell_ms");
		}
		if (coo_json_extract_optional_bool(cmd->payload, "persist",
						   &persistent, NULL) != 0) {
			return coo_cmd_error(out, cmd, "invalid persist");
		}
		rc = attenuator_calibration_start_manual(attenuator_index,
							 dwell_ms,
							 persistent,
							 &status);
		return atten_calibration_status_reply(
			cmd, &status, rc == 0 ? COO_CMD_RESP_OK : COO_CMD_RESP_ERROR, out);
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
		request.fiber = (char)choice_value;
	}
	if (coo_json_extract_optional_u32(cmd->payload, "dwell_ms",
					  &dwell_ms, NULL) != 0) {
		return coo_cmd_error(out, cmd, "invalid dwell_ms");
	}
	if (coo_json_extract_optional_bool(cmd->payload, "persist",
					   &persistent, NULL) != 0) {
		return coo_cmd_error(out, cmd, "invalid persist");
	}
	request.dwell_ms = dwell_ms;
	request.persistent = persistent;

	rc = attenuator_calibration_start_auto(&request, &status);
	if (rc != 0 && status.state == NULL) {
		return coo_cmd_error(out, cmd, "attenuator calibration start failed");
	}
	return atten_calibration_status_reply(
		cmd, &status, rc == 0 ? COO_CMD_RESP_OK : COO_CMD_RESP_ERROR, out);
}
