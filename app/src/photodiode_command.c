/*
 * Copyright (c) 2026 Caltech Optical Observatories
 * SPDX-License-Identifier: Apache-2.0
 */

#include "photodiode_command.h"

#include <errno.h>
#include <math.h>

#include <zephyr/kernel.h>
#include <zephyr/sys/util.h>

#include "app_settings.h"
#include "housekeeping.h"
#include "photodiode.h"
#include "throughput_monitor.h"

#include <coo_commons/command_dispatch.h>
#include <coo_commons/json_utils.h>

enum pd_action {
	PD_ACTION_MEASURE_DARK = 0,
	PD_ACTION_DARK_STATUS,
	PD_ACTION_RESET_LOWEST_DARK,
};

static const struct coo_json_string_choice pd_channel_choices[] = {
	{ "yj", PHOTODIODE_CHANNEL_YJ },
	{ "hk", PHOTODIODE_CHANNEL_HK },
};

static const struct coo_json_string_choice pd_action_choices[] = {
	{ "measure_dark", PD_ACTION_MEASURE_DARK },
	{ "dark_status", PD_ACTION_DARK_STATUS },
	{ "reset_lowest_dark", PD_ACTION_RESET_LOWEST_DARK },
};

static const struct coo_json_string_choice pd_power_choices[] = {
	{ "auto", APP_PD_POWER_AUTO },
	{ "override_on", APP_PD_POWER_OVERRIDE_ON },
	{ "override_off", APP_PD_POWER_OVERRIDE_OFF },
};

static const char *pd_power_mode_name(enum app_pd_power_mode mode)
{
	switch (mode) {
	case APP_PD_POWER_AUTO:
		return "auto";
	case APP_PD_POWER_OVERRIDE_ON:
		return "override_on";
	case APP_PD_POWER_OVERRIDE_OFF:
		return "override_off";
	default:
		return "unknown";
	}
}

static enum housekeeping_power_output pd_power_output(enum photodiode_channel channel)
{
	return (enum housekeeping_power_output)channel;
}

static bool pd_channel_power_is_off(enum photodiode_channel channel)
{
	bool powered = false;

	return housekeeping_power_get(pd_power_output(channel), &powered) == 0 && !powered;
}

static int pd_apply_power_mode(enum photodiode_channel channel,
			       enum app_pd_power_mode mode)
{
	const enum housekeeping_power_output output = pd_power_output(channel);

	if (mode == APP_PD_POWER_OVERRIDE_ON) {
		housekeeping_photodiode_autooff_cancel(output);
		return housekeeping_power_set(output, true);
	}
	if (mode == APP_PD_POWER_OVERRIDE_OFF) {
		housekeeping_photodiode_autooff_cancel(output);
		return housekeeping_power_set(output, false);
	}

	return 0;
}

static int
pd_average_status_response(const struct coo_cmd_request *cmd,
			   const struct photodiode_average_status *status,
			   struct coo_cmd_response *out);

static int pd_parse_channel_name(const char *name, enum photodiode_channel *channel)
{
	int value;

	if (channel == NULL) {
		return -EINVAL;
	}

	if (coo_json_match_string_choice(name, pd_channel_choices,
					 ARRAY_SIZE(pd_channel_choices),
					 &value) == 0) {
		*channel = (enum photodiode_channel)value;
		return 0;
	}

	return -ENOENT;
}

static int pd_parse_channel_from_key(const struct coo_cmd_request *cmd,
				     enum photodiode_channel *channel)
{
	char channel_name[8] = {0};

	if (cmd == NULL ||
	    (coo_cmd_key_suffix_segment_copy(cmd->key, "pd", channel_name,
					     sizeof(channel_name)) != 0 &&
	     coo_cmd_key_suffix_segment_copy(cmd->key, "pdsettings", channel_name,
					     sizeof(channel_name)) != 0)) {
		return -ENOENT;
	}

	return pd_parse_channel_name(channel_name, channel);
}

static int pd_parse_channel_from_payload_or_key(const struct coo_cmd_request *cmd,
						enum photodiode_channel *channel)
{
	int value;
	int parse_rc;

	if (channel == NULL) {
		return -EINVAL;
	}

	parse_rc = pd_parse_channel_from_key(cmd, channel);
	if (parse_rc == 0) {
		return 0;
	}

	parse_rc = coo_json_extract_string_choice(cmd->payload, "channel",
						  pd_channel_choices,
						  ARRAY_SIZE(pd_channel_choices),
						  &value);
	if (parse_rc == COO_JSON_EXTRACT_MISSING) {
		return -ENOENT;
	}
	if (parse_rc == COO_JSON_EXTRACT_ERR) {
		return -EINVAL;
	}

