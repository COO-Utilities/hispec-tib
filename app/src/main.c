/*
 * HiSPEC-TIB Main Application
 * Copyright (c) 2025 Caltech Optical Observatories
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/logging/log.h>
LOG_MODULE_REGISTER(main, LOG_LEVEL_DBG);

#include <zephyr/kernel.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/watchdog.h>
#include <zephyr/net/socket.h>
#include <zephyr/settings/settings.h>
#include <zephyr/net/mqtt.h>
#include <zephyr/net/net_if.h>
#include <zephyr/net/net_mgmt.h>
#include <zephyr/net/net_event.h>
#include <zephyr/net/net_ip.h>
#include <zephyr/net/conn_mgr_monitor.h>
#include <zephyr/net/conn_mgr_connectivity.h>

#include <coo_commons/mqtt_client.h>
#include <coo_commons/network.h>

#include "devices.h"
#include "command.h"
#include "photodiode.h"

/* Overall TODOs
TODO: Incorporate UUID generation: https://github.com/zephyrproject-rtos/zephyr/tree/main/samples/subsys/uuid
TODO: Verify how I'm doing logging makes sense: https://github.com/zephyrproject-rtos/zephyr/tree/main/samples/subsys/logging/logger
TODO: Incorporate settings persistance: https://github.com/zephyrproject-rtos/zephyr/blob/main/samples/subsys/settings/src/main.c#L392
TODO: Veryfy I'm dealing with networking properly and setup DHCP with fallback to static. see https://github.com/zephyrproject-rtos/zephyr/tree/main/samples/net/common
*/

// See
// https://docs.zephyrproject.org/latest/samples/index.html#samples
// https://docs.zephyrproject.org/latest/develop/application/index.html#application
// https://docs.zephyrproject.org/latest/build/kconfig/setting.html#initial-conf
// https://docs.zephyrproject.org/latest/boards/wiznet/w5500_evb_pico2/doc/index.html
// https://github.com/zephyrproject-rtos/zephyr/tree/main/boards/raspberrypi/rpi_pico
// https://github.com/zephyrproject-rtos/zephyr/blob/main/boards/wiznet/w5500_evb_pico2/doc/index.rst

#include <app_version.h>

#define MQTT_CMD_PREFIX "cmd/hsfib-tib/req/"

/* Thread stack sizes and priorities */
#define EXECUTOR_STACK_SIZE 1024
#define EXECUTOR_PRIORITY   5
#define PHOTODIODE_STACK_SIZE 500
#define PHOTODIODE_PRIORITY 5

/* Watchdog configuration */
#define WDT_FEED_INTERVAL_MS 1000
#define WDT_TIMEOUT_MS       5000

/* Settings Management - Stub for future use */
static int setting_handler(const char *name, size_t len, settings_read_cb read_cb, void *cb_arg)
{
	if (strcmp(name, "foo") == 0 && len == sizeof(uint8_t)) {
		uint8_t val;
		if (read_cb(cb_arg, &val, sizeof(val)) == sizeof(val)) {
			printk("Restored foo = %d\n", val);
			// store to your config struct
		}
	}
	return 0;
}

SETTINGS_STATIC_HANDLER_DEFINE(myapp, "tib",
	NULL, //get
	setting_handler, //set
	NULL, //commit
	NULL); //export

/* MQTT Infrastructure */
static struct mqtt_client client_ctx;

/* Executor thread */
static K_THREAD_STACK_DEFINE(exec_stack, EXECUTOR_STACK_SIZE);
static struct k_thread exec_thread_data;

/* Photodiode work */
static struct k_work_delayable photodiode_publish_work;

/* Photodiode thread */
K_THREAD_DEFINE(photodiode_tid, PHOTODIODE_STACK_SIZE,
                photodiode_thread, NULL, NULL, NULL,
                PHOTODIODE_PRIORITY, 0, 0);

/* Watchdog callback (optional - for notification before reset) */
static void wdt_callback(const struct device *wdt_dev, int channel_id)
{
	LOG_ERR("Watchdog callback triggered - system will reset!");
}

/* Initialize watchdog */
static int watchdog_init(const struct device **wdt_out, int *wdt_channel_out)
{
	const struct device *wdt;
	int wdt_channel_id;
	struct wdt_timeout_cfg wdt_config;

	wdt = DEVICE_DT_GET_OR_NULL(DT_ALIAS(watchdog0));
	if (!wdt || !device_is_ready(wdt)) {
		LOG_WRN("Watchdog device not available - continuing without watchdog");
		*wdt_out = NULL;
		return -ENODEV;
	}

	/* Configure watchdog */
	wdt_config.flags = WDT_FLAG_RESET_SOC;
	wdt_config.window.min = 0U;
	wdt_config.window.max = WDT_TIMEOUT_MS;
	wdt_config.callback = wdt_callback;

	wdt_channel_id = wdt_install_timeout(wdt, &wdt_config);
	if (wdt_channel_id < 0) {
		LOG_ERR("Failed to install watchdog timeout (%d)", wdt_channel_id);
		return wdt_channel_id;
	}

	if (wdt_setup(wdt, WDT_OPT_PAUSE_HALTED_BY_DBG) < 0) {
		LOG_ERR("Failed to setup watchdog");
		return -EIO;
	}

	LOG_INF("Watchdog initialized (timeout: %d ms)", WDT_TIMEOUT_MS);

	*wdt_out = wdt;
	*wdt_channel_out = wdt_channel_id;
	return 0;
}

