/*
 * Copyright (c) 2026 Caltech Optical Observatories
 * SPDX-License-Identifier: Apache-2.0
 */

#include "photodiode_command.h"

#include <errno.h>
#include <string.h>
#include <strings.h>

#include "app_settings.h"
#include "devices.h"
#include "photodiode.h"
#include "throughput_monitor.h"

#include <coo_commons/json_utils.h>

static int pd_parse_channel_name(const char *name, enum photodiode_channel *channel)
{
	if (name == NULL || channel == NULL) {
		return -EINVAL;
	}
	if (strcasecmp(name, "yj") == 0) {
		*channel = PHOTODIODE_CHANNEL_YJ;
		return 0;
	}
	if (strcasecmp(name, "hk") == 0) {
		*channel = PHOTODIODE_CHANNEL_HK;
		return 0;
	}

	return -ENOENT;
}

static int pd_parse_channel_from_key(const struct coo_cmd_request *cmd,
				     enum photodiode_channel *channel)
{
	const char *slash = strchr(cmd->key, '/');

	if (slash == NULL || slash[1] == '\0') {
		return -ENOENT;
	}

	return pd_parse_channel_name(slash + 1, channel);
}

static int pd_parse_channel_from_payload_or_key(const struct coo_cmd_request *cmd,
						enum photodiode_channel *channel)
{
	char channel_name[8] = {0};
	int parse_rc;

	parse_rc = pd_parse_channel_from_key(cmd, channel);
	if (parse_rc == 0) {
		return 0;
	}

	parse_rc = coo_json_extract_string(cmd->payload, "channel",
					   channel_name, sizeof(channel_name));
	if (parse_rc == COO_JSON_EXTRACT_MISSING) {
		return -ENOENT;
	}
	if (parse_rc == COO_JSON_EXTRACT_ERR) {
		return -EINVAL;
	}

	return pd_parse_channel_name(channel_name, channel);
}

struct coo_cmd_response pd_get(const struct coo_cmd_request *cmd)
{
	struct photodiode_status status;
	char payload[MAX_PAYLOAD_LEN] = {0};
	struct app_photodiode_settings settings;
	float yj_value;
	float hk_value;
	float yj_err;
	float hk_err;

	if (devices_board_type() != HISPEC_BOARD_TIB) {
		return coo_cmd_error(cmd, "photodiodes unavailable on this board");
	}

	photodiode_get_status(&status);
	app_settings_get_photodiode(&settings);

	yj_value = status.channel[PHOTODIODE_CHANNEL_YJ].power_uw;
	hk_value = status.channel[PHOTODIODE_CHANNEL_HK].power_uw;
	yj_err = (float)photodiode_power_uw_from_mv(
		status.channel[PHOTODIODE_CHANNEL_YJ].noise_rms_mv,
		&settings.channel[PHOTODIODE_CHANNEL_YJ]);
	hk_err = (float)photodiode_power_uw_from_mv(
		status.channel[PHOTODIODE_CHANNEL_HK].noise_rms_mv,
		&settings.channel[PHOTODIODE_CHANNEL_HK]);

	snprintk(payload, sizeof(payload),
		 "{\"yjvalue\":%.6f,\"yjvalue_err\":%.6f,"
		 "\"hkvalue\":%.6f,\"hkvalue_err\":%.6f,"
		 "\"yj_raw\":%d,\"hk_raw\":%d,\"yj_mv\":%.3f,\"hk_mv\":%.3f,"
		 "\"yj_noise_rms_mv\":%.3f,\"hk_noise_rms_mv\":%.3f,"
		 "\"yj_mean_mv_1s\":%.3f,\"hk_mean_mv_1s\":%.3f,"
		 "\"yj_rms_mv_0p5s\":%.3f,\"hk_rms_mv_0p5s\":%.3f,"
		 "\"uptime\":%lld}",
		 (double)yj_value,
		 (double)yj_err,
		 (double)hk_value,
		 (double)hk_err,
		 status.channel[PHOTODIODE_CHANNEL_YJ].raw,
		 status.channel[PHOTODIODE_CHANNEL_HK].raw,
		 (double)status.channel[PHOTODIODE_CHANNEL_YJ].mv,
		 (double)status.channel[PHOTODIODE_CHANNEL_HK].mv,
		 (double)status.channel[PHOTODIODE_CHANNEL_YJ].noise_rms_mv,
		 (double)status.channel[PHOTODIODE_CHANNEL_HK].noise_rms_mv,
		 (double)status.channel[PHOTODIODE_CHANNEL_YJ].mean_mv_1s,
		 (double)status.channel[PHOTODIODE_CHANNEL_HK].mean_mv_1s,
		 (double)status.channel[PHOTODIODE_CHANNEL_YJ].rms_mv_0p5s,
		 (double)status.channel[PHOTODIODE_CHANNEL_HK].rms_mv_0p5s,
		 status.uptime_ms);
	return coo_cmd_reply(cmd, COO_CMD_RESP_OK, payload);
}

