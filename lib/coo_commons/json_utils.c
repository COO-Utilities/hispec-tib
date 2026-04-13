/*
 * Copyright (c) 2024 Caltech Optical Observatories
 * SPDX-License-Identifier: Apache-2.0
 */

#include <coo_commons/json_utils.h>
#include <zephyr/data/json.h>
#include <zephyr/sys/util.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <stdlib.h>

struct json_type_msg {
	char msg_type[8];
};

static const char *find_json_key_value(const char *json, const char *key)
{
	char token[40];
	const char *p;

	if (json == NULL || key == NULL) {
		return NULL;
	}

	snprintf(token, sizeof(token), "\"%s\"", key);
	p = strstr(json, token);
	if (p == NULL) {
		return NULL;
	}

	p += strlen(token);
	while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') {
		p++;
	}
	if (*p != ':') {
		return NULL;
	}
	p++;
	while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') {
		p++;
	}

	return p;
}

bool coo_json_parse_msg_type(const char *payload, enum coo_msg_type *msg_type_out)
{
	struct json_type_msg msg = {0};
	const struct json_obj_descr descr[] = {
		JSON_OBJ_DESCR_PRIM(struct json_type_msg, msg_type, JSON_TOK_STRING)
	};

	if (payload == NULL || msg_type_out == NULL) {
		return false;
	}

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

bool coo_json_extract_bool(const char *json, const char *key, bool *value)
{
	const char *p = find_json_key_value(json, key);

	if (p == NULL || value == NULL) {
		return false;
	}

	if (strncmp(p, "true", 4) == 0) {
		*value = true;
		return true;
	}
	if (strncmp(p, "false", 5) == 0) {
		*value = false;
		return true;
	}

	return false;
}

bool coo_json_extract_u32(const char *json, const char *key, uint32_t *value)
{
	char *end;
	unsigned long tmp;
	const char *p = find_json_key_value(json, key);

	if (p == NULL || value == NULL) {
		return false;
	}

	tmp = strtoul(p, &end, 10);
	if (end == p) {
		return false;
	}

	*value = (uint32_t)tmp;
	return true;
}

bool coo_json_extract_u64(const char *json, const char *key, uint64_t *value)
{
	char *end;
	unsigned long long tmp;
	const char *p = find_json_key_value(json, key);

	if (p == NULL || value == NULL) {
		return false;
	}

	tmp = strtoull(p, &end, 10);
	if (end == p) {
		return false;
	}

	*value = (uint64_t)tmp;
	return true;
}

bool coo_json_extract_string(const char *json, const char *key, char *out, size_t out_len)
{
	const char *p = find_json_key_value(json, key);
	const char *end;
	size_t len;

	if (p == NULL || out == NULL || out_len == 0U || *p != '"') {
		return false;
	}
	p++;

	end = strchr(p, '"');
	if (end == NULL) {
		return false;
	}

	len = MIN((size_t)(end - p), out_len - 1U);
	memcpy(out, p, len);
	out[len] = '\0';
	return true;
}

bool coo_json_has_key(const char *json, const char *key)
{
	return find_json_key_value(json, key) != NULL;
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
