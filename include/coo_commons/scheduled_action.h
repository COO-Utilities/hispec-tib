/*
 * Copyright (c) 2026 Caltech Optical Observatories
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef COO_COMMONS_SCHEDULED_ACTION_H
#define COO_COMMONS_SCHEDULED_ACTION_H

#include <stdbool.h>
#include <stddef.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/atomic.h>

/**
 * @file scheduled_action.h
 * @brief Fixed-table wrapper for named Zephyr delayable work items.
 *
 * Callers own a static array of actions and stable numeric IDs. The helper does
 * not allocate memory or create threads; callbacks run in Zephyr's system
 * workqueue context and must stay short.
 */

typedef void (*coo_scheduled_action_handler_t)(size_t id, void *user_data);

struct coo_scheduled_action {
	const char *name;
	struct k_work_delayable work;
	coo_scheduled_action_handler_t handler;
	void *user_data;
	atomic_t pending;
	size_t id;
};

/** Initialize every action in a static caller-owned table. */
int coo_scheduled_actions_init(struct coo_scheduled_action *actions, size_t action_count);

/** Attach the callback and user data for one action ID. */
int coo_scheduled_action_register(struct coo_scheduled_action *actions,
				  size_t action_count,
				  size_t id,
				  coo_scheduled_action_handler_t handler,
				  void *user_data);

/** Schedule or refresh one named action after @p delay. */
int coo_scheduled_action_schedule(struct coo_scheduled_action *actions,
				  size_t action_count,
				  size_t id,
				  k_timeout_t delay);

/** Cancel one action if it has not already started running. */
int coo_scheduled_action_cancel(struct coo_scheduled_action *actions,
				size_t action_count,
				size_t id);

/** Return whether one action is currently pending. */
bool coo_scheduled_action_is_pending(const struct coo_scheduled_action *actions,
				     size_t action_count,
				     size_t id);

/** Get the approximate remaining delay in milliseconds. */
int coo_scheduled_action_remaining_ms(struct coo_scheduled_action *actions,
				      size_t action_count,
				      size_t id,
				      int64_t *remaining_ms);

/** Return a stable name for logs/status, or "unknown" for an invalid ID. */
const char *coo_scheduled_action_name(const struct coo_scheduled_action *actions,
				      size_t action_count,
				      size_t id);

#endif /* COO_COMMONS_SCHEDULED_ACTION_H */
