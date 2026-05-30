/*
 * Copyright (c) 2026 Caltech Optical Observatories
 * SPDX-License-Identifier: Apache-2.0
 */

#include "attenuator_command.h"

#include <errno.h>
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
	ATTENUATOR_SETTING_VALUE,
	ATTENUATOR_SETTING_VALUEDB,
};

static const struct coo_json_string_choice attenuator_setting_choices[] = {
	{ "coeff", ATTENUATOR_SETTING_COEFF },
	{ "value", ATTENUATOR_SETTING_VALUE },
	{ "valuedb", ATTENUATOR_SETTING_VALUEDB },
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

	if (cmd == NULL || setting == NULL || attenuator_index == NULL ||
	    coo_cmd_key_suffix_pair_copy(cmd->key, "atten",
					 laser_name, sizeof(laser_name),
					 setting_name, sizeof(setting_name)) != 0) {
		return -EINVAL;
	}

	if (coo_json_match_string_choice(setting_name, attenuator_setting_choices,
					 ARRAY_SIZE(attenuator_setting_choices),
					 &setting_value) != 0) {
		return -ENOTSUP;
	}

	{
		int rc = attenuator_index_from_name(laser_name, attenuator_index);

		if (rc != 0) {
			return rc;
		}
	}
	*setting = (enum attenuator_setting)setting_value;
	return 0;
}

struct coo_cmd_response atten_setting_get(const struct coo_cmd_request *cmd)
{
	enum attenuator_setting setting;
	uint8_t attenuator_index;
	int rc;
	char payload[MAX_PAYLOAD_LEN] = {0};

	rc = attenuator_index_from_command(cmd, &setting, &attenuator_index);
	if (rc == -EINVAL) {
		return coo_cmd_reply(cmd, COO_CMD_RESP_ERROR,
					  "{\"error\":\"Failed to parse atten/setting\"}");
	}
	if (rc == -ENOTSUP) {
		return coo_cmd_reply(cmd, COO_CMD_RESP_ERROR, "{\"error\":\"Invalid setting\"}");
	}
	if (rc == -ENOENT) {
		return coo_cmd_reply(cmd, COO_CMD_RESP_ERROR, "{\"error\":\"Invalid attenuator\"}");
	}
	if (rc != 0) {
		return coo_cmd_reply(cmd, COO_CMD_RESP_ERROR,
					  "{\"error\":\"Attenuator unavailable on this board\"}");
	}

	switch (setting) {
	case ATTENUATOR_SETTING_COEFF:
		snprintk(payload, sizeof(payload),
			 "{\"dac1\":[%.12g,%.12g],\"dac2\":[%.12g,%.12g]}",
			 attenuators[attenuator_index].coeff1.slope,
			 attenuators[attenuator_index].coeff1.offset,
			 attenuators[attenuator_index].coeff2.slope,
			 attenuators[attenuator_index].coeff2.offset);
		break;
	case ATTENUATOR_SETTING_VALUE:
	case ATTENUATOR_SETTING_VALUEDB: {
		struct attenuator_status status = {0};

		if (!attenuator_get(&attenuators[attenuator_index], &status)) {
			return coo_cmd_reply(cmd, COO_CMD_RESP_ERROR,
						  "{\"error\":\"Failed to read attenuator\"}");
		}
		snprintk(payload, sizeof(payload),
			 "{\"db\":%.6f,\"linear\":%.12g,"
			 "\"v1_mv\":%.6f,\"v2_mv\":%.6f,"
			 "\"db1\":%.6f,\"db2\":%.6f}",
			 status.attenuation_db,
			 status.linear,
			 status.voltage1,
			 status.voltage2,
			 status.attenuation_db1,
			 status.attenuation_db2);
		break;
	}
	default:
		return coo_cmd_reply(cmd, COO_CMD_RESP_ERROR, "{\"error\":\"Invalid setting\"}");
	}

	return coo_cmd_reply(cmd, COO_CMD_RESP_OK, payload);
}

struct coo_cmd_response atten_setting_set(const struct coo_cmd_request *cmd)
{
	enum attenuator_setting setting;
	uint8_t attenuator_index;
	int rc;

