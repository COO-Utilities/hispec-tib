/**
 * @file app_settings.c
 * @brief Runtime defaults and direct-NVS persistence for app-owned settings.
 *
 * Public update helpers copy into the protected runtime snapshot. When
 * persistence is requested they write one numeric Zephyr NVS ID and may block
 * on flash I/O. The app owns this fixed NVS ID map; no string setting names
 * are stored in flash.
 *
 * Copyright (c) 2026 Caltech Optical Observatories
 * SPDX-License-Identifier: Apache-2.0
 */

#include "app_settings.h"
#include "attenuator.h"
#include "lasers.h"
#include "photodiode.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <zephyr/device.h>
#include <zephyr/drivers/flash.h>
#include <zephyr/kernel.h>
#include <zephyr/kvss/nvs.h>
#include <zephyr/logging/log.h>
#include <zephyr/storage/flash_map.h>
#include <zephyr/sys/util.h>

LOG_MODULE_REGISTER(app_settings, LOG_LEVEL_INF);

#define APP_NVS_SCHEMA_MAGIC 0x48535653U /* "HSVS" */
#define APP_NVS_SCHEMA_VERSION 3U

enum app_nvs_id {
	APP_NVS_ID_SCHEMA = 0x0001,
	APP_NVS_ID_BOARD_TYPE = 0x0002,
	APP_NVS_ID_SERIAL_HOLDOFF_UNUSED = 0x0003,
	APP_NVS_ID_BOOT_COUNT = 0x0004,
	APP_NVS_ID_IP = 0x0005,
	APP_NVS_ID_MQTT = 0x0006,
	APP_NVS_ID_LASERBANK = 0x0007,
	APP_NVS_ID_LAST_KNOWN_UTC_MS = 0x0008,
	APP_NVS_ID_LAST_COMMAND = APP_SETTINGS_NVS_ID_LAST_COMMAND,
	APP_NVS_ID_ATTEN_CH0 = 0x0100,
	APP_NVS_ID_PD_CH0 = 0x0200,
	APP_NVS_ID_LASER_POLICY_CH0 = 0x0300,
	APP_NVS_ID_LASER_TOTAL_CH0 = 0x0340,
	APP_NVS_ID_ROUTE_LOSS_CH0 = 0x0400,
};

BUILD_ASSERT(APP_NVS_ID_ROUTE_LOSS_CH0 + APP_ROUTE_LOSS_RECORD_COUNT < 0x8000,
	     "app NVS IDs must stay below Zephyr settings backend IDs");
BUILD_ASSERT(APP_NVS_ID_ATTEN_CH0 + APP_ATTENUATOR_CHANNEL_COUNT <= APP_NVS_ID_PD_CH0,
	     "attenuator NVS ID block overlaps photodiode block");
BUILD_ASSERT(APP_NVS_ID_PD_CH0 + APP_PD_CHANNEL_COUNT <= APP_NVS_ID_LASER_POLICY_CH0,
	     "photodiode NVS ID block overlaps laser policy block");
BUILD_ASSERT(APP_NVS_ID_LASER_POLICY_CH0 + APP_LASER_CHANNEL_COUNT <= APP_NVS_ID_LASER_TOTAL_CH0,
	     "laser policy NVS ID block overlaps laser total block");
BUILD_ASSERT(APP_NVS_ID_LASER_TOTAL_CH0 + APP_LASER_CHANNEL_COUNT <= APP_NVS_ID_ROUTE_LOSS_CH0,
	     "laser total NVS ID block overlaps route-loss block");
BUILD_ASSERT(APP_NVS_ID_ROUTE_LOSS_CH0 + APP_ROUTE_LOSS_RECORD_COUNT <= 0x8000,
	     "route-loss NVS ID block overlaps reserved Zephyr settings IDs");
BUILD_ASSERT(APP_ATTENUATOR_PHYSICAL_COUNT == ATTENUATOR_PHYSICAL_COUNT,
	     "app settings and attenuator physical coefficient counts must match");
BUILD_ASSERT(APP_LASER_CHANNEL_COUNT == HISPEC_LASER_COUNT,
	     "app settings and laser channel counts must match");

struct app_nvs_schema_marker {
	uint32_t magic;
	uint16_t version;
	uint16_t reserved;
};

struct app_nvs_ip_settings {
	uint8_t try_dhcp_first;
	uint8_t prefer_dhcp_dns;
	uint8_t prefer_dhcp_ntp;
	uint8_t reserved;
	char ip[NET_IPV4_ADDR_LEN];
	char subnet[NET_IPV4_ADDR_LEN];
	char gateway[NET_IPV4_ADDR_LEN];
	char dns[NET_IPV4_ADDR_LEN];
	char ntp[NET_IPV4_ADDR_LEN];
};

struct app_nvs_pd_channel {
	float dark_mv;
	float lowest_dark_mv;
	uint32_t dark_duration_ms;
	float dark_noise_rms_mv;
	uint8_t lowest_dark_valid;
	uint8_t reserved[3];
	float noise_warn_rms_mv;
	double responsivity_a_per_w;
	double transimpedance_v_per_a;
};

