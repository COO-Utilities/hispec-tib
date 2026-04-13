/*
 * Copyright (c) 2024 Caltech Optical Observatories
 * SPDX-License-Identifier: Apache-2.0
 */

#include <coo_commons/network.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/net/net_mgmt.h>
#include <zephyr/net/net_event.h>
#include <zephyr/net/net_if.h>
#include <zephyr/net/conn_mgr_monitor.h>
#include <zephyr/net/conn_mgr_connectivity.h>
#include <zephyr/net/net_ip.h>
#if defined(CONFIG_NETWORK_HELPER_ENABLE_DHCP)
#include <zephyr/net/dhcpv4.h>
#endif
#include <errno.h>
#include <string.h>

LOG_MODULE_REGISTER(network, LOG_LEVEL_DBG);

static volatile bool network_online;
static enum network_ipv4_source active_source = NETWORK_IPV4_SOURCE_UNKNOWN;
static struct network_config active_cfg;
static network_event_cb_t user_event_cb;

#define NET_L4_EVENT_MASK (NET_EVENT_L4_CONNECTED | NET_EVENT_L4_DISCONNECTED)
static struct net_mgmt_event_callback net_l4_mgmt_cb;
static struct net_mgmt_event_callback net_ipv4_mgmt_cb;
static struct k_work_delayable reconnect_work;
static bool network_initialized;

//TODO is this really necessary, I'm inclined to axe it
static void str_set(char *dst, size_t dst_size, const char *src)
{
	if (dst == NULL || dst_size == 0U) {
		return;
	}

	if (src == NULL) {
		dst[0] = '\0';
		return;
	}

	strncpy(dst, src, dst_size - 1U);
	dst[dst_size - 1U] = '\0';
}

//TODO is this really necessary, I'm inclined to axe it, use net_addr_pton directly, this looks like defensive coding
static bool parse_ipv4(const char *text, struct in_addr *out)
{
	if (text == NULL || out == NULL || text[0] == '\0') {
		return false;
	}

	return net_addr_pton(AF_INET, text, out) == 0;
}

//TODO is this really necessary, I'm inclined to axe it and do the check where needed w/o a function call
static bool profile_has_valid_static_ipv4(const struct network_ipv4_profile *profile)
{
	struct in_addr ip = { 0 };
	struct in_addr subnet = { 0 };

	if (profile == NULL) {
		return false;
	}

	return parse_ipv4(profile->ip, &ip) && parse_ipv4(profile->subnet, &subnet);
}

static bool profile_matches_compiled_static_defaults(const struct network_ipv4_profile *profile)
{
	if (profile == NULL) {
		return false;
	}

	return strcmp(profile->ip, CONFIG_NETWORK_HELPER_STATIC_IPV4_ADDR) == 0 &&
	       strcmp(profile->subnet, CONFIG_NETWORK_HELPER_STATIC_IPV4_NETMASK) == 0 &&
	       strcmp(profile->gateway, CONFIG_NETWORK_HELPER_STATIC_IPV4_GW) == 0;
}

//TODO intent??
static void clear_iface_ipv4(struct net_if *iface)
{
	struct in_addr *addr;

	if (iface == NULL) {
		return;
	}

	while ((addr = net_if_ipv4_get_global_addr(iface, NET_ADDR_ANY_STATE)) != NULL) {
		struct in_addr copy = *addr;
		if (!net_if_ipv4_addr_rm(iface, &copy)) {
			break;
		}
	}
}

static int apply_static_profile(struct net_if *iface,
				const struct network_ipv4_profile *profile,
				enum network_ipv4_source source)
{
	struct in_addr ip;
	struct in_addr subnet;
	struct in_addr gateway;
	struct net_if_addr *if_addr;

	if (iface == NULL || profile == NULL) {
		return -EINVAL;
	}

	if (!parse_ipv4(profile->ip, &ip) || !parse_ipv4(profile->subnet, &subnet)) {
		return -EINVAL;
	}

#if defined(CONFIG_NETWORK_HELPER_ENABLE_DHCP)
	net_dhcpv4_stop(iface);
#endif

	clear_iface_ipv4(iface);

	if_addr = net_if_ipv4_addr_add(iface, &ip, NET_ADDR_MANUAL, 0);
	if (if_addr == NULL) {
		return -EADDRNOTAVAIL;
	}

	(void)net_if_ipv4_set_netmask_by_addr(iface, &ip, &subnet);

	if (parse_ipv4(profile->gateway, &gateway)) {
		net_if_ipv4_set_gw(iface, &gateway);
	}

	active_source = source;
	LOG_INF("Using static IPv4 (%s): %s / %s gw %s",
		network_ipv4_source_str(source),
		profile->ip,
		profile->subnet,
		profile->gateway[0] ? profile->gateway : "0.0.0.0");

