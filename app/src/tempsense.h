/**
 * @file tempsense.h
 * @brief DS18B20 ambient temperature sampling cache.
 */

#ifndef APP_TEMPSENSE_H
#define APP_TEMPSENSE_H

#include <stdbool.h>
#include <stdint.h>

struct tempsense_status {
	float ambient_c;
	uint32_t age_ms;
	int last_error;
	bool valid;
};

/** Copy the current temperature cache and compute age from Zephyr uptime. */
void tempsense_get_status(struct tempsense_status *out);

#endif /* APP_TEMPSENSE_H */
