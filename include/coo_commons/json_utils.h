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
int coo_json_extract_string(const char *json, const char *key, char *out, size_t out_len);

#endif /* APP_LIB_JSON_UTILS_H_ */