	*channel = (enum photodiode_channel)value;
	return 0;
}

int pd_get(const struct coo_cmd_request *cmd, struct coo_cmd_response *out)
{
	struct photodiode_status status;
	char payload[MAX_PAYLOAD_LEN] = {0};
	struct app_photodiode_settings settings;
	char action_text[32] = {0};
	int action_value;
	double yj_value;
	double hk_value;
	double yj_err;
	double hk_err;
	double yj_ontime_s;
	double hk_ontime_s;
	int parse_rc;
	enum photodiode_channel channel;
	bool wait_for_power = false;
	bool yj_pd_is_off;
	bool hk_pd_is_off;
	size_t off = 0U;

	parse_rc = coo_json_extract_string(cmd->payload, "action",
					   action_text, sizeof(action_text));
	if (parse_rc == COO_JSON_EXTRACT_ERR) {
		return coo_cmd_error(out, cmd, "invalid action");
	}
	if (parse_rc == COO_JSON_EXTRACT_OK) {
		struct photodiode_average_status average_status;
		int rc;

		if (coo_json_match_string_choice(action_text, pd_action_choices,
						 ARRAY_SIZE(pd_action_choices),
						 &action_value) != 0 ||
		    (enum pd_action)action_value != PD_ACTION_DARK_STATUS) {
			return coo_cmd_error(out, cmd, "unsupported query action");
		}
		rc = pd_parse_channel_from_payload_or_key(cmd, &channel);
		if (rc != 0) {
			return coo_cmd_error(out, cmd, "channel must be yj or hk");
		}
		rc = photodiode_get_average_status(channel, &average_status);
		if (rc != 0) {
			return coo_cmd_error(out, cmd, "dark status unavailable");
		}

		return pd_average_status_response(cmd, &average_status, out);
	}

	app_settings_get_photodiode(&settings);
	for (uint8_t i = 0U; i < PHOTODIODE_CHANNEL_COUNT; ++i) {
		bool was_off = false;

		if (settings.channel[i].power != APP_PD_POWER_AUTO) {
			continue;
		}
		if (housekeeping_photodiode_auto_enable(pd_power_output((enum photodiode_channel)i),
							settings.channel[i].autooff_s,
							&was_off) == 0 && was_off) {
			wait_for_power = true;
		}
	}
	if (wait_for_power) {
		k_sleep(K_MSEC(500));
	}

	photodiode_get_status(&status);
	yj_pd_is_off = pd_channel_power_is_off(PHOTODIODE_CHANNEL_YJ);
	hk_pd_is_off = pd_channel_power_is_off(PHOTODIODE_CHANNEL_HK);
	yj_ontime_s = housekeeping_power_on_time_s(pd_power_output(PHOTODIODE_CHANNEL_YJ));
	hk_ontime_s = housekeeping_power_on_time_s(pd_power_output(PHOTODIODE_CHANNEL_HK));

	yj_value = status.channel[PHOTODIODE_CHANNEL_YJ].power_uw;
	hk_value = status.channel[PHOTODIODE_CHANNEL_HK].power_uw;
	yj_err = (double)photodiode_power_uw_from_mv(
		status.channel[PHOTODIODE_CHANNEL_YJ].noise_rms_mv,
		&settings.channel[PHOTODIODE_CHANNEL_YJ]);
	hk_err = (double)photodiode_power_uw_from_mv(
		status.channel[PHOTODIODE_CHANNEL_HK].noise_rms_mv,
		&settings.channel[PHOTODIODE_CHANNEL_HK]);

	if (coo_json_append(payload, sizeof(payload), &off,
			    "{\"yjvalue\":%.6f,\"yjvalue_err\":%.6f,"
			    "\"hkvalue\":%.6f,\"hkvalue_err\":%.6f,"
			    "\"yj_raw\":%d,\"hk_raw\":%d,\"yj_mv\":%.3f,\"hk_mv\":%.3f,"
			    "\"yj_noise_rms_mv\":%.3f,\"hk_noise_rms_mv\":%.3f,"
			    "\"yj_mean_mv_1s\":%.3f,\"hk_mean_mv_1s\":%.3f,"
			    "\"yj_rms_mv_0p5s\":%.3f,\"hk_rms_mv_0p5s\":%.3f,"
			    "\"yj_ontime_s\":%.3f,\"hk_ontime_s\":%.3f",
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
			    yj_ontime_s,
			    hk_ontime_s) != 0 ||
	    (yj_pd_is_off &&
	     coo_json_append(payload, sizeof(payload), &off,
			     ",\"yj_pd_is_off\":true") != 0) ||
	    (hk_pd_is_off &&
	     coo_json_append(payload, sizeof(payload), &off,
			     ",\"hk_pd_is_off\":true") != 0) ||
	    coo_json_append(payload, sizeof(payload), &off, "}") != 0) {
		return coo_cmd_error(out, cmd, "pd response too large");
	}
	return coo_cmd_reply(out, cmd, COO_CMD_RESP_OK, payload);
}

