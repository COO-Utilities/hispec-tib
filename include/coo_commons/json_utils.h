/*
 * Copyright (c) 2024 Caltech Optical Observatories
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef APP_LIB_JSON_UTILS_H_
#define APP_LIB_JSON_UTILS_H_

#include <zephyr/data/json.h>
#include <stddef.h>
#include <stdbool.h>

/**
 * @file json_utils.h
 * @brief JSON utilities for COO applications
 *
 * This module provides helper functions for structured message handling
 * using JSON format for telemetry, commands, and configuration.
 */

/**
 * @brief Maximum JSON message size
 */
#define COO_JSON_MAX_SIZE 512

/**
 * @brief Message type enumeration for command/response handling
 */
enum coo_msg_type {
	COO_MSG_GET = 0,       /**< GET request */
	COO_MSG_SET = 1,       /**< SET request */
	COO_MSG_RESP_OK = 2,   /**< Successful response */
	COO_MSG_RESP_ERROR = 3 /**< Error response */
};

/**
 * @brief Standard error response strings
 */
#define COO_JSON_ERR_UNKNOWN      "{\"error\":\"Unknown request\"}"
#define COO_JSON_ERR_UNSUPPORTED  "{\"error\":\"Unsupported operation\"}"
#define COO_JSON_ERR_BUSY         "{\"error\":\"Busy\"}"
#define COO_JSON_ERR_INVALID      "{\"error\":\"Invalid or unrecognized command\"}"
#define COO_JSON_OK               "{\"status\":\"OK\"}"

/**
 * @brief Common telemetry message structure
 */
struct coo_telemetry_msg {
	/** Timestamp in milliseconds */
	int64_t timestamp;
	/** Device/sensor identifier */
	const char *device_id;
	/** Temperature value (if applicable) */
	float temperature;
	/** Status code */
	int status;
};

/**
 * @brief Encode telemetry message to JSON
 *
 * @param msg Telemetry message structure
 * @param buf Output buffer
 * @param buf_size Size of output buffer
 * @return Number of bytes written, or negative error code
 */
int coo_json_encode_telemetry(const struct coo_telemetry_msg *msg,
			       char *buf, size_t buf_size);

/**
 * @brief Parse JSON command message
 *
 * @param json_str JSON string to parse
 * @param cmd_out Output buffer for command string
 * @param cmd_size Size of command buffer
 * @param value_out Pointer to store numeric value (if present)
 * @return 0 on success, negative error code on failure
 */
int coo_json_parse_command(const char *json_str, char *cmd_out, size_t cmd_size,
			    float *value_out);

/**
 * @brief Parse message type from JSON payload
 *
 * Extracts the "msg_type" field from a JSON payload string.
 * Supports case-insensitive "get" and "set" values.
 *
 * Example JSON: {"msg_type": "get", ...}
 *
 * @param payload JSON payload string (null-terminated)
 * @param msg_type_out Pointer to store parsed message type
 * @return true if successfully parsed, false otherwise
 */
bool coo_json_parse_msg_type(const char *payload, enum coo_msg_type *msg_type_out);

/**
 * @brief Parse a key/value pair separated by slash
 *
 * Parses strings in the format "name/setting" into separate components.
 * Useful for hierarchical command keys like "laser1430/flux" or "atten/value".
 *
 * @param key Input string in format "name/setting"
 * @param out_name Buffer to store name component
 * @param max_name Size of out_name buffer
 * @param out_setting Buffer to store setting component
 * @param max_setting Size of out_setting buffer
 * @return 0 on success, negative error code on failure
 *         -1: no slash found
 *         -2: name empty or too long
 *         -3: setting empty or too long
 */
int coo_json_parse_key_pair(const char *key,
                             char *out_name, size_t max_name,
                             char *out_setting, size_t max_setting);

#endif /* APP_LIB_JSON_UTILS_H_ */