	return 0;
}

#if defined(CONFIG_NETWORK_HELPER_ENABLE_DHCP)
static int try_dhcp(struct net_if *iface, uint32_t timeout_ms)
{
	uint32_t elapsed = 0U;
	const uint32_t poll_ms = 100U;

	if (iface == NULL) {
		return -ENODEV;
	}

	net_dhcpv4_restart(iface);

	while (elapsed < timeout_ms || timeout_ms == 0U) {
		if (net_if_ipv4_get_global_addr(iface, NET_ADDR_PREFERRED) != NULL) {
			active_source = NETWORK_IPV4_SOURCE_DHCP;
			LOG_INF("DHCPv4 acquired address");
			return 0;
		}

		k_msleep(poll_ms);
		elapsed += poll_ms;

		if (timeout_ms == 0U && elapsed >= 10000U) {
			LOG_WRN("Still waiting for DHCPv4 address...");
			elapsed = 0U;
		}
	}

	return -ETIMEDOUT;
}
#endif

static void reconnect_work_handler(struct k_work *work)
{
	ARG_UNUSED(work);
	(void)conn_mgr_all_if_connect(true);
}

static void net_l4_evt_handler(struct net_mgmt_event_callback *cb,
                                uint64_t mgmt_event,
                                struct net_if *iface)
{
	ARG_UNUSED(cb);
	ARG_UNUSED(iface);

	switch (mgmt_event) {
	case NET_EVENT_L4_CONNECTED:
		network_online = true;
		LOG_INF("Network up!");
		if (user_event_cb) {
			user_event_cb(true);
		}
		break;
	case NET_EVENT_L4_DISCONNECTED:
		network_online = false;
		LOG_INF("Network down!");
		k_work_reschedule(&reconnect_work, K_MSEC(250));
		if (user_event_cb) {
			user_event_cb(false);
		}
		break;
	default:
		break;
	}
}

static enum network_ipv4_source infer_source_from_iface(struct net_if *iface)
{
	struct in_addr *addr;
	struct net_if *owner = NULL;
	struct net_if_addr *if_addr;

	if (iface == NULL) {
		return active_source;
	}

	addr = net_if_ipv4_get_global_addr(iface, NET_ADDR_PREFERRED);
	if (addr == NULL) {
		return NETWORK_IPV4_SOURCE_UNKNOWN;
	}

	if_addr = net_if_ipv4_addr_lookup(addr, &owner);
	if (if_addr == NULL || owner != iface) {
		return active_source;
	}

	if (if_addr->addr_type == NET_ADDR_DHCP) {
		return NETWORK_IPV4_SOURCE_DHCP;
	}

	return active_source;
}

static void net_ipv4_evt_handler(struct net_mgmt_event_callback *cb,
				 uint64_t mgmt_event,
				 struct net_if *iface)
{
	ARG_UNUSED(cb);

	if (mgmt_event == NET_EVENT_IPV4_ADDR_ADD) {
		struct network_ipv4_info info = {0};
		active_source = infer_source_from_iface(iface);

		if (network_get_ipv4_info(&info) == 0 && info.has_ipv4) {
			LOG_INF("IPv4 address: %s / %s gw %s",
				info.ip, info.netmask, info.gateway);
		}
	} else if (mgmt_event == NET_EVENT_IPV4_ADDR_DEL) {
		active_source = NETWORK_IPV4_SOURCE_UNKNOWN;
	}
}

static void format_in_addr(const struct net_in_addr *addr, char *out, size_t out_len)
{
	if (net_addr_ntop(AF_INET, addr, out, out_len) == NULL) {
		snprintk(out, out_len, "0.0.0.0");
	}
}

static int apply_active_config(struct net_if *iface)
{
	int rc = -EINVAL;

	if (iface == NULL) {
		return -ENODEV;
	}

#if defined(CONFIG_NETWORK_HELPER_ENABLE_DHCP)
	if (active_cfg.try_dhcp_first && network_feature_dhcp_enabled()) {
		rc = try_dhcp(iface, active_cfg.dhcp_timeout_ms);
		if (rc == 0) {
			return 0;
		}
		LOG_WRN("DHCPv4 timed out (%d), trying static", rc);
	}
#endif

	if (profile_has_valid_static_ipv4(&active_cfg.static_profile)) {
		const enum network_ipv4_source source =
			profile_matches_compiled_static_defaults(&active_cfg.static_profile) ?
			NETWORK_IPV4_SOURCE_COMPILED : NETWORK_IPV4_SOURCE_STATIC;
		rc = apply_static_profile(iface, &active_cfg.static_profile, source);
		if (rc == 0) {
			return 0;
		}
		LOG_WRN("Static profile failed (%d)", rc);
	}

	if (active_cfg.enable_fallback_profile &&
	    profile_has_valid_static_ipv4(&active_cfg.fallback_profile)) {
		rc = apply_static_profile(iface, &active_cfg.fallback_profile, NETWORK_IPV4_SOURCE_FALLBACK);
		if (rc == 0) {
			return 0;
		}
		LOG_WRN("Fallback profile failed (%d)", rc);
	}

#if defined(CONFIG_NETWORK_HELPER_ENABLE_DHCP)
	if (!active_cfg.try_dhcp_first && network_feature_dhcp_enabled()) {
		rc = try_dhcp(iface, active_cfg.dhcp_timeout_ms);
		if (rc == 0) {
			return 0;
		}
	}
#endif

	return rc;
}

