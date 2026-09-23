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

#include <stdbool.h>

struct coo_cmd_runtime;

/**
 * @brief Initialize command runtime identity, queues, hooks, and reboot work.
 *
 * Call once before polling command ingress or starting producers/the executor.
 */
int command_runtime_init(void);

/** Return the configured runtime for main's command ingress and output drain. */
struct coo_cmd_runtime *command_runtime_get(void);

/**
 * Read MCUboot image state and register SMP admission. Call after device GPIO
 * setup and before workers/ingress: a TIB trial sets the existing bank mode to
 * override_off. Reads flash and may take the laser owner's lock/touch its GPIO.
 */
int command_ota_init(void);

/**
 * Main-loop maintenance: close expired UDP windows and schedule trial rollback.
 * May take the OTA mutex/close a socket. False means stop watchdog feeding so
 * a stalled rollback reboot is recovered by hardware. Never publishes MQTT.
 */
bool command_ota_poll(void);

#endif //COMMAND_H
