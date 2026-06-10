/**
 * @file housekeeping.h
 * @brief Slow background polling for ambient sensing and power policy.
 */

#ifndef HISPEC_HOUSEKEEPING_H
#define HISPEC_HOUSEKEEPING_H

#include <stdbool.h>
#include <stdint.h>

struct k_work_q;

struct housekeeping_temperature_status {
	double ambient_c;
	uint32_t age_ms;
	int last_error;
	bool valid;
};

enum housekeeping_power_output {
	HOUSEKEEPING_POWER_YJ_PHOTODIODE = 0,
	HOUSEKEEPING_POWER_HK_PHOTODIODE,
	HOUSEKEEPING_POWER_BANK_HEATER,
};

/** Copy the ambient-temperature cache and compute age from Zephyr uptime. */
void housekeeping_get_temperature_status(struct housekeeping_temperature_status *out);

/**
 * Set one slow relay-box output: YJ PD power, HK PD power, or bank heater.
 *
 * This may sleep on the housekeeping mutex and writes a logical Zephyr GPIO
 * value; devicetree active flags own the electrical polarity.
 */
int housekeeping_power_set(enum housekeeping_power_output output, bool enabled);

/** Read one slow relay-box output's logical GPIO state. */
int housekeeping_power_get(enum housekeeping_power_output output, bool *enabled);

/**
 * Return current continuous relay-output on-time.
 *
 * Housekeeping owns this runtime estimate because it owns slow relay writes.
 * The value is zero when the output is off.
 */
double housekeeping_power_on_time_s(enum housekeeping_power_output output);

/**
 * Auto-enable one photodiode relay and arm its auto-off deadline.
 *
 * This may sleep on the housekeeping mutex and slow relay GPIO I/O. The
 * deadline is ignored while that channel is inhibited by a longer-running
 * owner such as throughput monitoring.
 */
int housekeeping_photodiode_auto_enable(enum housekeeping_power_output output,
					uint32_t autooff_s,
					bool *was_off);

/** Cancel one photodiode relay's auto-off deadline. */
void housekeeping_photodiode_autooff_cancel(enum housekeeping_power_output output);

/** Prevent or allow one photodiode auto-off deadline from firing. */
void housekeeping_photodiode_autooff_inhibit(enum housekeeping_power_output output,
					     bool inhibited);

/** Return seconds until auto-off can fire, or -1 when no active countdown exists. */
int64_t housekeeping_photodiode_autooff_remaining_s(enum housekeeping_power_output output);

/**
 * Start ambient-temperature cache refresh work.
 *
 * The delayable work is submitted to @p work_q and may block briefly on
 * DS18B20 sensor I/O. Relay power helpers remain direct calls.
 */
void housekeeping_start(struct k_work_q *work_q);

#endif /* HISPEC_HOUSEKEEPING_H */
