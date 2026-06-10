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

enum coo_json_extract_status {
	COO_JSON_EXTRACT_MISSING = -1,
	COO_JSON_EXTRACT_OK = 0,
	COO_JSON_EXTRACT_ERR = 1,
};

/* Fixed buffer used by coo_json_extract_string_choice(). */
#define COO_JSON_STRING_CHOICE_MAX 32U

struct coo_json_string_choice {
	const char *name;
	int value;
};

/** Return @p text advanced past ASCII JSON whitespace. */
const char *coo_json_skip_ws(const char *text);

/**
 * @brief Validate that all top-level object keys are in @p allowed_keys.
 *
 * @p allowed_keys is a comma-separated list of accepted top-level key names.
 * NULL or an empty string means no keys are accepted. Nested object/array keys
 * are skipped; callers that accept nested objects own nested validation.
 *
 * @retval 0 JSON is an object and every top-level key is allowed.
 * @retval -ENOENT A top-level key is not allowed; @p unknown_key receives it
 *                 when a destination buffer is supplied.
 * @retval -EINVAL Input is not a valid JSON object for this lightweight check.
 * @retval -ENOSPC A key does not fit in @p unknown_key.
 */
int coo_json_validate_top_level_keys(const char *json,
				     const char *allowed_keys,
				     char *unknown_key,
				     size_t unknown_key_len);

/**
 * Match @p text against a case-insensitive static string-choice table.
 *
 * This helper only validates the command token and writes the associated
 * integer value; command modules still own the table and enum meaning.
 */
int coo_json_match_string_choice(const char *text,
				 const struct coo_json_string_choice *choices,
				 size_t choice_count,
				 int *value);

/* Return values use enum coo_json_extract_status. */
int coo_json_extract_bool(const char *json, const char *key, bool *value);
int coo_json_extract_u32(const char *json, const char *key, uint32_t *value);
int coo_json_extract_u64(const char *json, const char *key, uint64_t *value);
/** Extract one required JSON number into a double. */
int coo_json_extract_double(const char *json, const char *key, double *value);
/** Extract a JSON number array into @p values. Supports up to 32 doubles. */
int coo_json_extract_double_array(const char *json, const char *key,
				  double *values, size_t max_values,
				  size_t *parsed_len);
/**
 * @brief Parse an optional bool field.
 *
 * Missing fields are not errors and leave @p value unchanged. When @p changed
 * is non-NULL, it is set true only when the field was present.
 *
 * @retval 0 Field was missing or parsed.
 * @retval -EINVAL Bad arguments or malformed JSON field.
 */
int coo_json_extract_optional_bool(const char *json, const char *key,
				   bool *value, bool *changed);
/**
 * @brief Parse an optional unsigned integer field.
 *
 * Missing fields are not errors and leave @p value unchanged. When @p changed
 * is non-NULL, it is set true only when the field was present.
 *
 * @retval 0 Field was missing or parsed.
 * @retval -EINVAL Bad arguments or malformed JSON field.
 */
int coo_json_extract_optional_u32(const char *json, const char *key,
				  uint32_t *value, bool *changed);
/** Parse an optional unsigned integer field into a uint16_t destination. */
int coo_json_extract_optional_u16(const char *json, const char *key,
				  uint16_t *value, bool *changed);
/**
 * @brief Parse an optional double field and reject values outside a range.
 *
 * Missing fields are not errors and leave @p value unchanged. On success with
 * a present field, @p value is updated and @p changed is set true.
 *
 * @retval 0 Field was missing or parsed within range.
 * @retval -EINVAL Bad arguments, malformed JSON field, or out-of-range value.
 */
int coo_json_extract_optional_double_range(const char *json, const char *key,
					   double *value, bool *changed,
					   double min_value, double max_value);
int coo_json_extract_string(const char *json, const char *key, char *out, size_t out_len);
/**
 * Extract a JSON string field and match it against a static choice table.
 *
 * Return values use enum coo_json_extract_status. Missing fields remain
 * distinguishable from malformed JSON or unknown values.
 */
int coo_json_extract_string_choice(const char *json,
				   const char *key,
				   const struct coo_json_string_choice *choices,
				   size_t choice_count,
				   int *value);
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