struct app_nvs_laser_policy {
	float nominal_current_ma;
	float max_current_ma;
	float threshold_current_ma;
	float efficiency_mw_per_ma;
	float wavelength_nm;
	float current_set_calibration_pct;
	float operating_temp_min_c;
	float operating_temp_max_c;
	float operating_temp_c;
	uint16_t tec_pid_p;
	uint16_t tec_pid_i;
	uint16_t tec_pid_d;
	uint8_t disable_tec_at_autooff;
	uint8_t reserved;
	float dlambda_dT_nm_per_k;
	float dlambda_dA_nm_per_ma;
	uint32_t autooff_s;
	float tune_delta_nm;
};

static const laserprops_t *const default_laser_props[APP_LASER_CHANNEL_COUNT] = {
	&LASER_1028,
	&LASER_1270,
	&LASER_1430,
	&LASER_1430,
	&LASER_1510,
	&LASER_2330,
};

struct app_settings_state {
	struct app_settings_snapshot snapshot;
	struct k_mutex lock;
};

static struct app_settings_state g_settings;
static struct nvs_fs app_nvs;
static bool app_nvs_ready;

static uint16_t attenuator_nvs_id(uint8_t channel)
{
	return APP_NVS_ID_ATTEN_CH0 + channel;
}

static uint16_t pd_nvs_id(uint8_t channel)
{
	return APP_NVS_ID_PD_CH0 + channel;
}

static uint16_t laser_policy_nvs_id(uint8_t channel)
{
	return APP_NVS_ID_LASER_POLICY_CH0 + channel;
}

static uint16_t laser_total_nvs_id(uint8_t channel)
{
	return APP_NVS_ID_LASER_TOTAL_CH0 + channel;
}

static uint16_t route_loss_nvs_id(uint8_t index)
{
	return APP_NVS_ID_ROUTE_LOSS_CH0 + index;
}

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

static bool double_in_range(double value, double min_value, double max_value)
{
	return value >= min_value && value <= max_value;
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
	str_set(s->ip.dns, sizeof(s->ip.dns), CONFIG_APP_DEFAULT_DNS_SERVER);
	str_set(s->ip.ntp, sizeof(s->ip.ntp), CONFIG_APP_DEFAULT_NTP_SERVER);
	broker_port = strtoul(default_port_str, &end, 10);
	if (end == NULL || end == default_port_str || *end != '\0' || broker_port > UINT16_MAX) {
		broker_port = 1883UL;
	}
	str_set(s->mqtt.broker_host, sizeof(s->mqtt.broker_host), CONFIG_COO_MQTT_BROKER_HOSTNAME);
	s->mqtt.broker_port = (uint16_t)broker_port;
	for (uint8_t ch = 0U; ch < APP_ATTENUATOR_CHANNEL_COUNT; ++ch) {
		for (uint8_t physical = 0U; physical < APP_ATTENUATOR_PHYSICAL_COUNT; ++physical) {
			/* Default maps the full 0-5000 mV attenuator drive span
			 * onto b=0..8 until lab-measured coefficients are stored.
			 */
			s->attenuator.channel[ch].physical[physical].slope = 8.0f / 5000.0f;
			s->attenuator.channel[ch].physical[physical].offset = 0.0f;
		}
	}
	s->photodiode.channel[PHOTODIODE_CHANNEL_YJ].dark_mv = PHOTODIODE_DEFAULT_DARK_MV;
	s->photodiode.channel[PHOTODIODE_CHANNEL_YJ].dark_duration_ms =
		APP_PD_DARK_DURATION_USER;
	s->photodiode.channel[PHOTODIODE_CHANNEL_YJ].dark_noise_rms_mv =
		PHOTODIODE_DEFAULT_DARK_NOISE_RMS_MV;
	s->photodiode.channel[PHOTODIODE_CHANNEL_YJ].lowest_dark_mv =
		PHOTODIODE_DEFAULT_LOWEST_DARK_MV;
	s->photodiode.channel[PHOTODIODE_CHANNEL_YJ].lowest_dark_valid = false;
	s->photodiode.channel[PHOTODIODE_CHANNEL_YJ].noise_warn_rms_mv =
		PHOTODIODE_YJ_DEFAULT_NOISE_WARN_RMS_MV;
	s->photodiode.channel[PHOTODIODE_CHANNEL_YJ].responsivity_a_per_w =
		PHOTODIODE_YJ_DEFAULT_RESPONSIVITY_A_PER_W;
	s->photodiode.channel[PHOTODIODE_CHANNEL_YJ].transimpedance_v_per_a =
		PHOTODIODE_YJ_DEFAULT_TRANSIMPEDANCE_V_PER_A;
	s->photodiode.channel[PHOTODIODE_CHANNEL_HK].dark_mv = PHOTODIODE_DEFAULT_DARK_MV;
	s->photodiode.channel[PHOTODIODE_CHANNEL_HK].dark_duration_ms =
		APP_PD_DARK_DURATION_USER;
	s->photodiode.channel[PHOTODIODE_CHANNEL_HK].dark_noise_rms_mv =
		PHOTODIODE_DEFAULT_DARK_NOISE_RMS_MV;
	s->photodiode.channel[PHOTODIODE_CHANNEL_HK].lowest_dark_mv =
		PHOTODIODE_DEFAULT_LOWEST_DARK_MV;
	s->photodiode.channel[PHOTODIODE_CHANNEL_HK].lowest_dark_valid = false;
	s->photodiode.channel[PHOTODIODE_CHANNEL_HK].noise_warn_rms_mv =
		PHOTODIODE_HK_DEFAULT_NOISE_WARN_RMS_MV;
	s->photodiode.channel[PHOTODIODE_CHANNEL_HK].responsivity_a_per_w =
		PHOTODIODE_HK_DEFAULT_RESPONSIVITY_A_PER_W;
	s->photodiode.channel[PHOTODIODE_CHANNEL_HK].transimpedance_v_per_a =
		PHOTODIODE_HK_DEFAULT_TRANSIMPEDANCE_V_PER_A;
	s->laserbank.heater_mode = LASERBANK_HEATER_MODE_AUTO;
	for (uint8_t i = 0U; i < APP_LASER_CHANNEL_COUNT; ++i) {
		s->laser.channel[i].properties = *default_laser_props[i];
		s->laser.channel[i].current_set_calibration_pct = 100.0f;
		s->laser.channel[i].disable_tec_at_autooff = true;
		s->laser.channel[i].autooff_s = 3U * 3600U;
		s->laser.channel[i].tune_delta_nm = 0.0f;
		s->laser.channel[i].total_emitting_s = 0.0;
	}
	s->boot_count = 0U;
	s->mqtt_revision = 0U;
}

