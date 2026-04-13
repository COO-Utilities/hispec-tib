/*
 * HiSPEC-TIB main application.
 * Copyright (c) 2026 Caltech Optical Observatories
 * SPDX-License-Identifier: Apache-2.0
 */

#include <app_version.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/drivers/watchdog.h>
#include <zephyr/devicetree.h>
#include <zephyr/device.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/net/mqtt.h>
#include <zephyr/sys/util.h>
#include <errno.h>
#include <string.h>
#include <strings.h>

#include <coo_commons/mqtt_client.h>
#include <coo_commons/network.h>

#include "app_settings.h"
#include "command.h"
#include "devices.h"
#include "photodiode.h"

LOG_MODULE_REGISTER(main, LOG_LEVEL_DBG);

#define MQTT_DEVICE_ID "hsfib-tib"
#define MQTT_CMD_PREFIX "cmd/" MQTT_DEVICE_ID "/req/"
#define MQTT_RESP_PREFIX "cmd/" MQTT_DEVICE_ID "/resp/"

#define EXECUTOR_STACK_SIZE 1400
#define EXECUTOR_PRIORITY 5
#define SERIAL_STACK_SIZE 1400
#define SERIAL_PRIORITY 6
#define PHOTODIODE_STACK_SIZE 500
#define PHOTODIODE_PRIORITY 5

#define SERIAL_LINE_MAX 220

#define WDT_TIMEOUT_MS 6000

static struct mqtt_client client_ctx;
static uint16_t mqtt_msg_id = 1;

static K_THREAD_STACK_DEFINE(exec_stack, EXECUTOR_STACK_SIZE);
static struct k_thread exec_thread_data;

static K_THREAD_STACK_DEFINE(serial_stack, SERIAL_STACK_SIZE);
static struct k_thread serial_thread_data;

static struct k_work_delayable photodiode_publish_work;

K_THREAD_DEFINE(photodiode_tid, PHOTODIODE_STACK_SIZE,
		photodiode_thread, NULL, NULL, NULL,
		PHOTODIODE_PRIORITY, 0, 0);

static volatile int64_t serial_network_ignore_until_ms;

static const struct device *console_uart;

static bool network_mqtt_allowed(void)
{
	return k_uptime_get() >= serial_network_ignore_until_ms;
}

static void serial_refresh_network_holdoff(void)
{
	const uint32_t holdoff_s = app_settings_get_serial_holdoff_s();
	serial_network_ignore_until_ms = k_uptime_get() + ((int64_t)holdoff_s * 1000LL);
}

static void load_network_config(struct network_config *cfg)
{
	struct app_ip_settings ip_cfg = {0};

	if (cfg == NULL) {
		return;
	}

	network_config_defaults(cfg);
	app_settings_get_ip(&ip_cfg);

	cfg->try_dhcp_first = ip_cfg.try_dhcp_first && network_feature_dhcp_enabled();
	cfg->prefer_dhcp_dns = ip_cfg.prefer_dhcp_dns;
	cfg->prefer_dhcp_ntp = ip_cfg.prefer_dhcp_ntp;

	strncpy(cfg->static_profile.ip, ip_cfg.ip, sizeof(cfg->static_profile.ip) - 1U);
	cfg->static_profile.ip[sizeof(cfg->static_profile.ip) - 1U] = '\0';
	strncpy(cfg->static_profile.subnet, ip_cfg.subnet, sizeof(cfg->static_profile.subnet) - 1U);
	cfg->static_profile.subnet[sizeof(cfg->static_profile.subnet) - 1U] = '\0';
	strncpy(cfg->static_profile.gateway, ip_cfg.gateway, sizeof(cfg->static_profile.gateway) - 1U);
	cfg->static_profile.gateway[sizeof(cfg->static_profile.gateway) - 1U] = '\0';

#if defined(CONFIG_DNS_RESOLVER)
	strncpy(cfg->static_profile.dns, ip_cfg.dns, sizeof(cfg->static_profile.dns) - 1U);
	cfg->static_profile.dns[sizeof(cfg->static_profile.dns) - 1U] = '\0';
#endif

#if defined(CONFIG_SNTP)
	strncpy(cfg->static_profile.ntp, ip_cfg.ntp, sizeof(cfg->static_profile.ntp) - 1U);
	cfg->static_profile.ntp[sizeof(cfg->static_profile.ntp) - 1U] = '\0';
#endif
}

