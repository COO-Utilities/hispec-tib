/**
 * @file app_settings.c
 * @brief Runtime defaults, settings callbacks, and persistent app state writes.
 *
 * Settings callbacks run during Zephyr settings load and update the protected
 * runtime snapshot. Public update helpers may call settings_save_one() and can
 * block on the configured settings backend.
 *
 * Copyright (c) 2026 Caltech Optical Observatories
 * SPDX-License-Identifier: Apache-2.0
 */

#include "app_settings.h"

#include <ctype.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/settings/settings.h>
#include <zephyr/sys/util.h>

#include <coo_commons/mqtt_client.h>

LOG_MODULE_REGISTER(app_settings, LOG_LEVEL_INF);

#define APP_SETTINGS_SERIAL_HOLDOFF_DEFAULT_S 30U

#define KEY_BOARD_TYPE "board/type"
#define KEY_SERIAL_HOLDOFF "serial/holdoff_s"
#define KEY_BOOT_COUNT "boot/count"
#define KEY_IP_TRY_DHCP "ip/trydhcpfirst"
#define KEY_IP_PREF_DNS "ip/preferdhcpdns"
#define KEY_IP_PREF_NTP "ip/preferdhcpntp"
#define KEY_IP_ADDR "ip/ip"
#define KEY_IP_SUBNET "ip/subnet"
#define KEY_IP_GATEWAY "ip/gateway"
#define KEY_IP_DNS "ip/dns"
#define KEY_IP_NTP "ip/ntp"
#define KEY_MQTT_BROKER "mqtt/broker"
#define KEY_ATTEN_PREFIX "atten"
#define KEY_PD_YJ_DARK_MV "pd/yj/dark_mv"
#define KEY_PD_YJ_LOWEST_DARK_MV "pd/yj/lowest_dark_mv"
#define KEY_PD_YJ_LOWEST_DARK_VALID "pd/yj/lowest_dark_valid"
#define KEY_PD_YJ_NOISE_WARN_MV "pd/yj/noise_warn_rms_mv"
#define KEY_PD_YJ_GAIN_V_PER_UW "pd/yj/gain_v_per_uw"
#define KEY_PD_HK_DARK_MV "pd/hk/dark_mv"
#define KEY_PD_HK_LOWEST_DARK_MV "pd/hk/lowest_dark_mv"
#define KEY_PD_HK_LOWEST_DARK_VALID "pd/hk/lowest_dark_valid"
#define KEY_PD_HK_NOISE_WARN_MV "pd/hk/noise_warn_rms_mv"
#define KEY_PD_HK_GAIN_V_PER_UW "pd/hk/gain_v_per_uw"
#define KEY_LASERBANK_HEATER_MODE "laserbank/heater"

struct app_settings_state {
	struct app_settings_snapshot snapshot;
	struct k_mutex lock;
};

static struct app_settings_state g_settings;

static void str_set(char *dst, size_t dst_size, const char *src)
{
	if (dst == NULL || dst_size == 0) {
		return;
	}

	if (src == NULL) {
		dst[0] = '\0';
		return;
	}

	strncpy(dst, src, dst_size - 1);
	dst[dst_size - 1] = '\0';
}

