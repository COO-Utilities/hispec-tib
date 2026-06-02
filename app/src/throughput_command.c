/*
 * Copyright (c) 2026 Caltech Optical Observatories
 * SPDX-License-Identifier: Apache-2.0
 */

#include "throughput_command.h"

#include <errno.h>
#include <string.h>

#include <zephyr/sys/util.h>

#include "devices.h"
#include "mems_switching.h"
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

static int throughput_input_for_laser(enum hispec_laser_id laser,
				      char *out, size_t out_len)
{
	const char *input;

	if (out == NULL || out_len == 0U) {
		return -EINVAL;
	}

	switch (laser) {
	case HISPEC_LASER_1430_YJ:
		input = "yj_1430";
		break;
	case HISPEC_LASER_1430_HK:
		input = "hk_1430";
		break;
	case HISPEC_LASER_1028_Y:
	case HISPEC_LASER_1270_J:
		input = "yj_laser";
		break;
	case HISPEC_LASER_1510_H:
	case HISPEC_LASER_2330_K:
		input = "hk_laser";
		break;
	default:
		return -EINVAL;
	}

	if (snprintk(out, out_len, "%s", input) >= out_len) {
		return -ENOSPC;
	}
	return 0;
}

static int throughput_channel_from_input(const char *input,
					 enum photodiode_channel *channel)
{
	if (input == NULL || channel == NULL) {
		return -EINVAL;
	}
	if (strncmp(input, "yj_", 3) == 0 || strcmp(input, "yj") == 0) {
		*channel = PHOTODIODE_CHANNEL_YJ;
		return 0;
	}
	if (strncmp(input, "hk_", 3) == 0 || strcmp(input, "hk") == 0) {
		*channel = PHOTODIODE_CHANNEL_HK;
		return 0;
	}
	return -EINVAL;
}

static int throughput_apply_route_if_requested(const char *input,
					       const char *output)
{
	const char *failed_switch = NULL;
	char failed_state = '\0';

	if (output == NULL || output[0] == '\0') {
		return 0;
	}
	if (input == NULL || input[0] == '\0') {
		return -EINVAL;
	}

	return mems_router_apply_named_route(&router, input, output,
					     &failed_switch, &failed_state);
}

int measure_throughput_set(const struct coo_cmd_request *cmd, struct coo_cmd_response *out)
{
	char stop[8] = {0};
	char laser_name[16] = {0};
	char input[MEMS_SOURCEDEST_MAX_LEN] = {0};
	char output[MEMS_SOURCEDEST_MAX_LEN] = {0};
	struct throughput_monitor_request request = {0};
	struct throughput_monitor_status status = {0};
	uint32_t stopafter_s = 0U;
	bool autolevel = true;
	bool max_flux_present = false;
	int choice_value;
	int parse_rc;
	int rc;

	parse_rc = coo_json_extract_string(cmd->payload, "stop", stop, sizeof(stop));
	if (parse_rc == COO_JSON_EXTRACT_OK) {
		if (coo_json_match_string_choice(stop, stop_choices,
						 ARRAY_SIZE(stop_choices),
						 &choice_value) != 0) {
			return coo_cmd_error(out, cmd, "stop must be yj, hk, or all");
		}

		rc = throughput_monitor_stop((uint8_t)choice_value, &status);
		if (rc != 0) {
			return coo_cmd_error(out, cmd, "stop failed");
		}

		return coo_cmd_ok(out, cmd);
	}
	if (parse_rc == COO_JSON_EXTRACT_ERR) {
		return coo_cmd_error(out, cmd, "invalid stop");
	}

	parse_rc = coo_json_extract_string(cmd->payload, "laser",
					   laser_name, sizeof(laser_name));
	if (parse_rc != COO_JSON_EXTRACT_OK) {
		return coo_cmd_error(out, cmd, "missing or invalid laser");
	}
	if (strcmp(laser_name, "none") == 0) {
		request.has_laser = false;
		request.laser = HISPEC_LASER_UNKNOWN;
	} else if (hispec_laser_id_from_name(laser_name, &request.laser) == 0) {
		request.has_laser = true;
	} else {
		return coo_cmd_error(out, cmd, "missing or invalid laser");
	}

	parse_rc = coo_json_extract_string_choice(cmd->payload, "fiber",
						  fiber_choices,
						  ARRAY_SIZE(fiber_choices),
						  &choice_value);
	if (parse_rc == COO_JSON_EXTRACT_ERR) {
		return coo_cmd_error(out, cmd, "fiber must be M or S");
	}
	if (parse_rc == COO_JSON_EXTRACT_OK) {
		request.fiber = (char)choice_value;
	} else {
		request.fiber = 'M';
	}

	if (coo_json_extract_optional_bool(cmd->payload, "autolevel",
					   &autolevel, NULL) != 0) {
		return coo_cmd_error(out, cmd, "invalid autolevel");
	}

	if (coo_json_extract_optional_u32(cmd->payload, "stopafter_s",
					  &stopafter_s, NULL) != 0) {
		return coo_cmd_error(out, cmd, "invalid stopafter_s");
	}

	if (coo_json_extract_optional_double_range(cmd->payload, "max_flux_ph_s",
						   &request.max_flux_ph_s,
						   &max_flux_present,
						   0.0, 1.0e30) != 0) {
		return coo_cmd_error(out, cmd, "invalid max_flux_ph_s");
	}

	parse_rc = coo_json_extract_string_choice(cmd->payload, "format",
						  format_choices,
						  ARRAY_SIZE(format_choices),
						  &choice_value);
	if (parse_rc == COO_JSON_EXTRACT_ERR) {
		return coo_cmd_error(out, cmd, "format must be json or binary");
	}
	request.binary = parse_rc == COO_JSON_EXTRACT_OK &&
			 choice_value == THROUGHPUT_FORMAT_BINARY;

	request.autolevel = autolevel;
	request.stopafter_s = stopafter_s;

	parse_rc = coo_json_extract_string(cmd->payload, "input", input, sizeof(input));
	if (parse_rc == COO_JSON_EXTRACT_ERR) {
		return coo_cmd_error(out, cmd, "invalid input");
	}
	if (parse_rc == COO_JSON_EXTRACT_MISSING && request.has_laser &&
	    throughput_input_for_laser(request.laser, input, sizeof(input)) != 0) {
		return coo_cmd_error(out, cmd, "invalid laser route");
	}

	parse_rc = coo_json_extract_string(cmd->payload, "output", output, sizeof(output));
	if (parse_rc == COO_JSON_EXTRACT_ERR) {
		return coo_cmd_error(out, cmd, "invalid output");
	}

	if (!request.has_laser) {
		if (autolevel || input[0] == '\0' || output[0] == '\0' ||
		    throughput_channel_from_input(input, &request.channel) != 0) {
			return coo_cmd_error(out, cmd, "laser none requires input, output, and autolevel false");
		}
	} else if (max_flux_present && !autolevel) {
		return coo_cmd_error(out, cmd, "max_flux_ph_s requires autolevel");
	}

	rc = throughput_apply_route_if_requested(input, output);
	if (rc != 0) {
		return coo_cmd_error(out, cmd, "failed to apply output route");
	}

	rc = throughput_monitor_start(&request, &status);
	if (rc != 0) {
		LOG_ERR("measure_throughput start failed: %d", rc);
		return coo_cmd_error(out, cmd, "measure_throughput start failed");
	}

	return coo_cmd_ok(out, cmd);
}
