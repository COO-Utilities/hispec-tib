/**
 * @file laser_command.h
 * @brief Command adapters for laser and laser-bank requests.
 *
 * These handlers parse documented MQTT/serial command payloads, validate
 * command-schema fields, shape responses, and delegate hardware behavior to
 * lasers.c and laserbank_control.c. Handlers may sleep or block indirectly
 * through those domain modules and may notify throughput monitoring when laser
 * output state changes.
 */

#ifndef HISPEC_LASER_COMMAND_H
#define HISPEC_LASER_COMMAND_H

#include "command.h"

struct OutMsg laser_get(const struct Command *cmd);
struct OutMsg laser_set(const struct Command *cmd);
struct OutMsg laser_tune_get(const struct Command *cmd);
struct OutMsg laser_tune_set(const struct Command *cmd);
struct OutMsg laser_settings_get(const struct Command *cmd);
struct OutMsg laser_settings_set(const struct Command *cmd);
struct OutMsg laser_status_get(const struct Command *cmd);
struct OutMsg laser_engstatus_get(const struct Command *cmd);

/** Query or set laser-bank power auto/override mode. */
struct OutMsg laserbank_power(const struct Command *cmd);

/** Clear laser-bank faults with a bounded laser-bank power cycle. */
struct OutMsg laserbank_clearfaults(const struct Command *cmd);

/** Query or set laser-bank heater auto/override mode. */
struct OutMsg laserbank_heater(const struct Command *cmd);

#endif /* HISPEC_LASER_COMMAND_H */