static void settings_defaults(struct app_settings_snapshot *s)
{
	unsigned long broker_port = 1883UL;
	const char *default_port_str = CONFIG_COO_MQTT_BROKER_PORT;
	char *end = NULL;

	memset(s, 0, sizeof(*s));

	str_set(s->board_type, sizeof(s->board_type), "");
	s->ip.try_dhcp_first = true;
	s->ip.prefer_dhcp_dns = true;
	s->ip.prefer_dhcp_ntp = true;
	str_set(s->ip.ip, sizeof(s->ip.ip), CONFIG_NET_CONFIG_MY_IPV4_ADDR);
	str_set(s->ip.subnet, sizeof(s->ip.subnet), CONFIG_NET_CONFIG_MY_IPV4_NETMASK);
	str_set(s->ip.gateway, sizeof(s->ip.gateway), CONFIG_NET_CONFIG_MY_IPV4_GW);
	str_set(s->ip.dns, sizeof(s->ip.dns), "0.0.0.0");
	str_set(s->ip.ntp, sizeof(s->ip.ntp), "0.0.0.0");
	broker_port = strtoul(default_port_str, &end, 10);
	if (end == NULL || end == default_port_str || *end != '\0' || broker_port > UINT16_MAX) {
		broker_port = 1883UL;
	}
	str_set(s->mqtt.broker_host, sizeof(s->mqtt.broker_host), CONFIG_COO_MQTT_BROKER_HOSTNAME);
	s->mqtt.broker_port = (uint16_t)broker_port;
	for (uint8_t ch = 0U; ch < APP_ATTENUATOR_CHANNEL_COUNT; ++ch) {
		for (uint8_t physical = 0U; physical < APP_ATTENUATOR_PHYSICAL_COUNT; ++physical) {
			/* Default maps the full 0-4096 mV DAC span onto b=0..8
			 * until lab-measured coefficients are stored.
			 */
			s->attenuator.channel[ch].physical[physical].slope =
				8.0f / 4096.0f;
			s->attenuator.channel[ch].physical[physical].offset = 0.0f;
		}
	}
	s->photodiode.channel[0].dark_mv = 0.0f;
	s->photodiode.channel[0].lowest_dark_mv = 0.0f;
	s->photodiode.channel[0].lowest_dark_valid = false;
	s->photodiode.channel[0].noise_warn_rms_mv = 3.0f;
	s->photodiode.channel[0].gain_v_per_uw = 47500.0f;
	s->photodiode.channel[1].dark_mv = 0.0f;
	s->photodiode.channel[1].lowest_dark_mv = 0.0f;
	s->photodiode.channel[1].lowest_dark_valid = false;
	s->photodiode.channel[1].noise_warn_rms_mv = 1.0f;
	s->photodiode.channel[1].gain_v_per_uw = 3.0875f;
	s->laserbank.heater_mode = LASERBANK_HEATER_MODE_AUTO;
	s->serial_holdoff_s = APP_SETTINGS_SERIAL_HOLDOFF_DEFAULT_S;
	s->boot_count = 0U;
	s->mqtt_revision = 0U;
}

static int read_bool(settings_read_cb read_cb, void *cb_arg, bool *out)
{
	uint8_t value = 0;
	int rc = read_cb(cb_arg, &value, sizeof(value));

	if (rc == sizeof(value)) {
		*out = (value != 0U);
		return 0;
	}

	return -EINVAL;
}

static int read_u32(settings_read_cb read_cb, void *cb_arg, uint32_t *out)
{
	int rc = read_cb(cb_arg, out, sizeof(*out));
	return (rc == sizeof(*out)) ? 0 : -EINVAL;
}

static int read_float(settings_read_cb read_cb, void *cb_arg, float *out)
{
	int rc = read_cb(cb_arg, out, sizeof(*out));
	return (rc == sizeof(*out)) ? 0 : -EINVAL;
}

static int read_str(settings_read_cb read_cb, void *cb_arg, char *out, size_t out_size)
{
	int rc;

	if (out == NULL || out_size == 0U) {
		return -EINVAL;
	}

	memset(out, 0, out_size);
	rc = read_cb(cb_arg, out, out_size - 1U);
	if (rc < 0) {
		return rc;
	}

	out[out_size - 1U] = '\0';
	return 0;
}

static void read_valid_float_or_warn(settings_read_cb read_cb, void *cb_arg,
				     const char *name, float *out,
				     float min_value, float max_value)
{
	float value;

	if (read_float(read_cb, cb_arg, &value) != 0 ||
	    !(value >= min_value && value <= max_value)) {
		LOG_WRN("Ignoring invalid stored setting %s", name);
		return;
	}

	*out = value;
}

static int parse_key_index(const char **cursor, uint8_t max_value, uint8_t *out)
{
	char *end = NULL;
	unsigned long value;

	if (cursor == NULL || *cursor == NULL || out == NULL ||
	    !isdigit((unsigned char)**cursor)) {
		return -EINVAL;
	}

	errno = 0;
	value = strtoul(*cursor, &end, 10);
	if (errno != 0 || end == *cursor || value > max_value) {
		return -EINVAL;
	}

	*out = (uint8_t)value;
	*cursor = end;
	return 0;
}