void log_mac_addr(struct net_if *iface)
{
	struct net_linkaddr *mac;

	mac = net_if_get_link_addr(iface);

	LOG_INF("MAC Address: %02X:%02X:%02X:%02X:%02X:%02X",
		mac->addr[0], mac->addr[1], mac->addr[2],
		mac->addr[3], mac->addr[4], mac->addr[5]);
}

void executor_thread_fn(void *p1, void *p2, void *p3)
{
	ARG_UNUSED(p1); ARG_UNUSED(p2); ARG_UNUSED(p3);

	struct Command cmd;
	struct OutMsg om;

	while (1) {
		/* wait for next command */
		k_msgq_get(&inbound_queue, &cmd, K_FOREVER);

		/* perform dispatch, get back JSON result string */
		om = dispatch_command(&cmd);

		/* enqueue for MQTT publish */
		if (k_msgq_put(&outbound_queue, &om, K_FOREVER) != 0) {
			LOG_WRN("Outbound queue full; dropping response");
		}
	}
}

static void photodiode_publish_handler(struct k_work *work) {
	struct OutMsg r;

	while (k_msgq_get(&photodiode_queue, &r, K_NO_WAIT) == 0) {
		if (k_msgq_put(&outbound_queue, &r, K_NO_WAIT) != 0) {
			LOG_WRN("Outbound queue full, dropping sample");
		}
	}

	// Re-schedule
	k_work_schedule(&photodiode_publish_work, K_MSEC(10)); // 100 Hz (production rate is 50Hz)
}

static void mqtt_command_handler(const struct mqtt_publish_param *pub)
{
	struct Command cmd = { 0 };

	/* Must start with our prefix */
	size_t prefix_len = strlen(MQTT_CMD_PREFIX);
	if (strncmp(pub->message.topic.topic.utf8, MQTT_CMD_PREFIX, prefix_len) != 0) {
		LOG_WRN("Shouldn't even be getting these messages");
		return;
	}

    /* 1) cmd.key ← everything after "cmd/hsfib-tib/req/" */
    const char *suffix = pub->message.topic.topic.utf8 + prefix_len;
    size_t suffix_len = strlen(suffix);
    if (suffix_len == 0 || suffix_len >= MAX_KEY_LEN) {
    	LOG_WRN("Topic too long, dropping command");
    	struct OutMsg r = invalid_command_response(&cmd);
    	k_msgq_put(&outbound_queue, &r, K_NO_WAIT);
        return;
    }

    memcpy(cmd.key, suffix, suffix_len);
    cmd.key[suffix_len] = '\0';

    /* 2) Copy raw JSON payload */
    if (pub->message.payload.len >= MAX_PAYLOAD_LEN) {
        return;
    }
    memcpy(cmd.payload, pub->message.payload.data, pub->message.payload.len);
    cmd.payload[pub->message.payload.len] = '\0';

	// Parse msg type from json payload
	if (!parse_msg_type_from_payload(cmd.payload, &cmd.msg_type)) {
		LOG_WRN("No valid msg_type in JSON for %s", cmd.key);
		struct OutMsg r = invalid_command_response(&cmd);
		k_msgq_put(&outbound_queue, &r, K_NO_WAIT);
		return;
	}

	if (!(pub->prop.response_topic.utf8 && pub->prop.response_topic.size < sizeof(cmd.response_topic))) {
		LOG_WRN("No valid response topic");
		struct OutMsg r = invalid_command_response(&cmd);
		k_msgq_put(&outbound_queue, &r, K_NO_WAIT);
		return;
	}
	memcpy(cmd.response_topic,
		   pub->prop.response_topic.utf8,
		   pub->prop.response_topic.size);
	cmd.response_topic[pub->prop.response_topic.size] = '\0';

    /* Save the broker's correlation_data so we can echo it back */
    if (pub->prop.correlation_data.len > 0 &&
        pub->prop.correlation_data.len < sizeof(cmd.correlation_data)) {
        memcpy(cmd.correlation_data,
               pub->prop.correlation_data.data,
               pub->prop.correlation_data.len);
        cmd.corr_len = pub->prop.correlation_data.len;
    }

	if (k_msgq_put(&inbound_queue, &cmd, K_NO_WAIT) != 0) {
		LOG_WRN("Executor busy; rejecting cmd=%s", cmd.key);
		/* Optional: send a "busy" NACK immediately */
		struct OutMsg r = busy_response(&cmd);
		k_msgq_put(&outbound_queue, &r, K_NO_WAIT);
	}
}

