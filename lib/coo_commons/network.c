/**
 * @file network.c
 * @brief IPv4 Ethernet policy wrapper for DHCP/static/fallback selection.
 *
 * This helper owns app-level address policy while leaving link, DHCP, DNS, and
 * interface primitives to Zephyr. It deliberately does not parse commands or
 * persist operator settings.
 */
/*
 * Copyright (c) 2024 Caltech Optical Observatories
 * SPDX-License-Identifier: Apache-2.0
 */

#include <coo_commons/network.h>

#include <errno.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/net/conn_mgr_connectivity.h>
#include <zephyr/net/conn_mgr_monitor.h>
#include <zephyr/net/ethernet.h>
#include <zephyr/net/net_event.h>
#include <zephyr/net/net_if.h>
#include <zephyr/net/net_ip.h>
#include <zephyr/net/net_mgmt.h>
#include <zephyr/sys/util.h>
#if defined(CONFIG_DNS_RESOLVER)
#include <zephyr/net/dns_resolve.h>
#endif
#if defined(CONFIG_NET_DHCPV4)
#include <zephyr/net/dhcpv4.h>
#endif

LOG_MODULE_REGISTER(network, LOG_LEVEL_DBG);

static bool network_online;
static bool dhcp_first_pending;
static int64_t dhcp_start_uptime_ms = -1;
static enum network_ipv4_source active_source = NETWORK_IPV4_SOURCE_UNKNOWN;
static struct network_config active_cfg;
static network_event_cb_t user_event_cb;

static struct net_mgmt_event_callback net_l4_mgmt_cb;
static struct net_mgmt_event_callback net_iface_mgmt_cb;
static struct net_mgmt_event_callback net_ipv4_mgmt_cb;
static struct k_work_delayable reconnect_work;
static struct k_work_delayable dhcp_fallback_work;
static bool network_initialized;

#define NET_L4_EVENT_MASK (NET_EVENT_L4_CONNECTED | NET_EVENT_L4_DISCONNECTED)

/* Tiny local copies keep profile/default setup readable without dynamic
 * allocation or repeating strncpy termination rules.
 */
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

static bool parse_ipv4(const char *text, struct in_addr *out)
{
	if (text == NULL || out == NULL || text[0] == '\0') {
		return false;
	}

	return net_addr_pton(AF_INET, text, out) == 0;
}

static bool parse_ipv4_nonzero(const char *text, struct in_addr *out)
{
	struct in_addr addr = {0};

	if (!parse_ipv4(text, &addr) || net_ipv4_is_addr_unspecified(&addr)) {
		return false;
	}

	if (out != NULL) {
		*out = addr;
	}
	return true;
}

#if defined(CONFIG_DNS_RESOLVER)
static int configure_manual_dns(const struct network_ipv4_profile *profile)
{
	struct dns_resolve_context *ctx;
	struct in_addr dns = {0};
	const char *servers[2];
	int rc;

	if (profile == NULL || profile->dns[0] == '\0') {
		return 0;
	}
	if (!parse_ipv4_nonzero(profile->dns, &dns)) {
		return -EINVAL;
	}

	ctx = dns_resolve_get_default();
	servers[0] = profile->dns;
	servers[1] = NULL;
	rc = dns_resolve_reconfigure(ctx, servers, NULL, DNS_SOURCE_MANUAL);
	if (rc != 0) {
		LOG_WRN("Manual DNS reconfigure failed (%d)", rc);
	}

	return rc;
}
#else
static int configure_manual_dns(const struct network_ipv4_profile *profile)
{
	ARG_UNUSED(profile);
	return 0;
}
#endif

static bool profile_has_valid_static_ipv4(const struct network_ipv4_profile *profile)
{
	struct in_addr ip = {0};
	struct in_addr subnet = {0};

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

#if defined(CONFIG_NET_CONFIG_MY_IPV4_ADDR) && \
	defined(CONFIG_NET_CONFIG_MY_IPV4_NETMASK) && \
	defined(CONFIG_NET_CONFIG_MY_IPV4_GW)
	return strcmp(profile->ip, CONFIG_NET_CONFIG_MY_IPV4_ADDR) == 0 &&
	       strcmp(profile->subnet, CONFIG_NET_CONFIG_MY_IPV4_NETMASK) == 0 &&
	       strcmp(profile->gateway, CONFIG_NET_CONFIG_MY_IPV4_GW) == 0;
#else
	return false;
#endif
}