static bool parse_attenuator_coeff_name(const char *name,
					uint8_t *channel,
					uint8_t *physical,
					uint8_t *coeff_index)
{
	const char *cursor = name;

	if (name == NULL || channel == NULL || physical == NULL ||
	    coeff_index == NULL) {
		return false;
	}

	if (parse_key_index(&cursor, APP_ATTENUATOR_CHANNEL_COUNT - 1U, channel) != 0 ||
	    *cursor != '/') {
		return false;
	}
	cursor++;

	if (strncmp(cursor, "physical/", 9U) != 0) {
		return false;
	}
	cursor += 9U;

	if (parse_key_index(&cursor, APP_ATTENUATOR_PHYSICAL_COUNT - 1U, physical) != 0 ||
	    *cursor != '/') {
		return false;
	}
	cursor++;

	if (strcmp(cursor, "slope") == 0) {
		*coeff_index = 0U;
		return true;
	}
	if (strcmp(cursor, "offset") == 0) {
		*coeff_index = 1U;
		return true;
	}

	return parse_key_index(&cursor, APP_ATTENUATOR_COEFF_COUNT - 1U, coeff_index) == 0 &&
	       *cursor == '\0';
}

static int settings_set_cb(const char *name, size_t len, settings_read_cb read_cb, void *cb_arg)
{
	uint8_t atten_channel;
	uint8_t atten_physical;
	uint8_t atten_coeff;

	ARG_UNUSED(len);

	k_mutex_lock(&g_settings.lock, K_FOREVER);

	if (strcmp(name, "type") == 0) {
		(void)read_str(read_cb, cb_arg,
			       g_settings.snapshot.board_type,
			       sizeof(g_settings.snapshot.board_type));
		goto out;
	}

	if (strcmp(name, "holdoff_s") == 0) {
		(void)read_u32(read_cb, cb_arg, &g_settings.snapshot.serial_holdoff_s);
		goto out;
	}

	if (strcmp(name, "count") == 0) {
		(void)read_u32(read_cb, cb_arg, &g_settings.snapshot.boot_count);
		goto out;
	}

	if (strcmp(name, "trydhcpfirst") == 0) {
		(void)read_bool(read_cb, cb_arg, &g_settings.snapshot.ip.try_dhcp_first);
		goto out;
	}

	if (strcmp(name, "preferdhcpdns") == 0) {
		(void)read_bool(read_cb, cb_arg, &g_settings.snapshot.ip.prefer_dhcp_dns);
		goto out;
	}

	if (strcmp(name, "preferdhcpntp") == 0) {
		(void)read_bool(read_cb, cb_arg, &g_settings.snapshot.ip.prefer_dhcp_ntp);
		goto out;
	}

	if (strcmp(name, "ip") == 0) {
		(void)read_str(read_cb, cb_arg, g_settings.snapshot.ip.ip, sizeof(g_settings.snapshot.ip.ip));
		goto out;
	}

	if (strcmp(name, "subnet") == 0) {
		(void)read_str(read_cb, cb_arg, g_settings.snapshot.ip.subnet, sizeof(g_settings.snapshot.ip.subnet));
		goto out;
	}

	if (strcmp(name, "gateway") == 0) {
		(void)read_str(read_cb, cb_arg, g_settings.snapshot.ip.gateway, sizeof(g_settings.snapshot.ip.gateway));
		goto out;
	}

	if (strcmp(name, "dns") == 0) {
		(void)read_str(read_cb, cb_arg, g_settings.snapshot.ip.dns, sizeof(g_settings.snapshot.ip.dns));
		goto out;
	}

	if (strcmp(name, "ntp") == 0) {
		(void)read_str(read_cb, cb_arg, g_settings.snapshot.ip.ntp, sizeof(g_settings.snapshot.ip.ntp));
		goto out;
	}

	if (strcmp(name, "broker") == 0) {
		char endpoint[160];
		struct coo_mqtt_broker_config parsed;

		if (read_str(read_cb, cb_arg, endpoint, sizeof(endpoint)) == 0 &&
		    coo_mqtt_parse_broker_endpoint(endpoint, &parsed)) {
			str_set(g_settings.snapshot.mqtt.broker_host,
				sizeof(g_settings.snapshot.mqtt.broker_host), parsed.host);
			g_settings.snapshot.mqtt.broker_port = parsed.port;
		} else {
			LOG_WRN("Ignoring invalid stored setting mqtt/broker");
		}
		goto out;
	}

	if (parse_attenuator_coeff_name(name, &atten_channel, &atten_physical,
					&atten_coeff)) {
		float *target = (atten_coeff == 0U) ?
			&g_settings.snapshot.attenuator.channel[atten_channel].physical[atten_physical].slope :
			&g_settings.snapshot.attenuator.channel[atten_channel].physical[atten_physical].offset;

		read_valid_float_or_warn(read_cb, cb_arg, name, target,
					 -1000000000.0f, 1000000000.0f);
		goto out;
	}

	if (strcmp(name, "yj/dark_mv") == 0) {
		read_valid_float_or_warn(read_cb, cb_arg, name,
					 &g_settings.snapshot.photodiode.channel[0].dark_mv,
					 -5000.0f, 5000.0f);
		goto out;
	}

	if (strcmp(name, "yj/lowest_dark_mv") == 0) {
		read_valid_float_or_warn(read_cb, cb_arg, name,
					 &g_settings.snapshot.photodiode.channel[0].lowest_dark_mv,
					 -5000.0f, 5000.0f);
		goto out;
	}

	if (strcmp(name, "yj/lowest_dark_valid") == 0) {
		(void)read_bool(read_cb, cb_arg,
				&g_settings.snapshot.photodiode.channel[0].lowest_dark_valid);
		goto out;
	}

	if (strcmp(name, "yj/noise_warn_rms_mv") == 0) {
		read_valid_float_or_warn(read_cb, cb_arg, name,
					 &g_settings.snapshot.photodiode.channel[0].noise_warn_rms_mv,
					 0.0f, 5000.0f);
		goto out;
	}

	if (strcmp(name, "yj/gain_v_per_uw") == 0) {
		read_valid_float_or_warn(read_cb, cb_arg, name,
					 &g_settings.snapshot.photodiode.channel[0].gain_v_per_uw,
					 0.000001f, 1000000000.0f);
		goto out;
	}

	if (strcmp(name, "hk/dark_mv") == 0) {
		read_valid_float_or_warn(read_cb, cb_arg, name,
					 &g_settings.snapshot.photodiode.channel[1].dark_mv,
					 -5000.0f, 5000.0f);
		goto out;
	}

	if (strcmp(name, "hk/lowest_dark_mv") == 0) {
		read_valid_float_or_warn(read_cb, cb_arg, name,
					 &g_settings.snapshot.photodiode.channel[1].lowest_dark_mv,
					 -5000.0f, 5000.0f);
		goto out;
	}

	if (strcmp(name, "hk/lowest_dark_valid") == 0) {
		(void)read_bool(read_cb, cb_arg,
				&g_settings.snapshot.photodiode.channel[1].lowest_dark_valid);
		goto out;
	}

	if (strcmp(name, "hk/noise_warn_rms_mv") == 0) {
		read_valid_float_or_warn(read_cb, cb_arg, name,
					 &g_settings.snapshot.photodiode.channel[1].noise_warn_rms_mv,
					 0.0f, 5000.0f);
		goto out;
	}

	if (strcmp(name, "hk/gain_v_per_uw") == 0) {
		read_valid_float_or_warn(read_cb, cb_arg, name,
					 &g_settings.snapshot.photodiode.channel[1].gain_v_per_uw,
					 0.000001f, 1000000000.0f);
		goto out;
	}

	if (strcmp(name, "heater") == 0) {
		uint32_t value;

		if (read_u32(read_cb, cb_arg, &value) == 0 &&
		    value <= LASERBANK_HEATER_MODE_OVERRIDE_OFF) {
			g_settings.snapshot.laserbank.heater_mode =
				(enum laserbank_heater_mode)value;
		} else {
			LOG_WRN("Ignoring invalid stored setting %s", name);
		}
		goto out;
	}

out:
	k_mutex_unlock(&g_settings.lock);
	return 0;
}