static int app_nvs_mount(void)
{
	struct flash_pages_info page_info;
	int rc;

	app_nvs.flash_device = PARTITION_DEVICE(storage_partition);
	if (!device_is_ready(app_nvs.flash_device)) {
		LOG_ERR("NVS flash device is not ready");
		return -ENODEV;
	}

	app_nvs.offset = PARTITION_OFFSET(storage_partition);
	rc = flash_get_page_info_by_offs(app_nvs.flash_device, app_nvs.offset, &page_info);
	if (rc != 0) {
		LOG_ERR("flash_get_page_info_by_offs failed (%d)", rc);
		return rc;
	}

	app_nvs.sector_size = page_info.size;
	app_nvs.sector_count = PARTITION_SIZE(storage_partition) / page_info.size;
	rc = nvs_mount(&app_nvs);
	if (rc != 0) {
		LOG_ERR("nvs_mount failed (%d)", rc);
		return rc;
	}

	app_nvs_ready = true;
	return 0;
}

static int app_nvs_write(uint16_t id, const void *data, size_t len)
{
	int rc;

	if (!app_nvs_ready) {
		return -EIO;
	}

	rc = nvs_write(&app_nvs, id, data, len);
	if (rc < 0) {
		LOG_WRN("NVS write id 0x%04x failed (%d)", id, rc);
		return rc;
	}

	return 0;
}

static int app_nvs_delete(uint16_t id)
{
	int rc;

	if (!app_nvs_ready) {
		return -EIO;
	}

	rc = nvs_delete(&app_nvs, id);
	if (rc != 0 && rc != -ENOENT) {
		LOG_WRN("NVS delete id 0x%04x failed (%d)", id, rc);
	}

	return rc == -ENOENT ? 0 : rc;
}

static bool app_nvs_read_exact(uint16_t id, void *data, size_t len, const char *name)
{
	int rc;

	if (!app_nvs_ready || data == NULL) {
		return false;
	}

	rc = nvs_read(&app_nvs, id, data, len);
	if (rc == (int)len) {
		return true;
	}
	if (rc != -ENOENT) {
		LOG_WRN("Ignoring invalid NVS %s id 0x%04x length (%d)", name, id, rc);
	}

	return false;
}

static int app_nvs_write_schema(void)
{
	const struct app_nvs_schema_marker marker = {
		.magic = APP_NVS_SCHEMA_MAGIC,
		.version = APP_NVS_SCHEMA_VERSION,
	};

	return app_nvs_write(APP_NVS_ID_SCHEMA, &marker, sizeof(marker));
}

static int app_nvs_ensure_schema(void)
{
	struct app_nvs_schema_marker marker = {0};
	int rc;

	if (app_nvs_read_exact(APP_NVS_ID_SCHEMA, &marker, sizeof(marker), "schema") &&
	    marker.magic == APP_NVS_SCHEMA_MAGIC &&
	    marker.version == APP_NVS_SCHEMA_VERSION) {
		return 0;
	}

	LOG_INF("Initializing app NVS schema %u; clearing old storage layout",
		APP_NVS_SCHEMA_VERSION);
	rc = nvs_clear(&app_nvs);
	if (rc != 0) {
		LOG_ERR("nvs_clear failed (%d)", rc);
		app_nvs_ready = false;
		return rc;
	}

	app_nvs_ready = false;
	rc = app_nvs_mount();
	if (rc != 0) {
		return rc;
	}

	return app_nvs_write_schema();
}

static void app_nvs_persist_board_type(const char *board_type)
{
	char value[APP_SETTINGS_BOARD_TYPE_MAX_LEN] = {0};

	str_set(value, sizeof(value), board_type);
	(void)app_nvs_write(APP_NVS_ID_BOARD_TYPE, value, sizeof(value));
}

