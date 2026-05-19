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
#include <ctype.h>
#include <errno.h>
#include <stdarg.h>
#include <zephyr/data/json.h>
#include <zephyr/sys/util.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>

#define COO_JSON_DOUBLE_ARRAY_MAX 8U

const char *coo_json_skip_ws(const char *text)
{
	while (text != NULL && isspace((unsigned char)*text)) {
		text++;
	}

	return text;
}

int coo_json_match_string_choice(const char *text,
				 const struct coo_json_string_choice *choices,
				 size_t choice_count,
				 int *value)
{
	if (text == NULL || choices == NULL || value == NULL) {
		return -EINVAL;
	}

	for (size_t i = 0U; i < choice_count; ++i) {
		if (choices[i].name != NULL &&
		    strcasecmp(text, choices[i].name) == 0) {
			*value = choices[i].value;
			return 0;
		}
	}

	return -ENOENT;
}

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

int coo_json_extract_optional_bool(const char *json, const char *key,
				   bool *value, bool *changed)
{
	bool parsed;
	int rc;

	if (value == NULL) {
		return -EINVAL;
	}

	rc = coo_json_extract_bool(json, key, &parsed);
	if (rc == COO_JSON_EXTRACT_MISSING) {
		return 0;
	}
	if (rc == COO_JSON_EXTRACT_ERR) {
		return -EINVAL;
	}

	*value = parsed;
	if (changed != NULL) {
		*changed = true;
	}
	return 0;
}

int coo_json_extract_optional_u32(const char *json, const char *key,
				  uint32_t *value, bool *changed)
{
	uint32_t parsed;
	int rc;

	if (value == NULL) {
		return -EINVAL;
	}

	rc = coo_json_extract_u32(json, key, &parsed);
	if (rc == COO_JSON_EXTRACT_MISSING) {
		return 0;
	}
	if (rc == COO_JSON_EXTRACT_ERR) {
		return -EINVAL;
	}

	*value = parsed;
	if (changed != NULL) {
		*changed = true;
	}
	return 0;
}

int coo_json_extract_optional_u16(const char *json, const char *key,
				  uint16_t *value, bool *changed)
{
	uint32_t parsed;
	bool present = false;

	if (value == NULL) {
		return -EINVAL;
	}

	parsed = *value;
	if (coo_json_extract_optional_u32(json, key, &parsed, &present) != 0 ||
	    parsed > UINT16_MAX) {
		return -EINVAL;
	}
	if (present) {
		*value = (uint16_t)parsed;
		if (changed != NULL) {
			*changed = true;
		}
	}

	return 0;
}

int coo_json_extract_optional_double_range(const char *json, const char *key,
					   double *value, bool *changed,
					   double min_value, double max_value)
{
	double parsed;
	int rc;

	if (value == NULL || changed == NULL || !(min_value <= max_value)) {
		return -EINVAL;
	}

	rc = coo_json_extract_double(json, key, &parsed);
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

int coo_json_extract_string_choice(const char *json,
				   const char *key,
				   const struct coo_json_string_choice *choices,
				   size_t choice_count,
				   int *value)
{
	char text[32] = {0};
	int rc;

	if (choices == NULL || choice_count == 0U || value == NULL) {
		return COO_JSON_EXTRACT_ERR;
	}

	rc = coo_json_extract_string(json, key, text, sizeof(text));
	if (rc != COO_JSON_EXTRACT_OK) {
		return rc;
	}

	return coo_json_match_string_choice(text, choices, choice_count, value) == 0 ?
	       COO_JSON_EXTRACT_OK : COO_JSON_EXTRACT_ERR;
}

int coo_json_extract_object(const char *json, const char *key, char *out, size_t out_len)
{
	char pattern[80];
	const char *match;
	const char *cursor;
	const char *start;
	int depth = 0;
	bool in_string = false;
	bool escaped = false;
	int written;

	if (json == NULL || key == NULL || out == NULL || out_len == 0U ||
	    strlen(key) > 60U) {
		return COO_JSON_EXTRACT_ERR;
	}

	written = snprintf(pattern, sizeof(pattern), "\"%s\"", key);
	if (written < 0 || written >= (int)sizeof(pattern)) {
		return COO_JSON_EXTRACT_ERR;
	}

	match = strstr(json, pattern);
	if (match == NULL) {
		return COO_JSON_EXTRACT_MISSING;
	}

	cursor = match + strlen(pattern);
	while (*cursor == ' ' || *cursor == '\t' || *cursor == '\r' || *cursor == '\n') {
		cursor++;
	}
	if (*cursor != ':') {
		return COO_JSON_EXTRACT_ERR;
	}
	cursor++;
	while (*cursor == ' ' || *cursor == '\t' || *cursor == '\r' || *cursor == '\n') {
		cursor++;
	}
	if (*cursor != '{') {
		return COO_JSON_EXTRACT_ERR;
	}

	start = cursor;
	for (; *cursor != '\0'; ++cursor) {
		char c = *cursor;

		if (escaped) {
			escaped = false;
			continue;
		}
		if (in_string && c == '\\') {
			escaped = true;
			continue;
		}
		if (c == '"') {
			in_string = !in_string;
			continue;
		}
		if (in_string) {
			continue;
		}
		if (c == '{') {
			depth++;
		} else if (c == '}') {
			depth--;
			if (depth == 0) {
				size_t len = (size_t)(cursor - start + 1);

				if (len >= out_len) {
					return COO_JSON_EXTRACT_ERR;
				}
				memcpy(out, start, len);
				out[len] = '\0';
				return COO_JSON_EXTRACT_OK;
			}
		}
	}

	return COO_JSON_EXTRACT_ERR;
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

int coo_json_append_float_or_null(char *buf, size_t buf_len, size_t *offset,
				  double value, int precision)
{
	if (value != value) {
		return coo_json_append(buf, buf_len, offset, "null");
	}

	switch (precision) {
	case 0:
		return coo_json_append(buf, buf_len, offset, "%.0f", value);
	case 1:
		return coo_json_append(buf, buf_len, offset, "%.1f", value);
	case 2:
		return coo_json_append(buf, buf_len, offset, "%.2f", value);
	case 4:
		return coo_json_append(buf, buf_len, offset, "%.4f", value);
	case 6:
		return coo_json_append(buf, buf_len, offset, "%.6f", value);
	default:
		return coo_json_append(buf, buf_len, offset, "%.3f", value);
	}
}