SETTINGS_STATIC_HANDLER_DEFINE(board_settings, "board", NULL, settings_set_cb, NULL, NULL);
SETTINGS_STATIC_HANDLER_DEFINE(serial_settings, "serial", NULL, settings_set_cb, NULL, NULL);
SETTINGS_STATIC_HANDLER_DEFINE(boot_settings, "boot", NULL, settings_set_cb, NULL, NULL);
SETTINGS_STATIC_HANDLER_DEFINE(ip_settings, "ip", NULL, settings_set_cb, NULL, NULL);
SETTINGS_STATIC_HANDLER_DEFINE(mqtt_settings, "mqtt", NULL, settings_set_cb, NULL, NULL);
SETTINGS_STATIC_HANDLER_DEFINE(atten_settings, "atten", NULL, settings_set_cb, NULL, NULL);
SETTINGS_STATIC_HANDLER_DEFINE(pd_settings, "pd", NULL, settings_set_cb, NULL, NULL);
SETTINGS_STATIC_HANDLER_DEFINE(laserbank_settings, "laserbank", NULL, settings_set_cb, NULL, NULL);

static void persist_bool(const char *key, bool value)
{
	uint8_t v = value ? 1U : 0U;
	(void)settings_save_one(key, &v, sizeof(v));
}

