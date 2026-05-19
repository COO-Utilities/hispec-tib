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

/** Copy the ambient-temperature cache and compute age from Zephyr uptime. */
void housekeeping_get_temperature_status(struct housekeeping_temperature_status *out);

/**
 * Background housekeeping actor. Samples ambient temperature, runs TIB
 * laser-bank heater policy, and services slow power timeouts. It can sleep and
 * may indirectly block on sensor, Modbus, or GPIO I/O through the domain
 * modules it calls.
 */
void housekeeping_thread(void *p1, void *p2, void *p3);

#endif /* HISPEC_HOUSEKEEPING_H */