static enum network_ipv4_source source_for_profile(const struct network_ipv4_profile *profile)
{
	return profile_matches_compiled_static_defaults(profile) ?
	       NETWORK_IPV4_SOURCE_COMPILED : NETWORK_IPV4_SOURCE_STATIC;
}

static void compiled_static_profile(struct network_ipv4_profile *profile)
{
	if (profile == NULL) {
		return;
	}

	memset(profile, 0, sizeof(*profile));
#if defined(CONFIG_NET_CONFIG_MY_IPV4_ADDR) && \
	defined(CONFIG_NET_CONFIG_MY_IPV4_NETMASK) && \
	defined(CONFIG_NET_CONFIG_MY_IPV4_GW)
	str_set(profile->ip, sizeof(profile->ip), CONFIG_NET_CONFIG_MY_IPV4_ADDR);
	str_set(profile->subnet, sizeof(profile->subnet), CONFIG_NET_CONFIG_MY_IPV4_NETMASK);
	str_set(profile->gateway, sizeof(profile->gateway), CONFIG_NET_CONFIG_MY_IPV4_GW);
#else
	str_set(profile->ip, sizeof(profile->ip), "0.0.0.0");
	str_set(profile->subnet, sizeof(profile->subnet), "0.0.0.0");
	str_set(profile->gateway, sizeof(profile->gateway), "0.0.0.0");
#endif
}

/* Use the first Ethernet L2 interface; this target has one managed port. */
static struct net_if *network_iface(void)
{
	return net_if_get_first_by_type(&NET_L2_GET_NAME(ETHERNET));
}

static void notify_ready(bool ready)
{
	if (network_online == ready) {
		return;
	}

	network_online = ready;
	LOG_INF("Network %s!", ready ? "up" : "down");
	if (user_event_cb != NULL) {
		user_event_cb(ready);
	}
}

/* Remove all global IPv4 addresses before applying a policy-selected profile. */
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

static void format_in_addr(const struct net_in_addr *addr, char *out, size_t out_len)
{
	if (net_addr_ntop(AF_INET, addr, out, out_len) == NULL) {
		snprintk(out, out_len, "0.0.0.0");
	}
}

static void log_current_ipv4(struct net_if *iface)
{
	struct network_ipv4_info info = {0};

	ARG_UNUSED(iface);

	if (network_get_ipv4_info(&info) == 0 && info.has_ipv4) {
		LOG_INF("IPv4 address: %s / %s gw %s",
			info.ip, info.netmask, info.gateway);
	}
}

static int add_static_profile(struct net_if *iface,
			      const struct network_ipv4_profile *profile,
			      enum network_ipv4_source source,
			      enum net_addr_type addr_type)
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

	clear_iface_ipv4(iface);
	active_source = source;

	/* In DHCP-first mode, NET_ADDR_OVERRIDABLE matches Zephyr net_config's
	 * static-with-DHCP behavior: one IPv4 slot can provide service now, then
	 * be replaced by a later DHCP lease without custom address juggling.
	 */
	if_addr = net_if_ipv4_addr_add(iface, &ip, addr_type, 0);
	if (if_addr == NULL) {
		active_source = NETWORK_IPV4_SOURCE_UNKNOWN;
		return -EADDRNOTAVAIL;
	}

	(void)net_if_ipv4_set_netmask_by_addr(iface, &ip, &subnet);

	if (parse_ipv4(profile->gateway, &gateway)) {
		net_if_ipv4_set_gw(iface, &gateway);
	}

	if (configure_manual_dns(profile) != 0) {
		return -EINVAL;
	}

	LOG_INF("Using static IPv4 (%s%s): %s / %s gw %s",
		network_ipv4_source_str(source),
		addr_type == NET_ADDR_OVERRIDABLE ? ", DHCP may override" : "",
		profile->ip,
		profile->subnet,
		profile->gateway[0] ? profile->gateway : "0.0.0.0");
	notify_ready(true);
	return 0;
}

static int apply_manual_static(struct net_if *iface,
			       const struct network_ipv4_profile *profile,
			       enum network_ipv4_source source)
{
#if defined(CONFIG_NET_DHCPV4)
	net_dhcpv4_stop(iface);
#endif
	dhcp_first_pending = false;
	k_work_cancel_delayable(&dhcp_fallback_work);

	return add_static_profile(iface, profile, source, NET_ADDR_MANUAL);
}

