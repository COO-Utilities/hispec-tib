/*
 * Copyright (c) 2024 Caltech Optical Observatories
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef APP_LIB_NETWORK_H_
#define APP_LIB_NETWORK_H_

#include <stdbool.h>
#include <stdint.h>
#include <zephyr/net/net_ip.h>

/**
 * @file network.h
 * @brief Simple IPv4 network bootstrap/config helper.
 */

typedef void (*network_event_cb_t)(bool connected);

enum network_ipv4_source {
	NETWORK_IPV4_SOURCE_UNKNOWN = 0,
	NETWORK_IPV4_SOURCE_COMPILED = 1,
	NETWORK_IPV4_SOURCE_STATIC = 2,
	NETWORK_IPV4_SOURCE_FALLBACK = 3,
	NETWORK_IPV4_SOURCE_DHCP = 4,
};

struct network_ipv4_profile {
	char ip[NET_IPV4_ADDR_LEN];
	char subnet[NET_IPV4_ADDR_LEN];
	char gateway[NET_IPV4_ADDR_LEN];
#if defined(CONFIG_DNS_RESOLVER)
	char dns[NET_IPV4_ADDR_LEN];
#endif
#if defined(CONFIG_SNTP)
	char ntp[NET_IPV4_ADDR_LEN];
#endif
};

struct network_config {
	bool try_dhcp_first;
	bool prefer_dhcp_dns;
	bool prefer_dhcp_ntp;
	bool enable_fallback_profile;
	uint32_t dhcp_timeout_ms;
	struct network_ipv4_profile static_profile;
};

/**
 * @brief Snapshot of current IPv4 state.
 */
struct network_ipv4_info {
	bool link_ready;
	bool has_ipv4;
	enum network_ipv4_source source;
	char ip[NET_IPV4_ADDR_LEN];
	char netmask[NET_IPV4_ADDR_LEN];
	char gateway[NET_IPV4_ADDR_LEN];
};

/**
 * @brief Fill config with compile-time defaults.
 */
void network_config_defaults(struct network_config *cfg);

/**
 * @brief Initialize network monitoring and apply initial config.
 */
int network_init(const struct network_config *cfg, network_event_cb_t event_cb);

/**
 * @brief Apply/replace config at runtime.
 *
 * This updates active network settings and re-applies addressing.
 */
int network_reconfigure(const struct network_config *cfg);

/**
 * @brief Wait for project IPv4 readiness.
 *
 * Readiness means the helper has either a DHCP lease or a policy-selected
 * static address. It does not wait for MQTT.
 */
int network_wait_ready(uint32_t timeout_ms);

/**
 * @brief Check if network is currently connected.
 */
bool network_is_ready(void);

/**
 * @brief Read the current IPv4 state.
 */
int network_get_ipv4_info(struct network_ipv4_info *out);

/**
 * @brief Return active config copy.
 */
int network_get_active_config(struct network_config *out);

/**
 * @brief Convert IPv4 source enum to string.
 */
const char *network_ipv4_source_str(enum network_ipv4_source source);

/**
 * @brief Log MAC address of default interface.
 */
void network_log_mac_addr(void);

#endif /* APP_LIB_NETWORK_H_ */