static void load_mqtt_config(struct coo_mqtt_broker_config *cfg)
{
	struct app_mqtt_settings mqtt_cfg = {0};

	if (cfg == NULL) {
		return;
	}

	memset(cfg, 0, sizeof(*cfg));
	app_settings_get_mqtt(&mqtt_cfg);
	strncpy(cfg->host, mqtt_cfg.broker_host, sizeof(cfg->host) - 1U);
	cfg->host[sizeof(cfg->host) - 1U] = '\0';
	cfg->port = mqtt_cfg.broker_port;
}

static void wdt_callback(const struct device *wdt_dev, int channel_id)
{
	ARG_UNUSED(wdt_dev);
	ARG_UNUSED(channel_id);
	LOG_ERR("Watchdog callback triggered - resetting");
}

static int watchdog_init(const struct device **wdt_out, int *wdt_channel_out)
{
	const struct device *wdt;
	struct wdt_timeout_cfg wdt_config;
	int wdt_channel_id;

	wdt = DEVICE_DT_GET_OR_NULL(DT_ALIAS(watchdog0));
	if (!wdt || !device_is_ready(wdt)) {
		LOG_WRN("Watchdog device unavailable");
		*wdt_out = NULL;
		return -ENODEV;
	}

	wdt_config.flags = WDT_FLAG_RESET_SOC;
	wdt_config.window.min = 0U;
	wdt_config.window.max = WDT_TIMEOUT_MS;
	wdt_config.callback = wdt_callback;

	wdt_channel_id = wdt_install_timeout(wdt, &wdt_config);
	if (wdt_channel_id < 0) {
		return wdt_channel_id;
	}

	if (wdt_setup(wdt, WDT_OPT_PAUSE_HALTED_BY_DBG) < 0) {
		return -EIO;
	}

	*wdt_out = wdt;
	*wdt_channel_out = wdt_channel_id;
	return 0;
}

static bool copy_topic(const struct mqtt_utf8 *topic, char *out, size_t out_len)
{
	if (topic == NULL || out == NULL || topic->size == 0U || topic->size >= out_len) {
		return false;
	}

	memcpy(out, topic->utf8, topic->size);
	out[topic->size] = '\0';
	return true;
}

static bool derive_default_response_topic(const char *key, char *topic_out, size_t topic_out_len)
{
	const int n = snprintk(topic_out, topic_out_len, "%s%s", MQTT_RESP_PREFIX, key);

	return n > 0 && n < (int)topic_out_len;
}

static void enqueue_serial_error(const char *msg)
{
	struct OutMsg out = {0};

	out.target = OUT_TARGET_SERIAL;
	out.msg_type = RESP_ERROR;
	out.payload_len = snprintk(out.payload, sizeof(out.payload),
				   "{\"status\":\"error\",\"msg\":\"%s\"}", msg);
	(void)k_msgq_put(&outbound_queue, &out, K_NO_WAIT);
}

