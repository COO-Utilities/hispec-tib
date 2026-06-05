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
#include <zephyr/sys/clock.h>
#include <zephyr/sys/util.h>
#include <errno.h>
#include <string.h>
#include <time.h>

#include <coo_commons/mqtt_client.h>
#include <coo_commons/network.h>

#include "app_identity.h"
#include "app_settings.h"
#include "command.h"
#include "devices.h"
#include "housekeeping.h"
#include "laserbank_tempcontrol.h"
#include "photodiode.h"
#include "throughput_monitor.h"
#if defined(CONFIG_SNTP)
#include "sntp_sync.h"
#endif

LOG_MODULE_REGISTER(main, LOG_LEVEL_DBG);

#define EXECUTOR_STACK_SIZE 8192
#define EXECUTOR_PRIORITY 6
#define PHOTODIODE_STACK_SIZE 2048 //1400
#define PHOTODIODE_PRIORITY 2
#define THROUGHPUT_MONITOR_STACK_SIZE 4096
/* Throughput/autolevel should run promptly when active, but it can write DACs
 * and lasers, so keep MEMS and photodiode sampling ahead of it.
 */
#define THROUGHPUT_MONITOR_PRIORITY 3
#define APP_TIMING_SUMMARY_LOGS 0

/* Network setup returns after starting DHCP/static policy; DHCP fallback is a
 * later system-work item, not part of the boot watchdog budget.
 */
#define WDT_TIMEOUT_MS 15000U
#define MQTT_CONNECT_RETRY_MS 5000

static struct mqtt_client client_ctx;
static char mqtt_cmd_subscription[MAX_TOPIC_LEN];

static K_THREAD_STACK_DEFINE(exec_stack, EXECUTOR_STACK_SIZE);
static struct k_thread exec_thread_data;

static K_THREAD_STACK_DEFINE(photodiode_stack, PHOTODIODE_STACK_SIZE);
static struct k_thread photodiode_thread_data;

static K_THREAD_STACK_DEFINE(throughput_monitor_stack, THROUGHPUT_MONITOR_STACK_SIZE);
static struct k_thread throughput_monitor_thread_data;

bool app_timing_summary_logs_enabled(void)
{
	return APP_TIMING_SUMMARY_LOGS != 0;
}

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

/**
 * @brief Configure the hardware watchdog used by the main network/MQTT loop.
 *
 * The STM32 IWDG resets the MCU if the main loop stops feeding it. It does not
 * use an application callback in this build, because Zephyr's STM32 IWDG driver
 * only supports callbacks when early-wakeup interrupt support is enabled. Setup
 * writes hardware watchdog registers and can briefly wait for them to settle.
 */
static int watchdog_init(const struct device **wdt_out, int *wdt_channel_out)
{
	const struct device *wdt;
	struct wdt_timeout_cfg wdt_config;
	int wdt_channel_id;
	int rc;

	wdt = DEVICE_DT_GET_OR_NULL(DT_ALIAS(watchdog0));
	if (!wdt || !device_is_ready(wdt)) {
		LOG_WRN("Watchdog device unavailable");
		*wdt_out = NULL;
		return -ENODEV;
	}

	wdt_config.flags = WDT_FLAG_RESET_SOC;
	wdt_config.window.min = 0U;
	wdt_config.window.max = WDT_TIMEOUT_MS;
	wdt_config.callback = NULL;

	wdt_channel_id = wdt_install_timeout(wdt, &wdt_config);
	if (wdt_channel_id < 0) {
		return wdt_channel_id;
	}

	rc = wdt_setup(wdt, WDT_OPT_PAUSE_HALTED_BY_DBG);
	if (rc < 0) {
		return rc;
	}

	*wdt_out = wdt;
	*wdt_channel_out = wdt_channel_id;
	return 0;
}

static void apply_last_known_time(void)
{
	struct timespec ts = {0};
	uint64_t utc_ms;
	int rc;

	if (!app_settings_get_last_known_utc_ms(&utc_ms)) {
		return;
	}

	ts.tv_sec = (time_t)(utc_ms / 1000ULL);
	ts.tv_nsec = (long)((utc_ms % 1000ULL) * 1000000ULL);
	rc = sys_clock_settime(SYS_CLOCK_REALTIME, &ts);
	if (rc != 0) {
		LOG_WRN("Failed to restore last known UTC time (%d)", rc);
		return;
	}

	LOG_INF("Restored last known UTC time from settings: %llu ms",
		(unsigned long long)utc_ms);
}

