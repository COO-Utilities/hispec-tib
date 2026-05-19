/*
 * Copyright (c) 2026 Caltech Optical Observatories
 * SPDX-License-Identifier: Apache-2.0
 */

#include "throughput_command.h"

#include <zephyr/sys/util.h>

#include "throughput_monitor.h"

#include <coo_commons/json_utils.h>

LOG_MODULE_DECLARE(throughput_monitor, LOG_LEVEL_INF);

enum throughput_format {
	THROUGHPUT_FORMAT_JSON = 0,
	THROUGHPUT_FORMAT_BINARY,
};

static const struct coo_json_string_choice stop_choices[] = {
	{ "yj", PHOTODIODE_CHANNEL_YJ },
	{ "hk", PHOTODIODE_CHANNEL_HK },
	{ "all", PHOTODIODE_CHANNEL_COUNT },
};

static const struct coo_json_string_choice fiber_choices[] = {
	{ "m", 'M' },
	{ "s", 'S' },
};

static const struct coo_json_string_choice format_choices[] = {
	{ "json", THROUGHPUT_FORMAT_JSON },
	{ "binary", THROUGHPUT_FORMAT_BINARY },
};

struct coo_cmd_response measure_throughput_set(const struct coo_cmd_request *cmd)
{
	char stop[8] = {0};
	char laser_name[16] = {0};
	struct throughput_monitor_request request = {0};
	struct throughput_monitor_status status = {0};
	uint32_t stopafter_s = 0U;
	bool autolevel = true;
	int choice_value;
	int parse_rc;
	int rc;

	parse_rc = coo_json_extract_string(cmd->payload, "stop", stop, sizeof(stop));
	if (parse_rc == COO_JSON_EXTRACT_OK) {
		if (coo_json_match_string_choice(stop, stop_choices,
						 ARRAY_SIZE(stop_choices),
						 &choice_value) != 0) {
			return coo_cmd_error(cmd, "stop must be yj, hk, or all");
		}

		rc = throughput_monitor_stop((uint8_t)choice_value, &status);
		if (rc != 0) {
			return coo_cmd_error(cmd, "stop failed");
		}

		return coo_cmd_ok(cmd);
	}
	if (parse_rc == COO_JSON_EXTRACT_ERR) {
		return coo_cmd_error(cmd, "invalid stop");
	}

	parse_rc = coo_json_extract_string(cmd->payload, "laser",
					   laser_name, sizeof(laser_name));
	if (parse_rc != COO_JSON_EXTRACT_OK ||
	    hispec_laser_id_from_name(laser_name, &request.laser) != 0) {
		return coo_cmd_error(cmd, "missing or invalid laser");
	}

	parse_rc = coo_json_extract_string_choice(cmd->payload, "fiber",
						  fiber_choices,
						  ARRAY_SIZE(fiber_choices),
						  &choice_value);
	if (parse_rc == COO_JSON_EXTRACT_ERR) {
		return coo_cmd_error(cmd, "fiber must be M or S");
	}
	if (parse_rc == COO_JSON_EXTRACT_OK) {
		request.fiber = (char)choice_value;
	} else {
		request.fiber = 'M';
	}

	if (coo_json_extract_optional_bool(cmd->payload, "autolevel",
					   &autolevel, NULL) != 0) {
		return coo_cmd_error(cmd, "invalid autolevel");
	}

	if (coo_json_extract_optional_u32(cmd->payload, "stopafter_s",
					  &stopafter_s, NULL) != 0) {
		return coo_cmd_error(cmd, "invalid stopafter_s");
	}

	parse_rc = coo_json_extract_string_choice(cmd->payload, "format",
						  format_choices,
						  ARRAY_SIZE(format_choices),
						  &choice_value);
	if (parse_rc == COO_JSON_EXTRACT_ERR) {
		return coo_cmd_error(cmd, "format must be json or binary");
	}
	request.binary = parse_rc == COO_JSON_EXTRACT_OK &&
			 choice_value == THROUGHPUT_FORMAT_BINARY;

	request.autolevel = autolevel;
	request.stopafter_s = stopafter_s;

	rc = throughput_monitor_start(&request, &status);
	if (rc != 0) {
		LOG_ERR("measure_throughput start failed: %d", rc);
		return coo_cmd_error(cmd, "measure_throughput start failed");
	}

	return coo_cmd_ok(cmd);
}