static int apply_compiled_fallback(struct net_if *iface, enum net_addr_type addr_type)
{
	struct network_ipv4_profile fallback;

	if (!active_cfg.enable_fallback_profile) {
		return -EINVAL;
	}

	compiled_static_profile(&fallback);
	if (!profile_has_valid_static_ipv4(&fallback)) {
		return -EINVAL;
	}

	return add_static_profile(iface, &fallback, NETWORK_IPV4_SOURCE_FALLBACK, addr_type);
}

#if defined(CONFIG_NET_DHCPV4)
static bool iface_has_preferred_dhcp_addr(struct net_if *iface)
{
	struct in_addr *addr;
	struct net_if *owner = NULL;
	struct net_if_addr *if_addr;

	if (iface == NULL) {
		return false;
	}

	addr = net_if_ipv4_get_global_addr(iface, NET_ADDR_PREFERRED);
	if (addr == NULL) {
		return false;
	}

	if_addr = net_if_ipv4_addr_lookup(addr, &owner);
	return if_addr != NULL && owner == iface &&
	       if_addr->addr_type == NET_ADDR_DHCP;
}

static void start_dhcp_on_ready_iface(struct net_if *iface)
{
	if (iface == NULL) {
		return;
	}

	if (!net_if_is_carrier_ok(iface) || !net_if_is_up(iface)) {
		LOG_INF("DHCPv4 pending; waiting for interface up");
		return;
	}

	/* Restart when the interface is operationally up. Starting DHCP while
	 * the PHY is still down can burn the immediate request and leave fallback
	 * racing Zephyr's later retry instead of a real first attempt on the wire.
	 */
	net_dhcpv4_restart(iface);
	dhcp_start_uptime_ms = k_uptime_get();
	k_work_reschedule(&dhcp_fallback_work, K_MSEC(active_cfg.dhcp_timeout_ms));
	LOG_INF("DHCPv4 started on interface up; static fallback in %u ms",
		active_cfg.dhcp_timeout_ms);
}

static void start_dhcp_first(struct net_if *iface)
{
	if (iface == NULL) {
		return;
	}

	dhcp_first_pending = true;
	active_source = NETWORK_IPV4_SOURCE_UNKNOWN;
	notify_ready(false);

	/* Stop first so any prior DHCP-owned address/state is released before
	 * this policy run clears the IPv4 slot and asks Zephyr for a fresh lease.
	 */
	net_dhcpv4_stop(iface);
	clear_iface_ipv4(iface);
	start_dhcp_on_ready_iface(iface);
}
#endif

static void dhcp_fallback_work_handler(struct k_work *work)
{
	struct net_if *iface;
	int rc;

	ARG_UNUSED(work);

#if defined(CONFIG_NET_DHCPV4)
	if (!dhcp_first_pending) {
		return;
	}
#endif

	iface = network_iface();
	if (iface == NULL) {
		LOG_WRN("No Ethernet interface for DHCP fallback");
		return;
	}

#if defined(CONFIG_NET_DHCPV4)
	if (iface_has_preferred_dhcp_addr(iface)) {
		dhcp_first_pending = false;
		return;
	}
#endif

	if (profile_has_valid_static_ipv4(&active_cfg.static_profile)) {
		rc = add_static_profile(iface, &active_cfg.static_profile,
					source_for_profile(&active_cfg.static_profile),
					NET_ADDR_OVERRIDABLE);
	} else {
		rc = apply_compiled_fallback(iface, NET_ADDR_OVERRIDABLE);
	}

	if (rc != 0) {
		LOG_WRN("DHCP fallback static profile failed (%d)", rc);
		return;
	}

	LOG_WRN("DHCPv4 not bound after %lld ms; using overridable static profile",
		dhcp_start_uptime_ms >= 0 ? k_uptime_get() - dhcp_start_uptime_ms : -1);
}

static int apply_active_config(struct net_if *iface)
{
	int rc = -EINVAL;

	if (iface == NULL) {
		return -ENODEV;
	}

	rc = conn_mgr_all_if_connect(true);
	if (rc != 0) {
		if (!net_if_is_up(iface)) {
			return rc;
		}
		LOG_WRN("conn_mgr_all_if_connect() failed on up interface (%d)", rc);
	}

#if defined(CONFIG_NET_DHCPV4)
	if (active_cfg.try_dhcp_first) {
		start_dhcp_first(iface);
		return 0;
	}
#endif

	if (profile_has_valid_static_ipv4(&active_cfg.static_profile)) {
		rc = apply_manual_static(iface, &active_cfg.static_profile,
					 source_for_profile(&active_cfg.static_profile));
		if (rc == 0) {
			return 0;
		}
		LOG_WRN("Static profile failed (%d)", rc);
	}

	rc = apply_compiled_fallback(iface, NET_ADDR_MANUAL);
	if (rc == 0) {
		return 0;
	}
	LOG_WRN("Compiled fallback profile failed (%d)", rc);

#if defined(CONFIG_NET_DHCPV4)
	start_dhcp_first(iface);
	return 0;
#else
	return rc;
#endif
}