static void network_event_handler(bool connected)
{
	LOG_INF("Network event: %s", connected ? "connected" : "disconnected");
	if (connected)
		sntp_sync_schedule_now();
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
	int64_t next_mqtt_connect_ms = 0;
	bool board_devices_ready;

	LOG_INF("HISPEC-FIB PCB  %s\n", APP_VERSION_STRING);

	devices_capture_boot_reset_cause();

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

	apply_last_known_time();
	app_settings_increment_boot_count();
	rc = command_runtime_init();
	if (rc != 0) {
		LOG_ERR("Command runtime init failed (%d)", rc);
		return rc;
	}
	cmd_runtime = command_runtime_get();
	devices_queue_boot_reset_telemetry();

	board_devices_ready = devices_ready();
	setup_mems_switches_and_routes();
	setup_attenuators();

	k_thread_create(&exec_thread_data, exec_stack, K_THREAD_STACK_SIZEOF(exec_stack),
			coo_cmd_runtime_executor_thread, cmd_runtime, NULL, NULL,
			EXECUTOR_PRIORITY, 0, K_NO_WAIT);
	k_thread_name_set(&exec_thread_data, "command_exec");

	housekeeping_start();
	if (devices_board_type() == HISPEC_BOARD_TIB) {
		if (board_devices_ready) {
			k_thread_create(&photodiode_thread_data,
					photodiode_stack,
					K_THREAD_STACK_SIZEOF(photodiode_stack),
					photodiode_thread, NULL, NULL, NULL,
					PHOTODIODE_PRIORITY, 0, K_NO_WAIT);
			k_thread_name_set(&photodiode_thread_data, "photodiode");
			k_thread_create(&throughput_monitor_thread_data,
					throughput_monitor_stack,
					K_THREAD_STACK_SIZEOF(throughput_monitor_stack),
					throughput_monitor_thread, NULL, NULL, NULL,
					THROUGHPUT_MONITOR_PRIORITY, 0, K_NO_WAIT);
			k_thread_name_set(&throughput_monitor_thread_data, "throughput");
			laserbank_tempcontrol_start();
		} else {
			LOG_WRN("TIB background workers disabled because board devices are not ready");
		}
	}

	sntp_sync_init();

	load_network_config(&net_cfg);
	(void)network_init(&net_cfg, network_event_handler);
	// wdt_feed(wdt, wdt_channel);

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
	coo_mqtt_set_message_callback(coo_cmd_runtime_mqtt_callback, cmd_runtime);

	//TODO get rid of this test. Software should verify the prefix + command string portion fits in buffer AND that all
	// command strings fit in max_command_suffix (and similarly for telemetry/warnings
	//also why is this in braces??
	{
		int written;
		size_t prefix_len;

		strncpy(mqtt_cmd_subscription, cmd_runtime->request_prefix, sizeof(mqtt_cmd_subscription) - 1U);
		mqtt_cmd_subscription[sizeof(mqtt_cmd_subscription) - 1U] = '\0';
		prefix_len = strlen(mqtt_cmd_subscription);
		written = snprintk(mqtt_cmd_subscription + prefix_len, sizeof(mqtt_cmd_subscription) - prefix_len, "#");
		// if (written < 0 ||
		//     written >= (int)(sizeof(mqtt_cmd_subscription) - prefix_len)) {
		// 	LOG_ERR("MQTT command subscription topic too long");
		// 	return -ENOSPC;
		// }
		(void)coo_mqtt_add_subscription(mqtt_cmd_subscription, MQTT_QOS_2_EXACTLY_ONCE);
	}
	// wdt_feed(wdt, wdt_channel);

	while (1) {
		/* MQTT stays connected whenever the network is ready. Serial override
		 * rejection happens in the command runtime, so requesters get
		 * an explicit response instead of a silent disconnect.
		 */
		bool mqtt_can_run = network_is_ready();
		uint32_t current_mqtt_revision = app_settings_get_mqtt_revision();

		coo_cmd_runtime_serial_poll(cmd_runtime);

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

		wdt_feed(wdt, wdt_channel);

		if (coo_mqtt_is_connected() && !mqtt_can_run) {
			(void)mqtt_disconnect(&client_ctx, NULL);
			mqtt_subscribed = false;
		}

		if (!coo_mqtt_is_connected() && mqtt_can_run &&
		    k_uptime_get() >= next_mqtt_connect_ms) {
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
			if (rc != 0) {
				next_mqtt_connect_ms = k_uptime_get() + MQTT_CONNECT_RETRY_MS;
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
			k_sleep(K_MSEC(50));
		}
	}
}
