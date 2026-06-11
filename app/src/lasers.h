/**
 * @file lasers.h
 * @brief Higher-level TIB laser-bank power and Maiman helper APIs.
 *
 * These helpers may sleep, block on Modbus, change laser-bank power, and write
 * Maiman EEPROM-backed parameters. Most are not yet exposed by the current
 * command dispatch table.
 *
 * Copyright (c) 2026 Caltech Optical Observatories
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef HISPEC_LASERS_H
#define HISPEC_LASERS_H

#include <stdbool.h>
#include <stdint.h>

#include "laser_properties.h"
#include "maiman.h"

struct app_laser_channel_settings;
struct k_work_q;

#define HISPEC_LASER_BANK_BOOT_DELAY_MS 1000U
#define HISPEC_LASER_BANK_FAULT_CLEAR_OFF_MS 250U

enum hispec_laser_id {
	HISPEC_LASER_1028_Y = 0,
	HISPEC_LASER_1270_J,
	HISPEC_LASER_1430_YJ,
	HISPEC_LASER_1430_HK,
	HISPEC_LASER_1510_H,
	HISPEC_LASER_2330_K,
	HISPEC_LASER_COUNT,
	HISPEC_LASER_UNKNOWN = -1,
};

enum hispec_laser_bank_power_mode {
	HISPEC_LASER_BANK_POWER_AUTO = 0,
	HISPEC_LASER_BANK_POWER_OVERRIDE_ON,
	HISPEC_LASER_BANK_POWER_OVERRIDE_OFF,
};

struct hispec_laser_driver_profile {
	enum hispec_laser_id id;
	const char *name;
	const char *modbus_name;
	uint8_t node_id;
	uint16_t expected_device_id;
	const laserprops_t *properties;
};

struct hispec_laser_status {
	enum hispec_laser_id id;
	const char *name;
	const laserprops_t *properties;
	bool bank_powered;
	bool serial_matches;
	uint16_t expected_device_id;
	uint16_t device_id;
	uint16_t expected_serial;
	uint16_t serial_number;
	uint16_t device_state;
	uint16_t tec_state;
	uint16_t lock_status;
	bool operation_started;
	bool current_set_internal;
	bool enable_internal;
	bool external_ntc_denied;
	bool interlock_denied;
	bool tec_started;
	bool tec_set_internal;
	bool tec_enable_internal;
	bool lock_interlock;
	bool lock_ld_overcurrent;
	bool lock_ld_overheat;
	bool lock_external_ntc_interlock;
	bool lock_tec_error;
	bool lock_tec_selfheat;
	bool ready_to_operate;
	uint16_t blocking_lock_status;
	const char *blocked_reason;
	double current_set_ma;
	double level_percent;
	double current_measured_ma;
	double current_min_ma;
	double current_max_ma;
	double current_max_limit_ma;
	double current_protection_threshold_ma;
	double voltage_v;
	bool current_runtime_active;
	bool tec_runtime_active;
	bool autooff_active;
	double current_on_time_s;
	double tec_on_time_s;
	double total_emitting_s;
	double tune_delta_nm;
	uint32_t autooff_s;
	int64_t off_in_s;
	double tec_temperature_set_c;
	double tec_temperature_measured_c;
	double pcb_temperature_c;
	double tec_current_measured_a;
	double tec_current_limit_a;
	double tec_voltage_v;
	double current_set_calibration_pct;
	double ntc_t_coefficient_per_c;
	tec_pid_t tec_pid;
	double estimated_power_mw;
	double estimated_wavelength_nm;
};

struct hispec_laser_tune_request {
	double desired_power_percent;
	double wavelength_nm;
	bool use_current;
	bool use_temperature;
	double maximum_power_shift_percent;
	bool apply;
};

struct hispec_laser_tune_result {
	double target_current_ma;
	double target_temperature_c;
	double estimated_power_mw;
	double estimated_wavelength_nm;
	double requested_wavelength_nm;
	double wavelength_error_nm;
	double power_error_mw;
	bool temperature_clamped;
	bool current_clamped;
};

struct hispec_laser_flux_estimate {
	double current_ma;
	double tec_temperature_c;
	double power_mw;
	double power_err_mw;
	double wavelength_nm;
	double flux_ph_s;
	double flux_err_ph_s;
};

struct hispec_laser_channel_temperature {
	enum hispec_laser_id id;
	bool valid;
	bool tec_enabled;
	double tec_temperature_c;
};

/**
 * @brief Parse a command/API laser name into a laser-bank channel.
 *
 * Accepts command names such as "1430yj" and validation-notebook names such
 * as "yj1430". Returns 0 on success or -EINVAL for an unknown name.
 */