	rc = attenuator_index_from_command(cmd, &setting, &attenuator_index);
	if (rc == -EINVAL) {
		return coo_cmd_reply(cmd, COO_CMD_RESP_ERROR,
					  "{\"error\":\"Failed to parse laser/setting\"}");
	}
	if (rc == -ENOTSUP) {
		return coo_cmd_reply(cmd, COO_CMD_RESP_ERROR, "{\"error\":\"Invalid setting\"}");
	}
	if (rc == -ENOENT) {
		return coo_cmd_reply(cmd, COO_CMD_RESP_ERROR, "{\"error\":\"Invalid attenuator\"}");
	}
	if (rc != 0) {
		return coo_cmd_reply(cmd, COO_CMD_RESP_ERROR,
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
		int parse_rc;

		parse_rc = coo_json_extract_double_array(cmd->payload, "dac1",
							 dac1_coeffs,
							 ATTENUATOR_COEFF_COUNT,
							 &dac1_len);
		if (parse_rc != COO_JSON_EXTRACT_OK || dac1_len != ATTENUATOR_COEFF_COUNT) {
			return coo_cmd_reply(cmd, COO_CMD_RESP_ERROR,
						  "{\"error\":\"Improper arguments\"}");
		}

		parse_rc = coo_json_extract_double_array(cmd->payload, "dac2",
							 dac2_coeffs,
							 ATTENUATOR_COEFF_COUNT,
							 &dac2_len);
		if (parse_rc != COO_JSON_EXTRACT_OK || dac2_len != ATTENUATOR_COEFF_COUNT) {
			return coo_cmd_reply(cmd, COO_CMD_RESP_ERROR,
						  "{\"error\":\"Improper arguments\"}");
		}

		if (coo_json_extract_optional_bool(cmd->payload, "persistent",
						   &persist, NULL) != 0) {
			return coo_cmd_reply(cmd, COO_CMD_RESP_ERROR,
						  "{\"error\":\"Invalid persistent flag\"}");
		}

		physical[0].slope = dac1_coeffs[0];
		physical[0].offset = dac1_coeffs[1];
		physical[1].slope = dac2_coeffs[0];
		physical[1].offset = dac2_coeffs[1];
		stored_coeffs.physical[0].slope = dac1_coeffs[0];
		stored_coeffs.physical[0].offset = dac1_coeffs[1];
		stored_coeffs.physical[1].slope = dac2_coeffs[0];
		stored_coeffs.physical[1].offset = dac2_coeffs[1];

		if (attenuator_apply_coefficients_preserve_db(
			    &attenuators[attenuator_index], physical) != 0) {
			return coo_cmd_reply(cmd, COO_CMD_RESP_ERROR,
						  "{\"error\":\"Failed to apply coefficients\"}");
		}
		app_settings_update_attenuator_channel(attenuator_index,
						       &stored_coeffs,
						       persist);
		break;
	}
	case ATTENUATOR_SETTING_VALUE:
	case ATTENUATOR_SETTING_VALUEDB: {
		double value;

		if (coo_json_extract_double(cmd->payload, "value", &value) !=
		    COO_JSON_EXTRACT_OK) {
			return coo_cmd_reply(cmd, COO_CMD_RESP_ERROR,
						  "{\"error\":\"Missing setting value\"}");
		}

		if (setting == ATTENUATOR_SETTING_VALUE) {
			if (!attenuator_set_linear(&attenuators[attenuator_index], value)) {
				return coo_cmd_reply(cmd, COO_CMD_RESP_ERROR,
							  "{\"error\":\"Invalid linear transmission\"}");
			}
		} else {
			if (!attenuator_set_db(&attenuators[attenuator_index], value)) {
				return coo_cmd_reply(cmd, COO_CMD_RESP_ERROR,
							  "{\"error\":\"Invalid dB attenuation\"}");
			}
		}
		break;
	}
	default:
		return coo_cmd_reply(cmd, COO_CMD_RESP_ERROR, "{\"error\":\"Invalid setting\"}");
	}

	throughput_monitor_note_attenuator_changed(attenuator_index);

	return coo_cmd_ok(cmd);
}

static struct coo_cmd_response atten_calibration_status_reply(
	const struct coo_cmd_request *cmd,
	const struct attenuator_calibration_status *status,
	enum coo_cmd_msg_type type)
{
	char payload[MAX_PAYLOAD_LEN] = {0};

	if (attenuator_calibration_format_status(payload, sizeof(payload), status) != 0) {
		return coo_cmd_reply(cmd, COO_CMD_RESP_ERROR,
				     "{\"error\":\"Calibration status payload too large\"}");
	}
	return coo_cmd_reply(cmd, type, payload);
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

	rc = coo_json_extract_double_array(object_json, "voltage_mv",
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

struct coo_cmd_response atten_calibration_get(const struct coo_cmd_request *cmd)
{
	struct attenuator_calibration_status status = {0};

	attenuator_calibration_get_status(&status);
	return atten_calibration_status_reply(cmd, &status, COO_CMD_RESP_OK);
}

struct coo_cmd_response atten_calibration_set(const struct coo_cmd_request *cmd)
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
		return coo_cmd_error(cmd, "invalid stop");
	}
	if (stop) {
		(void)attenuator_calibration_stop(&status);
		return atten_calibration_status_reply(cmd, &status, COO_CMD_RESP_OK);
	}

