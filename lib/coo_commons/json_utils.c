/*
 * Copyright (c) 2024 Caltech Optical Observatories
 * SPDX-License-Identifier: Apache-2.0
 */

#include <coo_commons/json_utils.h>
#include <zephyr/data/json.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>

/* JSON descriptor for telemetry message */
static const struct json_obj_descr telemetry_descr[] = {
	JSON_OBJ_DESCR_PRIM(struct coo_telemetry_msg, timestamp, JSON_TOK_NUMBER),
	JSON_OBJ_DESCR_PRIM(struct coo_telemetry_msg, device_id, JSON_TOK_STRING),
	JSON_OBJ_DESCR_PRIM(struct coo_telemetry_msg, temperature, JSON_TOK_FLOAT),
	JSON_OBJ_DESCR_PRIM(struct coo_telemetry_msg, status, JSON_TOK_NUMBER),
};

int coo_json_encode_telemetry(const struct coo_telemetry_msg *msg,
			       char *buf, size_t buf_size)
{
	int ret;

	ret = json_obj_encode_buf(telemetry_descr, ARRAY_SIZE(telemetry_descr),
				   msg, buf, buf_size);
	if (ret < 0) {
		return ret;
	}

	return ret;
}

int coo_json_parse_command(const char *json_str, char *cmd_out, size_t cmd_size,
			    float *value_out)
{
	/* Simple JSON parsing - expects format: {"cmd":"<command>","value":<number>} */
	/* For production, use Zephyr's JSON library with proper descriptors */

	const char *cmd_start, *cmd_end;
	const char *val_start;

	/* Find command field */
	cmd_start = strstr(json_str, "\"cmd\":\"");
	if (cmd_start == NULL) {
		return -EINVAL;
	}
	cmd_start += 7; /* Skip "cmd":" */

	cmd_end = strchr(cmd_start, '\"');
	if (cmd_end == NULL) {
		return -EINVAL;
	}

	size_t cmd_len = cmd_end - cmd_start;
	if (cmd_len >= cmd_size) {
		return -ENOMEM;
	}

	memcpy(cmd_out, cmd_start, cmd_len);
	cmd_out[cmd_len] = '\0';

	/* Find value field (optional) */
	if (value_out != NULL) {
		val_start = strstr(json_str, "\"value\":");
		if (val_start != NULL) {
			val_start += 8; /* Skip "value": */
			*value_out = strtof(val_start, NULL);
		} else {
			*value_out = 0.0f;
		}
	}

	return 0;
}

struct json_type_msg {
	char msg_type[8];
};

bool coo_json_parse_msg_type(const char *payload, enum coo_msg_type *msg_type_out)
{
	struct json_type_msg msg = {0};
	const struct json_obj_descr descr[] = {
		JSON_OBJ_DESCR_PRIM(struct json_type_msg, msg_type, JSON_TOK_STRING)
	};

	int rc = json_obj_parse((char *)payload, strlen(payload), descr, ARRAY_SIZE(descr), &msg);
	if (rc < 0) {
		return false;
	}

	/* Case-insensitive check for supported types */
	if (strncasecmp(msg.msg_type, "get", 4) == 0) {
		*msg_type_out = COO_MSG_GET;
		return true;
	}
	if (strncasecmp(msg.msg_type, "set", 4) == 0) {
		*msg_type_out = COO_MSG_SET;
		return true;
	}
	return false;
}

int coo_json_parse_key_pair(const char *key,
                             char *out_name, size_t max_name,
                             char *out_setting, size_t max_setting)
{
	/* Find the first slash */
	const char *slash = strchr(key, '/');
	if (!slash) {
		return -1;
	}

	size_t name_len = slash - key;
	if (name_len == 0 || name_len >= max_name) {
		/* Name empty or too long for buffer (including null) */
		return -2;
	}

	/* Copy name */
	memcpy(out_name, key, name_len);
	out_name[name_len] = '\0';

	/* Copy setting, up to max_setting-1 characters, null terminated */
	const char *setting_start = slash + 1;
	size_t setting_len = strcspn(setting_start, "/"); /* Up to next '/', or full string */
	if (setting_len == 0 || setting_len >= max_setting) {
		/* Setting empty or too long for buffer */
		return -3;
	}
	memcpy(out_setting, setting_start, setting_len);
	out_setting[setting_len] = '\0';

	return 0;
}
