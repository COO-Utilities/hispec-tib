/**
 * @file main.c
 * @brief Boot orchestration, watchdog, network/MQTT pump, and outbound publish.
 *
 * The main thread owns MQTT connection maintenance and all MQTT publish calls.
 * Worker threads and work items enqueue responses, warnings, and telemetry
 * instead of publishing directly.
 *
 * Copyright (c) 2026 Caltech Optical Observatories
 * SPDX-License-Identifier: Apache-2.0
 */

#include <app_version.h>
#include <zephyr/drivers/watchdog.h>
#include <zephyr/devicetree.h>
#include <zephyr/device.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/util.h>
#include <errno.h>
#include <string.h>

#include <coo_commons/mqtt_client.h>
#include <coo_commons/network.h>

#include "app_identity.h"
#include "app_settings.h"
#include "command.h"
#include "devices.h"
#include "housekeeping.h"
#include "photodiode.h"
#if defined(CONFIG_SNTP)
#include "sntp_sync.h"
#endif

LOG_MODULE_REGISTER(main, LOG_LEVEL_DBG);

#define EXECUTOR_STACK_SIZE 1400
#define EXECUTOR_PRIORITY 6
#define SERIAL_STACK_SIZE 1400
#define SERIAL_PRIORITY 6
#define PHOTODIODE_STACK_SIZE 500
#define PHOTODIODE_PRIORITY 3
#define HOUSEKEEPING_STACK_SIZE 1300
#define HOUSEKEEPING_PRIORITY 5

#define WDT_TIMEOUT_MS 6000

static struct mqtt_client client_ctx;
static char mqtt_cmd_subscription[MAX_TOPIC_LEN];

static K_THREAD_STACK_DEFINE(exec_stack, EXECUTOR_STACK_SIZE);
static struct k_thread exec_thread_data;

static K_THREAD_STACK_DEFINE(serial_stack, SERIAL_STACK_SIZE);
static struct k_thread serial_thread_data;

static K_THREAD_STACK_DEFINE(housekeeping_stack, HOUSEKEEPING_STACK_SIZE);
static struct k_thread housekeeping_thread_data;

K_THREAD_DEFINE(photodiode_tid, PHOTODIODE_STACK_SIZE,
		photodiode_thread, NULL, NULL, NULL,
		PHOTODIODE_PRIORITY, 0, 0);

