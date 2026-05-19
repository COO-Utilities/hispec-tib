/*
 * Copyright (c) 2026 Caltech Optical Observatories
 * SPDX-License-Identifier: Apache-2.0
 */

#include "attenuator_command.h"

#include <errno.h>

#include <zephyr/sys/util.h>

#include "app_settings.h"
#include "attenuator.h"
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

static int attenuator_index_from_command(const struct coo_cmd_request *cmd,
					 enum attenuator_setting *setting,
					 uint8_t *attenuator_index)
{
	char laser_name[16] = {0};
	char setting_name[16] = {0};
	enum hispec_laser_id laser_id;
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

	if (hispec_laser_id_from_name(laser_name, &laser_id) != 0 ||
	    attenuator_index_from_laser_id(laser_id, attenuator_index) != 0) {
		return -ENOENT;
	}

	if (!devices_attenuator_channel_available(*attenuator_index)) {
		return -ENODEV;
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
			 "{\"dac1\":[%.8f,%.8f],\"dac2\":[%.8f,%.8f]}",
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
			 "{\"db\":%.4f,\"linear\":%.6f,"
			 "\"voltage1\":%.4f,\"voltage2\":%.4f,"
			 "\"db1\":%.4f,\"db2\":%.4f}",
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
