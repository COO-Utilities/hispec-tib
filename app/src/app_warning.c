/**
 * @file app_warning.c
 * @brief Build warning JSON and enqueue best-effort MQTT warning messages.
 *
 * This module logs locally and attempts a non-blocking put to outbound_queue.
 * It never publishes MQTT directly and drops warnings when queueing fails.
 *
 * Copyright (c) 2026 Caltech Optical Observatories
 * SPDX-License-Identifier: Apache-2.0
 */

#include "app_warning.h"

#include <errno.h>
#include <string.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include "command.h"

LOG_MODULE_REGISTER(app_warning, LOG_LEVEL_INF);

#define MQTT_DEVICE_ID "hsfib-tib"
#define APP_WARNING_TOPIC "dt/" MQTT_DEVICE_ID "/warning"

static int append_json_string(char *buf, size_t buf_len, size_t *off,
			      const char *text)
{
	const char *s = text != NULL ? text : "";

	if (buf == NULL || off == NULL || *off >= buf_len) {
		return -EINVAL;
	}

	for (; *s != '\0'; ++s) {
		int written;

		if (*s == '"' || *s == '\\') {
			written = snprintk(buf + *off, buf_len - *off, "\\%c", *s);
		} else if ((unsigned char)*s < 0x20U) {
			written = snprintk(buf + *off, buf_len - *off, "?");
		} else {
			written = snprintk(buf + *off, buf_len - *off, "%c", *s);
		}

		if (written < 0 || written >= (int)(buf_len - *off)) {
			return -ENOSPC;
		}
		*off += (size_t)written;
	}

	return 0;
}

static int build_warning_payload(char *buf, size_t buf_len,
				 const char *code, const char *msg,
				 const char *context)
{
	size_t off;
	int written;

	written = snprintk(buf, buf_len,
			   "{\"severity\":\"warning\",\"code\":\"");
	if (written < 0 || written >= (int)buf_len) {
		return -ENOSPC;
	}
	off = (size_t)written;

	if (append_json_string(buf, buf_len, &off, code) != 0) {
		return -ENOSPC;
	}

	written = snprintk(buf + off, buf_len - off, "\",\"msg\":\"");
	if (written < 0 || written >= (int)(buf_len - off)) {
		return -ENOSPC;
	}
	off += (size_t)written;

	if (append_json_string(buf, buf_len, &off, msg) != 0) {
		return -ENOSPC;
	}

	written = snprintk(buf + off, buf_len - off, "\",\"context\":\"");
	if (written < 0 || written >= (int)(buf_len - off)) {
		return -ENOSPC;
	}
	off += (size_t)written;

	if (append_json_string(buf, buf_len, &off, context) != 0) {
		return -ENOSPC;
	}

	written = snprintk(buf + off, buf_len - off,
			   "\",\"uptime_ms\":%lld}",
			   (long long)k_uptime_get());
	if (written < 0 || written >= (int)(buf_len - off)) {
		return -ENOSPC;
	}

	return 0;
}

void app_warning_emit(const char *code, const char *msg, const char *context)
{
	struct OutMsg out = {0};

	LOG_WRN("%s: %s%s%s",
		code != NULL ? code : "warning",
		msg != NULL ? msg : "",
		context != NULL && context[0] != '\0' ? " context=" : "",
		context != NULL ? context : "");

	out.msg_type = RESP_OK;
	out.target = OUT_TARGET_MQTT_BEST_EFFORT;
	out.qos = 0U;
	snprintk(out.topic, sizeof(out.topic), APP_WARNING_TOPIC);

	if (build_warning_payload(out.payload, sizeof(out.payload), code, msg, context) != 0) {
		LOG_WRN("warning payload too large; MQTT warning dropped");
		return;
	}
	out.payload_len = strlen(out.payload);

	/* Non-blocking by design: warning publication must not break command
	 * execution or timing-sensitive loops.
	 */
	if (k_msgq_put(&outbound_queue, &out, K_NO_WAIT) != 0) {
		LOG_WRN("warning MQTT queue full; warning was only logged locally");
	}
}