static struct coo_cmd_response pd_dark_status_response(const struct coo_cmd_request *cmd,
					     const struct photodiode_dark_status *status)
{
	char payload[MAX_PAYLOAD_LEN] = {0};
	const struct photodiode_dark_result *result = &status->result;
	const char *state_name = photodiode_dark_state_name(status->state);

	if (status->state == PHOTODIODE_DARK_COMPLETE) {
		snprintk(payload, sizeof(payload),
			 "{\"state\":\"%s\",\"channel\":\"%s\",\"stored\":%s,"
			 "\"duration_ms\":%u,\"samples\":%u,\"target_samples\":%u,"
			 "\"mean_dark_mv\":%.3f,\"rms_mv\":%.3f,"
			 "\"min_mv\":%.3f,\"max_mv\":%.3f,"
			 "\"previous_dark_mv\":%.3f,\"configured_dark_mv\":%.3f,"
			 "\"lowest_dark_mv\":%.3f,\"lowest_dark_valid\":%s}",
			 state_name,
			 photodiode_channel_names[status->channel],
			 result->stored ? "true" : "false",
			 status->duration_ms,
			 status->samples,
			 status->target_samples,
			 (double)result->mean_mv,
			 (double)result->rms_mv,
			 (double)result->min_mv,
			 (double)result->max_mv,
			 (double)result->previous_dark_mv,
			 (double)result->configured_dark_mv,
			 (double)result->lowest_dark_mv,
			 result->lowest_dark_valid ? "true" : "false");
		return coo_cmd_reply(cmd, COO_CMD_RESP_OK, payload);
	}

	if (status->state == PHOTODIODE_DARK_ERROR) {
		snprintk(payload, sizeof(payload),
			 "{\"error\":\"dark measurement failed\",\"channel\":\"%s\",\"rc\":%d,"
			 "\"duration_ms\":%u,\"samples\":%u,\"target_samples\":%u}",
			 photodiode_channel_names[status->channel],
			 status->last_error,
			 status->duration_ms,
			 status->samples,
			 status->target_samples);
		return coo_cmd_reply(cmd, COO_CMD_RESP_ERROR, payload);
	}

	snprintk(payload, sizeof(payload),
		 "{\"state\":\"%s\",\"channel\":\"%s\",\"stored_on_complete\":%s,"
		 "\"duration_ms\":%u,\"samples\":%u,\"target_samples\":%u}",
		 state_name,
		 photodiode_channel_names[status->channel],
		 status->store ? "true" : "false",
		 status->duration_ms,
		 status->samples,
		 status->target_samples);
	return coo_cmd_reply(cmd, COO_CMD_RESP_OK, payload);
}

struct coo_cmd_response pd_set(const struct coo_cmd_request *cmd)
{
	char action[32] = {0};
	enum photodiode_channel channel;
	uint32_t duration_ms = 0U;
	bool store = false;
	bool persist = true;
	int parse_rc;
	int rc;

	if (devices_board_type() != HISPEC_BOARD_TIB) {
		return coo_cmd_error(cmd, "photodiodes unavailable on this board");
	}

	parse_rc = coo_json_extract_string(cmd->payload, "action", action, sizeof(action));
	if (parse_rc == COO_JSON_EXTRACT_MISSING) {
		return coo_cmd_error(cmd, "missing action");
	}
	if (parse_rc == COO_JSON_EXTRACT_ERR) {
		return coo_cmd_error(cmd, "invalid action");
	}

	rc = pd_parse_channel_from_payload_or_key(cmd, &channel);
	if (rc != 0) {
		return coo_cmd_error(cmd, "channel must be yj or hk");
	}

	if (strcasecmp(action, "measure_dark") == 0) {
		struct photodiode_dark_status status;

		if (throughput_monitor_autolevel_active(channel)) {
			return coo_cmd_error(cmd,
					    "dark measurement blocked by autolevel throughput monitor");
		}

		if (coo_json_extract_optional_u32(cmd->payload, "duration_ms",
						  &duration_ms, NULL) != 0) {
			return coo_cmd_error(cmd, "invalid duration_ms");
		}

		if (coo_json_extract_optional_bool(cmd->payload, "store",
						   &store, NULL) != 0) {
			return coo_cmd_error(cmd, "invalid store");
		}

		rc = photodiode_start_dark_measurement(channel, duration_ms, store, &status);
		if (rc != 0) {
			return coo_cmd_error_rc(cmd, "dark measurement failed", rc);
		}
		return pd_dark_status_response(cmd, &status);
	}

	if (strcasecmp(action, "dark_status") == 0) {
		struct photodiode_dark_status status;

		rc = photodiode_get_dark_status(channel, &status);
		if (rc != 0) {
			return coo_cmd_error(cmd, "dark status unavailable");
		}

		return pd_dark_status_response(cmd, &status);
	}

