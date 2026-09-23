/**
 * @file laser_command.h
 * @brief Command adapters for laser and laser-bank requests.
 *
 * These handlers parse documented MQTT/serial command payloads, validate
 * command-schema fields, shape responses, and delegate hardware behavior to
 * lasers.c and laserbank_tempcontrol.c. Handlers may sleep or block indirectly
 * through those domain modules and may notify throughput monitoring when laser
 * output state changes.
 */

#ifndef HISPEC_LASER_COMMAND_H
#define HISPEC_LASER_COMMAND_H

#include <coo_commons/command_dispatch.h>

int laser_get(const struct coo_cmd_request *cmd, struct coo_cmd_response *out);
int laser_set(const struct coo_cmd_request *cmd, struct coo_cmd_response *out);
int laser_tune_get(const struct coo_cmd_request *cmd, struct coo_cmd_response *out);
int laser_tune_set(const struct coo_cmd_request *cmd, struct coo_cmd_response *out);
int laser_settings_get(const struct coo_cmd_request *cmd, struct coo_cmd_response *out);
int laser_settings_set(const struct coo_cmd_request *cmd, struct coo_cmd_response *out);
int laser_status_get(const struct coo_cmd_request *cmd, struct coo_cmd_response *out);

/**
 * Query or set bank power mode, possibly blocking on the laser owner's I/O.
 * App dispatch supplies require_override_off for OTA; queries and selecting
 * override_off remain allowed. Does not change or persist OTA policy.
 */
int laserbank_power(const struct coo_cmd_request *cmd, struct coo_cmd_response *out,
		    bool require_override_off);

/** Clear laser-bank faults with a bounded laser-bank power cycle. */
int laserbank_clearfaults(const struct coo_cmd_request *cmd, struct coo_cmd_response *out);

/** Query or set laser-bank heater auto/override mode. */
int laserbank_heater(const struct coo_cmd_request *cmd, struct coo_cmd_response *out);

#endif /* HISPEC_LASER_COMMAND_H */
