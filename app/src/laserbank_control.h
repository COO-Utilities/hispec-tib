/**
 * @file laserbank_control.h
 * @brief TIB laser-bank heater auto/override control loop.
 */

#ifndef HISPEC_LASERBANK_CONTROL_H
#define HISPEC_LASERBANK_CONTROL_H

#include <stdbool.h>
#include <stdint.h>

#include "app_settings.h"

#define LASERBANK_CONTROL_POLL_INTERVAL_MS 10000U
#define LASERBANK_CONTROL_TEMP_STALE_MS (2U * LASERBANK_CONTROL_POLL_INTERVAL_MS)
#define LASERBANK_CONTROL_OVERRIDE_WARNING_MS (20U * 60U * 1000U)

struct laserbank_control_status {
	bool available;
	enum app_laserbank_heater_mode heater_mode;
	bool bank_powered;
	bool heater_on;
	bool ambient_valid;
	float ambient_c;
	uint32_t ambient_age_ms;
	uint8_t valid_temp_count;
	uint8_t stale_temp_count;
	bool any_disabled_below_15c;
	bool any_disabled_above_off_threshold;
	bool all_tecs_enabled;
	uint32_t all_tecs_enabled_ms;
	int last_error;
	uint32_t last_poll_age_ms;
};

/** Background control loop; blocks on sleeps, GPIO, and Maiman Modbus I/O. */
void laserbank_control_thread(void *p1, void *p2, void *p3);

/** Copy the latest control-loop state. */
void laserbank_control_get_status(struct laserbank_control_status *out);

/** Set heater mode. Auto runs the warmup policy; override modes force the relay. */
int laserbank_control_set_heater_mode(enum app_laserbank_heater_mode mode,
				      bool persist);

const char *laserbank_heater_mode_name(enum app_laserbank_heater_mode mode);

#endif /* HISPEC_LASERBANK_CONTROL_H */