static int pd_average_status_response(const struct coo_cmd_request *cmd,
				      const struct photodiode_average_status *status,
				      struct coo_cmd_response *out)
{
	char payload[MAX_PAYLOAD_LEN] = {0};
	size_t off = 0U;
	const struct photodiode_average_result *result = &status->result;
	const char *state_name = photodiode_average_state_name(status->state);

	if (status->state == PHOTODIODE_AVERAGE_COMPLETE) {
		struct app_photodiode_settings settings;
		const struct app_pd_channel_settings *channel_settings;

		app_settings_get_photodiode(&settings);
		channel_settings = &settings.channel[status->channel];
		if (coo_json_append(payload, sizeof(payload), &off,
				    "{\"state\":\"%s\",\"channel\":\"%s\",\"stored\":%s,"
				    "\"duration_ms\":%u,\"samples\":%u,\"target_samples\":%u,"
				    "\"mean_dark_mv\":%.3f,\"rms_mv\":%.3f,"
				    "\"dark_noise_rms_mv\":%.3f,"
				    "\"min_mv\":%.3f,\"max_mv\":%.3f,"
				    "\"previous_dark_mv\":%.3f,\"configured_dark_mv\":%.3f,"
				    "\"lowest_stored_dark_mv\":",
				    state_name,
				    photodiode_channel_names[status->channel],
				    status->store_dark ? "true" : "false",
				    result->duration_ms,
				    result->samples,
				    result->target_samples,
				    (double)result->mean_mv,
				    (double)result->rms_mv,
				    (double)channel_settings->dark_noise_rms_mv,
				    (double)result->min_mv,
				    (double)result->max_mv,
				    (double)(result->mean_mv - result->mean_net_mv),
				    (double)channel_settings->dark_mv) != 0 ||
		    coo_json_append_float_or_null(payload, sizeof(payload), &off,
						  channel_settings->lowest_dark_valid ?
							  channel_settings->lowest_dark_mv :
							  (double)NAN,
						  3) != 0 ||
		    coo_json_append(payload, sizeof(payload), &off, "}") != 0) {
			return coo_cmd_error(out, cmd, "dark status response too large");
		}
		return coo_cmd_reply(out, cmd, COO_CMD_RESP_OK, payload);
	}

	if (status->state == PHOTODIODE_AVERAGE_ERROR) {
		snprintk(payload, sizeof(payload),
			 "{\"error\":\"dark measurement failed\",\"channel\":\"%s\",\"rc\":%d,"
			 "\"duration_ms\":%u,\"samples\":%u,\"target_samples\":%u}",
			 photodiode_channel_names[status->channel],
			 status->last_error,
			 result->duration_ms,
			 result->samples,
			 result->target_samples);
		return coo_cmd_reply(out, cmd, COO_CMD_RESP_ERROR, payload);
	}

	snprintk(payload, sizeof(payload),
		 "{\"state\":\"%s\",\"channel\":\"%s\",\"stored_on_complete\":%s,"
		 "\"duration_ms\":%u,\"samples\":%u,\"target_samples\":%u}",
		 state_name,
		 photodiode_channel_names[status->channel],
		 status->store_dark ? "true" : "false",
		 result->duration_ms,
		 result->samples,
		 result->target_samples);
	return coo_cmd_reply(out, cmd, COO_CMD_RESP_OK, payload);
}

