/*
 * HiSPEC-TIB settings persistence helpers.
 * Copyright (c) 2026 Caltech Optical Observatories
 * SPDX-License-Identifier: Apache-2.0
 */

#include "app_settings.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/settings/settings.h>

LOG_MODULE_REGISTER(app_settings, LOG_LEVEL_INF);

#define APP_SETTINGS_SERIAL_HOLDOFF_DEFAULT_S 30U

#define KEY_SERIAL_HOLDOFF "tib/serial/holdoff_s"
#define KEY_BOOT_COUNT "tib/boot_count"
#define KEY_IP_TRY_DHCP "tib/ip/trydhcpfirst"
#define KEY_IP_PREF_DNS "tib/ip/preferdhcpdns"
#define KEY_IP_PREF_NTP "tib/ip/preferdhcpntp"
#define KEY_IP_ADDR "tib/ip/ip"
#define KEY_IP_SUBNET "tib/ip/subnet"
#define KEY_IP_GATEWAY "tib/ip/gateway"
#define KEY_IP_DNS "tib/ip/dns"
#define KEY_IP_NTP "tib/ip/ntp"
#define KEY_MQTT_HOST "tib/mqtt/host"
#define KEY_MQTT_PORT "tib/mqtt/port"

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

static int read_u16(settings_read_cb read_cb, void *cb_arg, uint16_t *out)
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

static int settings_set_cb(const char *name, size_t len, settings_read_cb read_cb, void *cb_arg)
{
	ARG_UNUSED(len);

	k_mutex_lock(&g_settings.lock, K_FOREVER);

	if (strcmp(name, "serial/holdoff_s") == 0) {
		(void)read_u32(read_cb, cb_arg, &g_settings.snapshot.serial_holdoff_s);
		goto out;
	}

	if (strcmp(name, "boot_count") == 0) {
		(void)read_u32(read_cb, cb_arg, &g_settings.snapshot.boot_count);
		goto out;
	}

	if (strcmp(name, "ip/trydhcpfirst") == 0) {
		(void)read_bool(read_cb, cb_arg, &g_settings.snapshot.ip.try_dhcp_first);
		goto out;
	}

	if (strcmp(name, "ip/preferdhcpdns") == 0) {
		(void)read_bool(read_cb, cb_arg, &g_settings.snapshot.ip.prefer_dhcp_dns);
		goto out;
	}

	if (strcmp(name, "ip/preferdhcpntp") == 0) {
		(void)read_bool(read_cb, cb_arg, &g_settings.snapshot.ip.prefer_dhcp_ntp);
		goto out;
	}

	if (strcmp(name, "ip/ip") == 0) {
		(void)read_str(read_cb, cb_arg, g_settings.snapshot.ip.ip, sizeof(g_settings.snapshot.ip.ip));
		goto out;
	}

	if (strcmp(name, "ip/subnet") == 0) {
		(void)read_str(read_cb, cb_arg, g_settings.snapshot.ip.subnet, sizeof(g_settings.snapshot.ip.subnet));
		goto out;
	}

	if (strcmp(name, "ip/gateway") == 0) {
		(void)read_str(read_cb, cb_arg, g_settings.snapshot.ip.gateway, sizeof(g_settings.snapshot.ip.gateway));
		goto out;
	}

	if (strcmp(name, "ip/dns") == 0) {
		(void)read_str(read_cb, cb_arg, g_settings.snapshot.ip.dns, sizeof(g_settings.snapshot.ip.dns));
		goto out;
	}

	if (strcmp(name, "ip/ntp") == 0) {
		(void)read_str(read_cb, cb_arg, g_settings.snapshot.ip.ntp, sizeof(g_settings.snapshot.ip.ntp));
		goto out;
	}

	if (strcmp(name, "mqtt/host") == 0) {
		(void)read_str(read_cb, cb_arg,
			       g_settings.snapshot.mqtt.broker_host,
			       sizeof(g_settings.snapshot.mqtt.broker_host));
		goto out;
	}

	if (strcmp(name, "mqtt/port") == 0) {
		(void)read_u16(read_cb, cb_arg, &g_settings.snapshot.mqtt.broker_port);
		goto out;
	}

out:
	k_mutex_unlock(&g_settings.lock);
	return 0;
}

SETTINGS_STATIC_HANDLER_DEFINE(tib_settings, "tib",
			       NULL,
			       settings_set_cb,
			       NULL,
			       NULL);

static void persist_bool(const char *key, bool value)
{
	uint8_t v = value ? 1U : 0U;
	(void)settings_save_one(key, &v, sizeof(v));
}

static void persist_u32(const char *key, uint32_t value)
{
	(void)settings_save_one(key, &value, sizeof(value));
}

static void persist_u16(const char *key, uint16_t value)
{
	(void)settings_save_one(key, &value, sizeof(value));
}

static void persist_str(const char *key, const char *value)
{
	const size_t len = strlen(value) + 1U;
	(void)settings_save_one(key, value, len);
}

int app_settings_init(void)
{
	int rc;

	k_mutex_init(&g_settings.lock);
	settings_defaults(&g_settings.snapshot);

	rc = settings_subsys_init();
	if (rc != 0) {
		LOG_ERR("settings_subsys_init failed (%d)", rc);
		return rc;
	}

	rc = settings_load_subtree("tib");
	if (rc != 0) {
		LOG_WRN("settings_load_subtree('tib') failed (%d)", rc);
	}

	return rc;
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
		persist_str(KEY_MQTT_HOST, mqtt->broker_host);
		persist_u16(KEY_MQTT_PORT, mqtt->broker_port);
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
