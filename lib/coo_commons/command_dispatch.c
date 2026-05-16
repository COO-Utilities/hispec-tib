/*
 * Copyright (c) 2026 Caltech Optical Observatories
 * SPDX-License-Identifier: Apache-2.0
 */

#include <coo_commons/command_dispatch.h>

#include <ctype.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <zephyr/sys/util.h>

bool coo_cmd_key_matches_prefix(const char *key, const char *prefix)
{
	size_t len;

	if (key == NULL || prefix == NULL) {
		return false;
	}

	len = strlen(prefix);
	if (strncmp(key, prefix, len) != 0) {
		return false;
	}

	return key[len] == '\0' || key[len] == '/';
}

const struct coo_cmd_dispatch_entry *
coo_cmd_find_dispatch(const struct coo_cmd_dispatch_entry *table,
		      size_t table_len,
		      const char *key)
{
	const struct coo_cmd_dispatch_entry *best = NULL;
	size_t best_len = 0U;

	if (table == NULL || key == NULL) {
		return NULL;
	}

	for (size_t i = 0U; i < table_len; ++i) {
		size_t len;

		if (!coo_cmd_key_matches_prefix(key, table[i].key)) {
			continue;
		}

		len = strlen(table[i].key);
		if (len > best_len) {
			best = &table[i];
			best_len = len;
		}
	}

	return best;
}

struct coo_cmd_response
coo_cmd_dispatch(const struct coo_cmd_request *cmd,
		 const struct coo_cmd_dispatch_entry *table,
		 size_t table_len,
		 coo_cmd_handler_fn unknown_handler,
		 coo_cmd_handler_fn unsupported_handler)
{
	const struct coo_cmd_dispatch_entry *entry;
	coo_cmd_handler_fn handler;

	entry = coo_cmd_find_dispatch(table, table_len, cmd != NULL ? cmd->key : NULL);
	if (entry == NULL) {
		return unknown_handler != NULL ? unknown_handler(cmd) :
		       (struct coo_cmd_response){0};
	}

	handler = (cmd != NULL && cmd->msg_type == COO_CMD_EFFECT) ?
		  entry->set_handler : entry->get_handler;
	if (handler == NULL) {
		return unsupported_handler != NULL ? unsupported_handler(cmd) :
		       (struct coo_cmd_response){0};
	}

	return handler(cmd);
}

bool coo_cmd_payload_empty(const struct coo_cmd_request *cmd)
{
	return cmd == NULL || cmd->payload_len == 0U || strcmp(cmd->payload, "{}") == 0;
}

bool coo_cmd_copy_mqtt_utf8(const struct mqtt_utf8 *topic,
			    char *out,
			    size_t out_len)
{
	if (topic == NULL || out == NULL || topic->size == 0U ||
	    topic->size >= out_len) {
		return false;
	}

	memcpy(out, topic->utf8, topic->size);
	out[topic->size] = '\0';
	return true;
}

static const char *skip_serial_space(const char *s)
{
	while (s != NULL && (*s == ' ' || *s == '\t')) {
		s++;
	}

	return s;
}

static bool next_serial_token(const char **cursor, char *out, size_t out_len)
{
	const char *start;
	size_t len;

	if (cursor == NULL || *cursor == NULL || out == NULL || out_len == 0U) {
		return false;
	}

	start = skip_serial_space(*cursor);
	if (*start == '\0') {
		*cursor = start;
		return false;
	}

	len = strcspn(start, " \t");
	if (len >= out_len) {
		len = out_len - 1U;
	}

	memcpy(out, start, len);
	out[len] = '\0';
	*cursor = start + strcspn(start, " \t");
	return true;
}

static bool serial_token_has_extra(const char *cursor)
{
	cursor = skip_serial_space(cursor);
	return cursor != NULL && *cursor != '\0';
}

static bool serial_token_is_number(const char *token)
{
	char *end = NULL;

	if (token == NULL || token[0] == '\0') {
		return false;
	}

	(void)strtod(token, &end);
	return end != token && end != NULL && *end == '\0';
}

