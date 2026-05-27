/**
 * @file command.h
 * @brief HISPEC command table and app-specific command runtime hooks.
 *
 * The common command runtime owns MQTT/serial ingress, topic formatting,
 * warning emission, executor threads, and outbound drain mechanics. This app
 * layer supplies command handlers, request classification, help metadata, and
 * the static queues used by the runtime.
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

/* Handler prototypes for command.c-owned commands (get/set where defined). */
struct coo_cmd_response ip_get(const struct coo_cmd_request *cmd);
struct coo_cmd_response ip_set(const struct coo_cmd_request *cmd);
struct coo_cmd_response mqtt_get(const struct coo_cmd_request *cmd);
struct coo_cmd_response mqtt_set(const struct coo_cmd_request *cmd);
struct coo_cmd_response time_get(const struct coo_cmd_request *cmd);
struct coo_cmd_response time_set(const struct coo_cmd_request *cmd);
struct coo_cmd_response reboot_set(const struct coo_cmd_request *cmd);

struct coo_cmd_response status_get(const struct coo_cmd_request *cmd);
struct coo_cmd_response temp_get(const struct coo_cmd_request *cmd);

/**
 * @brief Initialize command runtime identity, queues, hooks, and reboot work.
 *
 * Call once before starting command ingress threads.
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