static void app_nvs_persist_ip(const struct app_ip_settings *ip)
{
	struct app_nvs_ip_settings stored = {0};

	if (ip == NULL) {
		return;
	}

	stored.try_dhcp_first = ip->try_dhcp_first ? 1U : 0U;
	stored.prefer_dhcp_dns = ip->prefer_dhcp_dns ? 1U : 0U;
	stored.prefer_dhcp_ntp = ip->prefer_dhcp_ntp ? 1U : 0U;
	str_set(stored.ip, sizeof(stored.ip), ip->ip);
	str_set(stored.subnet, sizeof(stored.subnet), ip->subnet);
	str_set(stored.gateway, sizeof(stored.gateway), ip->gateway);
	str_set(stored.dns, sizeof(stored.dns), ip->dns);
	str_set(stored.ntp, sizeof(stored.ntp), ip->ntp);
	(void)app_nvs_write(APP_NVS_ID_IP, &stored, sizeof(stored));
}

static void app_nvs_persist_mqtt(const struct app_mqtt_settings *mqtt)
{
	struct app_mqtt_settings stored = {0};

	if (mqtt == NULL) {
		return;
	}

	str_set(stored.broker_host, sizeof(stored.broker_host), mqtt->broker_host);
	stored.broker_port = mqtt->broker_port;
	(void)app_nvs_write(APP_NVS_ID_MQTT, &stored, sizeof(stored));
}

static void app_nvs_persist_attenuator_channel(uint8_t channel,
					       const struct app_attenuator_channel_settings *atten)
{
	if (atten == NULL || channel >= APP_ATTENUATOR_CHANNEL_COUNT) {
		return;
	}

	(void)app_nvs_write(attenuator_nvs_id(channel), atten, sizeof(*atten));
}

static void app_nvs_persist_pd_channel(uint8_t channel,
				       const struct app_pd_channel_settings *pd)
{
	struct app_nvs_pd_channel stored = {0};

	if (pd == NULL || channel >= APP_PD_CHANNEL_COUNT) {
		return;
	}

	stored.dark_mv = pd->dark_mv;
	stored.lowest_dark_mv = pd->lowest_dark_mv;
	stored.dark_duration_ms = pd->dark_duration_ms;
	stored.dark_noise_rms_mv = pd->dark_noise_rms_mv;
	stored.lowest_dark_valid = pd->lowest_dark_valid ? 1U : 0U;
	stored.noise_warn_rms_mv = pd->noise_warn_rms_mv;
	stored.responsivity_a_per_w = pd->responsivity_a_per_w;
	stored.transimpedance_v_per_a = pd->transimpedance_v_per_a;
	(void)app_nvs_write(pd_nvs_id(channel), &stored, sizeof(stored));
}

static void laser_policy_from_settings(struct app_nvs_laser_policy *stored,
				       const struct app_laser_channel_settings *laser)
{
	if (stored == NULL || laser == NULL) {
		return;
	}

	memset(stored, 0, sizeof(*stored));
	stored->nominal_current_ma = laser->properties.nominal_current_ma;
	stored->max_current_ma = laser->properties.max_current_ma;
	stored->threshold_current_ma = laser->properties.threshold_current_ma;
	stored->efficiency_mw_per_ma = laser->properties.efficiency_mw_per_ma;
	stored->wavelength_nm = laser->properties.wavelength_nm;
	stored->current_set_calibration_pct = laser->current_set_calibration_pct;
	stored->operating_temp_min_c = laser->properties.operating_temp_range_c.min_c;
	stored->operating_temp_max_c = laser->properties.operating_temp_range_c.max_c;
	stored->operating_temp_c = laser->properties.operating_temp_c;
	stored->tec_pid_p = laser->properties.tec_pid.kp;
	stored->tec_pid_i = laser->properties.tec_pid.ki;
	stored->tec_pid_d = laser->properties.tec_pid.kd;
	stored->disable_tec_at_autooff = laser->disable_tec_at_autooff ? 1U : 0U;
	stored->dlambda_dT_nm_per_k = laser->properties.dlambda_dT_nm_per_k;
	stored->dlambda_dA_nm_per_ma = laser->properties.dlambda_dA_nm_per_ma;
	stored->autooff_s = laser->autooff_s;
	stored->tune_delta_nm = laser->tune_delta_nm;
}

static void app_nvs_persist_laser_channel(uint8_t channel,
					  const struct app_laser_channel_settings *laser)
{
	struct app_nvs_laser_policy stored;

	if (laser == NULL || channel >= APP_LASER_CHANNEL_COUNT) {
		return;
	}

	laser_policy_from_settings(&stored, laser);
	(void)app_nvs_write(laser_policy_nvs_id(channel), &stored, sizeof(stored));
	(void)app_nvs_write(laser_total_nvs_id(channel),
			    &laser->total_emitting_s,
			    sizeof(laser->total_emitting_s));
}

static void app_nvs_persist_laser_total(uint8_t channel, double total_emitting_s)
{
	if (channel >= APP_LASER_CHANNEL_COUNT) {
		return;
	}

	(void)app_nvs_write(laser_total_nvs_id(channel),
			    &total_emitting_s,
			    sizeof(total_emitting_s));
}

static void app_nvs_persist_laserbank(const struct app_laserbank_settings *laserbank)
{
	uint32_t mode;

	if (laserbank == NULL) {
		return;
	}

	mode = (uint32_t)laserbank->heater_mode;
	(void)app_nvs_write(APP_NVS_ID_LASERBANK, &mode, sizeof(mode));
}

