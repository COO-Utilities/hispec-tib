/**
 * @file command.h
 * @brief MQTT and serial command ingress, dispatch, and response queues.
 *
 * Commands from MQTT and the line-oriented serial console are normalized into
 * `struct Command` and executed by the common command executor thread. Handler
 * functions may touch hardware, sleep on Zephyr or bus I/O, enqueue warnings,
 * and return one `struct OutMsg` response for later serial/MQTT publication.
 */

#ifndef COMMAND_H
#define COMMAND_H

#include <zephyr/kernel.h>
#include <zephyr/data/json.h>
#include <zephyr/logging/log.h>
#include <zephyr/net/mqtt.h>
#include <string.h>
#include <coo_commons/command_dispatch.h>

#define MAX_TOPIC_LEN COO_CMD_TOPIC_MAX
#define MAX_KEY_LEN COO_CMD_KEY_MAX
#define MAX_REQID_LEN COO_CMD_REQID_MAX
#define MAX_SESSION_ID_LEN COO_CMD_SESSION_ID_MAX
#define MAX_PAYLOAD_LEN COO_CMD_PAYLOAD_MAX
#define MAX_CORRELATION_DATA COO_CMD_CORRELATION_MAX
#define MAX_PENDING_COMMANDS 2

/* Transitional app-local names retained while handlers move into domain files. */
#define MSG_GET COO_CMD_QUERY
#define MSG_SET COO_CMD_EFFECT
#define ACK COO_CMD_ACK
#define RESP_OK COO_CMD_RESP_OK
#define RESP_ERROR COO_CMD_RESP_ERROR
#define MsgType coo_cmd_msg_type
#define CMD_SRC_MQTT COO_CMD_SOURCE_MQTT
#define CMD_SRC_SERIAL COO_CMD_SOURCE_SERIAL
#define CommandSource coo_cmd_source
#define OUT_TARGET_MQTT COO_CMD_OUT_MQTT
#define OUT_TARGET_SERIAL COO_CMD_OUT_SERIAL
#define OUT_TARGET_MQTT_BEST_EFFORT COO_CMD_OUT_MQTT_BEST_EFFORT
#define OutMsgTarget coo_cmd_out_target
#define Command coo_cmd_request
#define OutMsg coo_cmd_response
#define CommandWork coo_cmd_work
#define DispatchFunc coo_cmd_handler_fn
#define DispatchEntry coo_cmd_dispatch_entry

/* Handler prototypes for command.c-owned commands (get/set where defined). */
struct OutMsg help_get(const struct Command *cmd);
struct OutMsg ip_get(const struct Command *cmd);
struct OutMsg ip_set(const struct Command *cmd);
struct OutMsg mqtt_get(const struct Command *cmd);
struct OutMsg mqtt_set(const struct Command *cmd);
struct OutMsg time_get(const struct Command *cmd);
struct OutMsg time_set(const struct Command *cmd);
struct OutMsg reboot_set(const struct Command *cmd);
struct OutMsg serial_guard_get(const struct Command *cmd);
struct OutMsg serial_guard_set(const struct Command *cmd);

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


struct OutMsg status_get(const struct Command *cmd);
struct OutMsg temp_get(const struct Command *cmd);


struct OutMsg invalid_command_response(const struct Command *cmd);
struct OutMsg unknown_response(const struct Command *cmd);
struct OutMsg unsupported_response(const struct Command *cmd);
struct OutMsg busy_response(const struct Command *cmd);
struct OutMsg serial_active_response(const struct Command *cmd);
/** Dispatch one normalized command to the longest matching command-table entry. */
struct OutMsg dispatch_command(const struct Command *cmd);

/** Executor task: blocks on inbound_queue, runs a handler, and enqueues one response. */
void command_executor_thread(void *p1, void *p2, void *p3);

/**
 * @brief Initialize command-layer delayed actions.
 *
 * Registers the callbacks used by serial-override expiration and delayed
 * reboot. Call once before starting command ingress threads.
 */
int command_runtime_init(void);

/** Serial task: blocks on Zephyr console lines and queues normalized commands. */
void command_serial_thread(void *p1, void *p2, void *p3);

/**
 * @brief MQTT receive callback.
 *
 * Copies the MQTT topic, payload, response-topic property, and correlation data
 * before returning. Request shape is inferred from the documented command
 * schema. Enqueues or publishes an immediate error when serial guard or queue
 * capacity rejects the command.
 */
void command_handle_mqtt_publish(const struct mqtt_publish_param *pub);

/** Extend the serial-command holdoff window that rejects MQTT command execution. */
void command_serial_note_activity(void);

/** Return false while serial override is active. */
bool command_network_mqtt_allowed(void);

/** Parse "<key> [payload]" into a queued Command; serial has no get/set words. */
void command_parse_serial_line(char *line);

/**
 * @brief Drain queued serial/MQTT responses.
 *
 * MQTT publishing happens only here from the main loop. Non-best-effort MQTT
 * messages are retried by requeueing when MQTT is down or publish fails.
 */
void command_drain_outbound_queue(struct mqtt_client *client, bool mqtt_available);

/**
 * @brief Emit a lightweight warning to local logs and best-effort MQTT.
 *
 * Warnings are for suspicious or degraded conditions that should be visible but
 * should not make a command fail by themselves. Emission uses the command
 * outbound queue and never publishes directly from the caller's context.
 *
 * @param code Short stable warning code, for example "serial_guard_active".
 * @param msg Human-readable warning text.
 * @param context Optional short context string, such as a command key.
 */
void app_warning_emit(const char *code, const char *msg, const char *context);


extern struct k_msgq inbound_queue;
extern struct k_msgq outbound_queue;

#endif //COMMAND_H
