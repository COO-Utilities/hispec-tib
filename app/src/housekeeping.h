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
 * Return relay-output on-time tracked since boot.
 *
 * Housekeeping owns this runtime estimate because it owns slow relay writes.
 * It does not read back or infer pre-boot relay state.
 */
double housekeeping_power_on_time_s(enum housekeeping_power_output output);

/**
 * Start ambient-temperature cache refresh work.
 *
 * The delayable work is submitted to @p work_q and may block briefly on
 * DS18B20 sensor I/O. Relay power helpers remain direct calls.
 */
void housekeeping_start(struct k_work_q *work_q);

#endif /* HISPEC_HOUSEKEEPING_H */
