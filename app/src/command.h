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

/**
 * @brief Initialize command runtime identity, queues, hooks, and reboot work.
 *
 * Call once before polling command ingress or starting producers/the executor.
 */
int command_runtime_init(void);

/** Return the configured runtime for main's command ingress and output drain. */
struct coo_cmd_runtime *command_runtime_get(void);

#endif //COMMAND_H
