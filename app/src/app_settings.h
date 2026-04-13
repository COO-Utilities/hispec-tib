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

struct app_settings_snapshot {
	struct app_ip_settings ip;
	uint32_t serial_holdoff_s;
	uint32_t boot_count;
};

int app_settings_init(void);
void app_settings_get_snapshot(struct app_settings_snapshot *out);
void app_settings_get_ip(struct app_ip_settings *out);
void app_settings_update_ip(const struct app_ip_settings *ip, bool persist);
uint32_t app_settings_get_serial_holdoff_s(void);
void app_settings_set_serial_holdoff_s(uint32_t seconds, bool persist);
uint32_t app_settings_get_boot_count(void);
void app_settings_increment_boot_count(void);

#endif /* HISPEC_APP_SETTINGS_H */
