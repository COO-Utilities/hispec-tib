/*
 * HiSPEC-TIB named scheduled/deferred actions.
 * Copyright (c) 2026 Caltech Optical Observatories
 * SPDX-License-Identifier: Apache-2.0
 */

#include "app_scheduled_actions.h"

#include <errno.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/sys/util.h>

LOG_MODULE_REGISTER(app_scheduled_actions, LOG_LEVEL_INF);

struct app_scheduled_action {
	const char *name;
	struct k_work_delayable work;
	app_scheduled_action_handler_t handler;
	void *user_data;
	atomic_t pending;
};

static struct app_scheduled_action g_actions[APP_SCHEDULED_ACTION_COUNT] = {
	[APP_SCHEDULED_ACTION_SERIAL_GUARD_EXPIRE] = {
		.name = "serial_guard_expire",
	},
	[APP_SCHEDULED_ACTION_REBOOT] = {
		.name = "reboot",
	},
};

static bool g_initialized;

static bool valid_id(enum app_scheduled_action_id id)
{
	return id >= 0 && id < APP_SCHEDULED_ACTION_COUNT;
}

static void scheduled_action_work_handler(struct k_work *work)
{
	struct k_work_delayable *dwork = k_work_delayable_from_work(work);
	struct app_scheduled_action *action =
		CONTAINER_OF(dwork, struct app_scheduled_action, work);
	enum app_scheduled_action_id id =
		(enum app_scheduled_action_id)(action - g_actions);

	(void)atomic_clear(&action->pending);

	if (action->handler == NULL) {
		LOG_WRN("Scheduled action %s has no handler", action->name);
		return;
	}

	action->handler(id, action->user_data);
}

int app_scheduled_actions_init(void)
{
	if (g_initialized) {
		return 0;
	}

	for (size_t i = 0; i < ARRAY_SIZE(g_actions); ++i) {
		/* k_work_init_delayable() binds each named action to the Zephyr
		 * system workqueue; no action creates its own thread.
		 */
		k_work_init_delayable(&g_actions[i].work, scheduled_action_work_handler);
		(void)atomic_clear(&g_actions[i].pending);
	}

	g_initialized = true;
	return 0;
}

int app_scheduled_action_register(enum app_scheduled_action_id id,
				  app_scheduled_action_handler_t handler,
				  void *user_data)
{
	if (!valid_id(id) || handler == NULL) {
		return -EINVAL;
	}
	if (!g_initialized) {
		return -EAGAIN;
	}

	g_actions[id].handler = handler;
	g_actions[id].user_data = user_data;
	return 0;
}

int app_scheduled_action_schedule(enum app_scheduled_action_id id,
				  k_timeout_t delay)
{
	int rc;

	if (!valid_id(id)) {
		return -EINVAL;
	}
	if (!g_initialized) {
		return -EAGAIN;
	}

	/* k_work_reschedule() is the Zephyr API that implements "do this later
	 * unless refreshed"; it updates the same pending work item in place.
	 */
	rc = k_work_reschedule(&g_actions[id].work, delay);
	if (rc >= 0) {
		(void)atomic_set(&g_actions[id].pending, 1);
	}
	return rc;
}

int app_scheduled_action_cancel(enum app_scheduled_action_id id)
{
	if (!valid_id(id)) {
		return -EINVAL;
	}
	if (!g_initialized) {
		return -EAGAIN;
	}

	/* k_work_cancel_delayable() prevents a pending delayed action from being
	 * submitted; work already running may still finish in the system queue.
	 */
	(void)atomic_clear(&g_actions[id].pending);
	return k_work_cancel_delayable(&g_actions[id].work);
}

bool app_scheduled_action_is_pending(enum app_scheduled_action_id id)
{
	if (!valid_id(id)) {
		return false;
	}

	return atomic_get(&g_actions[id].pending) != 0;
}

int app_scheduled_action_remaining_ms(enum app_scheduled_action_id id,
				      int64_t *remaining_ms)
{
	k_ticks_t remaining_ticks;

	if (!valid_id(id) || remaining_ms == NULL) {
		return -EINVAL;
	}
	if (!g_initialized) {
		return -EAGAIN;
	}

	/* k_work_delayable_remaining_get() reads the live Zephyr timer state.
	 * It returns zero when the action is not currently scheduled.
	 */
	remaining_ticks = k_work_delayable_remaining_get(&g_actions[id].work);
	*remaining_ms = k_ticks_to_ms_floor64(remaining_ticks);
	return 0;
}

const char *app_scheduled_action_name(enum app_scheduled_action_id id)
{
	if (!valid_id(id)) {
		return "unknown";
	}

	return g_actions[id].name;
}
