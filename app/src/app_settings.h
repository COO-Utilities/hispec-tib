/**
 * @file app_settings.h
 * @brief Zephyr settings-backed app configuration and calibration ownership.
 *
 * App-owned top-level settings subtrees store board identity, boot count,
 * operator network/MQTT configuration, serial guard duration, attenuator
 * coefficients, and photodiode calibration/noise thresholds.
 *
 * Copyright (c) 2026 Caltech Optical Observatories
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef HISPEC_APP_SETTINGS_H
#define HISPEC_APP_SETTINGS_H

#include <stdbool.h>
#include <stdint.h>
#include <zephyr/net/net_ip.h>

#include "laserbank_control.h"

struct app_ip_settings {
	bool try_dhcp_first;
	bool prefer_dhcp_dns;
	bool prefer_dhcp_ntp;
	char ip[NET_IPV4_ADDR_LEN];
	char subnet[NET_IPV4_ADDR_LEN];
	char gateway[NET_IPV4_ADDR_LEN];
	char dns[NET_IPV4_ADDR_LEN];
	char ntp[NET_IPV4_ADDR_LEN];
};

/** Runtime and optional persisted MQTT broker configuration. */
struct app_mqtt_settings {
	char broker_host[128];
	uint16_t broker_port;
};

/** Number of logical attenuator channels whose calibration may be persisted. */
#define APP_ATTENUATOR_CHANNEL_COUNT 6
/** Number of model coefficients per physical attenuator: b = slope * voltage + offset. */
#define APP_ATTENUATOR_COEFF_COUNT 2
#define APP_ATTENUATOR_PHYSICAL_COUNT 2
#define APP_PD_CHANNEL_COUNT 2
#define APP_SETTINGS_BOARD_TYPE_MAX_LEN 16
#define APP_ROUTE_LOSS_RECORD_COUNT 32
#define APP_ROUTE_LOSS_ROUTE_MAX_LEN 24
#define APP_ROUTE_LOSS_LASER_MAX_LEN 16

struct app_attenuator_physical_settings {
	float slope;
	float offset;
};

/** Persisted/runtime calibration for one logical attenuator channel. */
struct app_attenuator_channel_settings {
	struct app_attenuator_physical_settings physical[APP_ATTENUATOR_PHYSICAL_COUNT];
};

/** Persisted/runtime attenuator calibration snapshot. */
struct app_attenuator_settings {
	struct app_attenuator_channel_settings channel[APP_ATTENUATOR_CHANNEL_COUNT];
};

/** Photodiode calibration and warning thresholds owned by app settings. */
struct app_pd_channel_settings {
	float dark_mv;
	float lowest_dark_mv;
	bool lowest_dark_valid;
	float noise_warn_rms_mv;
	float gain_v_per_uw;
};

struct app_photodiode_settings {
	struct app_pd_channel_settings channel[APP_PD_CHANNEL_COUNT];
};

struct app_laserbank_settings {
	enum laserbank_heater_mode heater_mode;
};

/** User-provided optical route transmission keyed by route and laser names. */
struct app_route_loss_record {
	bool configured;
	char route[APP_ROUTE_LOSS_ROUTE_MAX_LEN];
	char laser[APP_ROUTE_LOSS_LASER_MAX_LEN];
	double transmission;
};

struct app_route_loss_settings {
	struct app_route_loss_record record[APP_ROUTE_LOSS_RECORD_COUNT];
};

/** Persisted/runtime settings snapshot copied under a module mutex. */
struct app_settings_snapshot {
	char board_type[APP_SETTINGS_BOARD_TYPE_MAX_LEN];
	struct app_ip_settings ip;
	struct app_mqtt_settings mqtt;
	struct app_attenuator_settings attenuator;
	struct app_photodiode_settings photodiode;
	struct app_laserbank_settings laserbank;
	struct app_route_loss_settings route_loss;
	uint32_t serial_holdoff_s;
	uint32_t boot_count;
	uint32_t mqtt_revision;
};

