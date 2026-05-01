/*
 * HiSPEC-TIB main application.
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

#include "app_settings.h"
#include "command.h"
#include "devices.h"
#include "photodiode.h"
#include "tempsense.h"
#include "sntp_sync.h"

LOG_MODULE_REGISTER(main, LOG_LEVEL_DBG);

#define MQTT_DEVICE_ID "hsfib-tib"
#define MQTT_CMD_PREFIX "cmd/" MQTT_DEVICE_ID "/req/"

#define EXECUTOR_STACK_SIZE 1400
#define EXECUTOR_PRIORITY 5
#define SERIAL_STACK_SIZE 1400
#define SERIAL_PRIORITY 6
#define PHOTODIODE_STACK_SIZE 500
#define PHOTODIODE_PRIORITY 5

#define TEMPSENSOR_STACK_SIZE 500
#define TEMPSENSOR_PRIORITY 5  //TODO this should be lowest


#define WDT_TIMEOUT_MS 6000

static struct mqtt_client client_ctx;

static K_THREAD_STACK_DEFINE(exec_stack, EXECUTOR_STACK_SIZE);
static struct k_thread exec_thread_data;

static K_THREAD_STACK_DEFINE(serial_stack, SERIAL_STACK_SIZE);
static struct k_thread serial_thread_data;

static struct k_work_delayable photodiode_publish_work;

K_THREAD_DEFINE(photodiode_tid, PHOTODIODE_STACK_SIZE,
		photodiode_thread, NULL, NULL, NULL,
		PHOTODIODE_PRIORITY, 0, 0);

K_THREAD_DEFINE(temp_tid, TEMPSENSOR_STACK_SIZE,
		tempsensor_thread, NULL, NULL, NULL,
		TEMPSENSOR_PRIORITY, 0, 0);

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
	struct network_config net_cfg;
	struct coo_mqtt_broker_config mqtt_cfg;

	LOG_INF("HiSPEC-FIB PCB  %s\n", APP_VERSION_STRING);

	(void)watchdog_init(&wdt, &wdt_channel);

	rc = app_settings_init();
	if (rc != 0) {
		LOG_WRN("Settings init failed (%d); continuing with defaults", rc);
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
	(void)devices_ready();
	setup_mems_switches_and_routes();
	setup_attenuators();

	rc = command_runtime_init();
	if (rc != 0) {
		LOG_ERR("Command runtime init failed (%d)", rc);
		return rc;
	}

	k_thread_create(&exec_thread_data, exec_stack, K_THREAD_STACK_SIZEOF(exec_stack),
			command_executor_thread, NULL, NULL, NULL,
			EXECUTOR_PRIORITY, 0, K_NO_WAIT);

	k_thread_create(&serial_thread_data, serial_stack, K_THREAD_STACK_SIZEOF(serial_stack),
			command_serial_thread, NULL, NULL, NULL,
			SERIAL_PRIORITY, 0, K_NO_WAIT);

	k_work_init_delayable(&photodiode_publish_work, photodiode_publish_handler);
	if (devices_has_photodiodes()) {
		k_work_schedule(&photodiode_publish_work, K_NO_WAIT);
	}

	sntp_sync_init();

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
	coo_mqtt_set_message_callback(command_handle_mqtt_publish);
	(void)coo_mqtt_add_subscription(MQTT_CMD_PREFIX "#", MQTT_QOS_2_EXACTLY_ONCE);

	while (1) {
		/* MQTT stays connected whenever the network is ready. Serial override
		 * rejection happens in command_handle_mqtt_publish(), so requesters get
		 * an explicit response instead of a silent disconnect.
		 */
		bool mqtt_can_run = network_is_ready();
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

		command_drain_outbound_queue(&client_ctx, coo_mqtt_is_connected() && mqtt_can_run);

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