static void app_nvs_persist_route_loss_index(uint8_t index,
					     const struct app_route_loss_record *record)
{
	if (record == NULL || index >= APP_ROUTE_LOSS_RECORD_COUNT) {
		return;
	}

	(void)app_nvs_write(route_loss_nvs_id(index), record, sizeof(*record));
}

static bool attenuator_channel_valid(const struct app_attenuator_channel_settings *atten)
{
	struct attenuator_model_coeffs physical[ATTENUATOR_PHYSICAL_COUNT];

	if (atten == NULL) {
		return false;
	}

	for (uint8_t i = 0U; i < APP_ATTENUATOR_PHYSICAL_COUNT; ++i) {
		const struct app_attenuator_physical_settings *p = &atten->physical[i];

		physical[i].slope = p->slope;
		physical[i].offset = p->offset;
	}

	return attenuator_model_coefficients_valid(physical);
}

static void pd_settings_from_nvs(struct app_pd_channel_settings *pd,
				 const struct app_nvs_pd_channel *stored)
{
	if (pd == NULL || stored == NULL) {
		return;
	}

	pd->dark_mv = stored->dark_mv;
	pd->lowest_dark_mv = stored->lowest_dark_mv;
	pd->dark_duration_ms = stored->dark_duration_ms;
	pd->dark_noise_rms_mv = stored->dark_noise_rms_mv;
	pd->lowest_dark_valid = stored->lowest_dark_valid != 0U;
	pd->noise_warn_rms_mv = stored->noise_warn_rms_mv;
	pd->responsivity_a_per_w = stored->responsivity_a_per_w;
	pd->transimpedance_v_per_a = stored->transimpedance_v_per_a;
}

static bool route_loss_record_valid(struct app_route_loss_record *record)
{
	if (record == NULL || !record->configured) {
		return false;
	}

	record->route[sizeof(record->route) - 1U] = '\0';
	record->laser[sizeof(record->laser) - 1U] = '\0';
	return record->route[0] != '\0' &&
	       record->laser[0] != '\0' &&
	       double_in_range(record->transmission, 0.000000001, 1.0);
}

static void app_nvs_load_board_type(struct app_settings_snapshot *s)
{
	char value[APP_SETTINGS_BOARD_TYPE_MAX_LEN] = {0};

	if (app_nvs_read_exact(APP_NVS_ID_BOARD_TYPE, value, sizeof(value), "board_type")) {
		value[sizeof(value) - 1U] = '\0';
		str_set(s->board_type, sizeof(s->board_type), value);
	}
}

static void app_nvs_load_ip(struct app_settings_snapshot *s)
{
	struct app_nvs_ip_settings stored;

	if (!app_nvs_read_exact(APP_NVS_ID_IP, &stored, sizeof(stored), "ip")) {
		return;
	}

	stored.ip[sizeof(stored.ip) - 1U] = '\0';
	stored.subnet[sizeof(stored.subnet) - 1U] = '\0';
	stored.gateway[sizeof(stored.gateway) - 1U] = '\0';
	stored.dns[sizeof(stored.dns) - 1U] = '\0';
	stored.ntp[sizeof(stored.ntp) - 1U] = '\0';
	s->ip.try_dhcp_first = stored.try_dhcp_first != 0U;
	s->ip.prefer_dhcp_dns = stored.prefer_dhcp_dns != 0U;
	s->ip.prefer_dhcp_ntp = stored.prefer_dhcp_ntp != 0U;
	str_set(s->ip.ip, sizeof(s->ip.ip), stored.ip);
	str_set(s->ip.subnet, sizeof(s->ip.subnet), stored.subnet);
	str_set(s->ip.gateway, sizeof(s->ip.gateway), stored.gateway);
	str_set(s->ip.dns, sizeof(s->ip.dns), stored.dns);
	str_set(s->ip.ntp, sizeof(s->ip.ntp), stored.ntp);
}

static void app_nvs_load_mqtt(struct app_settings_snapshot *s)
{
	struct app_mqtt_settings stored;

	if (!app_nvs_read_exact(APP_NVS_ID_MQTT, &stored, sizeof(stored), "mqtt")) {
		return;
	}

	stored.broker_host[sizeof(stored.broker_host) - 1U] = '\0';
	if (stored.broker_host[0] == '\0' || stored.broker_port == 0U) {
		LOG_WRN("Ignoring invalid stored MQTT settings");
		return;
	}

	str_set(s->mqtt.broker_host, sizeof(s->mqtt.broker_host), stored.broker_host);
	s->mqtt.broker_port = stored.broker_port;
}

static void app_nvs_load_attenuator(struct app_settings_snapshot *s)
{
	for (uint8_t channel = 0U; channel < APP_ATTENUATOR_CHANNEL_COUNT; ++channel) {
		struct app_attenuator_channel_settings stored;

		if (!app_nvs_read_exact(attenuator_nvs_id(channel), &stored,
					sizeof(stored), "attenuator")) {
			continue;
		}
		if (!attenuator_channel_valid(&stored)) {
			LOG_WRN("Ignoring invalid stored attenuator channel %u", channel);
			continue;
		}

		s->attenuator.channel[channel] = stored;
	}
}