/**
 * @brief Initialize Zephyr settings, load app subtrees, and keep defaults on failure.
 *
 * Calls `settings_subsys_init()` and `settings_load_subtree()`, so it may
 * block on the configured settings backend.
 */
int app_settings_init(void);
void app_settings_get_snapshot(struct app_settings_snapshot *out);
/**
 * @brief Record the immutable PCB board type in persistent settings.
 *
 * If a stored board type exists and differs from @p board_type, all other
 * persisted app settings are deleted and runtime settings return to defaults.
 * That treats a physically different solder-strap identity as a first boot.
 */
int app_settings_note_board_type(const char *board_type, bool *changed);
/** @brief Copy current IP settings. */
void app_settings_get_ip(struct app_ip_settings *out);
/** @brief Replace IP settings and optionally persist each IP key. */
void app_settings_update_ip(const struct app_ip_settings *ip, bool persist);
/** @brief Copy current MQTT broker settings. */
void app_settings_get_mqtt(struct app_mqtt_settings *out);
/** @brief Replace MQTT settings, increment reconnect revision, and optionally persist. */
void app_settings_update_mqtt(const struct app_mqtt_settings *mqtt, bool persist);
/** @brief Copy the current attenuator calibration snapshot. */
void app_settings_get_attenuator(struct app_attenuator_settings *out);
/**
 * @brief Update one logical attenuator channel's calibration coefficients.
 *
 * @param channel Zero-based logical attenuator channel index.
 * @param atten Channel coefficient settings copied into the runtime snapshot.
 * @param persist If true, save this channel's coefficients through Zephyr settings.
 */
void app_settings_update_attenuator_channel(uint8_t channel,
					    const struct app_attenuator_channel_settings *atten,
					    bool persist);
/** @brief Copy current photodiode calibration and warning settings. */
void app_settings_get_photodiode(struct app_photodiode_settings *out);
/** @brief Replace all photodiode settings and optionally persist both channels. */
void app_settings_update_photodiode(const struct app_photodiode_settings *pd, bool persist);
/**
 * @brief Update one photodiode channel's calibration/settings.
 *
 * @param channel Zero-based photodiode channel index.
 * @param pd Channel settings to copy into the runtime snapshot.
 * @param persist If true, save only this channel's keys through Zephyr settings.
 */
void app_settings_update_photodiode_channel(uint8_t channel,
					    const struct app_pd_channel_settings *pd,
					    bool persist);
/** @brief Copy current laser-bank heater mode setting. */
void app_settings_get_laserbank(struct app_laserbank_settings *out);
/** @brief Replace laser-bank heater mode setting. */
void app_settings_update_laserbank(const struct app_laserbank_settings *laserbank,
				   bool persist);
/**
 * @brief Get one route-loss record.
 *
 * Missing records are not errors; @p configured is false and @p transmission
 * is returned as 1.0 so optical math can treat unspecified routes as loss-free.
 */
int app_settings_get_route_loss(const char *route, const char *laser,
				double *transmission, bool *configured);
/**
 * @brief Store or update one route-loss record.
 *
 * The record is keyed only by route and laser names. It does not change MEMS
 * route structs or switch state. If @p persist is true, the value is saved via
 * Zephyr settings and may block on the backend.
 */
int app_settings_set_route_loss(const char *route, const char *laser,
				double transmission, bool persist);
/** @brief Monotonic runtime counter used by main.c to reconnect MQTT. */
uint32_t app_settings_get_mqtt_revision(void);
/** @brief Get serial-command guard duration in seconds. */
uint32_t app_settings_get_serial_holdoff_s(void);
/** @brief Set serial-command guard duration and optionally persist. */
void app_settings_set_serial_holdoff_s(uint32_t seconds, bool persist);
/** @brief Get persisted boot count. */
uint32_t app_settings_get_boot_count(void);
/** @brief Increment and persist boot count. */
void app_settings_increment_boot_count(void);

#endif /* HISPEC_APP_SETTINGS_H */
