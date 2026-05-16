/**
 * @file app_warning.c
 * @brief HISPEC warning topic wrapper around reusable command warning emit.
 *
 * Copyright (c) 2026 Caltech Optical Observatories
 * SPDX-License-Identifier: Apache-2.0
 */

#include "app_warning.h"

#include "app_identity.h"
#include "command.h"

void app_warning_emit(const char *code, const char *msg, const char *context)
{
	char topic[MAX_TOPIC_LEN] = {0};

	if (app_mqtt_format_data_topic("warning", topic, sizeof(topic)) != 0) {
		return;
	}

	(void)coo_cmd_warning_emit(&outbound_queue, topic, code, msg, context);
}
