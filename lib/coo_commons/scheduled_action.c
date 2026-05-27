/*
 * Copyright (c) 2026 Caltech Optical Observatories
 * SPDX-License-Identifier: Apache-2.0
 */

#include <coo_commons/scheduled_action.h>

#include <errno.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/util.h>

LOG_MODULE_REGISTER(coo_scheduled_action, LOG_LEVEL_INF);

static bool valid_action(const struct coo_scheduled_action *actions,
			 size_t action_count,
			 size_t id)
{
	return actions != NULL && id < action_count;
}

static void scheduled_action_work_handler(struct k_work *work)
{
	struct k_work_delayable *dwork = k_work_delayable_from_work(work);
	struct coo_scheduled_action *action =
		CONTAINER_OF(dwork, struct coo_scheduled_action, work);

	(void)atomic_clear(&action->pending);

	if (action->handler == NULL) {
		LOG_WRN("Scheduled action %s has no handler",
			action->name != NULL ? action->name : "unknown");
		return;
	}

	action->handler(action->id, action->user_data);
}

int coo_scheduled_actions_init(struct coo_scheduled_action *actions, size_t action_count)
{
	if (actions == NULL || action_count == 0U) {
		return -EINVAL;
	}

	for (size_t i = 0U; i < action_count; ++i) {
		/* k_work_init_delayable() binds each action to Zephyr's system
		 * workqueue; callers should not do slow I/O in handlers.
		 */
		k_work_init_delayable(&actions[i].work, scheduled_action_work_handler);
		(void)atomic_clear(&actions[i].pending);
		actions[i].id = i;
	}

	return 0;
}

int coo_scheduled_action_register(struct coo_scheduled_action *actions,
				  size_t action_count,
				  size_t id,
				  coo_scheduled_action_handler_t handler,
				  void *user_data)
{
	if (!valid_action(actions, action_count, id) || handler == NULL) {
		return -EINVAL;
	}

	actions[id].handler = handler;
	actions[id].user_data = user_data;
	return 0;
}

int coo_scheduled_action_schedule(struct coo_scheduled_action *actions,
				  size_t action_count,
				  size_t id,
				  k_timeout_t delay)
{
	int rc;

	if (!valid_action(actions, action_count, id)) {
		return -EINVAL;
	}

	/* k_work_reschedule() implements "do this later unless refreshed" by
	 * updating the same delayable work item in place.
	 */
	rc = k_work_reschedule(&actions[id].work, delay);
	if (rc >= 0) {
		(void)atomic_set(&actions[id].pending, 1);
	}
	return rc;
}

int coo_scheduled_action_cancel(struct coo_scheduled_action *actions,
				size_t action_count,
				size_t id)
{
	if (!valid_action(actions, action_count, id)) {
		return -EINVAL;
	}

	(void)atomic_clear(&actions[id].pending);
	return k_work_cancel_delayable(&actions[id].work);
}

bool coo_scheduled_action_is_pending(const struct coo_scheduled_action *actions,
				     size_t action_count,
				     size_t id)
{
	if (!valid_action(actions, action_count, id)) {
		return false;
	}

	return atomic_get(&actions[id].pending) != 0;
}

int coo_scheduled_action_remaining_ms(struct coo_scheduled_action *actions,
				      size_t action_count,
				      size_t id,
				      int64_t *remaining_ms)
{
	k_ticks_t remaining_ticks;

	if (!valid_action(actions, action_count, id) || remaining_ms == NULL) {
		return -EINVAL;
	}

	remaining_ticks = k_work_delayable_remaining_get(&actions[id].work);
	*remaining_ms = k_ticks_to_ms_floor64(remaining_ticks);
	return 0;
}

const char *coo_scheduled_action_name(const struct coo_scheduled_action *actions,
				      size_t action_count,
				      size_t id)
{
	if (!valid_action(actions, action_count, id) || actions[id].name == NULL) {
		return "unknown";
	}

	return actions[id].name;
}
