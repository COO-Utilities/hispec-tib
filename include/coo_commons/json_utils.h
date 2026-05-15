/*
 * Copyright (c) 2024 Caltech Optical Observatories
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef APP_LIB_JSON_UTILS_H_
#define APP_LIB_JSON_UTILS_H_

#include <zephyr/data/json.h>
#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdarg.h>

/**
 * @file json_utils.h
 * @brief Lightweight JSON convenience wrappers for command handling.
 */

enum coo_msg_type {
	COO_MSG_GET = 0,
	COO_MSG_SET = 1,
};

enum coo_json_extract_status {
	COO_JSON_EXTRACT_MISSING = -1,
	COO_JSON_EXTRACT_OK = 0,
	COO_JSON_EXTRACT_ERR = 1,
};

bool coo_json_parse_msg_type(const char *payload, enum coo_msg_type *msg_type_out);

/* Return values use enum coo_json_extract_status. */
int coo_json_extract_bool(const char *json, const char *key, bool *value);
int coo_json_extract_u32(const char *json, const char *key, uint32_t *value);
int coo_json_extract_u64(const char *json, const char *key, uint64_t *value);
int coo_json_extract_float(const char *json, const char *key, float *value);
/** Extract one required JSON number into a double. */
int coo_json_extract_double(const char *json, const char *key, double *value);
/** Extract a JSON number array into @p values. Supports up to eight doubles. */
int coo_json_extract_double_array(const char *json, const char *key,
				  double *values, size_t max_values,
				  size_t *parsed_len);
/**
 * @brief Parse an optional float field and reject values outside a range.
 *
 * Missing fields are not errors and leave @p value unchanged. On success with
 * a present field, @p value is updated and @p changed is set true.
 *
 * @retval 0 Field was missing or parsed within range.
 * @retval -EINVAL Bad arguments, malformed JSON field, or out-of-range value.
 */
int coo_json_extract_optional_float_range(const char *json, const char *key,
					  float *value, bool *changed,
					  float min_value, float max_value);
int coo_json_extract_string(const char *json, const char *key, char *out, size_t out_len);
/**
 * @brief Copy a nested JSON object value into @p out.
 *
 * This is a bounded helper for command schemas with optional nested objects.
 * The copied string includes the surrounding braces.
 */
int coo_json_extract_object(const char *json, const char *key, char *out, size_t out_len);
/**
 * @brief Append formatted JSON text to a fixed buffer.
 *
 * Updates @p offset only on success. This is intentionally small and format-
 * oriented because command telemetry uses static buffers and must avoid
 * dynamic allocation.
 */
int coo_json_append(char *buf, size_t buf_len, size_t *offset,
		    const char *fmt, ...);
int coo_json_vappend(char *buf, size_t buf_len, size_t *offset,
		     const char *fmt, va_list args);
/** Append a JSON number or null when @p value is NaN. */
int coo_json_append_float_or_null(char *buf, size_t buf_len, size_t *offset,
				  double value, int precision);

#endif /* APP_LIB_JSON_UTILS_H_ */