int pd_set(const struct coo_cmd_request *cmd, struct coo_cmd_response *out)
{
	enum pd_action action;
	char action_text[32] = {0};
	int action_value;
	enum photodiode_channel channel;
	uint32_t duration_ms = 0U;
	bool store = false;
	bool persist = true;
	int parse_rc;
	int rc;

	parse_rc = coo_json_extract_string(cmd->payload, "action",
					   action_text, sizeof(action_text));
	if (parse_rc == COO_JSON_EXTRACT_MISSING) {
		return coo_cmd_error(out, cmd, "missing action");
	}
	if (parse_rc == COO_JSON_EXTRACT_ERR) {
		return coo_cmd_error(out, cmd, "invalid action");
	}
	if (coo_json_match_string_choice(action_text, pd_action_choices,
					 ARRAY_SIZE(pd_action_choices),
					 &action_value) != 0) {
		return coo_cmd_error(out, cmd, "unknown action");
	}
	action = (enum pd_action)action_value;

	rc = pd_parse_channel_from_payload_or_key(cmd, &channel);
	if (rc != 0) {
		return coo_cmd_error(out, cmd, "channel must be yj or hk");
	}

	switch (action) {
	case PD_ACTION_MEASURE_DARK: {
		struct photodiode_average_status status;

		if (throughput_monitor_autolevel_active(channel)) {
			return coo_cmd_error(out, cmd,
					    "dark measurement blocked by autolevel throughput monitor");
		}

		if (coo_json_extract_optional_u32(cmd->payload, "duration_ms",
						  &duration_ms, NULL) != 0) {
			return coo_cmd_error(out, cmd, "invalid duration_ms");
		}

		if (coo_json_extract_optional_bool(cmd->payload, "store",
						   &store, NULL) != 0) {
			return coo_cmd_error(out, cmd, "invalid store");
		}

		rc = photodiode_start_dark_measurement(channel, duration_ms, store, &status);
		if (rc != 0) {
			return coo_cmd_error_rc(out, cmd, "dark measurement failed", rc);
		}
		return pd_average_status_response(cmd, &status, out);
	}
	case PD_ACTION_DARK_STATUS: {
		struct photodiode_average_status status;

		rc = photodiode_get_average_status(channel, &status);
		if (rc != 0) {
			return coo_cmd_error(out, cmd, "dark status unavailable");
		}

		return pd_average_status_response(cmd, &status, out);
	}
	case PD_ACTION_RESET_LOWEST_DARK:
		if (coo_json_extract_optional_bool(cmd->payload, "persistent",
						   &persist, NULL) != 0) {
			return coo_cmd_error(out, cmd, "invalid persistent");
		}

		rc = photodiode_reset_lowest_dark(channel, persist);
		if (rc != 0) {
			return coo_cmd_error(out, cmd, "reset failed");
		}
		return coo_cmd_ok(out, cmd);
	default:
		return coo_cmd_error(out, cmd, "unknown action");
	}
}

static int pd_append_dark_duration(char *payload, size_t payload_len,
				   size_t *off,
				   const struct app_pd_channel_settings *ch)
{
	if (ch == NULL || ch->dark_duration_ms == APP_PD_DARK_DURATION_USER) {
		return coo_json_append(payload, payload_len, off, "\"user\"");
	}
	return coo_json_append(payload, payload_len, off, "%u", ch->dark_duration_ms);
}

static int pd_settings_channel_json(char *payload, size_t payload_len,
				    enum photodiode_channel channel,
				    const struct app_pd_channel_settings *ch)
{
	size_t off = 0U;
	int64_t off_in_s = housekeeping_photodiode_autooff_remaining_s(pd_power_output(channel));

	if (coo_json_append(payload, payload_len, &off,
			    "{\"channel\":\"%s\",\"dark_mv\":%.3f,"
			    "\"dark_duration_ms\":",
			    photodiode_channel_names[channel],
			    (double)ch->dark_mv) != 0 ||
	    pd_append_dark_duration(payload, payload_len, &off, ch) != 0 ||
	    coo_json_append(payload, payload_len, &off,
			    ",\"dark_noise_rms_mv\":%.3f",
			    (double)ch->dark_noise_rms_mv) != 0 ||
	    coo_json_append(payload, payload_len, &off,
			    ",\"lowest_stored_dark_mv\":") != 0 ||
	    coo_json_append_float_or_null(payload, payload_len, &off,
					  ch->lowest_dark_valid ? ch->lowest_dark_mv : (double)NAN,
					  3) != 0 ||
		    coo_json_append(payload, payload_len, &off,
				    ",\"noise_rms_mV\":%.3f,"
				    "\"responsivity_a_per_w\":%.9f,"
				    "\"transimpedance_v_per_a\":%.6e,"
				    "\"power\":\"%s\",\"autooff_s\":%u,\"off_in_s\":",
				    (double)ch->noise_warn_rms_mv,
				    ch->responsivity_a_per_w,
				    ch->transimpedance_v_per_a,
				    pd_power_mode_name(ch->power),
				    ch->autooff_s) != 0) {
		return -ENOSPC;
	}

	if (off_in_s >= 0 &&
	    coo_json_append(payload, payload_len, &off, "%lld",
			    (long long)off_in_s) != 0) {
		return -ENOSPC;
	}
	if (off_in_s < 0 &&
	    coo_json_append(payload, payload_len, &off, "null") != 0) {
		return -ENOSPC;
	}
	if (coo_json_append(payload, payload_len, &off, "}") != 0) {
		return -ENOSPC;
	}

	return 0;
}