static void mqtt_command_handler(const struct mqtt_publish_param *pub)
{
	struct Command cmd = {0};
	char req_topic[MAX_TOPIC_LEN];
	const char *suffix;
	size_t prefix_len;
	size_t suffix_len;

	if (!copy_topic(&pub->message.topic.topic, req_topic, sizeof(req_topic))) {
		return;
	}

	prefix_len = strlen(MQTT_CMD_PREFIX);
	if (strncmp(req_topic, MQTT_CMD_PREFIX, prefix_len) != 0) {
		return;
	}

	suffix = req_topic + prefix_len;
	suffix_len = strlen(suffix);
	if (suffix_len == 0U || suffix_len >= sizeof(cmd.key)) {
		LOG_WRN("Invalid MQTT command topic suffix");
		return;
	}

	cmd.source = CMD_SRC_MQTT;
	memcpy(cmd.key, suffix, suffix_len);
	cmd.key[suffix_len] = '\0';

	if (pub->message.payload.len >= MAX_PAYLOAD_LEN) {
		struct OutMsg r = invalid_command_response(&cmd);
		(void)k_msgq_put(&outbound_queue, &r, K_NO_WAIT);
		return;
	}

	if (pub->message.payload.len > 0U) {
		memcpy(cmd.payload, pub->message.payload.data, pub->message.payload.len);
		cmd.payload[pub->message.payload.len] = '\0';
		cmd.payload_len = pub->message.payload.len;
		if (!parse_msg_type_from_payload(cmd.payload, &cmd.msg_type)) {
			cmd.msg_type = MSG_SET;
		}
	} else {
		cmd.msg_type = MSG_GET;
		snprintk(cmd.payload, sizeof(cmd.payload), "{}");
		cmd.payload_len = strlen(cmd.payload);
	}

	if (pub->prop.response_topic.utf8 != NULL &&
	    pub->prop.response_topic.size > 0U &&
	    pub->prop.response_topic.size < sizeof(cmd.response_topic)) {
		memcpy(cmd.response_topic, pub->prop.response_topic.utf8, pub->prop.response_topic.size);
		cmd.response_topic[pub->prop.response_topic.size] = '\0';
	} else if (!derive_default_response_topic(cmd.key, cmd.response_topic, sizeof(cmd.response_topic))) {
		struct OutMsg r = invalid_command_response(&cmd);
		(void)k_msgq_put(&outbound_queue, &r, K_NO_WAIT);
		return;
	}

	if (pub->prop.correlation_data.len > 0U &&
	    pub->prop.correlation_data.len <= sizeof(cmd.correlation_data)) {
		memcpy(cmd.correlation_data,
		       pub->prop.correlation_data.data,
		       pub->prop.correlation_data.len);
		cmd.corr_len = pub->prop.correlation_data.len;
	}

	if (k_msgq_put(&inbound_queue, &cmd, K_NO_WAIT) != 0) {
		struct OutMsg r = busy_response(&cmd);
		(void)k_msgq_put(&outbound_queue, &r, K_NO_WAIT);
	}
}

static void serial_parse_line(char *line)
{
	struct Command cmd = {0};
	char *cursor = line;
	char *op;
	char *key;
	char *payload = NULL;
	char *sep;

	while (*cursor == ' ' || *cursor == '\t') {
		cursor++;
	}

	if (*cursor == '\0') {
		return;
	}

	serial_refresh_network_holdoff();

	sep = strpbrk(cursor, " \t");
	if (sep == NULL) {
		op = cursor;
		key = cursor;
		cmd.msg_type = MSG_GET;
	} else {
		*sep = '\0';
		op = cursor;
		cursor = sep + 1;
		while (*cursor == ' ' || *cursor == '\t') {
			cursor++;
		}

		if (strcasecmp(op, "get") == 0 || strcasecmp(op, "set") == 0) {
			key = cursor;
			sep = strpbrk(cursor, " \t");
			if (sep != NULL) {
				*sep = '\0';
				payload = sep + 1;
				while (payload && (*payload == ' ' || *payload == '\t')) {
					payload++;
				}
			}
			cmd.msg_type = (strcasecmp(op, "set") == 0) ? MSG_SET : MSG_GET;
		} else {
			key = op;
			payload = cursor;
			cmd.msg_type = (*payload == '\0') ? MSG_GET : MSG_SET;
		}
	}

	if (key == NULL || *key == '\0') {
		enqueue_serial_error("missing command key");
		return;
	}

	cmd.source = CMD_SRC_SERIAL;
	strncpy(cmd.key, key, sizeof(cmd.key) - 1);
	cmd.key[sizeof(cmd.key) - 1] = '\0';

	if (payload == NULL || *payload == '\0') {
		snprintk(cmd.payload, sizeof(cmd.payload), "{}");
		cmd.payload_len = strlen(cmd.payload);
	} else {
		if (strlen(payload) >= sizeof(cmd.payload)) {
			enqueue_serial_error("serial payload too large");
			return;
		}
		strncpy(cmd.payload, payload, sizeof(cmd.payload) - 1);
		cmd.payload[sizeof(cmd.payload) - 1] = '\0';
		cmd.payload_len = strlen(cmd.payload);
	}

	if (k_msgq_put(&inbound_queue, &cmd, K_NO_WAIT) != 0) {
		struct OutMsg r = busy_response(&cmd);
		(void)k_msgq_put(&outbound_queue, &r, K_NO_WAIT);
	}
}