static void persist_u32(const char *key, uint32_t value)
{
	(void)settings_save_one(key, &value, sizeof(value));
}

static void persist_float(const char *key, float value)
{
	(void)settings_save_one(key, &value, sizeof(value));
}

static void persist_str(const char *key, const char *value)
{
	const size_t len = strlen(value) + 1U;
	(void)settings_save_one(key, value, len);
}

static void attenuator_coeff_key(char *key, size_t key_len,
				 uint8_t channel, uint8_t physical,
				 uint8_t coeff_index)
{
	(void)snprintk(key, key_len, "%s/%u/physical/%u/%s",
		       KEY_ATTEN_PREFIX, channel, physical,
		       coeff_index == 0U ? "slope" : "offset");
}

static void attenuator_legacy_coeff_key(char *key, size_t key_len,
					uint8_t channel,
					const char *coeff_name,
					uint8_t coeff_index)
{
	(void)snprintk(key, key_len, "%s/%u/%s/%u",
		       KEY_ATTEN_PREFIX, channel, coeff_name, coeff_index);
}

static void persist_attenuator_channel(uint8_t channel,
				       const struct app_attenuator_channel_settings *atten)
{
	char key[40];

	if (atten == NULL || channel >= APP_ATTENUATOR_CHANNEL_COUNT) {
		return;
	}

	for (uint8_t physical = 0U; physical < APP_ATTENUATOR_PHYSICAL_COUNT; ++physical) {
		attenuator_coeff_key(key, sizeof(key), channel, physical, 0U);
		persist_float(key, atten->physical[physical].slope);

		attenuator_coeff_key(key, sizeof(key), channel, physical, 1U);
		persist_float(key, atten->physical[physical].offset);
	}
}

static void persist_photodiode_channel(uint8_t channel,
				       const struct app_pd_channel_settings *pd)
{
	if (channel == 0U) {
		persist_float(KEY_PD_YJ_DARK_MV, pd->dark_mv);
		persist_float(KEY_PD_YJ_LOWEST_DARK_MV, pd->lowest_dark_mv);
		persist_bool(KEY_PD_YJ_LOWEST_DARK_VALID, pd->lowest_dark_valid);
		persist_float(KEY_PD_YJ_NOISE_WARN_MV, pd->noise_warn_rms_mv);
		persist_float(KEY_PD_YJ_GAIN_V_PER_UW, pd->gain_v_per_uw);
		return;
	}

	if (channel == 1U) {
		persist_float(KEY_PD_HK_DARK_MV, pd->dark_mv);
		persist_float(KEY_PD_HK_LOWEST_DARK_MV, pd->lowest_dark_mv);
		persist_bool(KEY_PD_HK_LOWEST_DARK_VALID, pd->lowest_dark_valid);
		persist_float(KEY_PD_HK_NOISE_WARN_MV, pd->noise_warn_rms_mv);
		persist_float(KEY_PD_HK_GAIN_V_PER_UW, pd->gain_v_per_uw);
	}
}