static void load_network_config(struct network_config *cfg)
{
	struct app_ip_settings ip_cfg = {0};
#if defined(CONFIG_NET_DHCPV4)
	const bool dhcp_supported = true;
#else
	const bool dhcp_supported = false;
#endif

	if (cfg == NULL) {
		return;
	}

	network_config_defaults(cfg);
	app_settings_get_ip(&ip_cfg);

	cfg->try_dhcp_first = ip_cfg.try_dhcp_first && dhcp_supported;
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

static bool mqtt_config_equal(const struct coo_mqtt_broker_config *a,
			      const struct coo_mqtt_broker_config *b)
{
	return a != NULL && b != NULL &&
	       a->port == b->port &&
	       strcmp(a->host, b->host) == 0;
}

static void restore_mqtt_config(const struct coo_mqtt_broker_config *cfg)
{
	struct app_mqtt_settings mqtt_cfg = {0};

	if (cfg == NULL) {
		return;
	}

	strncpy(mqtt_cfg.broker_host, cfg->host, sizeof(mqtt_cfg.broker_host) - 1U);
	mqtt_cfg.broker_host[sizeof(mqtt_cfg.broker_host) - 1U] = '\0';
	mqtt_cfg.broker_port = cfg->port;
	/* Revert persistent settings too so a resolvable but unreachable broker
	 * does not strand the next boot on the rejected endpoint.
	 */
	app_settings_update_mqtt(&mqtt_cfg, true);
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

static void network_event_handler(bool connected)
{
	LOG_INF("Network event: %s", connected ? "connected" : "disconnected");
#if defined(CONFIG_SNTP)
	if (connected) sntp_sync_schedule_now();
#endif
}

int main(void)
{
	int rc;
	const struct device *wdt = NULL;
	int wdt_channel = -1;
	bool mqtt_subscribed = false;
	uint32_t mqtt_cfg_revision = 0U;
	bool mqtt_revert_on_connect_failure = false;
	struct network_config net_cfg;
	struct coo_mqtt_broker_config mqtt_cfg;
	struct coo_mqtt_broker_config prior_mqtt_cfg = {0};
	struct coo_cmd_runtime *cmd_runtime;

	LOG_INF("HiSPEC-FIB PCB  %s\n", APP_VERSION_STRING);

	/* Watchdog availability is a boot requirement. The main loop feeds it only
	 * from the MQTT/network path so a wedged main path can still reset the MCU.
	 */
	rc = watchdog_init(&wdt, &wdt_channel);
	if (rc != 0) {
		LOG_ERR("Watchdog init failed (%d); refusing to boot", rc);
		return rc;
	}

	rc = app_settings_init();
	if (rc != 0) {
		LOG_ERR("Settings init/load failed (%d); refusing to boot", rc);
		return rc;
	}

	rc = devices_detect_board_type();
	if (rc != 0) {
		LOG_ERR("Board type detection failed (%d); optional hardware setup will stay disabled", rc);
	} else {
		bool board_changed = false;

		rc = app_settings_note_board_type(devices_board_type_name(), &board_changed);
		if (rc != 0) {
			LOG_WRN("Failed to persist board type %s (%d)",
				devices_board_type_name(), rc);
		} else if (board_changed) {
			LOG_WRN("Settings reset after board type change to %s",
				devices_board_type_name());
		}
	}

	app_settings_increment_boot_count();
	rc = command_runtime_init();
	if (rc != 0) {
		LOG_ERR("Command runtime init failed (%d)", rc);
		return rc;
	}
	cmd_runtime = command_runtime_get();

	(void)devices_ready();
	setup_mems_switches_and_routes();
	setup_attenuators();

	k_thread_create(&exec_thread_data, exec_stack, K_THREAD_STACK_SIZEOF(exec_stack),
			coo_cmd_runtime_executor_thread, cmd_runtime, NULL, NULL,
			EXECUTOR_PRIORITY, 0, K_NO_WAIT);

	k_thread_create(&serial_thread_data, serial_stack, K_THREAD_STACK_SIZEOF(serial_stack),
			coo_cmd_runtime_serial_thread, cmd_runtime, NULL, NULL,
			SERIAL_PRIORITY, 0, K_NO_WAIT);

	k_thread_create(&housekeeping_thread_data,
			housekeeping_stack,
			K_THREAD_STACK_SIZEOF(housekeeping_stack),
			housekeeping_thread, NULL, NULL, NULL,
			HOUSEKEEPING_PRIORITY, 0, K_NO_WAIT);

#if defined(CONFIG_SNTP)
	sntp_sync_init();
#endif

	load_network_config(&net_cfg);
	(void)network_init(&net_cfg, network_event_handler);

	rc = coo_mqtt_init(&client_ctx, app_mqtt_device_id());
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
	coo_mqtt_set_message_callback(command_handle_mqtt_publish);
	{
		int written;
		size_t prefix_len;

		strncpy(mqtt_cmd_subscription, cmd_runtime->request_prefix,
			sizeof(mqtt_cmd_subscription) - 1U);
		mqtt_cmd_subscription[sizeof(mqtt_cmd_subscription) - 1U] = '\0';
		prefix_len = strlen(mqtt_cmd_subscription);
		written = snprintk(mqtt_cmd_subscription + prefix_len,
				   sizeof(mqtt_cmd_subscription) - prefix_len, "#");
		if (written < 0 ||
		    written >= (int)(sizeof(mqtt_cmd_subscription) - prefix_len)) {
			LOG_ERR("MQTT command subscription topic too long");
			return -ENOSPC;
		}
		(void)coo_mqtt_add_subscription(mqtt_cmd_subscription,
						MQTT_QOS_2_EXACTLY_ONCE);
	}

	while (1) {
		/* MQTT stays connected whenever the network is ready. Serial override
		 * rejection happens in the command runtime, so requesters get
		 * an explicit response instead of a silent disconnect.
		 */
		bool mqtt_can_run = network_is_ready();
		uint32_t current_mqtt_revision = app_settings_get_mqtt_revision();

		if (current_mqtt_revision != mqtt_cfg_revision) {
			struct coo_mqtt_broker_config new_mqtt_cfg;

			mqtt_cfg_revision = current_mqtt_revision;
			load_mqtt_config(&new_mqtt_cfg);
			rc = coo_mqtt_set_broker_config(&new_mqtt_cfg);
			if (rc != 0) {
				LOG_ERR("MQTT broker reconfigure rejected (%d)", rc);
			} else {
				if (!mqtt_config_equal(&new_mqtt_cfg, &mqtt_cfg)) {
					prior_mqtt_cfg = mqtt_cfg;
					mqtt_cfg = new_mqtt_cfg;
					mqtt_revert_on_connect_failure = true;
				}
				if (coo_mqtt_is_connected()) {
					(void)mqtt_disconnect(&client_ctx, NULL);
					mqtt_subscribed = false;
				}
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
				mqtt_revert_on_connect_failure = false;
			} else if (mqtt_revert_on_connect_failure) {
				char context[160];

				snprintk(context, sizeof(context),
					 "host=%s port=%u rc=%d",
					 mqtt_cfg.host, mqtt_cfg.port, rc);
				coo_cmd_runtime_warning_emit(command_runtime_get(), "mqtt_broker_revert",
						 "MQTT broker connection failed; reverting to prior broker",
						 context);
				LOG_WRN("MQTT broker connection failed (%d), reverting to %s:%u",
					rc, prior_mqtt_cfg.host, prior_mqtt_cfg.port);
				mqtt_cfg = prior_mqtt_cfg;
				(void)coo_mqtt_set_broker_config(&mqtt_cfg);
				restore_mqtt_config(&mqtt_cfg);
				mqtt_cfg_revision = app_settings_get_mqtt_revision();
				mqtt_revert_on_connect_failure = false;
			}
		}

		if (coo_mqtt_is_connected() && !mqtt_subscribed) {
			rc = coo_mqtt_subscribe(&client_ctx);
			if (rc == 0) {
				mqtt_subscribed = true;
			}
		}

		coo_cmd_runtime_drain_outbound(cmd_runtime, &client_ctx,
					       coo_mqtt_is_connected() && mqtt_can_run);

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