static const char *serial_token_bool_json(const char *token)
{
	if (token == NULL) {
		return NULL;
	}

	if (strcasecmp(token, "true") == 0 || strcasecmp(token, "on") == 0 ||
	    strcasecmp(token, "yes") == 0) {
		return "true";
	}
	if (strcasecmp(token, "false") == 0 || strcasecmp(token, "off") == 0 ||
	    strcasecmp(token, "no") == 0) {
		return "false";
	}

	return NULL;
}

static int append_serial_json_value(char *out, size_t out_len, size_t *off,
				    const char *token)
{
	const char *bool_json = serial_token_bool_json(token);
	int written;

	if (out == NULL || off == NULL || token == NULL || *off >= out_len) {
		return -EINVAL;
	}

	if (bool_json != NULL) {
		written = snprintk(out + *off, out_len - *off, "%s", bool_json);
	} else if (serial_token_is_number(token) || strcasecmp(token, "null") == 0) {
		written = snprintk(out + *off, out_len - *off, "%s", token);
	} else {
		if (strchr(token, '"') != NULL || strchr(token, '\\') != NULL) {
			return -EINVAL;
		}
		written = snprintk(out + *off, out_len - *off, "\"%s\"", token);
	}

	if (written < 0 || written >= (int)(out_len - *off)) {
		return -ENOSPC;
	}
	*off += (size_t)written;
	return 0;
}

static int append_serial_json_field(char *out, size_t out_len, size_t *off,
				    const char *key, const char *token,
				    bool comma)
{
	int written;

	if (key == NULL || token == NULL || key[0] == '\0' ||
	    strchr(key, '"') != NULL || strchr(key, '\\') != NULL ||
	    *off >= out_len) {
		return -EINVAL;
	}

	written = snprintk(out + *off, out_len - *off,
			   "%s\"%s\":", comma ? "," : "", key);
	if (written < 0 || written >= (int)(out_len - *off)) {
		return -ENOSPC;
	}
	*off += (size_t)written;

	return append_serial_json_value(out, out_len, off, token);
}

static int serial_payload_from_key_values(const char *payload, char *out,
					  size_t out_len)
{
	const char *cursor = payload;
	char token[128];
	bool first = true;
	size_t off = 0U;
	int written;

	written = snprintk(out, out_len, "{");
	if (written < 0 || written >= (int)out_len) {
		return -ENOSPC;
	}
	off = (size_t)written;

	while (next_serial_token(&cursor, token, sizeof(token))) {
		char *eq = strchr(token, '=');

		if (eq == NULL || eq == token || eq[1] == '\0') {
			return -EINVAL;
		}
		*eq = '\0';

		if (append_serial_json_field(out, out_len, &off, token, eq + 1,
					     !first) != 0) {
			return -EINVAL;
		}
		first = false;
	}

	written = snprintk(out + off, out_len - off, "}");
	return (written < 0 || written >= (int)(out_len - off)) ? -ENOSPC : 0;
}

static int serial_payload_from_value(const char *payload, char *out, size_t out_len)
{
	const char *cursor = payload;
	char token[128] = {0};
	size_t off = 0U;
	int written;

	if (!next_serial_token(&cursor, token, sizeof(token)) ||
	    serial_token_has_extra(cursor)) {
		return -EINVAL;
	}

	written = snprintk(out, out_len, "{\"value\":");
	if (written < 0 || written >= (int)out_len) {
		return -ENOSPC;
	}
	off = (size_t)written;
	if (append_serial_json_value(out, out_len, &off, token) != 0) {
		return -EINVAL;
	}
	written = snprintk(out + off, out_len - off, "}");
	return (written < 0 || written >= (int)(out_len - off)) ? -ENOSPC : 0;
}

int coo_cmd_normalize_serial_payload(const char *key,
				     const char *payload,
				     coo_cmd_serial_shorthand_fn shorthand,
				     void *user_data,
				     char *out,
				     size_t out_len)
{
	if (out == NULL || out_len == 0U) {
		return -EINVAL;
	}

	payload = skip_serial_space(payload);
	if (payload == NULL || payload[0] == '\0') {
		int written = snprintk(out, out_len, "{}");

		return (written < 0 || written >= (int)out_len) ? -ENOSPC : 0;
	}

	if (payload[0] == '{') {
		if (strlen(payload) >= out_len) {
			return -ENOSPC;
		}
		strncpy(out, payload, out_len - 1U);
		out[out_len - 1U] = '\0';
		return 0;
	}

	if (strchr(payload, '=') != NULL) {
		return serial_payload_from_key_values(payload, out, out_len);
	}

	if (shorthand != NULL) {
		return shorthand(key, payload, out, out_len, user_data);
	}

	return serial_payload_from_value(payload, out, out_len);
}