static const char *const resettable_setting_keys[] = {
	KEY_SERIAL_HOLDOFF,
	KEY_BOOT_COUNT,
	KEY_IP_TRY_DHCP,
	KEY_IP_PREF_DNS,
	KEY_IP_PREF_NTP,
	KEY_IP_ADDR,
	KEY_IP_SUBNET,
	KEY_IP_GATEWAY,
	KEY_IP_DNS,
	KEY_IP_NTP,
	KEY_MQTT_BROKER,
	KEY_PD_YJ_DARK_MV,
	KEY_PD_YJ_LOWEST_DARK_MV,
	KEY_PD_YJ_LOWEST_DARK_VALID,
	KEY_PD_YJ_NOISE_WARN_MV,
	KEY_PD_YJ_GAIN_V_PER_UW,
	KEY_PD_HK_DARK_MV,
	KEY_PD_HK_LOWEST_DARK_MV,
	KEY_PD_HK_LOWEST_DARK_VALID,
	KEY_PD_HK_NOISE_WARN_MV,
	KEY_PD_HK_GAIN_V_PER_UW,
	KEY_LASERBANK_HEATER_MODE,
};

static void delete_setting_key(const char *key)
{
	int rc = settings_delete(key);

	if (rc != 0 && rc != -ENOENT) {
		LOG_WRN("settings_delete(%s) failed (%d)", key, rc);
	}
}

static void delete_attenuator_settings(void)
{
	char key[40];

	for (uint8_t channel = 0U; channel < APP_ATTENUATOR_CHANNEL_COUNT; ++channel) {
		for (uint8_t coeff = 0U; coeff < 3U; ++coeff) {
			attenuator_legacy_coeff_key(key, sizeof(key), channel, "db2volt",
						    coeff);
			delete_setting_key(key);

			attenuator_legacy_coeff_key(key, sizeof(key), channel, "volt2db",
						    coeff);
			delete_setting_key(key);
		}

		for (uint8_t physical = 0U; physical < APP_ATTENUATOR_PHYSICAL_COUNT; ++physical) {
			attenuator_coeff_key(key, sizeof(key), channel, physical, 0U);
			delete_setting_key(key);

			attenuator_coeff_key(key, sizeof(key), channel, physical, 1U);
			delete_setting_key(key);
		}
	}
}

static void delete_resettable_settings(void)
{
	for (uint8_t i = 0; i < ARRAY_SIZE(resettable_setting_keys); ++i) {
		/* settings_delete() removes one persisted key from the Zephyr
		 * settings backend; missing keys are fine during first boot.
		 */
		delete_setting_key(resettable_setting_keys[i]);
	}

	delete_attenuator_settings();
}

int app_settings_init(void)
{
	static const char *const app_settings_subtrees[] = {
		"board", "serial", "boot", "ip", "mqtt", "atten", "pd",
	};
	int rc;

	k_mutex_init(&g_settings.lock);
	settings_defaults(&g_settings.snapshot);

	/* settings_subsys_init() attaches the configured Zephyr settings backend
	 * before any app-owned keys can be loaded or saved.
	 */
	rc = settings_subsys_init();
	if (rc != 0) {
		LOG_ERR("settings_subsys_init failed (%d)", rc);
		return rc;
	}

	for (uint8_t i = 0U; i < ARRAY_SIZE(app_settings_subtrees); ++i) {
		rc = settings_load_subtree(app_settings_subtrees[i]);
		if (rc != 0) {
			LOG_WRN("settings_load_subtree('%s') failed (%d)",
				app_settings_subtrees[i], rc);
			return rc;
		}
	}

	return 0;
}

void app_settings_get_snapshot(struct app_settings_snapshot *out)
{
	if (out == NULL) {
		return;
	}

	k_mutex_lock(&g_settings.lock, K_FOREVER);
	*out = g_settings.snapshot;
	k_mutex_unlock(&g_settings.lock);
}