int hispec_laser_id_from_name(const char *name, enum hispec_laser_id *out);

/** @brief Return the stable command/API name for a laser channel. */
const char *hispec_laser_name(enum hispec_laser_id id);

/** @brief Return the fixed diode properties used for safety checks and estimates. */
const laserprops_t *hispec_laser_properties(enum hispec_laser_id id);

/** @brief Return the fixed Modbus address/serial profile for a channel. */
int hispec_laser_get_driver_profile(enum hispec_laser_id id,
				    const struct hispec_laser_driver_profile **out);

/** @brief Initialize a Maiman driver handle for the selected laser channel. */
int hispec_laser_make_driver(enum hispec_laser_id id, maiman_driver_t *drv);

/**
 * @brief Return the firmware-requested TIB laser-bank supply state.
 *
 * The Nucleo overlay drives the regulator inhibit line with an open-drain
 * GPIO. A released pin may not read high, so command/control flow uses the
 * firmware-requested state as its source of truth.
 */
bool hispec_laser_bank_power_is_enabled(void);

/**
 * @brief Return the current laser-bank supply on-duration in seconds.
 *
 * The laser-bank power owner latches this runtime-only value when the bank
 * supply is observed or driven on. It returns 0 when the bank is off.
 */
uint32_t hispec_laser_bank_power_on_duration_s(void);

/** @brief Get or set the runtime laser-bank power override mode. */
enum hispec_laser_bank_power_mode hispec_laser_bank_power_mode_get(void);
const char *hispec_laser_bank_power_mode_name(enum hispec_laser_bank_power_mode mode);
int hispec_laser_bank_power_mode_set(enum hispec_laser_bank_power_mode mode);

/**
 * @brief Bind laser auto-off delayable work to an app-owned blocking workqueue.
 *
 * Auto-off may stop Maiman output over Modbus, so it must not run on Zephyr's
 * system workqueue where Modbus client RX completion also runs.
 */
void hispec_laser_autooff_start(struct k_work_q *work_q);

/**
 * @brief Set the TIB laser-bank supply GPIO.
 *
 * Side effect: drives the board power switch. With the Nucleo open-drain test
 * configuration, enabling releases the GPIO and disabling sinks it low. When
 * enabling, sleeps for HISPEC_LASER_BANK_BOOT_DELAY_MS so the Maiman
 * controllers can boot before a following Modbus transaction. When disabling a
 * powered bank, first writes driver currents to zero and stops TECs as
 * practical; Modbus shutdown failures are logged but do not block the GPIO
 * power-off transition.
 */
int hispec_laser_bank_power_set(bool enabled, bool *transitioned);

/**
 * @brief Power-cycle the laser bank to clear latched Maiman overcurrent faults.
 *
 * If the bank is off or no powered driver reports an overcurrent fault, this
 * is a no-op and @p actual_off_ms is set to 0. Otherwise this disables and
 * re-enables the whole bank supply, sleeps for the requested off interval, and
 * reports the interval through @p actual_off_ms. This is not a per-laser reset
 * and should not be used while another channel is intentionally emitting.
 */
int hispec_laser_bank_clear_faults(uint32_t off_ms,
				   uint32_t *actual_off_ms);

/**
 * @brief Poll TEC temperatures and TEC-running state for each laser channel.
 *
 * This call blocks on Modbus RTU transactions while holding the laser-bank
 * mutex so command handlers do not interleave RS-485 traffic. It returns
 * -EBUSY if another laser operation keeps the shared bus occupied past the
 * command wait budget. If the bank is off, channel readings are marked invalid
 * but the caller still receives one initialized entry per known laser channel.
 */
int hispec_laser_bank_read_temperatures(
	struct hispec_laser_channel_temperature channels[HISPEC_LASER_COUNT]);

/**
 * @brief Opportunistically poll TEC temperatures for background heater control.
 *
 * This variant never waits for the laser-bank mutex and releases the mutex
 * between driver nodes. It returns -EBUSY when a command or auto-off operation
 * owns the shared Modbus bus, letting foreground access cut in instead of
 * waiting behind a full background sweep.
 */
int hispec_laser_bank_poll_temperatures(
	struct hispec_laser_channel_temperature channels[HISPEC_LASER_COUNT]);

/**
 * @brief Verify that the expected Maiman driver is at a channel's Modbus address.
 *
 * This checks the device ID and records the observed serial number. Serial
 * changes are accepted as driver replacements after the device ID matches.
 */
int hispec_laser_verify_driver(enum hispec_laser_id id, uint16_t *serial_out);

