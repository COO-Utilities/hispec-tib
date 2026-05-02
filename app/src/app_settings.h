/*
 * HiSPEC-TIB settings persistence helpers.
 * Copyright (c) 2026 Caltech Optical Observatories
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef HISPEC_APP_SETTINGS_H
#define HISPEC_APP_SETTINGS_H

#include <stdbool.h>
#include <stdint.h>
#include <zephyr/net/net_ip.h>

struct app_ip_settings {
	bool try_dhcp_first;
	bool prefer_dhcp_dns;
	bool prefer_dhcp_ntp;
	char ip[NET_IPV4_ADDR_LEN];
	char subnet[NET_IPV4_ADDR_LEN];
	char gateway[NET_IPV4_ADDR_LEN];
	char dns[NET_IPV4_ADDR_LEN];
	char ntp[NET_IPV4_ADDR_LEN];
};

struct app_mqtt_settings {
	char broker_host[128];
	uint16_t broker_port;
};

#define APP_PD_CHANNEL_COUNT 2
#define APP_SETTINGS_BOARD_TYPE_MAX_LEN 16

struct app_pd_channel_settings {
	float dark_mv;
	float lowest_dark_mv;
	bool lowest_dark_valid;
	float noise_warn_rms_mv;
	float gain_v_per_uw;
};

struct app_photodiode_settings {
	struct app_pd_channel_settings channel[APP_PD_CHANNEL_COUNT];
};

struct app_settings_snapshot {
	char board_type[APP_SETTINGS_BOARD_TYPE_MAX_LEN];
	struct app_ip_settings ip;
	struct app_mqtt_settings mqtt;
	struct app_photodiode_settings photodiode;
	uint32_t serial_holdoff_s;
	uint32_t boot_count;
	uint32_t mqtt_revision;
};

int app_settings_init(void);
void app_settings_get_snapshot(struct app_settings_snapshot *out);
/**
 * @brief Record the immutable PCB board type in persistent settings.
 *
 * If a stored board type exists and differs from @p board_type, all other
 * persisted app settings are deleted and runtime settings return to defaults.
 * That treats a physically different solder-strap identity as a first boot.
 */
int app_settings_note_board_type(const char *board_type, bool *changed);
void app_settings_get_ip(struct app_ip_settings *out);
void app_settings_update_ip(const struct app_ip_settings *ip, bool persist);
void app_settings_get_mqtt(struct app_mqtt_settings *out);
void app_settings_update_mqtt(const struct app_mqtt_settings *mqtt, bool persist);
void app_settings_get_photodiode(struct app_photodiode_settings *out);
void app_settings_update_photodiode(const struct app_photodiode_settings *pd, bool persist);
/**
 * @brief Update one photodiode channel's calibration/settings.
 *
 * @param channel Zero-based photodiode channel index.
 * @param pd Channel settings to copy into the runtime snapshot.
 * @param persist If true, save only this channel's keys through Zephyr settings.
 */
void app_settings_update_photodiode_channel(uint8_t channel,
					    const struct app_pd_channel_settings *pd,
					    bool persist);
uint32_t app_settings_get_mqtt_revision(void);
uint32_t app_settings_get_serial_holdoff_s(void);
void app_settings_set_serial_holdoff_s(uint32_t seconds, bool persist);
uint32_t app_settings_get_boot_count(void);
void app_settings_increment_boot_count(void);

#endif /* HISPEC_APP_SETTINGS_H */