int app_settings_note_board_type(const char *board_type, bool *changed)
{
	bool changed_local = false;
	bool persist_needed = false;
	bool reset_needed = false;

	if (changed != NULL) {
		*changed = false;
	}
	if (board_type == NULL || board_type[0] == '\0' ||
	    strlen(board_type) >= APP_SETTINGS_BOARD_TYPE_MAX_LEN) {
		return -EINVAL;
	}

	k_mutex_lock(&g_settings.lock, K_FOREVER);
	if (g_settings.snapshot.board_type[0] == '\0') {
		str_set(g_settings.snapshot.board_type,
			sizeof(g_settings.snapshot.board_type),
			board_type);
		persist_needed = true;
	} else if (strcmp(g_settings.snapshot.board_type, board_type) != 0) {
		char previous[APP_SETTINGS_BOARD_TYPE_MAX_LEN];

		str_set(previous, sizeof(previous), g_settings.snapshot.board_type);
		LOG_WRN("Board type changed from %s to %s; clearing persisted settings",
			previous, board_type);
		settings_defaults(&g_settings.snapshot);
		str_set(g_settings.snapshot.board_type,
			sizeof(g_settings.snapshot.board_type),
			board_type);
		changed_local = true;
		reset_needed = true;
		persist_needed = true;
	}
	k_mutex_unlock(&g_settings.lock);

	if (reset_needed) {
		delete_resettable_settings();
	}
	if (persist_needed) {
		persist_str(KEY_BOARD_TYPE, board_type);
	}
	if (changed != NULL) {
		*changed = changed_local;
	}

	return 0;
}

void app_settings_get_ip(struct app_ip_settings *out)
{
	if (out == NULL) {
		return;
	}

	k_mutex_lock(&g_settings.lock, K_FOREVER);
	*out = g_settings.snapshot.ip;
	k_mutex_unlock(&g_settings.lock);
}

void app_settings_update_ip(const struct app_ip_settings *ip, bool persist)
{
	if (ip == NULL) {
		return;
	}

	k_mutex_lock(&g_settings.lock, K_FOREVER);
	g_settings.snapshot.ip = *ip;
	k_mutex_unlock(&g_settings.lock);

	if (persist) {
		persist_bool(KEY_IP_TRY_DHCP, ip->try_dhcp_first);
		persist_bool(KEY_IP_PREF_DNS, ip->prefer_dhcp_dns);
		persist_bool(KEY_IP_PREF_NTP, ip->prefer_dhcp_ntp);
		persist_str(KEY_IP_ADDR, ip->ip);
		persist_str(KEY_IP_SUBNET, ip->subnet);
		persist_str(KEY_IP_GATEWAY, ip->gateway);
		persist_str(KEY_IP_DNS, ip->dns);
		persist_str(KEY_IP_NTP, ip->ntp);
	}
}

void app_settings_get_mqtt(struct app_mqtt_settings *out)
{
	if (out == NULL) {
		return;
	}

	k_mutex_lock(&g_settings.lock, K_FOREVER);
	*out = g_settings.snapshot.mqtt;
	k_mutex_unlock(&g_settings.lock);
}

void app_settings_update_mqtt(const struct app_mqtt_settings *mqtt, bool persist)
{
	if (mqtt == NULL) {
		return;
	}

	k_mutex_lock(&g_settings.lock, K_FOREVER);
	g_settings.snapshot.mqtt = *mqtt;
	g_settings.snapshot.mqtt_revision++;
	k_mutex_unlock(&g_settings.lock);

	if (persist) {
		struct coo_mqtt_broker_config cfg = {
			.port = mqtt->broker_port,
		};
		char endpoint[160];

		str_set(cfg.host, sizeof(cfg.host), mqtt->broker_host);
		if (coo_mqtt_format_broker_endpoint(&cfg, endpoint, sizeof(endpoint)) == 0) {
			persist_str(KEY_MQTT_BROKER, endpoint);
		}
	}
}

void app_settings_get_attenuator(struct app_attenuator_settings *out)
{
	if (out == NULL) {
		return;
	}

	k_mutex_lock(&g_settings.lock, K_FOREVER);
	*out = g_settings.snapshot.attenuator;
	k_mutex_unlock(&g_settings.lock);
}

