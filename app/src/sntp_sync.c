/*
 * HiSPEC-TIB SNTP synchronization helpers.
 * Copyright (c) 2026 Caltech Optical Observatories
 * SPDX-License-Identifier: Apache-2.0
 */

#include "sntp_sync.h"

#include <errno.h>
#include <string.h>
#include <time.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/net/net_if.h>
#include <zephyr/net/net_ip.h>
#if defined(CONFIG_SNTP)
#include <zephyr/net/sntp.h>
#endif

#include <coo_commons/network.h>

#include "app_settings.h"

LOG_MODULE_REGISTER(sntp_sync, LOG_LEVEL_INF);

#define SNTP_SYNC_TIMEOUT_MS 3000U
#define SNTP_SYNC_RETRY_INTERVAL_MS 30000U
#define SNTP_SYNC_RESYNC_INTERVAL_MS 3600000U

struct sntp_sync_runtime {
	struct k_mutex lock;
	struct k_work_delayable sync_work;
	struct sntp_sync_status status;
	bool initialized;
};

static struct sntp_sync_runtime g_sntp;

const char *sntp_sync_source_str(enum sntp_sync_source source)
{
	switch (source) {
	case SNTP_SYNC_SOURCE_MANUAL:
		return "manual";
	case SNTP_SYNC_SOURCE_DHCP:
		return "dhcp";
	case SNTP_SYNC_SOURCE_NONE:
	default:
		return "none";
	}
}

static void set_status_server(enum sntp_sync_source source, const char *server)
{
	k_mutex_lock(&g_sntp.lock, K_FOREVER);
	g_sntp.status.source = source;
	if (server == NULL) {
		g_sntp.status.server[0] = '\0';
	} else {
		strncpy(g_sntp.status.server, server, sizeof(g_sntp.status.server) - 1U);
		g_sntp.status.server[sizeof(g_sntp.status.server) - 1U] = '\0';
	}
	k_mutex_unlock(&g_sntp.lock);
}

static void set_status_result(int rc, uint64_t utc_ms)
{
	k_mutex_lock(&g_sntp.lock, K_FOREVER);
	g_sntp.status.last_error = rc;
	if (rc == 0) {
		g_sntp.status.synced = true;
		g_sntp.status.last_sync_utc_ms = utc_ms;
		g_sntp.status.last_sync_uptime_ms = k_uptime_get();
	}
	k_mutex_unlock(&g_sntp.lock);
}

static bool parse_ipv4_nonzero(const char *text, struct in_addr *out)
{
	struct in_addr addr = {0};

	if (text == NULL || out == NULL || text[0] == '\0') {
		return false;
	}

	if (net_addr_pton(AF_INET, text, &addr) != 0) {
		return false;
	}

	if (net_ipv4_is_addr_unspecified(&addr)) {
		return false;
	}

	*out = addr;
	return true;
}

static bool copy_dhcp_ntp_server(char *out, size_t out_len)
{
#if defined(CONFIG_NET_DHCPV4_OPTION_NTP_SERVER)
	struct net_if *iface;

	if (out == NULL || out_len == 0U) {
		return false;
	}

	iface = net_if_get_default();
	if (iface == NULL) {
		return false;
	}

	if (net_ipv4_is_addr_unspecified(&iface->config.dhcpv4.ntp_addr)) {
		return false;
	}

	return net_addr_ntop(AF_INET, &iface->config.dhcpv4.ntp_addr, out, out_len) != NULL;
#else
	ARG_UNUSED(out);
	ARG_UNUSED(out_len);
	return false;
#endif
}

static enum sntp_sync_source choose_ntp_server(char *out, size_t out_len)
{
	struct app_ip_settings ip_cfg = {0};
	struct in_addr manual = {0};

	if (out == NULL || out_len == 0U) {
		return SNTP_SYNC_SOURCE_NONE;
	}

	app_settings_get_ip(&ip_cfg);

	if (parse_ipv4_nonzero(ip_cfg.ntp, &manual)) {
		strncpy(out, ip_cfg.ntp, out_len - 1U);
		out[out_len - 1U] = '\0';
		return SNTP_SYNC_SOURCE_MANUAL;
	}

	/* Design intent: DHCP-provided NTP is used when no manual override exists. */
	if (copy_dhcp_ntp_server(out, out_len)) {
		return SNTP_SYNC_SOURCE_DHCP;
	}

