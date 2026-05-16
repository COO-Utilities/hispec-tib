/*
 * Copyright (c) 2026 Caltech Optical Observatories
 * SPDX-License-Identifier: Apache-2.0
 */

#include "attenuator_command.h"

#include <errno.h>
#include <string.h>
#include <strings.h>

#include "app_settings.h"
#include "attenuator.h"
#include "devices.h"
#include "lasers.h"
#include "throughput_monitor.h"

#include <coo_commons/json_utils.h>

static int parse_atten_key(const char *key,
			   char *laser_name,
			   size_t laser_name_len,
			   char *setting,
			   size_t setting_len)
{
	const char prefix[] = "atten/";
	const char *laser_start;
	const char *slash;
	size_t laser_len;
	size_t parsed_setting_len;

	if (key == NULL || laser_name == NULL || setting == NULL ||
	    strncmp(key, prefix, strlen(prefix)) != 0) {
		return -EINVAL;
	}

	laser_start = key + strlen(prefix);
	slash = strchr(laser_start, '/');
	if (slash == NULL) {
		return -EINVAL;
	}

	laser_len = (size_t)(slash - laser_start);
	parsed_setting_len = strcspn(slash + 1, "/");
	if (laser_len == 0U || laser_len >= laser_name_len ||
	    parsed_setting_len == 0U || parsed_setting_len >= setting_len ||
	    (slash + 1)[parsed_setting_len] != '\0') {
		return -EINVAL;
	}

	memcpy(laser_name, laser_start, laser_len);
	laser_name[laser_len] = '\0';
	memcpy(setting, slash + 1, parsed_setting_len);
	setting[parsed_setting_len] = '\0';
	return 0;
}

static int attenuator_index_from_command(const struct Command *cmd,
					 char *setting,
					 size_t setting_len,
					 uint8_t *attenuator_index)
{
	char laser_name[16] = {0};
	enum hispec_laser_id laser_id;

	if (parse_atten_key(cmd->key, laser_name, sizeof(laser_name),
			    setting, setting_len) != 0) {
		return -EINVAL;
	}

	if (hispec_laser_id_from_name(laser_name, &laser_id) != 0 ||
	    attenuator_index_from_laser_id(laser_id, attenuator_index) != 0) {
		return -ENOENT;
	}

	if (!devices_attenuator_channel_available(*attenuator_index)) {
		return -ENODEV;
	}

	return 0;
}

struct OutMsg atten_setting_get(const struct Command *cmd)
{
	char setting[16] = {0};
	uint8_t attenuator_index;
	int rc;
	char payload[MAX_PAYLOAD_LEN] = {0};

	rc = attenuator_index_from_command(cmd, setting, sizeof(setting), &attenuator_index);
	if (rc == -EINVAL) {
		return coo_cmd_reply(cmd, RESP_ERROR,
					  "{\"error\":\"Failed to parse atten/setting\"}");
	}
	if (rc == -ENOENT) {
		return coo_cmd_reply(cmd, RESP_ERROR, "{\"error\":\"Invalid attenuator\"}");
	}
	if (rc != 0) {
		return coo_cmd_reply(cmd, RESP_ERROR,
					  "{\"error\":\"Attenuator unavailable on this board\"}");
	}

	if (strcasecmp(setting, "coeff") == 0) {
		snprintk(payload, sizeof(payload),
			 "{\"dac1\":[%.8f,%.8f],\"dac2\":[%.8f,%.8f]}",
			 attenuators[attenuator_index].coeff1.slope,
			 attenuators[attenuator_index].coeff1.offset,
			 attenuators[attenuator_index].coeff2.slope,
			 attenuators[attenuator_index].coeff2.offset);
	} else if (strcasecmp(setting, "value") == 0 ||
		   strcasecmp(setting, "valuedb") == 0) {
		struct attenuator_status status = {0};

		if (!attenuator_get(&attenuators[attenuator_index], &status)) {
			return coo_cmd_reply(cmd, RESP_ERROR,
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
	} else {
		return coo_cmd_reply(cmd, RESP_ERROR, "{\"error\":\"Invalid setting\"}");
	}

	return coo_cmd_reply(cmd, RESP_OK, payload);
}

struct OutMsg atten_setting_set(const struct Command *cmd)
{
	char setting[16] = {0};
	uint8_t attenuator_index;
	int rc;

	rc = attenuator_index_from_command(cmd, setting, sizeof(setting), &attenuator_index);
	if (rc == -EINVAL) {
		return coo_cmd_reply(cmd, RESP_ERROR,
					  "{\"error\":\"Failed to parse laser/setting\"}");
	}
	if (rc == -ENOENT) {
		return coo_cmd_reply(cmd, RESP_ERROR, "{\"error\":\"Invalid attenuator\"}");
	}
	if (rc != 0) {
		return coo_cmd_reply(cmd, RESP_ERROR,
					  "{\"error\":\"Attenuator unavailable on this board\"}");
	}

	if (strcasecmp(setting, "coeff") == 0) {
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
			return coo_cmd_reply(cmd, RESP_ERROR,
						  "{\"error\":\"Improper arguments\"}");
		}

		parse_rc = coo_json_extract_double_array(cmd->payload, "dac2",
							 dac2_coeffs,
							 ATTENUATOR_COEFF_COUNT,
							 &dac2_len);
		if (parse_rc != COO_JSON_EXTRACT_OK || dac2_len != ATTENUATOR_COEFF_COUNT) {
			return coo_cmd_reply(cmd, RESP_ERROR,
						  "{\"error\":\"Improper arguments\"}");
		}

		parse_rc = coo_json_extract_bool(cmd->payload, "persistent", &persist);
		if (parse_rc == COO_JSON_EXTRACT_ERR) {
			return coo_cmd_reply(cmd, RESP_ERROR,
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
			return coo_cmd_reply(cmd, RESP_ERROR,
						  "{\"error\":\"Failed to apply coefficients\"}");
		}
		app_settings_update_attenuator_channel(attenuator_index,
						       &stored_coeffs,
						       persist);
	} else if (strcasecmp(setting, "value") == 0 ||
		   strcasecmp(setting, "valuedb") == 0) {
		double value;

		if (coo_json_extract_double(cmd->payload, "value", &value) !=
		    COO_JSON_EXTRACT_OK) {
			return coo_cmd_reply(cmd, RESP_ERROR,
						  "{\"error\":\"Missing setting value\"}");
		}

		if (strcasecmp(setting, "value") == 0) {
			if (!attenuator_set_linear(&attenuators[attenuator_index], value)) {
				return coo_cmd_reply(cmd, RESP_ERROR,
							  "{\"error\":\"Invalid linear transmission\"}");
			}
		} else {
			if (!attenuator_set_db(&attenuators[attenuator_index], value)) {
				return coo_cmd_reply(cmd, RESP_ERROR,
							  "{\"error\":\"Invalid dB attenuation\"}");
			}
		}
	} else {
		return coo_cmd_reply(cmd, RESP_ERROR, "{\"error\":\"Invalid setting\"}");
	}

	throughput_monitor_note_attenuator_changed(attenuator_index);

	return coo_cmd_ok(cmd);
}
