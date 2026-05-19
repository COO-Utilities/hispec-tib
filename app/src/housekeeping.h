/**
 * @file housekeeping.h
 * @brief Slow background polling for ambient sensing and power policy.
 */

#ifndef HISPEC_HOUSEKEEPING_H
#define HISPEC_HOUSEKEEPING_H

#include <stdbool.h>
#include <stdint.h>

struct housekeeping_temperature_status {
	float ambient_c;
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
float housekeeping_power_on_time_s(enum housekeeping_power_output output);

/**
 * Background housekeeping actor. Samples ambient temperature, runs TIB
 * laser-bank heater policy, and services slow power timeouts. It can sleep and
 * may indirectly block on sensor, Modbus, or GPIO I/O through the domain
 * modules it calls.
 */
void housekeeping_thread(void *p1, void *p2, void *p3);

#endif /* HISPEC_HOUSEKEEPING_H */
