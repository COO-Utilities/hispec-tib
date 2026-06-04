/**
 * @file app_settings.h
 * @brief Direct-NVS app configuration and calibration ownership.
 *
 * App-owned numeric NVS IDs store board identity, boot count, operator
 * network/MQTT configuration, attenuator coefficients, photodiode
 * calibration/response settings, laser policy, and route loss.
 *
 * Copyright (c) 2026 Caltech Optical Observatories
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef HISPEC_APP_SETTINGS_H
#define HISPEC_APP_SETTINGS_H

#include <stdbool.h>
#include <stdint.h>
#include <zephyr/net/net_ip.h>

#include "laser_properties.h"
#include "laserbank_tempcontrol.h"

#define APP_SETTINGS_NVS_ID_LAST_COMMAND 0x0009U

struct nvs_fs;

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
#define APP_ROUTE_LOSS_RECORD_COUNT 18
#define APP_ROUTE_LOSS_ROUTE_MAX_LEN 24
#define APP_ROUTE_LOSS_LASER_MAX_LEN 16
#define APP_LASER_CHANNEL_COUNT 6
#define APP_PD_DARK_DURATION_USER UINT32_MAX
#define APP_PD_DARK_DURATION_MAX_MS 2000U

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

/** Photodiode calibration, response, and warning thresholds owned by app settings. */
struct app_pd_channel_settings {
	float dark_mv;
	/* APP_PD_DARK_DURATION_USER means dark_mv was set directly, not measured. */
	uint32_t dark_duration_ms;
	float lowest_dark_mv;
	bool lowest_dark_valid;
	float noise_warn_rms_mv;
	double responsivity_a_per_w;
	double transimpedance_v_per_a;
};

struct app_photodiode_settings {
	struct app_pd_channel_settings channel[APP_PD_CHANNEL_COUNT];
};

struct app_laserbank_settings {
	enum laserbank_heater_mode heater_mode;
};

/** App-owned laser policy/calibration settings. Driver EEPROM owns raw driver persistence. */
struct app_laser_channel_settings {
	laserprops_t properties;
	float current_set_calibration_pct;
	bool disable_tec_at_autooff;
	uint32_t autooff_s;
	float tune_delta_nm;
	double total_emitting_s;
};

struct app_laser_settings {
	struct app_laser_channel_settings channel[APP_LASER_CHANNEL_COUNT];
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
	struct app_laser_settings laser;
	struct app_route_loss_settings route_loss;
	uint32_t boot_count;
	uint64_t last_known_utc_ms;
	uint32_t mqtt_revision;
};

/**
 * @brief Mount app NVS storage, load persisted values, and keep defaults on failure.
 *
 * Uses Zephyr NVS directly with app-owned numeric IDs, so it may block on
 * flash I/O. If the stored schema marker is missing or incompatible, app NVS
 * storage is cleared and defaults are used.
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
/**
 * @brief Erase persisted app settings except IP settings and boot count.
 *
 * This may block on Zephyr NVS flash I/O. It also resets the runtime settings
 * snapshot to defaults while preserving the current IP snapshot and boot count.
 * The schema marker is preserved as storage metadata.
 */
int app_settings_erase_non_ip_settings(void);
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
 * @param persist If true, save this channel's coefficients through Zephyr NVS.
 */
void app_settings_update_attenuator_channel(uint8_t channel,
					    const struct app_attenuator_channel_settings *atten,
					    bool persist);
/** @brief Copy current photodiode calibration and warning settings. */
void app_settings_get_photodiode(struct app_photodiode_settings *out);
/**
 * @brief Try to copy photodiode settings without sleeping.
 *
 * Timing-sensitive samplers use this to refresh cached calibration data without
 * waiting behind command-thread settings updates. A false return leaves the
 * caller's prior snapshot authoritative until a later refresh succeeds.
 */
bool app_settings_try_get_photodiode(struct app_photodiode_settings *out);
/** @brief Replace all photodiode settings and optionally persist both channels. */
void app_settings_update_photodiode(const struct app_photodiode_settings *pd, bool persist);
/**
 * @brief Update one photodiode channel's calibration/settings.
 *
 * @param channel Zero-based photodiode channel index.
 * @param pd Channel settings to copy into the runtime snapshot.
 * @param persist If true, save only this channel's NVS record.
 */
void app_settings_update_photodiode_channel(uint8_t channel,
					    const struct app_pd_channel_settings *pd,
					    bool persist);
/** @brief Copy current laser-bank heater mode setting. */
void app_settings_get_laserbank(struct app_laserbank_settings *out);
/** @brief Replace laser-bank heater mode setting. */
void app_settings_update_laserbank(const struct app_laserbank_settings *laserbank,
				   bool persist);
/** @brief Copy app-owned laser settings and lifetime counters. */
void app_settings_get_laser(struct app_laser_settings *out);
/** @brief Copy one app-owned laser channel's settings. */
int app_settings_get_laser_channel(uint8_t channel,
				   struct app_laser_channel_settings *out);
/**
 * @brief Replace one laser channel's app-owned settings.
 *
 * Driver-backed values are only app intent here; lasers.c owns applying those
 * values to the Maiman module. Laser output current is intentionally absent.
 */
int app_settings_update_laser_channel(uint8_t channel,
				      const struct app_laser_channel_settings *laser,
				      bool persist);
/** @brief Update the persisted/runtime total emitting counter for one laser. */
int app_settings_update_laser_total_emitting(uint8_t channel,
					     double total_emitting_s,
					     bool persist);
/**
 * @brief Get one route-loss record.
 *
 * Missing records are not errors; @p transmission is returned as 1.0 so
 * optical math can treat unspecified routes as loss-free.
 */
int app_settings_get_route_loss(const char *route, const char *laser,
				double *transmission);
/**
 * @brief Store or update one route-loss record.
 *
 * The record is keyed only by route and laser names. It does not change MEMS
 * route structs or switch state. If @p persist is true, the indexed route-loss
 * record is saved via Zephyr NVS and may block on flash I/O.
 */
int app_settings_set_route_loss(const char *route, const char *laser,
				double transmission, bool persist);
/** @brief Monotonic runtime counter used by main.c to reconnect MQTT. */
uint32_t app_settings_get_mqtt_revision(void);
/** @brief Get persisted boot count. */
uint32_t app_settings_get_boot_count(void);
/** @brief Increment and persist boot count. */
void app_settings_increment_boot_count(void);
/** @brief Return true and copy the last persisted UTC time if one is known. */
bool app_settings_get_last_known_utc_ms(uint64_t *utc_ms);
/** @brief Store a known-good UTC time for boot-time clock initialization. */
void app_settings_note_time_utc_ms(uint64_t utc_ms);
/** @brief Return app NVS storage for command-dispatch owned records, or NULL if unavailable. */
struct nvs_fs *app_settings_nvs_fs(void);

#endif /* HISPEC_APP_SETTINGS_H */
