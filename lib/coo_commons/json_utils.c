/**
 * @file json_utils.c
 * @brief Small JSON extraction helpers used by constrained command parsing.
 *
 * These helpers wrap Zephyr JSON descriptors so command handlers can validate
 * individual fields without building command-specific descriptor structs for
 * every optional value.
 */
/*
 * Copyright (c) 2024 Caltech Optical Observatories
 * SPDX-License-Identifier: Apache-2.0
 */

#include <coo_commons/json_utils.h>
#include <errno.h>
#include <stdarg.h>
#include <zephyr/data/json.h>
#include <zephyr/sys/util.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>

struct json_type_msg {
	char msg_type[8];
};

#define COO_JSON_DOUBLE_ARRAY_MAX 8U

/**
 * @brief Extract one JSON field by key using Zephyr's descriptor parser.
 *
 * This helper constructs a one-field descriptor at runtime so callers can
 * query arbitrary keys while still relying on `json_obj_parse()`.
 *
 * @param json Input JSON object string.
 * @param key Field name to parse.
 * @param token_type Expected token type for the field.
 * @param storage Struct or buffer receiving parsed output.
 * @param storage_size Size of destination field in bytes.
 * @param storage_offset Offset of destination field within @p storage.
 * @param align_shift Alignment shift for the storage type.
 *
 * @retval COO_JSON_EXTRACT_OK Field exists and was parsed successfully.
 * @retval COO_JSON_EXTRACT_MISSING Field is not present.
 * @retval COO_JSON_EXTRACT_ERR Parse failure or invalid arguments.
 */
