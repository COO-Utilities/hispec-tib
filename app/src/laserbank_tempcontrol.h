/**
 * @file laserbank_tempcontrol.h
 * @brief TIB laser-bank temperature-control loop.
 */

#ifndef HISPEC_LASERBANK_TEMPCONTROL_H
#define HISPEC_LASERBANK_TEMPCONTROL_H

#include <stdbool.h>
#include <stdint.h>

#define LASERBANK_TEMPCONTROL_POLL_INTERVAL_MS 10000U
#define LASERBANK_TEMPCONTROL_TEMP_STALE_MS (2U * LASERBANK_TEMPCONTROL_POLL_INTERVAL_MS)
#define LASERBANK_TEMPCONTROL_OVERRIDE_WARNING_MS (20U * 60U * 1000U)

enum laserbank_heater_mode {
	LASERBANK_HEATER_MODE_AUTO = 0,
	LASERBANK_HEATER_MODE_OVERRIDE_ON,
	LASERBANK_HEATER_MODE_OVERRIDE_OFF,
};

struct laserbank_tempcontrol_status {
	bool available;
	enum laserbank_heater_mode heater_mode;
	bool bank_powered;
	bool heater_on;
	bool ambient_valid;
	double ambient_c;
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

/**
 * Start the TIB heater-policy delayable work.
 *
 * The work runs in Zephyr system workqueue context and may block on Modbus and
 * relay GPIO I/O. Heater-mode commands reschedule the same work for immediate
 * policy application.
 */
void laserbank_tempcontrol_start(void);

/** Copy the latest temperature-control loop state. */
void laserbank_tempcontrol_get_status(struct laserbank_tempcontrol_status *out);

/** Set heater mode. Auto runs the warmup policy; override modes force the relay. */
int laserbank_tempcontrol_set_heater_mode(enum laserbank_heater_mode mode,
					  bool persist);

const char *laserbank_heater_mode_name(enum laserbank_heater_mode mode);

#endif /* HISPEC_LASERBANK_TEMPCONTROL_H */
