/**
 * @file app_scheduled_actions.h
 * @brief Small named k_work_delayable actions used by command handlers.
 *
 * This is a fixed firmware-action table, not a user-programmable scheduler.
 * Callbacks run in Zephyr system workqueue context.
 *
 * Copyright (c) 2026 Caltech Optical Observatories
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef HISPEC_APP_SCHEDULED_ACTIONS_H
#define HISPEC_APP_SCHEDULED_ACTIONS_H

#include <stdbool.h>
#include <stdint.h>
#include <zephyr/kernel.h>

/**
 * @brief Explicit delayed actions owned by the application.
 *
 * Keep this enum short. These are named system behaviors, not user-created
 * jobs, so command handlers can reschedule/cancel known actions without
 * spawning arbitrary threads or timers.
 */
enum app_scheduled_action_id {
	APP_SCHEDULED_ACTION_SERIAL_GUARD_EXPIRE = 0,
	APP_SCHEDULED_ACTION_REBOOT,
	APP_SCHEDULED_ACTION_COUNT,
};

typedef void (*app_scheduled_action_handler_t)(enum app_scheduled_action_id id,
					       void *user_data);

/**
 * @brief Initialize all named delayable-work objects.
 *
 * This wraps Zephyr's k_work_delayable API so the rest of the app deals with
 * stable action names instead of scattered work items.
 */
int app_scheduled_actions_init(void);

/**
 * @brief Attach the callback for one named action.
 *
 * The callback runs in Zephyr's system workqueue context. It must stay short:
 * update state, log, enqueue work, or schedule another thread to do slow I/O.
 */
int app_scheduled_action_register(enum app_scheduled_action_id id,
				  app_scheduled_action_handler_t handler,
				  void *user_data);

/**
 * @brief Schedule or reschedule one named action after @p delay.
 *
 * Uses k_work_reschedule(), so repeated calls refresh the same delayed action
 * rather than creating another task or timer.
 */
int app_scheduled_action_schedule(enum app_scheduled_action_id id,
				  k_timeout_t delay);

/**
 * @brief Cancel one named action if it has not run yet.
 */
int app_scheduled_action_cancel(enum app_scheduled_action_id id);

/**
 * @brief Return whether a named action is currently pending.
 */
bool app_scheduled_action_is_pending(enum app_scheduled_action_id id);

/**
 * @brief Get the approximate remaining delay in milliseconds.
 */
int app_scheduled_action_remaining_ms(enum app_scheduled_action_id id,
				      int64_t *remaining_ms);

/**
 * @brief Human-readable name for logs and status output.
 */
const char *app_scheduled_action_name(enum app_scheduled_action_id id);

#endif /* HISPEC_APP_SCHEDULED_ACTIONS_H */
