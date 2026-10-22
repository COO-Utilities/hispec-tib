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

/** Query a coherent calibration status; no hardware I/O. Runtime sends the reply. */
int laser_calibration_get(const struct coo_cmd_request *cmd, struct coo_cmd_response *out);

/** Parse and delegate start/continue/stop/defaults restore. Domain calls may
 * block on hardware, cancellation joins and requested persistence. */
int laser_calibration_set(const struct coo_cmd_request *cmd, struct coo_cmd_response *out);

/** Copy retained metadata/records into a binary MQTT reply under the data lock. */
int laser_calibration_records_get(const struct coo_cmd_request *cmd, struct coo_cmd_response *out);

/** Read compact laser status; may block on controller I/O without starting emission. */
int laser_get(const struct coo_cmd_request *cmd, struct coo_cmd_response *out);
/** Validate level/STOP and delegate physical control; disables matching autolevel and may sleep. */
int laser_set(const struct coo_cmd_request *cmd, struct coo_cmd_response *out);
/** Return the stored wavelength offset; no physical tune or register read. */
int laser_tune_get(const struct coo_cmd_request *cmd, struct coo_cmd_response *out);
/** Store a relative tune request and release matching monitoring; no immediate optical tune. */
int laser_tune_set(const struct coo_cmd_request *cmd, struct coo_cmd_response *out);
/** Format PCB-owned channel configuration, not controller readback; state-copy only. */
int laser_settings_get(const struct coo_cmd_request *cmd, struct coo_cmd_response *out);
/** Quiesce monitoring and apply validated policy; may stop/program hardware and write NVS. */
int laser_settings_set(const struct coo_cmd_request *cmd, struct coo_cmd_response *out);
/** Read engineering controller status without starting output; may block on Modbus. */
int laser_status_get(const struct coo_cmd_request *cmd, struct coo_cmd_response *out);

/**
 * Query or set bank power mode, possibly blocking on the laser owner's I/O.
 * Command dispatch checks OTA permission before calling this handler.
 */
int laserbank_power(const struct coo_cmd_request *cmd, struct coo_cmd_response *out);

/** Clear laser-bank faults with a bounded laser-bank power cycle. */
int laserbank_clearfaults(const struct coo_cmd_request *cmd, struct coo_cmd_response *out);

/** Query or set laser-bank heater auto/override mode. */
int laserbank_heater(const struct coo_cmd_request *cmd, struct coo_cmd_response *out);

#endif /* HISPEC_LASER_COMMAND_H */