void network_config_defaults(struct network_config *cfg)
{
	if (cfg == NULL) {
		return;
	}

	memset(cfg, 0, sizeof(*cfg));
	cfg->try_dhcp_first = true;
	cfg->prefer_dhcp_dns = true;
	cfg->prefer_dhcp_ntp = true;
	cfg->enable_fallback_profile = true;
	cfg->dhcp_timeout_ms = CONFIG_NETWORK_HELPER_DHCP_TIMEOUT_MS;

	str_set(cfg->static_profile.ip, sizeof(cfg->static_profile.ip),
		CONFIG_NETWORK_HELPER_STATIC_IPV4_ADDR);
	str_set(cfg->static_profile.subnet, sizeof(cfg->static_profile.subnet),
		CONFIG_NETWORK_HELPER_STATIC_IPV4_NETMASK);
	str_set(cfg->static_profile.gateway, sizeof(cfg->static_profile.gateway),
		CONFIG_NETWORK_HELPER_STATIC_IPV4_GW);
#if defined(CONFIG_NETWORK_HELPER_ENABLE_DNS)
	str_set(cfg->static_profile.dns, sizeof(cfg->static_profile.dns),
		CONFIG_NETWORK_HELPER_STATIC_DNS_IPV4_ADDR);
#endif
#if defined(CONFIG_NETWORK_HELPER_ENABLE_NTP)
	str_set(cfg->static_profile.ntp, sizeof(cfg->static_profile.ntp),
		CONFIG_NETWORK_HELPER_STATIC_NTP_IPV4_ADDR);
#endif

	str_set(cfg->fallback_profile.ip, sizeof(cfg->fallback_profile.ip),
		CONFIG_NETWORK_HELPER_FALLBACK_IPV4_ADDR);
	str_set(cfg->fallback_profile.subnet, sizeof(cfg->fallback_profile.subnet),
		CONFIG_NETWORK_HELPER_FALLBACK_IPV4_NETMASK);
	str_set(cfg->fallback_profile.gateway, sizeof(cfg->fallback_profile.gateway),
		CONFIG_NETWORK_HELPER_FALLBACK_IPV4_GW);

#if defined(CONFIG_NETWORK_HELPER_ENABLE_DHCP)
	if (!network_feature_dhcp_enabled()) {
		cfg->try_dhcp_first = false;
	}
#else
	cfg->try_dhcp_first = false;
#endif
}

int network_get_ipv4_info(struct network_ipv4_info *out)
{
	struct net_if *iface;
	struct net_in_addr *addr;

	if (out == NULL) {
		return -EINVAL;
	}

	memset(out, 0, sizeof(*out));
	out->link_ready = network_online;
	out->source = active_source;
	snprintk(out->ip, sizeof(out->ip), "0.0.0.0");
	snprintk(out->netmask, sizeof(out->netmask), "0.0.0.0");
	snprintk(out->gateway, sizeof(out->gateway), "0.0.0.0");

	iface = net_if_get_default();
	if (iface == NULL) {
		return -ENODEV;
	}

	addr = net_if_ipv4_get_global_addr(iface, NET_ADDR_PREFERRED);
	if (addr == NULL) {
		return 0;
	}

	out->has_ipv4 = true;
	out->source = infer_source_from_iface(iface);
	format_in_addr(addr, out->ip, sizeof(out->ip));

	{
		struct net_in_addr netmask = net_if_ipv4_get_netmask_by_addr(iface, addr);
		struct net_in_addr gateway = net_if_ipv4_get_gw(iface);

		format_in_addr(&netmask, out->netmask, sizeof(out->netmask));
		format_in_addr(&gateway, out->gateway, sizeof(out->gateway));
	}

	return 0;
}

int network_get_active_config(struct network_config *out)
{
	if (out == NULL) {
		return -EINVAL;
	}

	*out = active_cfg;
	return 0;
}