static void app_nvs_load_photodiode(struct app_settings_snapshot *s)
{
	for (uint8_t channel = 0U; channel < APP_PD_CHANNEL_COUNT; ++channel) {
		struct app_nvs_pd_channel stored;
		struct app_pd_channel_settings *pd = &s->photodiode.channel[channel];
		struct app_pd_channel_settings candidate;

		if (!app_nvs_read_exact(pd_nvs_id(channel), &stored,
					sizeof(stored), "photodiode")) {
			continue;
		}
		candidate = *pd;
		pd_settings_from_nvs(&candidate, &stored);
		if (!photodiode_settings_valid(&candidate)) {
			LOG_WRN("Ignoring invalid stored photodiode channel %u", channel);
			continue;
		}
		*pd = candidate;
	}
}

static void app_nvs_load_laserbank(struct app_settings_snapshot *s)
{
	uint32_t value;

	if (!app_nvs_read_exact(APP_NVS_ID_LASERBANK, &value, sizeof(value), "laserbank")) {
		return;
	}
	if (value > LASERBANK_HEATER_MODE_OVERRIDE_OFF) {
		LOG_WRN("Ignoring invalid stored laser-bank heater mode");
		return;
	}

	s->laserbank.heater_mode = (enum laserbank_heater_mode)value;
}

static void app_nvs_apply_laser_policy(struct app_laser_channel_settings *laser,
				       const struct app_nvs_laser_policy *stored)
{
	laser->properties.nominal_current_ma = stored->nominal_current_ma;
	laser->properties.max_current_ma = stored->max_current_ma;
	laser->properties.threshold_current_ma = stored->threshold_current_ma;
	laser->properties.efficiency_mw_per_ma = stored->efficiency_mw_per_ma;
	laser->properties.wavelength_nm = stored->wavelength_nm;
	laser->current_set_calibration_pct = stored->current_set_calibration_pct;
	laser->properties.operating_temp_range_c.min_c = stored->operating_temp_min_c;
	laser->properties.operating_temp_range_c.max_c = stored->operating_temp_max_c;
	laser->properties.operating_temp_c = stored->operating_temp_c;
	laser->properties.tec_pid.kp = stored->tec_pid_p;
	laser->properties.tec_pid.ki = stored->tec_pid_i;
	laser->properties.tec_pid.kd = stored->tec_pid_d;
	laser->disable_tec_at_autooff = stored->disable_tec_at_autooff != 0U;
	laser->properties.dlambda_dT_nm_per_k = stored->dlambda_dT_nm_per_k;
	laser->properties.dlambda_dA_nm_per_ma = stored->dlambda_dA_nm_per_ma;
	laser->autooff_s = stored->autooff_s;
	laser->tune_delta_nm = stored->tune_delta_nm;
}

static void app_nvs_load_laser(struct app_settings_snapshot *s)
{
	for (uint8_t channel = 0U; channel < APP_LASER_CHANNEL_COUNT; ++channel) {
		struct app_nvs_laser_policy policy;
		struct app_laser_channel_settings laser;
		double total_emitting_s;

		if (app_nvs_read_exact(laser_policy_nvs_id(channel), &policy,
				       sizeof(policy), "laser policy")) {
			laser = s->laser.channel[channel];
			app_nvs_apply_laser_policy(&laser, &policy);
			if (hispec_laser_validate_channel_settings((enum hispec_laser_id)channel,
								   &laser) == 0) {
				s->laser.channel[channel] = laser;
			} else {
				LOG_WRN("Ignoring invalid stored laser policy channel %u", channel);
			}
		}

		if (app_nvs_read_exact(laser_total_nvs_id(channel), &total_emitting_s,
				       sizeof(total_emitting_s), "laser total") &&
		    double_in_range(total_emitting_s, 0.0, 1.0e12)) {
			s->laser.channel[channel].total_emitting_s = total_emitting_s;
		}
	}
}

static void app_nvs_load_route_loss(struct app_settings_snapshot *s)
{
	for (uint8_t i = 0U; i < APP_ROUTE_LOSS_RECORD_COUNT; ++i) {
		struct app_route_loss_record stored;

		if (!app_nvs_read_exact(route_loss_nvs_id(i), &stored,
					sizeof(stored), "route loss")) {
			continue;
		}
		if (!route_loss_record_valid(&stored)) {
			LOG_WRN("Ignoring invalid stored route-loss record %u", i);
			continue;
		}

		s->route_loss.record[i] = stored;
	}
}

static void app_nvs_load_all(struct app_settings_snapshot *s)
{
	uint32_t value;

	app_nvs_load_board_type(s);
	if (app_nvs_read_exact(APP_NVS_ID_BOOT_COUNT, &value, sizeof(value), "boot count")) {
		s->boot_count = value;
	}
	if (app_nvs_read_exact(APP_NVS_ID_LAST_KNOWN_UTC_MS, &s->last_known_utc_ms,
			       sizeof(s->last_known_utc_ms), "last known UTC") &&
	    s->last_known_utc_ms == 0U) {
		LOG_WRN("Ignoring invalid stored last known UTC");
	}
	app_nvs_load_ip(s);
	app_nvs_load_mqtt(s);
	app_nvs_load_attenuator(s);
	app_nvs_load_photodiode(s);
	app_nvs_load_laserbank(s);
	app_nvs_load_laser(s);
	app_nvs_load_route_loss(s);
}

static void delete_setting_record(uint16_t id, int *first_rc)
{
	int rc = app_nvs_delete(id);

	if (rc != 0 && first_rc != NULL && *first_rc == 0) {
		*first_rc = rc;
	}
}