/**
 * @brief Program diode-specific limits into the Maiman driver.
 *
 * Writes settings that must be restored if a Maiman module is replaced:
 * current max, current calibration, TEC setpoint, TEC current limit, TEC PID,
 * and CW modulation registers. If @p save_to_eeprom is true, the module's
 * EEPROM save command is sent afterward.
 */
int hispec_laser_program_driver_profile(enum hispec_laser_id id, bool save_to_eeprom);

/** @brief Save or reset the Maiman module's own EEPROM-backed settings. */
int hispec_laser_save_driver_settings(enum hispec_laser_id id);
int hispec_laser_reset_driver_settings(enum hispec_laser_id id);

/** @brief Read a best-effort snapshot of one laser channel. */
int hispec_laser_get_status(enum hispec_laser_id id, struct hispec_laser_status *out);

/** @brief Stop one channel's emission and write current 0 when possible. */
int hispec_laser_stop_output(enum hispec_laser_id id, bool stop_tec);

/** @brief Stop every channel's emission and write driver current 0 when possible. */
int hispec_laser_stop_all_outputs(bool stop_tecs);

/**
 * @brief Set raw diode current in mA.
 *
 * A positive current powers and prepares the bank, starts the TEC if needed,
 * writes the current setpoint, and starts emission. A zero current stops
 * emission without exposing a public startup/shutdown primitive.
 */
int hispec_laser_set_current_ma(enum hispec_laser_id id, double current_ma);

/** @brief Set output percent and configure auto-off deadline. */
int hispec_laser_set_output_percent_autooff(enum hispec_laser_id id,
					    double percent,
					    uint32_t autooff_s);

/** @brief Set estimated output power in mW using the diode efficiency model. */
int hispec_laser_set_output_mw(enum hispec_laser_id id, double power_mw);

/** @brief Set output as 0-100 percent of nominal current above threshold. */
int hispec_laser_set_output_percent(enum hispec_laser_id id, double percent);

/**
 * @brief Set the TEC temperature setpoint after preparing the driver.
 *
 * Preparation applies `default_operating_temp_c` and starts the TEC at that
 * default if it is stopped. This API is not a way to choose a different initial
 * TEC-start setpoint; the requested temperature is written only after startup
 * preparation succeeds.
 */
int hispec_laser_set_tec_temperature_c(enum hispec_laser_id id, double temperature_c);

/** @brief Configure TEC PID coefficients on the Maiman driver. */
int hispec_laser_set_tec_pid(enum hispec_laser_id id, tec_pid_t pid);

/**
 * @brief Get/update app-owned laser channel settings.
 *
 * The laser module validates the complete settings record and decides whether
 * driver-backed values changed enough to require Maiman reprogramming. Updates
 * may sleep, power the bank temporarily, stop emission, write Modbus registers,
 * and persist app-owned settings when requested.
 */
int hispec_laser_get_channel_settings(enum hispec_laser_id id,
				      struct app_laser_channel_settings *out);
/** @brief Validate one complete app-owned laser channel settings record. */
int hispec_laser_validate_channel_settings(enum hispec_laser_id id,
					   const struct app_laser_channel_settings *settings);
int hispec_laser_update_channel_settings(enum hispec_laser_id id,
					 const struct app_laser_channel_settings *settings,
					 bool persist);
int hispec_laser_set_tune_delta_nm(enum hispec_laser_id id, double delta_nm,
				   bool persist);
double hispec_laser_get_tune_delta_nm(enum hispec_laser_id id);

/** @brief Estimate optical power from current using the fixed diode properties. */
double hispec_laser_estimate_power_mw(const laserprops_t *properties, double current_ma);

/**
 * @brief Estimate emitted photon flux from the laser module's operating state.
 *
 * The laser module owns the current/TEC setpoints used for this estimate. This
 * reads only module state under a mutex; it does not perform Modbus I/O, change
 * GPIO state, enqueue, publish, or persist settings.
 */
int laser_estimate_flux(enum hispec_laser_id id,
			double fractional_noise,
			double constant_noise_mw,
			struct hispec_laser_flux_estimate *out);

/** @brief Return current-emission on-time tracked by this module since boot. */
double hispec_laser_current_on_time_s(enum hispec_laser_id id);

/** @brief Estimate wavelength from TEC temperature and current. */
double hispec_laser_estimate_wavelength_nm(const laserprops_t *properties,
					  double tec_temperature_c,
					  double current_ma);

/**
 * @brief Compute and optionally apply a wavelength-tuning point.
 *
 * This ports the Python tune_wavelength() math without plotting or monitoring.
 */
int hispec_laser_tune_wavelength(enum hispec_laser_id id,
				 const struct hispec_laser_tune_request *request,
				 struct hispec_laser_tune_result *result);

#endif /* HISPEC_LASERS_H */