	if (strcasecmp(action, "reset_lowest_dark") == 0) {
		if (coo_json_extract_optional_bool(cmd->payload, "persistent",
						   &persist, NULL) != 0) {
			return coo_cmd_error(cmd, "invalid persistent");
		}

		rc = photodiode_reset_lowest_dark(channel, persist);
		if (rc != 0) {
			return coo_cmd_error(cmd, "reset failed");
		}
		return coo_cmd_ok(cmd);
	}

	return coo_cmd_error(cmd, "unknown action");
}

static int pd_settings_channel_json(char *payload, size_t payload_len,
				    enum photodiode_channel channel,
				    const struct app_pd_channel_settings *ch)
{
	struct photodiode_dark_status dark = {0};
	int written;

	(void)photodiode_get_dark_status(channel, &dark);

	written = snprintk(payload, payload_len,
			   "{\"channel\":\"%s\",\"dark_mv\":%.3f,"
			   "\"lowest_dark_mv\":%.3f,"
			   "\"lowest_dark_valid\":%s,"
			   "\"dark_measurement\":\"%s\","
			   "\"dark_measurement_duration_ms\":%u,"
			   "\"dark_measurement_samples\":%u,"
			   "\"dark_measurement_target_samples\":%u,"
			   "\"noise_rms_mV\":%.3f,"
			   "\"responsivity_a_per_w\":%.9f,"
			   "\"transimpedance_v_per_a\":%.6e}",
			   photodiode_channel_names[channel],
			   (double)ch->dark_mv,
			   (double)ch->lowest_dark_mv,
			   ch->lowest_dark_valid ? "true" : "false",
			   photodiode_dark_state_name(dark.state),
			   dark.duration_ms,
			   dark.samples,
			   dark.target_samples,
			   (double)ch->noise_warn_rms_mv,
			   ch->responsivity_a_per_w,
			   ch->transimpedance_v_per_a);

	return (written >= 0 && written < (int)payload_len) ? 0 : -ENOSPC;
}

struct coo_cmd_response pd_settings_get(const struct coo_cmd_request *cmd)
{
	struct app_photodiode_settings settings;
	char payload[MAX_PAYLOAD_LEN] = {0};
	enum photodiode_channel channel;
	int rc;

	if (devices_board_type() != HISPEC_BOARD_TIB) {
		return coo_cmd_error(cmd, "photodiodes unavailable on this board");
	}

	rc = pd_parse_channel_from_key(cmd, &channel);
	if (rc != 0) {
		return coo_cmd_error(cmd, "pdsettings key must be pdsettings/yj or pdsettings/hk");
	}

	app_settings_get_photodiode(&settings);
	rc = pd_settings_channel_json(payload, sizeof(payload), channel,
				      &settings.channel[channel]);
	if (rc != 0) {
		return coo_cmd_error(cmd, "pdsettings response too large");
	}

	return coo_cmd_reply(cmd, COO_CMD_RESP_OK, payload);
}

struct coo_cmd_response pd_settings_set(const struct coo_cmd_request *cmd)
{
	struct app_photodiode_settings settings;
	struct app_pd_channel_settings channel_settings;
	enum photodiode_channel channel;
	bool persist = false;
	bool changed = false;
	int rc;

	if (devices_board_type() != HISPEC_BOARD_TIB) {
		return coo_cmd_error(cmd, "photodiodes unavailable on this board");
	}

	rc = pd_parse_channel_from_key(cmd, &channel);
	if (rc != 0) {
		return coo_cmd_error(cmd, "pdsettings key must be pdsettings/yj or pdsettings/hk");
	}

	app_settings_get_photodiode(&settings);
	channel_settings = settings.channel[channel];

	if (coo_json_extract_optional_bool(cmd->payload, "persistent",
					   &persist, NULL) != 0) {
		return coo_cmd_error(cmd, "invalid persistent");
	}

	if (coo_json_extract_optional_float_range(cmd->payload, "dark_mv",
						  &channel_settings.dark_mv,
						  &changed, -5000.0f, 5000.0f) != 0 ||
	    coo_json_extract_optional_float_range(cmd->payload, "noise_rms_mV",
						  &channel_settings.noise_warn_rms_mv,
						  &changed, 0.0f, 5000.0f) != 0 ||
	    coo_json_extract_optional_double_range(cmd->payload, "responsivity_a_per_w",
						   &channel_settings.responsivity_a_per_w,
						   &changed, 0.000001, 10.0) != 0 ||
	    coo_json_extract_optional_double_range(cmd->payload, "transimpedance_v_per_a",
						   &channel_settings.transimpedance_v_per_a,
						   &changed, 1.0, 1.0e12) != 0) {
		return coo_cmd_error(cmd, "invalid pdsettings value");
	}

	if (!changed) {
		return coo_cmd_error(cmd, "no pdsettings fields supplied");
	}

	app_settings_update_photodiode_channel((uint8_t)channel,
					       &channel_settings,
					       persist);
	return coo_cmd_ok(cmd);
}