static int delete_resettable_settings(bool keep_ip, bool keep_boot_count)
{
	int first_rc = 0;

	delete_setting_record(APP_NVS_ID_SERIAL_HOLDOFF_UNUSED, &first_rc);
	delete_setting_record(APP_NVS_ID_BOARD_TYPE, &first_rc);
	if (!keep_boot_count) {
		delete_setting_record(APP_NVS_ID_BOOT_COUNT, &first_rc);
	}
	delete_setting_record(APP_NVS_ID_LAST_KNOWN_UTC_MS, &first_rc);
	delete_setting_record(APP_NVS_ID_LAST_COMMAND, &first_rc);
	if (!keep_ip) {
		delete_setting_record(APP_NVS_ID_IP, &first_rc);
	}
	delete_setting_record(APP_NVS_ID_MQTT, &first_rc);
	delete_setting_record(APP_NVS_ID_LASERBANK, &first_rc);

	for (uint8_t channel = 0U; channel < APP_ATTENUATOR_CHANNEL_COUNT; ++channel) {
		delete_setting_record(attenuator_nvs_id(channel), &first_rc);
	}
	for (uint8_t channel = 0U; channel < APP_PD_CHANNEL_COUNT; ++channel) {
		delete_setting_record(pd_nvs_id(channel), &first_rc);
	}
	for (uint8_t channel = 0U; channel < APP_LASER_CHANNEL_COUNT; ++channel) {
		delete_setting_record(laser_policy_nvs_id(channel), &first_rc);
		delete_setting_record(laser_total_nvs_id(channel), &first_rc);
	}
	for (uint8_t i = 0U; i < APP_ROUTE_LOSS_RECORD_COUNT; ++i) {
		delete_setting_record(route_loss_nvs_id(i), &first_rc);
	}

	return first_rc;
}

static int route_loss_record_index_locked(const char *route, const char *laser,
					  bool allocate)
{
	int first_free = -1;

	for (uint8_t i = 0U; i < APP_ROUTE_LOSS_RECORD_COUNT; ++i) {
		struct app_route_loss_record *record =
			&g_settings.snapshot.route_loss.record[i];

		if (!record->configured) {
			if (first_free < 0) {
				first_free = i;
			}
			continue;
		}

		if (strcmp(record->route, route) == 0 &&
		    strcmp(record->laser, laser) == 0) {
			return i;
		}
	}

	return allocate ? first_free : -1;
}

int app_settings_init(void)
{
	int rc;

	k_mutex_init(&g_settings.lock);
	settings_defaults(&g_settings.snapshot);

	/* Direct NVS keeps Zephyr's flash wear-leveling and recovery behavior
	 * without storing human-readable setting names alongside small values.
	 */
	rc = app_nvs_mount();
	if (rc != 0) {
		return rc;
	}

	rc = app_nvs_ensure_schema();
	if (rc != 0) {
		return rc;
	}

	app_nvs_load_all(&g_settings.snapshot);
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
	bool persist_needed = false;
	bool reset_needed = false;

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
		persist_needed = true;
		reset_needed = true;
	}
	k_mutex_unlock(&g_settings.lock);

	if (reset_needed) {
		(void)delete_resettable_settings(false, false);
	}
	if (persist_needed) {
		app_nvs_persist_board_type(board_type);
	}
	if (changed != NULL) {
		*changed = reset_needed;
	}

	return 0;
}

int app_settings_erase_non_ip_settings(void)
{
	struct app_ip_settings ip;
	uint32_t boot_count;
	int rc;

	if (!app_nvs_ready) {
		return -EIO;
	}

	k_mutex_lock(&g_settings.lock, K_FOREVER);
	ip = g_settings.snapshot.ip;
	boot_count = g_settings.snapshot.boot_count;
	settings_defaults(&g_settings.snapshot);
	g_settings.snapshot.ip = ip;
	g_settings.snapshot.boot_count = boot_count;
	k_mutex_unlock(&g_settings.lock);

	rc = delete_resettable_settings(true, true);
	if (rc != 0) {
		LOG_WRN("Failed to erase all non-IP settings (%d)", rc);
		return rc;
	}

	LOG_WRN("Erased stored non-IP settings; preserved IP settings and boot count");
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
		app_nvs_persist_ip(ip);
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
		app_nvs_persist_mqtt(mqtt);
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
		app_nvs_persist_attenuator_channel(channel, atten);
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

bool app_settings_try_get_photodiode(struct app_photodiode_settings *out)
{
	if (out == NULL) {
		return false;
	}

	if (k_mutex_lock(&g_settings.lock, K_NO_WAIT) != 0) {
		return false;
	}

	*out = g_settings.snapshot.photodiode;
	k_mutex_unlock(&g_settings.lock);
	return true;
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
		app_nvs_persist_pd_channel(0U, &pd->channel[0]);
		app_nvs_persist_pd_channel(1U, &pd->channel[1]);
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
		app_nvs_persist_pd_channel(channel, pd);
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
		app_nvs_persist_laserbank(laserbank);
	}
}

void app_settings_get_laser(struct app_laser_settings *out)
{
	if (out == NULL) {
		return;
	}

	k_mutex_lock(&g_settings.lock, K_FOREVER);
	*out = g_settings.snapshot.laser;
	k_mutex_unlock(&g_settings.lock);
}

