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

struct coo_cmd_runtime;
struct k_work_q;

/** Initialize runtime identity, queues, hooks and reboot work before ingress.
 * Reboot preparation uses the app queue because it can block on Modbus.
 * Configuration only: main starts the supplied queue before consumers run.
 */
int command_runtime_init(struct k_work_q *blocking_work_q);

/** Return the configured runtime for main's command ingress and output drain. */
struct coo_cmd_runtime *command_runtime_get(void);

#endif //COMMAND_H
