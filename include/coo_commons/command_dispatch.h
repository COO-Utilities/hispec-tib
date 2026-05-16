/*
 * Copyright (c) 2026 Caltech Optical Observatories
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef COO_COMMONS_COMMAND_DISPATCH_H
#define COO_COMMONS_COMMAND_DISPATCH_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include <zephyr/kernel.h>
#include <zephyr/net/mqtt.h>

/**
 * @file command_dispatch.h
 * @brief Fixed-buffer command request, dispatch, and response helpers.
 *
 * This utility is intentionally small. Applications own their queues, command
 * table, request classification policy, and domain handlers. The helper only
 * provides reusable static dispatch, bounded serial payload normalization, and
 * transport-shaped response handling for MQTT plus line-oriented serial.
 */

#define COO_CMD_TOPIC_MAX 96
#define COO_CMD_KEY_MAX 48
#define COO_CMD_REQID_MAX 32
#define COO_CMD_SESSION_ID_MAX 48
#define COO_CMD_CORRELATION_MAX 16
#define COO_CMD_SERIAL_WRAP_COLUMN 80U

#if defined(CONFIG_COO_MQTT_PAYLOAD_SIZE)
#define COO_CMD_PAYLOAD_MAX CONFIG_COO_MQTT_PAYLOAD_SIZE
#else
#define COO_CMD_PAYLOAD_MAX 256
#endif

/**
 * @brief Request/response class used by the static dispatcher.
 *
 * Requests use only COO_CMD_QUERY and COO_CMD_EFFECT. Responses use only
 * COO_CMD_RESP_OK and COO_CMD_RESP_ERROR. The enum remains shared so existing
 * simple dispatch tables can choose a handler and build a response without a
 * second conversion type.
 */
enum coo_cmd_msg_type {
	COO_CMD_QUERY = 0,
	COO_CMD_EFFECT = 1,
	COO_CMD_ACK = 2,
	COO_CMD_RESP_OK = 3,
	COO_CMD_RESP_ERROR = 4,
};

/** Ingress path used for response routing and app-local guard policy. */
enum coo_cmd_source {
	COO_CMD_SOURCE_MQTT = 0,
	COO_CMD_SOURCE_SERIAL = 1,
};

/** Publication target for responses, warnings, and telemetry. */
enum coo_cmd_out_target {
	COO_CMD_OUT_MQTT = 0,
	COO_CMD_OUT_SERIAL = 1,
	COO_CMD_OUT_MQTT_BEST_EFFORT = 2,
};

struct coo_cmd_request {
	enum coo_cmd_msg_type msg_type;
	enum coo_cmd_source source;
	char key[COO_CMD_KEY_MAX];
	char session_id[COO_CMD_SESSION_ID_MAX];
	char response_topic[COO_CMD_TOPIC_MAX];
	size_t payload_len;
	char payload[COO_CMD_PAYLOAD_MAX];
	uint8_t correlation_data[COO_CMD_CORRELATION_MAX];
	uint32_t corr_len;
};

struct coo_cmd_response {
	enum coo_cmd_msg_type msg_type;
	enum coo_cmd_out_target target;
	char topic[COO_CMD_TOPIC_MAX];
	uint8_t qos;
	size_t payload_len;
	char payload[COO_CMD_PAYLOAD_MAX];
	uint8_t correlation_data[COO_CMD_CORRELATION_MAX];
	size_t corr_len;
};

struct coo_cmd_work {
	struct k_work work;
	struct coo_cmd_request cmd;
};

typedef struct coo_cmd_response (*coo_cmd_handler_fn)(const struct coo_cmd_request *cmd);

struct coo_cmd_dispatch_entry {
	const char *key;
	coo_cmd_handler_fn get_handler;
	coo_cmd_handler_fn set_handler;
};

typedef int (*coo_cmd_format_response_topic_fn)(const char *key,
						char *out,
						size_t out_len,
						void *user_data);

typedef int (*coo_cmd_serial_shorthand_fn)(const char *key,
					   const char *payload,
					   char *out,
					   size_t out_len,
					   void *user_data);

/** Return true when @p key starts with @p prefix and is exact or slash-delimited. */
bool coo_cmd_key_matches_prefix(const char *key, const char *prefix);

/** Longest exact-or-slash-prefix match in a static command table. */
const struct coo_cmd_dispatch_entry *
coo_cmd_find_dispatch(const struct coo_cmd_dispatch_entry *table,
		      size_t table_len,
		      const char *key);

/**
 * @brief Dispatch one normalized request through a static table.
 *
 * @p unknown_handler is called when no table entry matches. @p unsupported_handler
 * is called when the matching entry has no handler for the request kind.
 */
struct coo_cmd_response
coo_cmd_dispatch(const struct coo_cmd_request *cmd,
		 const struct coo_cmd_dispatch_entry *table,
		 size_t table_len,
		 coo_cmd_handler_fn unknown_handler,
		 coo_cmd_handler_fn unsupported_handler);

/** True for a missing, empty, or "{}" payload. */
bool coo_cmd_payload_empty(const struct coo_cmd_request *cmd);

/** Copy one Zephyr MQTT UTF-8 field into a C string. */
bool coo_cmd_copy_mqtt_utf8(const struct mqtt_utf8 *topic,
			    char *out,
			    size_t out_len);

/**
 * @brief Normalize a serial payload into compact JSON.
 *
 * Empty payload becomes "{}"; raw JSON beginning with "{" is copied unchanged;
 * key=value tokens become a JSON object. Other payloads are passed to
 * @p shorthand when supplied, otherwise a single token becomes {"value":...}.
 */
int coo_cmd_normalize_serial_payload(const char *key,
				     const char *payload,
				     coo_cmd_serial_shorthand_fn shorthand,
				     void *user_data,
				     char *out,
				     size_t out_len);

/**
 * @brief Build a response that preserves request routing metadata.
 *
 * The formatter is called for the default response topic. A request-provided
 * response_topic overrides it when present and fitting the fixed topic buffer.
 * MQTT correlation data is echoed exactly when it fits the request buffer.
 */
struct coo_cmd_response
coo_cmd_make_response(const struct coo_cmd_request *cmd,
		      enum coo_cmd_msg_type msg_type,
		      const char *payload,
		      coo_cmd_format_response_topic_fn format_topic,
		      void *user_data);

/** Publish a formatted MQTT response/publication. May block in the socket layer. */
int coo_cmd_publish_mqtt(struct mqtt_client *client,
			 const struct coo_cmd_response *out,
			 uint16_t *message_id);

/** Print a serial response as topic then tab-indented wrapped payload. */
void coo_cmd_print_serial_response(const struct coo_cmd_response *out,
				   uint16_t wrap_column);

#endif /* COO_COMMONS_COMMAND_DISPATCH_H */