static int find_json_key_value(const char *json,
			       const char *key,
			       enum json_tokens token_type,
			       void *storage,
			       size_t storage_size,
			       uint16_t storage_offset,
			       uint8_t align_shift)
{
	struct json_obj_descr descr;
	size_t key_len;
	int64_t rc;

	if (json == NULL || key == NULL || storage == NULL) {
		return COO_JSON_EXTRACT_ERR;
	}

	key_len = strlen(key);
	if (key_len == 0U || key_len > 127U) {
		return COO_JSON_EXTRACT_ERR;
	}

	memset(&descr, 0, sizeof(descr));
	descr.field_name = key;
	descr.field_name_len = key_len;
	descr.align_shift = align_shift;
	descr.type = token_type;
	descr.offset = storage_offset;
	descr.field.size = storage_size;

	rc = json_obj_parse((char *)json, strlen(json), &descr, 1, storage);
	if (rc < 0) {
		return COO_JSON_EXTRACT_ERR;
	}
	if ((rc & BIT64(0)) == 0) {
		return COO_JSON_EXTRACT_MISSING;
	}

	return COO_JSON_EXTRACT_OK;
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

int coo_json_extract_bool(const char *json, const char *key, bool *value)
{
	struct json_bool_field {
		bool value;
	} parsed = { 0 };
	int rc;

	if (value == NULL) {
		return COO_JSON_EXTRACT_ERR;
	}

	rc = find_json_key_value(json, key,
				 JSON_TOK_TRUE,
				 &parsed,
				 sizeof(parsed.value),
				 offsetof(struct json_bool_field, value),
				 Z_ALIGN_SHIFT(struct json_bool_field));
	if (rc == COO_JSON_EXTRACT_OK) {
		*value = parsed.value;
	}
	return rc;
}

int coo_json_extract_u32(const char *json, const char *key, uint32_t *value)
{
	struct json_u32_field {
		uint32_t value;
	} parsed = { 0 };
	int rc;

	if (value == NULL) {
		return COO_JSON_EXTRACT_ERR;
	}

	rc = find_json_key_value(json, key,
				 JSON_TOK_UINT,
				 &parsed,
				 sizeof(parsed.value),
				 offsetof(struct json_u32_field, value),
				 Z_ALIGN_SHIFT(struct json_u32_field));
	if (rc == COO_JSON_EXTRACT_OK) {
		*value = parsed.value;
	}
	return rc;
}

int coo_json_extract_u64(const char *json, const char *key, uint64_t *value)
{
	struct json_u64_field {
		uint64_t value;
	} parsed = { 0 };
	int rc;

	if (value == NULL) {
		return COO_JSON_EXTRACT_ERR;
	}

	rc = find_json_key_value(json, key,
				 JSON_TOK_UINT64,
				 &parsed,
				 sizeof(parsed.value),
				 offsetof(struct json_u64_field, value),
				 Z_ALIGN_SHIFT(struct json_u64_field));
	if (rc == COO_JSON_EXTRACT_OK) {
		*value = parsed.value;
	}
	return rc;
}

int coo_json_extract_float(const char *json, const char *key, float *value)
{
	struct json_float_field {
		float value;
	} parsed = { 0 };
	int rc;

	if (value == NULL) {
		return COO_JSON_EXTRACT_ERR;
	}

	rc = find_json_key_value(json, key,
				 JSON_TOK_NUMBER,
				 &parsed,
				 sizeof(parsed.value),
				 offsetof(struct json_float_field, value),
				 Z_ALIGN_SHIFT(struct json_float_field));
	if (rc == COO_JSON_EXTRACT_OK) {
		*value = parsed.value;
	}
	return rc;
}

int coo_json_extract_double(const char *json, const char *key, double *value)
{
	struct json_double_field {
		double value;
	} parsed = { 0 };
	int rc;

	if (value == NULL) {
		return COO_JSON_EXTRACT_ERR;
	}

	rc = find_json_key_value(json, key,
				 JSON_TOK_DOUBLE_FP,
				 &parsed,
				 sizeof(parsed.value),
				 offsetof(struct json_double_field, value),
				 Z_ALIGN_SHIFT(struct json_double_field));
	if (rc == COO_JSON_EXTRACT_OK) {
		*value = parsed.value;
	}
	return rc;
}

int coo_json_extract_double_array(const char *json, const char *key,
				  double *values, size_t max_values,
				  size_t *parsed_len)
{
	struct json_double_array_field {
		double values[COO_JSON_DOUBLE_ARRAY_MAX];
		size_t values_len;
	} parsed = { 0 };
	struct json_obj_descr descr[] = {
		JSON_OBJ_DESCR_ARRAY(struct json_double_array_field, values,
				     COO_JSON_DOUBLE_ARRAY_MAX, values_len,
				     JSON_TOK_DOUBLE_FP),
	};
	size_t key_len;
	int64_t rc;

	if (json == NULL || key == NULL || values == NULL || parsed_len == NULL ||
	    max_values == 0U || max_values > COO_JSON_DOUBLE_ARRAY_MAX) {
		return COO_JSON_EXTRACT_ERR;
	}
	*parsed_len = 0U;

	key_len = strlen(key);
	if (key_len == 0U || key_len > 127U) {
		return COO_JSON_EXTRACT_ERR;
	}

	descr[0].field_name = key;
	descr[0].field_name_len = key_len;

	rc = json_obj_parse((char *)json, strlen(json), descr, ARRAY_SIZE(descr), &parsed);
	if (rc < 0) {
		return COO_JSON_EXTRACT_ERR;
	}
	if ((rc & BIT64(0)) == 0) {
		return COO_JSON_EXTRACT_MISSING;
	}
	if (parsed.values_len > max_values) {
		return COO_JSON_EXTRACT_ERR;
	}

	memcpy(values, parsed.values, parsed.values_len * sizeof(values[0]));
	*parsed_len = parsed.values_len;
	return COO_JSON_EXTRACT_OK;
}

int coo_json_extract_optional_float_range(const char *json, const char *key,
					  float *value, bool *changed,
					  float min_value, float max_value)
{
	float parsed;
	int rc;

	if (value == NULL || changed == NULL || !(min_value <= max_value)) {
		return -EINVAL;
	}

	rc = coo_json_extract_float(json, key, &parsed);
	if (rc == COO_JSON_EXTRACT_MISSING) {
		return 0;
	}
	if (rc == COO_JSON_EXTRACT_ERR ||
	    !(parsed >= min_value && parsed <= max_value)) {
		return -EINVAL;
	}

	*value = parsed;
	*changed = true;
	return 0;
}

int coo_json_extract_string(const char *json, const char *key, char *out, size_t out_len)
{
	if (out == NULL || out_len == 0U) {
		return COO_JSON_EXTRACT_ERR;
	}

	return find_json_key_value(json, key,
				   JSON_TOK_STRING_BUF,
				   out,
				   out_len,
				   0U,
				   0U);
}

int coo_json_vappend(char *buf, size_t buf_len, size_t *offset,
		     const char *fmt, va_list args)
{
	int written;

	if (buf == NULL || offset == NULL || fmt == NULL || *offset >= buf_len) {
		return -ENOSPC;
	}

	written = vsnprintf(buf + *offset, buf_len - *offset, fmt, args);
	if (written < 0 || written >= (int)(buf_len - *offset)) {
		return -ENOSPC;
	}

	*offset += (size_t)written;
	return 0;
}

int coo_json_append(char *buf, size_t buf_len, size_t *offset,
		    const char *fmt, ...)
{
	va_list args;
	int rc;

	va_start(args, fmt);
	rc = coo_json_vappend(buf, buf_len, offset, fmt, args);
	va_end(args);

	return rc;
}