int main(void)
{
	//TODO Outstanding questions:
	// 1. I need to ensure that things are setup so that DHCP falls back to a static IP and that if the link goes
	// down (e.g. cable unplug) it comes back up automatically. I'm not clear if I need to use the connection manager to ensure this.
	// 2. I need to ensure that MQTT properly stops/restarts across link failures.

	int rc;
	struct net_if *iface;
	static uint16_t msg_id = 1;
	const struct device *wdt = NULL;
	int wdt_channel = -1;
	int64_t last_wdt_feed = 0;

	printk("HiSPEC-TIB Application %s\n", APP_VERSION_STRING);

	/* Initialize watchdog */
	watchdog_init(&wdt, &wdt_channel);

	/* Initialize devices */
	devices_ready();
	setup_mems_switches_and_routes();
	setup_attenuators();

    /* Initialize settings (persistent storage) */
    rc = settings_subsys_init();
    if (rc) {
        LOG_ERR("Settings init failed (%d)", rc);
    } else {
        settings_load();
    }

	iface = net_if_get_default();
	if (iface == NULL) {
		LOG_ERR("No network interface configured");
		return -ENETDOWN;
	} else {
		log_mac_addr(iface);
	}

	LOG_INF("Bringing up network..");

    /* Bring up all network interfaces managed by conn_mgr */
    rc = conn_mgr_all_if_up(true);
    if (rc) {
        LOG_ERR("conn_mgr_all_if_up() failed (%d)", rc);
    }

	/* Wait for network using coo-common helper */
	coo_network_init(NULL);
	coo_network_wait_ready(0);

	LOG_INF("Network stack ready (DHCP or static IP set).");

	/* Initialize MQTT using coo-common library */
	rc = coo_mqtt_init(&client_ctx, "hsfib-tib");
	if (rc != 0) {
		LOG_ERR("MQTT Init failed [%d]", rc);
		return rc;
	}

	coo_mqtt_add_subscription(MQTT_CMD_PREFIX "#", MQTT_QOS_2_EXACTLY_ONCE);
	coo_mqtt_set_message_callback(mqtt_command_handler);

	/* Start executor thread */
	k_thread_create(&exec_thread_data, exec_stack,
				K_THREAD_STACK_SIZEOF(exec_stack),
				executor_thread_fn,
				NULL, NULL, NULL,
				EXECUTOR_PRIORITY, 0, K_NO_WAIT);

	/* Start photodiode publisher */
	k_work_init_delayable(&photodiode_publish_work, photodiode_publish_handler);
	k_work_schedule(&photodiode_publish_work, K_NO_WAIT);

	/* Main loop */
	while (1) {
		/* Block until MQTT connection is up */
		coo_mqtt_connect(&client_ctx);
		coo_mqtt_subscribe(&client_ctx);

		/* Thread will primarily remain in this loop */
		while (coo_mqtt_is_connected()) {

			/* Feed watchdog periodically */
			if (wdt && (k_uptime_get() - last_wdt_feed) >= WDT_FEED_INTERVAL_MS) {
				rc = wdt_feed(wdt, wdt_channel);
				if (rc) {
					LOG_ERR("Failed to feed watchdog (%d)", rc);
				}
				last_wdt_feed = k_uptime_get();
			}

			/* 1) drain outbound_queue: publish everything */
			struct OutMsg om;
			while (k_msgq_get(&outbound_queue, &om, K_NO_WAIT) == 0) {

				struct mqtt_publish_param param = {
					.message.topic.qos = om.qos,
					.message.topic.topic.utf8 = (uint8_t *)om.topic,
					.message.topic.topic.size = strlen(om.topic),
					.message.payload.data = (uint8_t *)om.payload,
					.message.payload.len = om.payload_len,
					.prop.correlation_data.data = om.correlation_data,
					.prop.correlation_data.len = om.corr_len,
					.message_id = msg_id++,
					.dup_flag = 0,
					.retain_flag = 0,
				};

				rc = mqtt_publish(&client_ctx, &param);
				if (rc != 0) {
					LOG_ERR("MQTT Publish failed [%d]", rc);
				}
			}

			rc = coo_mqtt_process(&client_ctx);
			if (rc != 0) {
				break;
			}
		}
		/* Gracefully close connection */
		mqtt_disconnect(&client_ctx, NULL);

		LOG_INF("MQTT disconnected, will retry...");
	}

	return rc;
}
