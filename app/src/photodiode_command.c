/*
 * Copyright (c) 2026 Caltech Optical Observatories
 * SPDX-License-Identifier: Apache-2.0
 */

#include "photodiode_command.h"

#include <errno.h>
#include <math.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/sys/util.h>

#include "app_settings.h"
#include "housekeeping.h"
#include "photodiode.h"

#include <coo_commons/command_dispatch.h>
#include <coo_commons/json_utils.h>

static const struct coo_json_string_choice pd_channel_choices[] = {
	{ "yj", PHOTODIODE_CHANNEL_YJ },
	{ "hk", PHOTODIODE_CHANNEL_HK },
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

static int pd_parse_channel_from_key_base(const struct coo_cmd_request *cmd,
					  const char *base,
					  enum photodiode_channel *channel)
{
	char channel_name[8] = {0};

	if (cmd == NULL ||
	    coo_cmd_key_suffix_segment_copy(cmd->key, base, channel_name,
					    sizeof(channel_name)) != 0) {
		return -ENOENT;
	}

	return pd_parse_channel_name(channel_name, channel);
}

static uint32_t pd_window_length_ms(const struct photodiode_window_result *window)
{
	return window == NULL ? 0U :
	       (uint32_t)window->sample_length * PUBLISH_INTERVAL_MS;
}

static int pd_append_float_field(char *payload, size_t payload_len, size_t *off,
				 const char *name, double value, uint8_t precision)
{
	if (coo_json_append(payload, payload_len, off, ",\"%s\":", name) != 0 ||
	    coo_json_append_float_or_null(payload, payload_len, off, value,
					  precision) != 0) {
		return -ENOSPC;
	}
	return 0;
}

static int pd_append_window_json(char *payload, size_t payload_len, size_t *off,
				 const struct photodiode_window_result *window)
{
	const bool valid = window != NULL && window->valid;
	const uint32_t length_ms = pd_window_length_ms(window);
	const uint16_t failed_samples = window == NULL ? 0U : window->failed_samples;

	if (coo_json_append(payload, payload_len, off,
			    "{\"length_ms\":%u,\"failed_samples\":%u",
			    length_ms, failed_samples) != 0 ||
	    pd_append_float_field(payload, payload_len, off, "mean_mv",
				  valid ? window->mean_mv : (double)NAN, 3) != 0 ||
	    pd_append_float_field(payload, payload_len, off, "mean_net_mv",
				  valid ? window->mean_net_mv : (double)NAN, 3) != 0 ||
	    pd_append_float_field(payload, payload_len, off, "rms_mv",
				  valid ? window->rms_mv : (double)NAN, 3) != 0 ||
	    pd_append_float_field(payload, payload_len, off, "mean_net_err_mv",
				  valid ? window->mean_net_err_mv : (double)NAN, 3) != 0 ||
	    pd_append_float_field(payload, payload_len, off, "min_mv",
				  valid ? window->min_mv : (double)NAN, 3) != 0 ||
	    pd_append_float_field(payload, payload_len, off, "max_mv",
				  valid ? window->max_mv : (double)NAN, 3) != 0 ||
	    pd_append_float_field(payload, payload_len, off, "power_uw",
				  valid ? window->power_uw : (double)NAN, 6) != 0 ||
	    pd_append_float_field(payload, payload_len, off, "power_err_uw",
				  valid ? window->power_err_uw : (double)NAN, 6) != 0 ||
	    coo_json_append(payload, payload_len, off, "}") != 0) {
		return -ENOSPC;
	}

	return 0;
}

static int pd_append_channel_json(char *payload, size_t payload_len, size_t *off,
				  enum photodiode_channel channel,
				  const struct photodiode_channel_status *status)
{
	bool pd_is_off = pd_channel_power_is_off(channel);
	uint64_t ontime_s = (uint64_t)housekeeping_power_on_time_s(pd_power_output(channel));
	const struct photodiode_window_result *dark =
		status == NULL ? NULL : &status->dark_window;

	if (status == NULL ||
	    coo_json_append(payload, payload_len, off,
			    "\"%s\":{\"raw\":%d",
			    photodiode_channel_names[channel],
			    status->raw) != 0 ||
	    pd_append_float_field(payload, payload_len, off, "mv",
				  status->mv, 3) != 0 ||
	    pd_append_float_field(payload, payload_len, off, "net_mv",
				  status->net_mv, 3) != 0 ||
	    pd_append_float_field(payload, payload_len, off, "net_err_mv",
				  status->net_err_mv, 3) != 0 ||
	    pd_append_float_field(payload, payload_len, off, "power_uw",
				  status->power_uw, 6) != 0 ||
	    pd_append_float_field(payload, payload_len, off, "power_err_uw",
				  status->power_err_uw, 6) != 0 ||
	    pd_append_float_field(payload, payload_len, off, "dark_mv",
				  dark != NULL && dark->valid ?
					  dark->mean_mv : (double)NAN, 3) != 0 ||
	    pd_append_float_field(payload, payload_len, off, "dark_err_mv",
				  dark != NULL && dark->valid ?
					  dark->rms_mv : (double)NAN, 3) != 0 ||
	    coo_json_append(payload, payload_len, off, ",\"window\":") != 0 ||
	    pd_append_window_json(payload, payload_len, off, &status->fixed_window) != 0 ||
	    coo_json_append(payload, payload_len, off,
			    ",\"pd_is_off\":%s,\"ontime_s\":%llu}",
			    pd_is_off ? "true" : "false",
			    (unsigned long long)ontime_s) != 0) {
		return -ENOSPC;
	}
	return 0;
}

static int pd_query_channels(const struct coo_cmd_request *cmd,
			     bool include[PHOTODIODE_CHANNEL_COUNT])
{
	enum photodiode_channel channel;
	int rc;

	if (cmd == NULL || include == NULL) {
		return -EINVAL;
	}

	memset(include, 0, sizeof(bool) * PHOTODIODE_CHANNEL_COUNT);
	if (strcmp(cmd->key, "pd") == 0) {
		include[PHOTODIODE_CHANNEL_YJ] = true;
		include[PHOTODIODE_CHANNEL_HK] = true;
		return 0;
	}

	rc = pd_parse_channel_from_key_base(cmd, "pd", &channel);
	if (rc != 0) {
		return rc;
	}
	include[channel] = true;
	return 0;
}

static void pd_auto_enable_selected(const bool include[PHOTODIODE_CHANNEL_COUNT])
{
	struct app_photodiode_settings settings;
	bool wait_for_power = false;

	app_settings_get_photodiode(&settings);
	for (uint8_t i = 0U; i < PHOTODIODE_CHANNEL_COUNT; ++i) {
		bool was_off = false;

		if (!include[i] ||
		    settings.channel[i].power != APP_PD_POWER_AUTO) {
			continue;
		}
		if (housekeeping_photodiode_auto_enable(
			    pd_power_output((enum photodiode_channel)i),
			    settings.channel[i].autooff_s,
			    &was_off) == 0 && was_off) {
			wait_for_power = true;
		}
	}

	if (wait_for_power) {
		k_sleep(K_MSEC(500));
	}
}

int pd_get(const struct coo_cmd_request *cmd, struct coo_cmd_response *out)
{
	struct photodiode_status status;
	char payload[MAX_PAYLOAD_LEN] = {0};
	bool include[PHOTODIODE_CHANNEL_COUNT];
	size_t off = 0U;
	bool appended = false;
	int rc;

	rc = pd_query_channels(cmd, include);
	if (rc != 0) {
		return coo_cmd_error(out, cmd, "pd key must be pd, pd/yj, or pd/hk");
	}

	pd_auto_enable_selected(include);
	photodiode_get_status(&status);
	if (coo_json_append(payload, sizeof(payload), &off, "{") != 0) {
		return coo_cmd_error(out, cmd, "pd response too large");
	}
	for (uint8_t i = 0U; i < PHOTODIODE_CHANNEL_COUNT; ++i) {
		if (!include[i]) {
			continue;
		}
		if ((appended &&
		     coo_json_append(payload, sizeof(payload), &off, ",") != 0) ||
		    pd_append_channel_json(payload, sizeof(payload), &off,
					   (enum photodiode_channel)i,
					   &status.channel[i]) != 0) {
			return coo_cmd_error(out, cmd, "pd response too large");
		}
		appended = true;
	}
	if (coo_json_append(payload, sizeof(payload), &off, "}") != 0) {
		return coo_cmd_error(out, cmd, "pd response too large");
	}
	return coo_cmd_reply(out, cmd, COO_CMD_RESP_OK, payload);
}

static struct app_pd_dark_result
pd_dark_from_window(const struct photodiode_window_result *window)
{
	struct app_pd_dark_result dark = {0};

	if (window == NULL) {
		return dark;
	}
	dark.length_ms = pd_window_length_ms(window);
	dark.failed_samples = window->failed_samples;
	dark.mean_mv = window->mean_mv;
	dark.rms_mv = window->rms_mv;
	dark.min_mv = window->min_mv;
	dark.max_mv = window->max_mv;
	dark.max_raw = window->max_raw;
	return dark;
}

static struct app_pd_dark_result pd_forced_dark(double mean_mv, double rms_mv)
{
	return (struct app_pd_dark_result){
		.mean_mv = mean_mv,
		.rms_mv = rms_mv,
		.min_mv = mean_mv,
		.max_mv = mean_mv,
	};
}

static void pd_update_lowest_dark(struct app_pd_channel_settings *ch, bool reset_lowest)
{
	if (ch == NULL) {
		return;
	}
	if (reset_lowest || !ch->lowest_dark_valid ||
	    ch->dark.mean_mv < ch->lowest_dark.mean_mv) {
		ch->lowest_dark = ch->dark;
		ch->lowest_dark_valid = true;
	}
}

static int pd_dark_response(const struct coo_cmd_request *cmd,
			    enum photodiode_channel channel,
			    struct coo_cmd_response *out)
{
	struct photodiode_status status;
	char payload[MAX_PAYLOAD_LEN] = {0};
	size_t off = 0U;

	photodiode_get_status(&status);
	if (coo_json_append(payload, sizeof(payload), &off,
			    "{\"channel\":\"%s\",\"dark\":",
			    photodiode_channel_names[channel]) != 0 ||
	    pd_append_window_json(payload, sizeof(payload), &off,
				  &status.channel[channel].dark_window) != 0 ||
	    coo_json_append(payload, sizeof(payload), &off, ",\"lowest_dark\":") != 0 ||
	    pd_append_window_json(payload, sizeof(payload), &off,
				  &status.channel[channel].lowest_dark_window) != 0 ||
	    coo_json_append(payload, sizeof(payload), &off, "}") != 0) {
		return coo_cmd_error(out, cmd, "pd dark response too large");
	}
	return coo_cmd_reply(out, cmd, COO_CMD_RESP_OK, payload);
}

int pd_dark_get(const struct coo_cmd_request *cmd, struct coo_cmd_response *out)
{
	enum photodiode_channel channel;
	int rc;

	rc = pd_parse_channel_from_key_base(cmd, "pd/dark", &channel);
	if (rc != 0) {
		return coo_cmd_error(out, cmd, "pd/dark key must be pd/dark/yj or pd/dark/hk");
	}
	return pd_dark_response(cmd, channel, out);
}

int pd_dark_set(const struct coo_cmd_request *cmd, struct coo_cmd_response *out)
{
	struct app_photodiode_settings settings;
	struct photodiode_status status;
	const struct photodiode_window_result *window;
	enum photodiode_channel channel;
	uint32_t duration_ms = 0U;
	double dark_mv = NAN;
	double rms_mv = PHOTODIODE_FORCED_DARK_RMS_DEFAULT_MV;
	bool persist = false;
	bool reset_lowest = false;
	bool duration_supplied = false;
	bool dark_supplied = false;
	bool rms_supplied = false;
	bool reset_supplied = false;
	int rc;

	rc = pd_parse_channel_from_key_base(cmd, "pd/dark", &channel);
	if (rc != 0) {
		return coo_cmd_error(out, cmd, "pd/dark key must be pd/dark/yj or pd/dark/hk");
	}
	if (coo_json_extract_optional_bool(cmd->payload, "persist",
					   &persist, NULL) != 0) {
		return coo_cmd_error(out, cmd, "invalid persist");
	}
	if (coo_json_extract_optional_u32(cmd->payload, "duration_ms",
					  &duration_ms,
					  &duration_supplied) != 0) {
		return coo_cmd_error(out, cmd, "invalid duration_ms");
	}
	if (coo_json_extract_optional_double_range(cmd->payload, "dark_mv",
						  &dark_mv,
						  &dark_supplied,
						  PHOTODIODE_DARK_MIN_MV,
						  PHOTODIODE_DARK_MAX_MV) != 0) {
		return coo_cmd_error(out, cmd, "invalid dark_mv");
	}
	if (coo_json_extract_optional_double_range(cmd->payload, "rms_mv",
						  &rms_mv,
						  &rms_supplied,
						  PHOTODIODE_NOISE_RMS_MIN_MV,
						  PHOTODIODE_NOISE_RMS_MAX_MV) != 0) {
		return coo_cmd_error(out, cmd, "invalid rms_mv");
	}
	if (coo_json_extract_optional_bool(cmd->payload, "reset_lowest",
					   &reset_lowest,
					   &reset_supplied) != 0) {
		return coo_cmd_error(out, cmd, "invalid reset_lowest");
	}
	if (duration_supplied && dark_supplied) {
		return coo_cmd_error(out, cmd, "duration_ms conflicts with dark_mv");
	}
	if (rms_supplied && !dark_supplied) {
		return coo_cmd_error(out, cmd, "rms_mv requires dark_mv");
	}
	if (!duration_supplied && !dark_supplied &&
	    !(reset_supplied && reset_lowest)) {
		return coo_cmd_error(out, cmd, "duration_ms, dark_mv, or reset_lowest required");
	}

	app_settings_get_photodiode(&settings);
	if (duration_supplied) {
		if (duration_ms == 0U ||
		    duration_ms > APP_PD_DARK_DURATION_MAX_MS) {
			return coo_cmd_error(out, cmd, "duration_ms out of range");
		}
		rc = photodiode_set_configurable_window_duration(channel, duration_ms);
		if (rc != 0) {
			return coo_cmd_error_rc(out, cmd, "dark window update failed", rc);
		}
		k_sleep(K_MSEC(duration_ms));
		photodiode_get_status(&status);
		window = &status.channel[channel].configurable_window;
		if (!window->valid || window->sample_length == 0U ||
		    window->sample_length == window->failed_samples) {
			return coo_cmd_error(out, cmd, "dark window has no valid samples");
		}
		settings.channel[channel].dark = pd_dark_from_window(window);
		pd_update_lowest_dark(&settings.channel[channel], reset_lowest);
	} else if (dark_supplied) {
		settings.channel[channel].dark = pd_forced_dark(dark_mv, rms_mv);
		pd_update_lowest_dark(&settings.channel[channel], reset_lowest);
	} else if (reset_lowest) {
		pd_update_lowest_dark(&settings.channel[channel], true);
	}

	app_settings_update_photodiode_channel((uint8_t)channel,
					       &settings.channel[channel],
					       persist);
	return pd_dark_response(cmd, channel, out);
}

static int pd_settings_channel_json(char *payload, size_t payload_len,
				    enum photodiode_channel channel,
				    const struct app_pd_channel_settings *ch)
{
	size_t off = 0U;
	int64_t off_in_s = housekeeping_photodiode_autooff_remaining_s(pd_power_output(channel));

	if (coo_json_append(payload, payload_len, &off,
			    "{\"channel\":\"%s\",\"noise_rms_mv\":%.3f,"
			    "\"responsivity_a_per_w\":%.9f,"
				    "\"transimpedance_v_per_a\":%.6e,"
				    "\"power\":\"%s\",\"autooff_s\":%u,\"off_in_s\":",
			    photodiode_channel_names[channel],
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

	rc = pd_parse_channel_from_key_base(cmd, "pdsettings", &channel);
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
	int power_value;
	int parse_rc;
	int rc;

	rc = pd_parse_channel_from_key_base(cmd, "pdsettings", &channel);
	if (rc != 0) {
		return coo_cmd_error(out, cmd, "pdsettings key must be pdsettings/yj or pdsettings/hk");
	}

	app_settings_get_photodiode(&settings);
	channel_settings = settings.channel[channel];

	if (coo_json_extract_optional_bool(cmd->payload, "persist",
					   &persist, NULL) != 0) {
		return coo_cmd_error(out, cmd, "invalid persist");
	}
	if (coo_json_extract_optional_double_range(cmd->payload, "noise_rms_mv",
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
