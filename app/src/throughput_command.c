/*
 * Copyright (c) 2026 Caltech Optical Observatories
 * SPDX-License-Identifier: Apache-2.0
 */

#include "throughput_command.h"

#include <ctype.h>
#include <strings.h>

#include "devices.h"
#include "throughput_monitor.h"

#include <coo_commons/json_utils.h>

LOG_MODULE_DECLARE(throughput_monitor, LOG_LEVEL_INF);

struct coo_cmd_response measure_throughput_set(const struct coo_cmd_request *cmd)
{
	char stop[8] = {0};
	char laser_name[16] = {0};
	char fiber_text[4] = "M";
	char format[8] = "json";
	struct throughput_monitor_request request = {0};
	struct throughput_monitor_status status = {0};
	uint32_t stopafter_s = 0U;
	bool autolevel = true;
	int parse_rc;
	int rc;

	if (devices_board_type() != HISPEC_BOARD_TIB) {
		return coo_cmd_error(cmd, "measure_throughput unavailable on this board");
	}

	parse_rc = coo_json_extract_string(cmd->payload, "stop", stop, sizeof(stop));
	if (parse_rc == COO_JSON_EXTRACT_OK) {
		uint8_t channel;

		if (strcasecmp(stop, "all") == 0) {
			rc = throughput_monitor_stop(PHOTODIODE_CHANNEL_COUNT, &status);
			return rc == 0 ? coo_cmd_ok(cmd) :
				coo_cmd_error(cmd, "stop failed");
		}

		if (strcasecmp(stop, "yj") == 0) {
			channel = PHOTODIODE_CHANNEL_YJ;
		} else if (strcasecmp(stop, "hk") == 0) {
			channel = PHOTODIODE_CHANNEL_HK;
		} else {
			return coo_cmd_error(cmd, "stop must be yj, hk, or all");
		}

		rc = throughput_monitor_stop(channel, &status);
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

	parse_rc = coo_json_extract_string(cmd->payload, "fiber",
					   fiber_text, sizeof(fiber_text));
	if (parse_rc == COO_JSON_EXTRACT_ERR || fiber_text[0] == '\0' ||
	    fiber_text[1] != '\0') {
		return coo_cmd_error(cmd, "fiber must be M or S");
	}
	fiber_text[0] = (char)toupper((unsigned char)fiber_text[0]);
	if (fiber_text[0] != 'M' && fiber_text[0] != 'S') {
		return coo_cmd_error(cmd, "fiber must be M or S");
	}

	parse_rc = coo_json_extract_bool(cmd->payload, "autolevel", &autolevel);
	if (parse_rc == COO_JSON_EXTRACT_ERR) {
		return coo_cmd_error(cmd, "invalid autolevel");
	}

	parse_rc = coo_json_extract_u32(cmd->payload, "stopafter_s", &stopafter_s);
	if (parse_rc == COO_JSON_EXTRACT_ERR) {
		return coo_cmd_error(cmd, "invalid stopafter_s");
	}

	parse_rc = coo_json_extract_string(cmd->payload, "format",
					   format, sizeof(format));
	if (parse_rc == COO_JSON_EXTRACT_ERR ||
	    (strcasecmp(format, "json") != 0 && strcasecmp(format, "binary") != 0)) {
		return coo_cmd_error(cmd, "format must be json or binary");
	}

	request.autolevel = autolevel;
	request.binary = strcasecmp(format, "binary") == 0;
	request.fiber = fiber_text[0];
	request.stopafter_s = stopafter_s;

	rc = throughput_monitor_start(&request, &status);
	if (rc != 0) {
		LOG_ERR("measure_throughput start failed: %d", rc);
		return coo_cmd_error(cmd, "measure_throughput start failed");
	}

	return coo_cmd_ok(cmd);
}