static void serial_thread_fn(void *p1, void *p2, void *p3)
{
	uint8_t c;
	char line[SERIAL_LINE_MAX];
	size_t used = 0U;

	ARG_UNUSED(p1);
	ARG_UNUSED(p2);
	ARG_UNUSED(p3);

	if (console_uart == NULL || !device_is_ready(console_uart)) {
		LOG_WRN("Console UART unavailable; serial commanding disabled");
		return;
	}

	while (1) {
		if (uart_poll_in(console_uart, &c) == 0) {
			if (c == '\r' || c == '\n') {
				if (used > 0U) {
					line[used] = '\0';
					serial_parse_line(line);
					used = 0U;
				}
			} else if (c == 0x08 || c == 0x7f) {
				if (used > 0U) {
					used--;
				}
			} else if (used < (sizeof(line) - 1U)) {
				line[used++] = (char)c;
			}
		} else {
			k_sleep(K_MSEC(10));
		}
	}
}

void executor_thread_fn(void *p1, void *p2, void *p3)
{
	struct Command cmd;
	struct OutMsg out;

	ARG_UNUSED(p1);
	ARG_UNUSED(p2);
	ARG_UNUSED(p3);

	while (1) {
		k_msgq_get(&inbound_queue, &cmd, K_FOREVER);
		out = dispatch_command(&cmd);
		if (k_msgq_put(&outbound_queue, &out, K_NO_WAIT) != 0) {
			LOG_WRN("Outbound queue full; dropping command response");
		}
	}
}

static void photodiode_publish_handler(struct k_work *work)
{
	struct OutMsg out;

	ARG_UNUSED(work);

	while (k_msgq_get(&photodiode_queue, &out, K_NO_WAIT) == 0) {
		if (k_msgq_put(&outbound_queue, &out, K_NO_WAIT) != 0) {
			(void)k_msgq_put(&photodiode_queue, &out, K_NO_WAIT);
			break;
		}
	}

	k_work_schedule(&photodiode_publish_work, K_MSEC(10));
}

static void network_event_handler(bool connected)
{
	LOG_INF("Network event: %s", connected ? "connected" : "disconnected");
}

static int publish_outmsg(const struct OutMsg *out)
{
	int rc;
	struct mqtt_publish_param param;

	memset(&param, 0, sizeof(param));
	param.message.topic.qos = out->qos;
	param.message.topic.topic.utf8 = (uint8_t *)out->topic;
	param.message.topic.topic.size = strlen(out->topic);
	param.message.payload.data = (uint8_t *)out->payload;
	param.message.payload.len = out->payload_len;
	param.prop.correlation_data.data = (uint8_t *)out->correlation_data;
	param.prop.correlation_data.len = out->corr_len;
	param.message_id = mqtt_msg_id++;
	param.dup_flag = 0U;
	param.retain_flag = 0U;

	rc = mqtt_publish(&client_ctx, &param);
	return rc;
}

static void drain_outbound_queue(bool mqtt_available)
{
	struct OutMsg out;
	int budget = 8;

	while (budget-- > 0 && k_msgq_get(&outbound_queue, &out, K_NO_WAIT) == 0) {
		if (out.target == OUT_TARGET_SERIAL) {
			printk("%s\n", out.payload);
			continue;
		}

		if (!mqtt_available) {
			if (k_msgq_put(&outbound_queue, &out, K_NO_WAIT) != 0) {
				LOG_WRN("Dropping MQTT msg (queue full while requeueing)");
			}
			continue;
		}

		if (publish_outmsg(&out) != 0) {
			LOG_WRN("MQTT publish failed; will retry");
			if (k_msgq_put(&outbound_queue, &out, K_NO_WAIT) != 0) {
				LOG_WRN("Dropping MQTT msg (queue full after publish failure)");
			}
			break;
		}
	}
}

