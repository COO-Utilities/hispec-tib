/**
 * @file command.h
 * @brief HISPEC command table and app-specific command runtime hooks.
 *
 * The common command runtime owns MQTT/serial ingress, topic formatting,
 * warning emission, executor threads, and outbound drain mechanics. This app
 * layer supplies command handlers, request classification, and the static
 * queues used by the runtime.
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

struct OutMsg status_get(const struct Command *cmd);
struct OutMsg temp_get(const struct Command *cmd);


struct OutMsg unknown_response(const struct Command *cmd);
struct OutMsg unsupported_response(const struct Command *cmd);
/** Dispatch one normalized command to the longest matching command-table entry. */
struct OutMsg dispatch_command(const struct Command *cmd);

/**
 * @brief Initialize command runtime identity, queues, hooks, and delayed actions.
 *
 * Registers the callbacks used by serial-override expiration and delayed
 * reboot. Call once before starting command ingress threads.
 */
int command_runtime_init(void);

/** Return the app's configured command runtime for main-loop and warning use. */
struct coo_cmd_runtime *command_runtime_get(void);

/**
 * @brief MQTT receive callback.
 *
 * The MQTT wrapper lacks callback user data, so this app shim forwards the
 * publish event to the configured command runtime.
 */
void command_handle_mqtt_publish(const struct mqtt_publish_param *pub);

extern struct k_msgq inbound_queue;
extern struct k_msgq outbound_queue;

#endif //COMMAND_H