static void reconnect_work_handler(struct k_work *work)
{
	int rc;

	ARG_UNUSED(work);

	rc = conn_mgr_all_if_connect(true);
	if (rc != 0) {
		LOG_WRN("conn_mgr_all_if_connect() failed (%d)", rc);
	}
}

static void net_l4_evt_handler(struct net_mgmt_event_callback *cb,
			       uint64_t mgmt_event,
			       struct net_if *iface)
{
	ARG_UNUSED(cb);
	ARG_UNUSED(iface);

	if (mgmt_event == NET_EVENT_L4_CONNECTED) {
		notify_ready(true);
	} else if (mgmt_event == NET_EVENT_L4_DISCONNECTED) {
		k_work_reschedule(&reconnect_work, K_MSEC(250));
		notify_ready(false);
	}
}

static void net_iface_evt_handler(struct net_mgmt_event_callback *cb,
				  uint64_t mgmt_event,
				  struct net_if *iface)
{
	ARG_UNUSED(cb);

	if (iface != network_iface()) {
		return;
	}

	if (mgmt_event == NET_EVENT_IF_UP) {
#if defined(CONFIG_NET_DHCPV4)
		if (dhcp_first_pending && !iface_has_preferred_dhcp_addr(iface)) {
			start_dhcp_on_ready_iface(iface);
		}
#endif
	} else if (mgmt_event == NET_EVENT_IF_DOWN) {
		dhcp_first_pending = active_cfg.try_dhcp_first;
		active_source = NETWORK_IPV4_SOURCE_UNKNOWN;
		k_work_cancel_delayable(&dhcp_fallback_work);
		notify_ready(false);
		k_work_reschedule(&reconnect_work, K_MSEC(250));
	}
}

static void net_ipv4_evt_handler(struct net_mgmt_event_callback *cb,
				 uint64_t mgmt_event,
				 struct net_if *iface)
{
	ARG_UNUSED(cb);

	if (iface != network_iface()) {
		return;
	}

	if (mgmt_event == NET_EVENT_IPV4_DHCP_BOUND) {
		const bool was_ready = network_online;

		dhcp_first_pending = false;
		k_work_cancel_delayable(&dhcp_fallback_work);
		active_source = NETWORK_IPV4_SOURCE_DHCP;
#if defined(CONFIG_DNS_RESOLVER)
		if (!active_cfg.prefer_dhcp_dns &&
		    configure_manual_dns(&active_cfg.static_profile) != 0) {
			LOG_WRN("Manual DNS override failed after DHCP bound");
		}
#endif
		LOG_INF("DHCPv4 acquired address after %lld ms",
			dhcp_start_uptime_ms >= 0 ? k_uptime_get() - dhcp_start_uptime_ms : -1);
		log_current_ipv4(iface);
		notify_ready(true);
		/* A DHCP lease may arrive after an overridable static fallback already
		 * made IPv4 "ready". Refresh the app callback so DHCP-derived options
		 * such as NTP can be consumed immediately.
		 */
		if (was_ready && user_event_cb != NULL) {
			user_event_cb(true);
		}
	} else if (mgmt_event == NET_EVENT_IPV4_ADDR_ADD) {
		active_source = infer_source_from_iface(iface);
		log_current_ipv4(iface);
	} else if (mgmt_event == NET_EVENT_IPV4_ADDR_DEL) {
		active_source = infer_source_from_iface(iface);
		if (active_source == NETWORK_IPV4_SOURCE_UNKNOWN) {
			notify_ready(false);
		}
	}
}