int app_settings_get_laser_channel(uint8_t channel,
				   struct app_laser_channel_settings *out)
{
	if (out == NULL || channel >= APP_LASER_CHANNEL_COUNT) {
		return -EINVAL;
	}

	k_mutex_lock(&g_settings.lock, K_FOREVER);
	*out = g_settings.snapshot.laser.channel[channel];
	k_mutex_unlock(&g_settings.lock);
	return 0;
}

int app_settings_update_laser_channel(uint8_t channel,
				      const struct app_laser_channel_settings *laser,
				      bool persist)
{
	if (laser == NULL || channel >= APP_LASER_CHANNEL_COUNT) {
		return -EINVAL;
	}

	k_mutex_lock(&g_settings.lock, K_FOREVER);
	g_settings.snapshot.laser.channel[channel] = *laser;
	k_mutex_unlock(&g_settings.lock);

	if (persist) {
		app_nvs_persist_laser_channel(channel, laser);
	}

	return 0;
}

int app_settings_update_laser_total_emitting(uint8_t channel,
					     double total_emitting_s,
					     bool persist)
{
	if (channel >= APP_LASER_CHANNEL_COUNT || !(total_emitting_s >= 0.0)) {
		return -EINVAL;
	}

	k_mutex_lock(&g_settings.lock, K_FOREVER);
	g_settings.snapshot.laser.channel[channel].total_emitting_s = total_emitting_s;
	k_mutex_unlock(&g_settings.lock);

	if (persist) {
		app_nvs_persist_laser_total(channel, total_emitting_s);
	}

	return 0;
}

int app_settings_get_route_loss(const char *route, const char *laser,
				double *transmission)
{
	int index;

	if (transmission == NULL || route == NULL || laser == NULL) {
		return -EINVAL;
	}
	if (route[0] == '\0' || laser[0] == '\0' ||
	    strlen(route) >= APP_ROUTE_LOSS_ROUTE_MAX_LEN ||
	    strlen(laser) >= APP_ROUTE_LOSS_LASER_MAX_LEN) {
		return -EINVAL;
	}

	*transmission = 1.0;

	k_mutex_lock(&g_settings.lock, K_FOREVER);
	index = route_loss_record_index_locked(route, laser, false);
	if (index >= 0) {
		*transmission = g_settings.snapshot.route_loss.record[index].transmission;
	}
	k_mutex_unlock(&g_settings.lock);

	return 0;
}

int app_settings_set_route_loss(const char *route, const char *laser,
				double transmission, bool persist)
{
	int index;
	struct app_route_loss_record record;

	if (route == NULL || laser == NULL ||
	    route[0] == '\0' || laser[0] == '\0' ||
	    strlen(route) >= APP_ROUTE_LOSS_ROUTE_MAX_LEN ||
	    strlen(laser) >= APP_ROUTE_LOSS_LASER_MAX_LEN ||
	    !(transmission > 0.0 && transmission <= 1.0)) {
		return -EINVAL;
	}

	memset(&record, 0, sizeof(record));
	record.configured = true;
	str_set(record.route, sizeof(record.route), route);
	str_set(record.laser, sizeof(record.laser), laser);
	record.transmission = transmission;

	k_mutex_lock(&g_settings.lock, K_FOREVER);
	index = route_loss_record_index_locked(route, laser, true);
	if (index >= 0) {
		g_settings.snapshot.route_loss.record[index] = record;
	}
	k_mutex_unlock(&g_settings.lock);

	if (index < 0) {
		return -ENOSPC;
	}

	if (persist) {
		app_nvs_persist_route_loss_index((uint8_t)index, &record);
	}

	return 0;
}

uint32_t app_settings_get_mqtt_revision(void)
{
	uint32_t value;

	k_mutex_lock(&g_settings.lock, K_FOREVER);
	value = g_settings.snapshot.mqtt_revision;
	k_mutex_unlock(&g_settings.lock);

	return value;
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

	(void)app_nvs_write(APP_NVS_ID_BOOT_COUNT, &value, sizeof(value));
}

bool app_settings_get_last_known_utc_ms(uint64_t *utc_ms)
{
	uint64_t value;

	if (utc_ms == NULL) {
		return false;
	}

	k_mutex_lock(&g_settings.lock, K_FOREVER);
	value = g_settings.snapshot.last_known_utc_ms;
	k_mutex_unlock(&g_settings.lock);

	if (value == 0U) {
		return false;
	}

	*utc_ms = value;
	return true;
}

void app_settings_note_time_utc_ms(uint64_t utc_ms)
{
	if (utc_ms == 0U) {
		return;
	}

	k_mutex_lock(&g_settings.lock, K_FOREVER);
	if (utc_ms == g_settings.snapshot.last_known_utc_ms) {
		k_mutex_unlock(&g_settings.lock);
		return;
	}
	g_settings.snapshot.last_known_utc_ms = utc_ms;
	k_mutex_unlock(&g_settings.lock);

	(void)app_nvs_write(APP_NVS_ID_LAST_KNOWN_UTC_MS, &utc_ms, sizeof(utc_ms));
}

struct nvs_fs *app_settings_nvs_fs(void)
{
	return app_nvs_ready ? &app_nvs : NULL;
}