	out[0] = '\0';
	return SNTP_SYNC_SOURCE_NONE;
}

#if defined(CONFIG_SNTP)
static int apply_sntp_time(const struct sntp_time *sntp_time, uint64_t *utc_ms_out)
{
	struct timespec ts = {0};
	uint64_t utc_ms;

	if (sntp_time == NULL) {
		return -EINVAL;
	}

	ts.tv_sec = (time_t)sntp_time->seconds;
	ts.tv_nsec = (long)(((uint64_t)sntp_time->fraction * NSEC_PER_SEC) >> 32);
	if (clock_settime(CLOCK_REALTIME, &ts) != 0) {
		return -errno;
	}

	utc_ms = ((uint64_t)ts.tv_sec * 1000ULL) + ((uint64_t)ts.tv_nsec / 1000000ULL);
	if (utc_ms_out != NULL) {
		*utc_ms_out = utc_ms;
	}

	return 0;
}

static int sntp_sync_now_internal(void)
{
	char server[NET_IPV4_ADDR_LEN] = {0};
	enum sntp_sync_source source;
	struct sntp_time sntp_time = {0};
	uint64_t utc_ms = 0U;
	int rc;

	if (!network_is_ready()) {
		return -ENETDOWN;
	}

	source = choose_ntp_server(server, sizeof(server));
	set_status_server(source, server);

	if (source == SNTP_SYNC_SOURCE_NONE) {
		return -ENOENT;
	}

	rc = sntp_simple(server, SNTP_SYNC_TIMEOUT_MS, &sntp_time);
	if (rc != 0) {
		return rc;
	}

	rc = apply_sntp_time(&sntp_time, &utc_ms);
	if (rc != 0) {
		return rc;
	}

	LOG_INF("SNTP sync complete (%s: %s)", sntp_sync_source_str(source), server);
	set_status_result(0, utc_ms);
	return 0;
}
#else
static int sntp_sync_now_internal(void)
{
	return -ENOTSUP;
}
#endif

static void sntp_sync_work_handler(struct k_work *work)
{
	int rc;
	uint32_t next_ms;

	ARG_UNUSED(work);

	rc = sntp_sync_now_internal();
	if (rc != 0) {
		LOG_WRN("SNTP sync failed (%d)", rc);
		set_status_result(rc, 0U);
		next_ms = SNTP_SYNC_RETRY_INTERVAL_MS;
	} else {
		next_ms = SNTP_SYNC_RESYNC_INTERVAL_MS;
	}

	(void)k_work_reschedule(&g_sntp.sync_work, K_MSEC(next_ms));
}

void sntp_sync_init(void)
{
	memset(&g_sntp, 0, sizeof(g_sntp));
	k_mutex_init(&g_sntp.lock);
	k_work_init_delayable(&g_sntp.sync_work, sntp_sync_work_handler);

	k_mutex_lock(&g_sntp.lock, K_FOREVER);
#if defined(CONFIG_SNTP)
	g_sntp.status.enabled = true;
	g_sntp.status.last_error = -ENETDOWN;
#else
	g_sntp.status.enabled = false;
	g_sntp.status.last_error = -ENOTSUP;
#endif
	g_sntp.status.source = SNTP_SYNC_SOURCE_NONE;
	g_sntp.initialized = true;
	k_mutex_unlock(&g_sntp.lock);

#if defined(CONFIG_SNTP)
	(void)k_work_schedule(&g_sntp.sync_work, K_SECONDS(1));
#endif
}

void sntp_sync_schedule_now(void)
{
	if (!g_sntp.initialized) {
		return;
	}

#if defined(CONFIG_SNTP)
	(void)k_work_reschedule(&g_sntp.sync_work, K_NO_WAIT);
#endif
}

void sntp_sync_get_status(struct sntp_sync_status *out)
{
	if (out == NULL) {
		return;
	}

	if (!g_sntp.initialized) {
		memset(out, 0, sizeof(*out));
		out->source = SNTP_SYNC_SOURCE_NONE;
		out->last_error = -ENODEV;
		return;
	}

	k_mutex_lock(&g_sntp.lock, K_FOREVER);
	*out = g_sntp.status;
	k_mutex_unlock(&g_sntp.lock);
}