void app_settings_update_attenuator_channel(uint8_t channel,
					    const struct app_attenuator_channel_settings *atten,
					    bool persist)
{
	if (atten == NULL || channel >= APP_ATTENUATOR_CHANNEL_COUNT) {
		return;
	}

	k_mutex_lock(&g_settings.lock, K_FOREVER);
	g_settings.snapshot.attenuator.channel[channel] = *atten;
	k_mutex_unlock(&g_settings.lock);

	if (persist) {
		persist_attenuator_channel(channel, atten);
	}
}

void app_settings_get_photodiode(struct app_photodiode_settings *out)
{
	if (out == NULL) {
		return;
	}

	k_mutex_lock(&g_settings.lock, K_FOREVER);
	*out = g_settings.snapshot.photodiode;
	k_mutex_unlock(&g_settings.lock);
}

void app_settings_update_photodiode(const struct app_photodiode_settings *pd, bool persist)
{
	if (pd == NULL) {
		return;
	}

	k_mutex_lock(&g_settings.lock, K_FOREVER);
	g_settings.snapshot.photodiode = *pd;
	k_mutex_unlock(&g_settings.lock);

	if (persist) {
		persist_photodiode_channel(0U, &pd->channel[0]);
		persist_photodiode_channel(1U, &pd->channel[1]);
	}
}

void app_settings_update_photodiode_channel(uint8_t channel,
					    const struct app_pd_channel_settings *pd,
					    bool persist)
{
	if (pd == NULL || channel >= APP_PD_CHANNEL_COUNT) {
		return;
	}

	k_mutex_lock(&g_settings.lock, K_FOREVER);
	g_settings.snapshot.photodiode.channel[channel] = *pd;
	k_mutex_unlock(&g_settings.lock);

	if (persist) {
		persist_photodiode_channel(channel, pd);
	}
}

void app_settings_get_laserbank(struct app_laserbank_settings *out)
{
	if (out == NULL) {
		return;
	}

	k_mutex_lock(&g_settings.lock, K_FOREVER);
	*out = g_settings.snapshot.laserbank;
	k_mutex_unlock(&g_settings.lock);
}

void app_settings_update_laserbank(const struct app_laserbank_settings *laserbank,
				   bool persist)
{
	if (laserbank == NULL) {
		return;
	}

	k_mutex_lock(&g_settings.lock, K_FOREVER);
	g_settings.snapshot.laserbank = *laserbank;
	k_mutex_unlock(&g_settings.lock);

	if (persist) {
		persist_u32(KEY_LASERBANK_HEATER_MODE,
			    (uint32_t)laserbank->heater_mode);
	}
}

uint32_t app_settings_get_mqtt_revision(void)
{
	uint32_t value;

	k_mutex_lock(&g_settings.lock, K_FOREVER);
	value = g_settings.snapshot.mqtt_revision;
	k_mutex_unlock(&g_settings.lock);

	return value;
}

uint32_t app_settings_get_serial_holdoff_s(void)
{
	uint32_t value;

	k_mutex_lock(&g_settings.lock, K_FOREVER);
	value = g_settings.snapshot.serial_holdoff_s;
	k_mutex_unlock(&g_settings.lock);

	return value;
}

void app_settings_set_serial_holdoff_s(uint32_t seconds, bool persist)
{
	k_mutex_lock(&g_settings.lock, K_FOREVER);
	g_settings.snapshot.serial_holdoff_s = seconds;
	k_mutex_unlock(&g_settings.lock);

	if (persist) {
		persist_u32(KEY_SERIAL_HOLDOFF, seconds);
	}
}

uint32_t app_settings_get_boot_count(void)
{
	uint32_t value;

	k_mutex_lock(&g_settings.lock, K_FOREVER);
	value = g_settings.snapshot.boot_count;
	k_mutex_unlock(&g_settings.lock);

	return value;
}

void app_settings_increment_boot_count(void)
{
	uint32_t value;

	k_mutex_lock(&g_settings.lock, K_FOREVER);
	g_settings.snapshot.boot_count++;
	value = g_settings.snapshot.boot_count;
	k_mutex_unlock(&g_settings.lock);

	persist_u32(KEY_BOOT_COUNT, value);
}