void network_log_mac_addr(void)
{
	struct net_if *iface = net_if_get_default();
	struct net_linkaddr *mac;

	if (!iface) {
		LOG_WRN("No default network interface");
		return;
	}

	mac = net_if_get_link_addr(iface);

	LOG_INF("MAC Address: %02X:%02X:%02X:%02X:%02X:%02X",
		mac->addr[0], mac->addr[1], mac->addr[2],
		mac->addr[3], mac->addr[4], mac->addr[5]);
}

//TODO doesn't this fucntion ALSO need volatile qualifier given the global's qualifier?
bool network_is_ready(void)
{
	return network_online;
}

//todo net_if_get_default seems silly.
int network_reconfigure(const struct network_config *cfg)
{
	int rc;
	struct net_if *iface;

	if (cfg == NULL) {
		return -EINVAL;
	}

	iface = net_if_get_default();
	if (iface == NULL) {
		LOG_ERR("No network interface configured");
		return -ENETDOWN;
	}

	active_cfg = *cfg;
	rc = apply_active_config(iface);
	if (rc != 0) {
		LOG_WRN("No IPv4 configuration could be applied (%d)", rc);
	}

	return rc;
}

int network_init(const struct network_config *cfg, network_event_cb_t event_cb)
{
	int rc;
	struct net_if *iface;

	user_event_cb = event_cb;

	iface = net_if_get_default();
	if (iface == NULL) {
		LOG_ERR("No network interface configured");
		return -ENETDOWN;
	}

	if (cfg != NULL) {
		active_cfg = *cfg;
	} else {
		network_config_defaults(&active_cfg);
	}

	if (!network_initialized) {
		k_work_init_delayable(&reconnect_work, reconnect_work_handler);

		net_mgmt_init_event_callback(&net_l4_mgmt_cb, net_l4_evt_handler, NET_L4_EVENT_MASK);
		net_mgmt_add_event_callback(&net_l4_mgmt_cb);

		net_mgmt_init_event_callback(&net_ipv4_mgmt_cb, net_ipv4_evt_handler,
					     NET_EVENT_IPV4_ADDR_ADD | NET_EVENT_IPV4_ADDR_DEL);
		net_mgmt_add_event_callback(&net_ipv4_mgmt_cb);

		network_initialized = true;
	}

	network_log_mac_addr();


	LOG_INF("Bringing up network interfaces...");

	rc = conn_mgr_all_if_up(true);
	if (rc) {
		LOG_ERR("conn_mgr_all_if_up() failed (%d)", rc);
		return rc;
	}

	rc = conn_mgr_all_if_connect(true);
	if (rc) {
		LOG_WRN("conn_mgr_all_if_connect() failed (%d)", rc);
	}

	conn_mgr_mon_resend_status();

	return network_reconfigure(&active_cfg);
}

int network_wait_ready(uint32_t timeout_ms)
{
	uint32_t elapsed = 0;
	const uint32_t check_interval = 100; /* ms */

	LOG_INF("Waiting for network connection...");

	if (timeout_ms == 0) {
		/* Wait forever */
		while (!network_online) {
			k_msleep(check_interval);
			elapsed += check_interval;
			if (elapsed >= 10000) { /* Log every 10 seconds */
				LOG_WRN("Network not ready yet (waiting...)");
				elapsed = 0;
			}
		}
	} else {
		/* Wait with timeout */
		while (!network_online && elapsed < timeout_ms) {
			k_msleep(check_interval);
			elapsed += check_interval;
		}

		if (!network_online) {
			LOG_ERR("Network connection timeout after %u ms", timeout_ms);
			return -ETIMEDOUT;
		}
	}

	LOG_INF("Network stack ready (DHCP or static IP set).");
	return 0;
}

const char *network_ipv4_source_str(enum network_ipv4_source source)
{
	switch (source) {
	case NETWORK_IPV4_SOURCE_COMPILED:
		return "compiled";
	case NETWORK_IPV4_SOURCE_STATIC:
		return "static";
	case NETWORK_IPV4_SOURCE_FALLBACK:
		return "fallback";
	case NETWORK_IPV4_SOURCE_DHCP:
		return "dhcp";
	case NETWORK_IPV4_SOURCE_UNKNOWN:
	default:
		return "unknown";
	}
}

bool network_feature_dhcp_enabled(void)
{
#if defined(CONFIG_NETWORK_HELPER_ENABLE_DHCP)
	return true;
#else
	return false;
#endif
}

bool network_feature_dns_enabled(void)
{
#if defined(CONFIG_NETWORK_HELPER_ENABLE_DNS)
	return true;
#else
	return false;
#endif
}

bool network_feature_ntp_enabled(void)
{
#if defined(CONFIG_NETWORK_HELPER_ENABLE_NTP)
	return true;
#else
	return false;
#endif
}