	if (coo_json_extract_optional_bool(cmd->payload, "continue",
					   &cont, &cont_present) != 0) {
		return coo_cmd_error(cmd, "invalid continue");
	}
	if (cont_present) {
		if (coo_json_extract_optional_double_range(cmd->payload, "other_mv",
							   &other_mv,
							   &other_present,
							   0.0,
							   ATTENUATOR_DRIVE_MAX_MV) != 0) {
			return coo_cmd_error(cmd, "invalid other_mv");
		}
		if (cont) {
			rc = attenuator_calibration_manual_continue(other_present,
								   other_mv,
								   &status);
			if (rc != 0) {
				return atten_calibration_status_reply(cmd, &status,
								     COO_CMD_RESP_ERROR);
			}
		} else {
			attenuator_calibration_get_status(&status);
		}
		return atten_calibration_status_reply(cmd, &status, COO_CMD_RESP_OK);
	}

	parse_rc = parse_calibration_batch_object(cmd->payload, "dac1", &batch[0]);
	if (parse_rc == COO_JSON_EXTRACT_OK) {
		if (parse_calibration_batch_object(cmd->payload, "dac2", &batch[1]) !=
		    COO_JSON_EXTRACT_OK) {
			return coo_cmd_error(cmd, "manual fit requires dac1 and dac2 batches");
		}
		(void)coo_json_extract_string(cmd->payload, "attenuator",
					      atten_name, sizeof(atten_name));
		if (attenuator_index_from_name(atten_name, &attenuator_index) != 0) {
			return coo_cmd_error(cmd, "invalid attenuator");
		}
		if (coo_json_extract_optional_bool(cmd->payload, "persistent",
						   &persistent, NULL) != 0) {
			return coo_cmd_error(cmd, "invalid persistent");
		}
		rc = attenuator_calibration_fit_manual(attenuator_index, batch,
						       persistent, &status);
		return atten_calibration_status_reply(
			cmd, &status, rc == 0 ? COO_CMD_RESP_OK : COO_CMD_RESP_ERROR);
	}
	if (parse_rc == COO_JSON_EXTRACT_ERR) {
		return coo_cmd_error(cmd, "invalid manual fit batch");
	}

	(void)coo_json_extract_string(cmd->payload, "mode", mode, sizeof(mode));
	if (strcmp(mode, "manual") == 0) {
		(void)coo_json_extract_string(cmd->payload, "attenuator",
					      atten_name, sizeof(atten_name));
		if (attenuator_index_from_name(atten_name, &attenuator_index) != 0) {
			return coo_cmd_error(cmd, "invalid attenuator");
		}
		if (coo_json_extract_optional_u32(cmd->payload, "dwell_ms",
						  &dwell_ms, NULL) != 0) {
			return coo_cmd_error(cmd, "invalid dwell_ms");
		}
		if (coo_json_extract_optional_bool(cmd->payload, "persistent",
						   &persistent, NULL) != 0) {
			return coo_cmd_error(cmd, "invalid persistent");
		}
		rc = attenuator_calibration_start_manual(attenuator_index,
							 dwell_ms,
							 persistent,
							 &status);
		return atten_calibration_status_reply(
			cmd, &status, rc == 0 ? COO_CMD_RESP_OK : COO_CMD_RESP_ERROR);
	}

	parse_rc = coo_json_extract_string(cmd->payload, "laser",
					   laser_name, sizeof(laser_name));
	if (parse_rc != COO_JSON_EXTRACT_OK ||
	    hispec_laser_id_from_name(laser_name, &request.laser) != 0) {
		return coo_cmd_error(cmd, "missing or invalid laser");
	}
	parse_rc = coo_json_extract_string(cmd->payload, "output",
					   output, sizeof(output));
	if (parse_rc != COO_JSON_EXTRACT_OK) {
		return coo_cmd_error(cmd, "missing or invalid output");
	}
	parse_rc = coo_json_extract_string_choice(cmd->payload, "fiber",
						  attenuator_cal_fiber_choices,
						  ARRAY_SIZE(attenuator_cal_fiber_choices),
						  &choice_value);
	if (parse_rc == COO_JSON_EXTRACT_ERR) {
		return coo_cmd_error(cmd, "fiber must be M or S");
	}
	if (parse_rc == COO_JSON_EXTRACT_OK) {
		request.fiber = (char)choice_value;
	}
	if (coo_json_extract_optional_u32(cmd->payload, "dwell_ms",
					  &dwell_ms, NULL) != 0) {
		return coo_cmd_error(cmd, "invalid dwell_ms");
	}
	if (coo_json_extract_optional_bool(cmd->payload, "persistent",
					   &persistent, NULL) != 0) {
		return coo_cmd_error(cmd, "invalid persistent");
	}
	request.dwell_ms = dwell_ms;
	request.persistent = persistent;

	rc = attenuator_calibration_start_auto(&request, &status);
	if (rc != 0 && status.state == NULL) {
		return coo_cmd_error(cmd, "attenuator calibration start failed");
	}
	return atten_calibration_status_reply(
		cmd, &status, rc == 0 ? COO_CMD_RESP_OK : COO_CMD_RESP_ERROR);
}