int pd_settings_get(const struct coo_cmd_request *cmd, struct coo_cmd_response *out)
{
	struct app_photodiode_settings settings;
	char payload[MAX_PAYLOAD_LEN] = {0};
	enum photodiode_channel channel;
	int rc;

	rc = pd_parse_channel_from_key(cmd, &channel);
	if (rc != 0) {
		return coo_cmd_error(out, cmd, "pdsettings key must be pdsettings/yj or pdsettings/hk");
	}

	app_settings_get_photodiode(&settings);
	rc = pd_settings_channel_json(payload, sizeof(payload), channel,
				      &settings.channel[channel]);
	if (rc != 0) {
		return coo_cmd_error(out, cmd, "pdsettings response too large");
	}

	return coo_cmd_reply(out, cmd, COO_CMD_RESP_OK, payload);
}

int pd_settings_set(const struct coo_cmd_request *cmd, struct coo_cmd_response *out)
{
	struct app_photodiode_settings settings;
	struct app_pd_channel_settings channel_settings;
	enum photodiode_channel channel;
	bool persist = false;
	bool changed = false;
	bool dark_changed = false;
	int power_value;
	int parse_rc;
	int rc;

	rc = pd_parse_channel_from_key(cmd, &channel);
	if (rc != 0) {
		return coo_cmd_error(out, cmd, "pdsettings key must be pdsettings/yj or pdsettings/hk");
	}

	app_settings_get_photodiode(&settings);
	channel_settings = settings.channel[channel];

	if (coo_json_extract_optional_bool(cmd->payload, "persistent",
					   &persist, NULL) != 0) {
		return coo_cmd_error(out, cmd, "invalid persistent");
	}

	if (coo_json_extract_optional_double_range(cmd->payload, "dark_mv",
						  &channel_settings.dark_mv,
						  &dark_changed,
						  PHOTODIODE_DARK_MIN_MV,
						  PHOTODIODE_DARK_MAX_MV) != 0 ||
	    coo_json_extract_optional_double_range(cmd->payload, "noise_rms_mV",
						  &channel_settings.noise_warn_rms_mv,
						  &changed,
						  PHOTODIODE_NOISE_RMS_MIN_MV,
						  PHOTODIODE_NOISE_RMS_MAX_MV) != 0 ||
	    coo_json_extract_optional_double_range(cmd->payload, "responsivity_a_per_w",
						   &channel_settings.responsivity_a_per_w,
						   &changed,
						   PHOTODIODE_RESPONSIVITY_MIN_A_PER_W,
						   PHOTODIODE_RESPONSIVITY_MAX_A_PER_W) != 0 ||
	    coo_json_extract_optional_double_range(cmd->payload, "transimpedance_v_per_a",
						   &channel_settings.transimpedance_v_per_a,
						   &changed,
						   PHOTODIODE_TRANSIMPEDANCE_MIN_V_PER_A,
						   PHOTODIODE_TRANSIMPEDANCE_MAX_V_PER_A) != 0) {
		return coo_cmd_error(out, cmd, "invalid pdsettings value");
	}
	parse_rc = coo_json_extract_string_choice(cmd->payload, "power",
						  pd_power_choices,
						  ARRAY_SIZE(pd_power_choices),
						  &power_value);
	if (parse_rc == COO_JSON_EXTRACT_ERR) {
		return coo_cmd_error(out, cmd, "invalid power");
	}
	if (parse_rc == COO_JSON_EXTRACT_OK) {
		channel_settings.power = (enum app_pd_power_mode)power_value;
		changed = true;
	}
	if (coo_json_extract_optional_u32(cmd->payload, "autooff_s",
					  &channel_settings.autooff_s,
					  &changed) != 0) {
		return coo_cmd_error(out, cmd, "invalid autooff_s");
	}
	if (dark_changed) {
		channel_settings.dark_duration_ms = APP_PD_DARK_DURATION_USER;
		changed = true;
	}

	if (!changed) {
		return coo_cmd_error(out, cmd, "no pdsettings fields supplied");
	}

	if (channel_settings.power != settings.channel[channel].power) {
		rc = pd_apply_power_mode(channel, channel_settings.power);
		if (rc != 0) {
			return coo_cmd_error_rc(out, cmd, "photodiode power mode failed", rc);
		}
	}
	app_settings_update_photodiode_channel((uint8_t)channel,
					       &channel_settings,
					       persist);
	return coo_cmd_ok(out, cmd);
}