int main(void)
{
	int rc;
	const struct device *wdt = NULL;
	int wdt_channel = -1;
	bool mqtt_subscribed = false;
	uint32_t mqtt_cfg_revision = 0U;
	struct network_config net_cfg;
	struct coo_mqtt_broker_config mqtt_cfg;

	printk("HiSPEC-TIB Application %s\n", APP_VERSION_STRING);

	console_uart = DEVICE_DT_GET_OR_NULL(DT_CHOSEN(zephyr_console));

	(void)watchdog_init(&wdt, &wdt_channel);

	rc = app_settings_init();
	if (rc != 0) {
		LOG_WRN("Settings init failed (%d); continuing with defaults", rc);
	}
	app_settings_increment_boot_count();
	serial_network_ignore_until_ms = 0;

	(void)devices_ready();
	setup_mems_switches_and_routes();
	setup_attenuators();

	k_thread_create(&exec_thread_data, exec_stack, K_THREAD_STACK_SIZEOF(exec_stack),
			executor_thread_fn, NULL, NULL, NULL,
			EXECUTOR_PRIORITY, 0, K_NO_WAIT);

	k_thread_create(&serial_thread_data, serial_stack, K_THREAD_STACK_SIZEOF(serial_stack),
			serial_thread_fn, NULL, NULL, NULL,
			SERIAL_PRIORITY, 0, K_NO_WAIT);

	k_work_init_delayable(&photodiode_publish_work, photodiode_publish_handler);
	k_work_schedule(&photodiode_publish_work, K_NO_WAIT);

	load_network_config(&net_cfg);
	(void)network_init(&net_cfg, network_event_handler);

	rc = coo_mqtt_init(&client_ctx, MQTT_DEVICE_ID);
	if (rc != 0) {
		LOG_ERR("MQTT init failed (%d)", rc);
		return rc;
	}
	load_mqtt_config(&mqtt_cfg);
	rc = coo_mqtt_set_broker_config(&mqtt_cfg);
	if (rc != 0) {
		LOG_ERR("MQTT broker config invalid (%d)", rc);
		return rc;
	}
	mqtt_cfg_revision = app_settings_get_mqtt_revision();
	coo_mqtt_set_message_callback(mqtt_command_handler);
	(void)coo_mqtt_add_subscription(MQTT_CMD_PREFIX "#", MQTT_QOS_2_EXACTLY_ONCE);

	while (1) {
		bool mqtt_can_run = network_is_ready() && network_mqtt_allowed();
		uint32_t current_mqtt_revision = app_settings_get_mqtt_revision();

		if (current_mqtt_revision != mqtt_cfg_revision) {
			mqtt_cfg_revision = current_mqtt_revision;
			load_mqtt_config(&mqtt_cfg);
			rc = coo_mqtt_set_broker_config(&mqtt_cfg);
			if (rc != 0) {
				LOG_ERR("MQTT broker reconfigure rejected (%d)", rc);
			} else if (coo_mqtt_is_connected()) {
				(void)mqtt_disconnect(&client_ctx, NULL);
				mqtt_subscribed = false;
			}
		}

		if (wdt) {
			(void)wdt_feed(wdt, wdt_channel);
		}

		if (coo_mqtt_is_connected() && !mqtt_can_run) {
			(void)mqtt_disconnect(&client_ctx, NULL);
			mqtt_subscribed = false;
		}

		if (!coo_mqtt_is_connected() && mqtt_can_run) {
			rc = coo_mqtt_connect(&client_ctx);
			if (rc == 0) {
				mqtt_subscribed = false;
			}
		}

		if (coo_mqtt_is_connected() && !mqtt_subscribed) {
			rc = coo_mqtt_subscribe(&client_ctx);
			if (rc == 0) {
				mqtt_subscribed = true;
			}
		}

		drain_outbound_queue(coo_mqtt_is_connected() && mqtt_can_run);

		if (coo_mqtt_is_connected()) {
			rc = coo_mqtt_process(&client_ctx);
			if (rc != 0) {
				(void)mqtt_disconnect(&client_ctx, NULL);
				mqtt_subscribed = false;
			}
		} else {
			k_sleep(K_MSEC(20));
		}
	}
}