struct coo_cmd_response
coo_cmd_make_response(const struct coo_cmd_request *cmd,
		      enum coo_cmd_msg_type msg_type,
		      const char *payload,
		      coo_cmd_format_response_topic_fn format_topic,
		      void *user_data)
{
	static const char overflow_msg[] = "{\"error\":\"response too large\"}";
	struct coo_cmd_response r = {0};

	r.msg_type = msg_type;
	r.target = (cmd != NULL && cmd->source == COO_CMD_SOURCE_SERIAL) ?
		   COO_CMD_OUT_SERIAL : COO_CMD_OUT_MQTT;
	r.qos = MQTT_QOS_1_AT_LEAST_ONCE;

	if (format_topic != NULL) {
		(void)format_topic(cmd != NULL ? cmd->key : "",
				   r.topic, sizeof(r.topic), user_data);
	}

	if (cmd != NULL && cmd->response_topic[0] != '\0' &&
	    strlen(cmd->response_topic) < sizeof(r.topic)) {
		strncpy(r.topic, cmd->response_topic, sizeof(r.topic) - 1U);
	}

	if (cmd != NULL && cmd->corr_len > 0U &&
	    cmd->corr_len <= sizeof(r.correlation_data)) {
		memcpy(r.correlation_data, cmd->correlation_data, cmd->corr_len);
		r.corr_len = cmd->corr_len;
	}

	if (payload != NULL && strlen(payload) >= sizeof(r.payload)) {
		r.msg_type = COO_CMD_RESP_ERROR;
		snprintk(r.payload, sizeof(r.payload), "%s", overflow_msg);
		r.payload_len = strlen(r.payload);
		return r;
	}

	snprintk(r.payload, sizeof(r.payload), "%s", payload != NULL ? payload : "");
	r.payload_len = strlen(r.payload);
	return r;
}

int coo_cmd_publish_mqtt(struct mqtt_client *client,
			 const struct coo_cmd_response *out,
			 uint16_t *message_id)
{
	struct mqtt_publish_param param;

	if (client == NULL || out == NULL || message_id == NULL) {
		return -EINVAL;
	}

	memset(&param, 0, sizeof(param));
	param.message.topic.qos = out->qos;
	param.message.topic.topic.utf8 = (uint8_t *)out->topic;
	param.message.topic.topic.size = strlen(out->topic);
	param.message.payload.data = (uint8_t *)out->payload;
	param.message.payload.len = out->payload_len;
	param.prop.correlation_data.data = (uint8_t *)out->correlation_data;
	param.prop.correlation_data.len = out->corr_len;
	param.message_id = (*message_id)++;
	param.dup_flag = 0U;
	param.retain_flag = 0U;

	return mqtt_publish(client, &param);
}

void coo_cmd_print_serial_response(const struct coo_cmd_response *out,
				   uint16_t wrap_column)
{
	size_t len;
	uint16_t col = 0U;

	if (out == NULL) {
		return;
	}

	printk("%s\n\t", out->topic[0] != '\0' ? out->topic : "serial");
	col = 8U;
	len = out->payload_len > 0U ? out->payload_len : strlen(out->payload);

	for (size_t i = 0U; i < len && out->payload[i] != '\0'; ++i) {
		const char ch = out->payload[i];

		if (ch == '\n' || col >= wrap_column) {
			printk("\n\t");
			col = 8U;
			if (ch == '\n') {
				continue;
			}
		}

		printk("%c", ch);
		col++;

		if ((ch == ',' || ch == '}') && col >= (wrap_column - 8U) &&
		    i + 1U < len) {
			printk("\n\t");
			col = 8U;
		}
	}

	printk("\n");
}
