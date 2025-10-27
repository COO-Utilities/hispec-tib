/*
 * Copyright (c) 2021 Nordic Semiconductor ASA
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/kernel.h>
#include <zephyr/drivers/sensor.h>
#include <zephyr/drivers/watchdog.h>
#include <zephyr/settings/settings.h>
#include <zephyr/logging/log.h>

#include <app/drivers/blink.h>
#include <coo_commons/pid.h>

#include <app_version.h>

LOG_MODULE_REGISTER(main, CONFIG_APP_LOG_LEVEL);

#define BLINK_PERIOD_MS_STEP 100U
#define BLINK_PERIOD_MS_MAX  1000U

/* Watchdog configuration */
#define WDT_FEED_INTERVAL_MS 1000
#define WDT_TIMEOUT_MS       5000

/* Persistent settings storage */
static unsigned int saved_period_ms = BLINK_PERIOD_MS_MAX;

/* Settings handlers */
static int period_settings_set(const char *name, size_t len,
			       settings_read_cb read_cb, void *cb_arg)
{
	const char *next;
	int rc;

	if (settings_name_steq(name, "period", &next) && !next) {
		if (len != sizeof(saved_period_ms)) {
			return -EINVAL;
		}

		rc = read_cb(cb_arg, &saved_period_ms, sizeof(saved_period_ms));
		if (rc >= 0) {
			LOG_INF("Loaded saved period: %u ms", saved_period_ms);
			return 0;
		}
		return rc;
	}

	return -ENOENT;
}

static struct settings_handler period_conf = {
	.name = "app",
	.h_set = period_settings_set,
};

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

int main(void)
{
	int ret;
	unsigned int period_ms;
	const struct device *sensor, *blink;
	const struct device *wdt = NULL;
	int wdt_channel = -1;
	struct sensor_value last_val = { 0 }, val;
	int64_t last_wdt_feed = 0;

	printk("Zephyr COO Template Application %s\n", APP_VERSION_STRING);

	/* Initialize persistent settings */
	ret = settings_subsys_init();
	if (ret) {
		LOG_ERR("Settings init failed (%d)", ret);
		return 0;
	}

	ret = settings_register(&period_conf);
	if (ret) {
		LOG_ERR("Settings register failed (%d)", ret);
		return 0;
	}

	ret = settings_load();
	if (ret) {
		LOG_ERR("Settings load failed (%d)", ret);
	}

	/* Use saved period or default */
	period_ms = saved_period_ms;
	LOG_INF("Starting with period: %u ms", period_ms);

	/* Initialize watchdog */
	watchdog_init(&wdt, &wdt_channel);

	sensor = DEVICE_DT_GET_OR_NULL(DT_NODELABEL(example_sensor));
	if (!sensor || !device_is_ready(sensor)) {
		LOG_WRN("Sensor not available - continuing without sensor (QEMU mode?)");
		sensor = NULL;
	}

	blink = DEVICE_DT_GET_OR_NULL(DT_NODELABEL(blink_led));
	if (!blink || !device_is_ready(blink)) {
		LOG_WRN("Blink LED not available - continuing without LED (QEMU mode?)");
		blink = NULL;
	}

	/* If neither sensor nor LED are available, just demonstrate NVS/watchdog */
	if (!sensor && !blink) {
		LOG_INF("Running in QEMU mode - demonstrating NVS and watchdog only");
		printk("Use QEMU to test persistent settings and watchdog\n");
	}

	if (blink) {
		ret = blink_off(blink);
		if (ret < 0) {
			LOG_ERR("Could not turn off LED (%d)", ret);
			return 0;
		}
	}

	if (sensor && blink) {
		printk("Use the sensor to change LED blinking period\n");
	}

	while (1) {
		/* Only read sensor if available */
		if (sensor) {
			ret = sensor_sample_fetch(sensor);
			if (ret < 0) {
				LOG_ERR("Could not fetch sample (%d)", ret);
				return 0;
			}

			ret = sensor_channel_get(sensor, SENSOR_CHAN_PROX, &val);
			if (ret < 0) {
				LOG_ERR("Could not get sample (%d)", ret);
				return 0;
			}

			if ((last_val.val1 == 0) && (val.val1 == 1)) {
				if (period_ms == 0U) {
					period_ms = BLINK_PERIOD_MS_MAX;
				} else {
					period_ms -= BLINK_PERIOD_MS_STEP;
				}

				printk("Proximity detected, setting LED period to %u ms\n",
				       period_ms);

				if (blink) {
					blink_set_period_ms(blink, period_ms);
				}

				/* Save new period to persistent storage */
				saved_period_ms = period_ms;
				ret = settings_save_one("app/period", &saved_period_ms,
							sizeof(saved_period_ms));
				if (ret) {
					LOG_WRN("Failed to save period (%d)", ret);
				} else {
					LOG_DBG("Saved period: %u ms", period_ms);
				}
			}

			last_val = val;
		}

		/* Feed watchdog periodically */
		if (wdt && (k_uptime_get() - last_wdt_feed) >= WDT_FEED_INTERVAL_MS) {
			ret = wdt_feed(wdt, wdt_channel);
			if (ret) {
				LOG_ERR("Failed to feed watchdog (%d)", ret);
			}
			last_wdt_feed = k_uptime_get();
		}

		k_sleep(K_MSEC(100));
	}

	return 0;
}

