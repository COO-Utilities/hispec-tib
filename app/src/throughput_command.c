/*
 * Copyright (c) 2026 Caltech Optical Observatories
 * SPDX-License-Identifier: Apache-2.0
 */

#include "throughput_command.h"

#include <errno.h>
#include <math.h>
#include <string.h>

#include <zephyr/sys/util.h>

#include "app_settings.h"
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

static const struct coo_json_string_choice channel_choices[] = {
	{ "yj", PHOTODIODE_CHANNEL_YJ },
	{ "hk", PHOTODIODE_CHANNEL_HK },
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

	if (snprintk(out, out_len, "%s", input) >= (int)out_len) {
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

int measure_throughput_set(const struct coo_cmd_request *cmd, struct coo_cmd_response *out)
{
	char stop[8] = {0};
	char laser_name[16] = {0};
	char input[MEMS_SOURCEDEST_MAX_LEN] = {0};
	char output[MEMS_SOURCEDEST_MAX_LEN] = {0};
	struct throughput_monitor_request request = {.initial_level = THROUGHPUT_DEFAULT_INITIAL_LEVEL};
	struct throughput_monitor_status status = {0};
	uint32_t off_in_s = 0U;
	bool autolevel = true;
	bool max_flux_present = false;
	bool initial_level_present = false;
	int choice_value;
	int parse_rc;
	int rc;

	parse_rc = coo_json_extract_string(cmd->payload, "stop", stop, sizeof(stop));
	if (parse_rc == COO_JSON_EXTRACT_OK) {
		/* Stop takes priority over accompanying start options. */
		if (coo_json_match_string_choice(stop, stop_choices,
						 ARRAY_SIZE(stop_choices),
						 &choice_value) != 0) {
			return coo_cmd_error(out, cmd, "stop must be yj, hk, or all");
		}

		rc = throughput_monitor_stop((uint8_t)choice_value, &status);
		if (rc != 0) {
			return coo_cmd_error(out, cmd,
					     "stream stopped; laser shutdown failed, retry stop");
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
	if (coo_json_extract_optional_double_range(cmd->payload, "initial_level",
		&request.initial_level, &initial_level_present, 0.0, 1.0) != 0) {
		return coo_cmd_error(out, cmd, "initial_level must be 0..1");
	}
	if (initial_level_present && !autolevel) {
		return coo_cmd_error(out, cmd, "initial_level requires autolevel");
	}

	if (coo_json_extract_optional_u32(cmd->payload, "off_in_s",
					  &off_in_s, NULL) != 0) {
		return coo_cmd_error(out, cmd, "invalid off_in_s");
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
	request.off_in_s = off_in_s;

	parse_rc = coo_json_extract_string_choice(cmd->payload, "channel", channel_choices,
		ARRAY_SIZE(channel_choices), &choice_value);
	if (parse_rc == COO_JSON_EXTRACT_ERR) {
		return coo_cmd_error(out, cmd, "channel must be yj or hk");
	}
	if (request.has_laser) {
		/* Infer the PD from the laser's canonical launch input, not an override. */
		(void)throughput_input_for_laser(request.laser, input, sizeof(input));
		(void)throughput_channel_from_input(input, &request.channel);
		if (parse_rc == COO_JSON_EXTRACT_OK && choice_value != (int)request.channel) {
			return coo_cmd_error(out, cmd, "channel does not match laser");
		}
	} else {
		if (autolevel || parse_rc != COO_JSON_EXTRACT_OK) {
			return coo_cmd_error(out, cmd, "laser none requires channel and autolevel false");
		}
		request.channel = (enum photodiode_channel)choice_value;
	}
	if (max_flux_present && !autolevel) {
		return coo_cmd_error(out, cmd, "max_flux_ph_s requires autolevel");
	}

	/* Optional input replaces the inferred launch input only when present. */
	char requested_input[MEMS_SOURCEDEST_MAX_LEN] = {0};
	parse_rc = coo_json_extract_string(cmd->payload, "input", requested_input, sizeof(requested_input));
	if (parse_rc == COO_JSON_EXTRACT_ERR ||
	    (parse_rc == COO_JSON_EXTRACT_OK && requested_input[0] == '\0')) {
		return coo_cmd_error(out, cmd, "invalid input");
	}
	if (parse_rc == COO_JSON_EXTRACT_OK) {
		memcpy(input, requested_input, sizeof(input));
	}
	parse_rc = coo_json_extract_string(cmd->payload, "output", output, sizeof(output));
	if (parse_rc == COO_JSON_EXTRACT_ERR ||
	    (parse_rc == COO_JSON_EXTRACT_OK && output[0] == '\0')) {
		return coo_cmd_error(out, cmd, "invalid output");
	}
	if (request.has_laser && output[0] == '\0') {
		return coo_cmd_error(out, cmd, "laser measurement requires output");
	}
	if ((input[0] == '\0') != (output[0] == '\0')) {
		return coo_cmd_error(out, cmd, "passive launch requires both input and output");
	}

	const char *prefix = photodiode_channel_names[request.channel];
	char return_input[MEMS_SOURCEDEST_MAX_LEN], return_output[MEMS_SOURCEDEST_MAX_LEN];
	snprintk(return_input, sizeof(return_input), "%s_%s", prefix, request.fiber == 'M' ? "mm" : "sm");
	snprintk(return_output, sizeof(return_output), "%s_pd", prefix);
	const struct mems_route *return_route = mems_router_get_route(&router, return_input, return_output);
	const struct mems_route *launch_route = NULL;
	if (input[0] != '\0') {
		enum photodiode_channel route_channel;
		if (throughput_channel_from_input(input, &route_channel) != 0 || route_channel != request.channel ||
		    throughput_channel_from_input(output, &route_channel) != 0 || route_channel != request.channel ||
		    (strcmp(output, "yj_ao") != 0 && strcmp(output, "yj_fei") != 0 &&
		     strcmp(output, "hk_ao") != 0 && strcmp(output, "hk_fei") != 0)) {
			return coo_cmd_error(out, cmd, "launch must route to this channel's ao or fei");
		}
		launch_route = mems_router_get_route(&router, input, output);
		if (launch_route == NULL) {
			return coo_cmd_error(out, cmd, "unknown launch route");
		}
	}
	if (return_route == NULL) {
		return coo_cmd_error(out, cmd, "unknown photodiode return route");
	}

	/* Both paths are validated before disturbing an existing measurement. */
	char route[APP_ROUTE_LOSS_ROUTE_MAX_LEN];
	const char *source = request.has_laser ? laser_name : NULL;
	if (snprintk(route, sizeof(route), "%s_to_%s", return_route->key.input_name,
		return_route->key.output_name) >= (int)sizeof(route)) {
		return coo_cmd_error(out, cmd, "return route key too long");
	}
	(void)app_settings_get_route_loss(route, source, &request.pd_route_tx);
	request.laser_route_tx = NAN;
	if (request.has_laser) {
		if (snprintk(route, sizeof(route), "%s_to_%s", launch_route->key.input_name,
			launch_route->key.output_name) >= (int)sizeof(route)) {
			return coo_cmd_error(out, cmd, "launch route key too long");
		}
		(void)app_settings_get_route_loss(route, source, &request.laser_route_tx);
	}
	rc = throughput_monitor_prepare_start(&request);
	if (rc != 0) {
		goto start_error;
	}

	/* Launch and PD return use independent MEMS switches. Passive capture may
	 * leave launch untouched, but always selects the requested MM/SM return.
	 */
	const char *failed_switch = NULL;
	char failed_state = '\0';
	if (launch_route != NULL) {
		rc = mems_router_apply_route(&router, launch_route, false, &failed_switch, &failed_state);
	}
	if (rc == 0) {
		rc = mems_router_apply_route(&router, return_route, false, &failed_switch, &failed_state);
	}
	if (rc != 0) {
		LOG_ERR("measure_throughput route failed at %s:%c (%d)",
			failed_switch != NULL ? failed_switch : "unknown", failed_state, rc);
		if (throughput_monitor_stop(request.channel, NULL) != 0) {
			return coo_cmd_error(out, cmd, "route failed; stream stopped; laser shutdown failed, retry stop");
		}
		return coo_cmd_error(out, cmd, "route setup failed; stream stopped; MEMS may be partially changed");
	}
	rc = throughput_monitor_start(&request, &status);
	if (rc != 0 && throughput_monitor_stop(request.channel, NULL) != 0) {
		return coo_cmd_error(out, cmd, "start failed; stream stopped; laser shutdown failed, retry stop");
	}
start_error:
	if (rc != 0) {
		LOG_ERR("measure_throughput start failed: %d", rc);
		if (rc == -EACCES) {
			return coo_cmd_error(out, cmd, "photodiode power override_off");
		}
		if (rc == -EBUSY) {
			return coo_cmd_error(out, cmd, "throughput start busy: dark/calibration or autolevel owner");
		}
		return coo_cmd_error(out, cmd, "measure_throughput start failed");
	}

	return coo_cmd_ok(out, cmd);
}