void network_config_defaults(struct network_config *cfg)
{
	if (cfg == NULL) {
		return;
	}

	memset(cfg, 0, sizeof(*cfg));
#if defined(CONFIG_NET_DHCPV4)
	cfg->try_dhcp_first = true;
#else
	cfg->try_dhcp_first = false;
#endif
	cfg->prefer_dhcp_dns = true;
	cfg->prefer_dhcp_ntp = true;
	cfg->enable_fallback_profile = true;
	cfg->dhcp_timeout_ms = (uint32_t)CONFIG_NETWORK_HELPER_DHCP_TIMEOUT_MS;

	compiled_static_profile(&cfg->static_profile);
#if defined(CONFIG_DNS_RESOLVER)
	str_set(cfg->static_profile.dns, sizeof(cfg->static_profile.dns), "0.0.0.0");
#endif
#if defined(CONFIG_SNTP)
	str_set(cfg->static_profile.ntp, sizeof(cfg->static_profile.ntp), "0.0.0.0");
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

	iface = network_iface();
	if (iface == NULL) {
		return -ENETDOWN;
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
	struct net_if *iface = network_iface();
	struct net_linkaddr *mac;

	if (!iface) {
		LOG_WRN("No Ethernet network interface");
		return;
	}

	mac = net_if_get_link_addr(iface);

	LOG_INF("MAC Address: %02X:%02X:%02X:%02X:%02X:%02X",
		mac->addr[0], mac->addr[1], mac->addr[2],
		mac->addr[3], mac->addr[4], mac->addr[5]);
}

bool network_is_ready(void)
{
	return network_online;
}

int network_reconfigure(const struct network_config *cfg)
{
	struct net_if *iface;
	int rc;

	if (cfg == NULL) {
		return -EINVAL;
	}

	iface = network_iface();
	if (iface == NULL) {
		LOG_ERR("No Ethernet network interface configured");
		return -ENETDOWN;
	}

	k_work_cancel_delayable(&dhcp_fallback_work);
	active_cfg = *cfg;
	rc = apply_active_config(iface);
	if (rc != 0) {
		LOG_WRN("No IPv4 configuration could be applied (%d)", rc);
		active_source = NETWORK_IPV4_SOURCE_UNKNOWN;
		notify_ready(false);
	}

	return rc;
}

int network_init(const struct network_config *cfg, network_event_cb_t event_cb)
{
	struct net_if *iface;
	int rc;

	user_event_cb = event_cb;

	iface = network_iface();
	if (iface == NULL) {
		LOG_ERR("No Ethernet network interface configured");
		return -ENETDOWN;
	}

	if (cfg != NULL) {
		active_cfg = *cfg;
	} else {
		network_config_defaults(&active_cfg);
	}

	if (!network_initialized) {
		k_work_init_delayable(&reconnect_work, reconnect_work_handler);
		k_work_init_delayable(&dhcp_fallback_work, dhcp_fallback_work_handler);

		net_mgmt_init_event_callback(&net_l4_mgmt_cb, net_l4_evt_handler,
					     NET_L4_EVENT_MASK);
		net_mgmt_add_event_callback(&net_l4_mgmt_cb);

		net_mgmt_init_event_callback(&net_iface_mgmt_cb, net_iface_evt_handler,
					     NET_EVENT_IF_UP | NET_EVENT_IF_DOWN);
		net_mgmt_add_event_callback(&net_iface_mgmt_cb);

		net_mgmt_init_event_callback(&net_ipv4_mgmt_cb, net_ipv4_evt_handler,
					     NET_EVENT_IPV4_ADDR_ADD |
					     NET_EVENT_IPV4_ADDR_DEL |
					     NET_EVENT_IPV4_DHCP_BOUND);
		net_mgmt_add_event_callback(&net_ipv4_mgmt_cb);

		network_initialized = true;
	}

	network_log_mac_addr();
	LOG_INF("Bringing up network interface...");

	rc = network_reconfigure(&active_cfg);
	conn_mgr_mon_resend_status();
	return rc;
}

int network_wait_ready(uint32_t timeout_ms)
{
	uint32_t elapsed = 0;
	const uint32_t check_interval = 100; /* ms */

	LOG_INF("Waiting for IPv4 readiness...");

	if (timeout_ms == 0) {
		while (!network_online) {
			k_msleep(check_interval);
			elapsed += check_interval;
			if (elapsed >= 10000) {
				LOG_WRN("Network not ready yet (waiting...)");
				elapsed = 0;
			}
		}
	} else {
		while (!network_online && elapsed < timeout_ms) {
			k_msleep(check_interval);
			elapsed += check_interval;
		}

		if (!network_online) {
			LOG_ERR("IPv4 readiness timeout after %u ms", timeout_ms);
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
