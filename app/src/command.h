/**
 * @file command.h
 * @brief HISPEC command table and app-specific command runtime hooks.
 *
 * The common command runtime owns MQTT/serial ingress, topic formatting,
 * warning emission, executor threads, and outbound drain mechanics. This app
 * layer supplies command handlers, help metadata, and the static queues used by
 * the runtime.
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
/** Format configured network policy and runtime status; response publication belongs to main. */
int ip_get(const struct coo_cmd_request *cmd, struct coo_cmd_response *out);
/** Validate/apply network policy and optional NVS persistence; DHCP completion is asynchronous. */
int ip_set(const struct coo_cmd_request *cmd, struct coo_cmd_response *out);
/** Copy broker configuration into the reply; no broker connection is attempted here. */
int mqtt_get(const struct coo_cmd_request *cmd, struct coo_cmd_response *out);
/** Validate/resolve and save broker intent; main owns reconnect/revert. May block on DNS/NVS. */
int mqtt_set(const struct coo_cmd_request *cmd, struct coo_cmd_response *out);
/** Format current UTC and SNTP status; no synchronization request is sent. */
int time_get(const struct coo_cmd_request *cmd, struct coo_cmd_response *out);
/** Set UTC through the shared RTC guard; may perform RTC I/O, never persists a last-time NVS value. */
int time_set(const struct coo_cmd_request *cmd, struct coo_cmd_response *out);

/** Assemble bounded status; optional laser/DAC sections can block on hardware reads. */
int status_get(const struct coo_cmd_request *cmd, struct coo_cmd_response *out);
/** Read available temperatures without powering the bank; idle TECs alone define bank temperature. */
int temps_get(const struct coo_cmd_request *cmd, struct coo_cmd_response *out);

struct k_work_q;
/** Initialize runtime identity, queues, hooks and reboot work before ingress.
 * Reboot preparation uses the app queue because it can block on Modbus.
 * Configuration only: main starts the supplied queue before consumers run.
 */
int command_runtime_init(struct k_work_q *blocking_work_q);

/** Return the app's configured command runtime for main-loop and warning use. */
struct coo_cmd_runtime *command_runtime_get(void);

extern struct k_msgq inbound_queue;
extern struct k_msgq outbound_queue;

#endif //COMMAND_H
