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

typedef void (*coo_cmd_serial_line_fn)(char *line, void *user_data);

/**
 * @brief Runtime wiring for a simple command executor and output drain.
 *
 * The application owns the queues, command table, optional execute hook, line
 * parser, warning topic, and MQTT message-id storage. If execute_handler is
 * NULL, the executor uses the static dispatch table directly. The runtime
 * helpers do not allocate memory; they block only in the executor queue wait,
 * Zephyr console line read, and MQTT publish path used by the outbound drain.
 */
struct coo_cmd_runtime {
	struct k_msgq *inbound_queue;
	struct k_msgq *outbound_queue;
	coo_cmd_handler_fn execute_handler;
	const struct coo_cmd_dispatch_entry *dispatch_table;
	size_t dispatch_count;
	coo_cmd_handler_fn unknown_handler;
	coo_cmd_handler_fn unsupported_handler;
	uint16_t *mqtt_msg_id;
	const char *warning_topic;
	uint16_t serial_wrap_column;
	coo_cmd_serial_line_fn serial_line_handler;
	void *serial_line_user_data;
	bool outbound_full_warning_seen;
	bool outbound_full_warning_mqtt_seen;
};

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

/** Return the next whitespace-delimited serial token and advance @p cursor. */
bool coo_cmd_serial_next_token(const char **cursor, char *out, size_t out_len);

/** Return true when non-space payload text remains at @p cursor. */
bool coo_cmd_serial_has_extra(const char *cursor);

/** Return true when @p token is a complete JSON-compatible number token. */
bool coo_cmd_serial_token_is_number(const char *token);

/** Append one token as a JSON value, preserving numbers/bools/null. */
int coo_cmd_serial_append_json_value(char *out, size_t out_len, size_t *off,
				     const char *token);

/** Append one `"key":value` field, optionally preceded by a comma. */
int coo_cmd_serial_append_json_field(char *out, size_t out_len, size_t *off,
				     const char *key, const char *token,
				     bool comma);

/**
 * @brief Build a response that preserves request routing metadata.
 *
 * When @p format_topic is non-NULL, it is called for the default response
 * topic. A request-provided response_topic overrides it when present and
 * fitting the fixed topic buffer. When @p format_topic is NULL, the already
 * normalized cmd->response_topic is used directly.
 *
 * MQTT correlation data is echoed exactly when it fits the request buffer.
 */
struct coo_cmd_response
coo_cmd_make_response(const struct coo_cmd_request *cmd,
		      enum coo_cmd_msg_type msg_type,
		      const char *payload,
		      coo_cmd_format_response_topic_fn format_topic,
		      void *user_data);

/**
 * @brief Build a response using the request's normalized response topic.
 *
 * Applications that normalize cmd->response_topic before dispatch should use
 * this helper rather than repeating a local response-topic wrapper in each
 * command adapter.
 */
struct coo_cmd_response
coo_cmd_reply(const struct coo_cmd_request *cmd,
		 enum coo_cmd_msg_type msg_type,
		 const char *payload);

/** @brief Build the standard data-less success response: {"status":"ok"}. */
struct coo_cmd_response coo_cmd_ok(const struct coo_cmd_request *cmd);

/** @brief Build a structured error response with one error string. */
struct coo_cmd_response coo_cmd_error(const struct coo_cmd_request *cmd,
				      const char *msg);

/** @brief Build a structured error response with one error string and rc. */
struct coo_cmd_response coo_cmd_error_rc(const struct coo_cmd_request *cmd,
					 const char *msg,
					 int rc);

/** @brief Build the standard malformed-command error response. */
struct coo_cmd_response coo_cmd_invalid_response(const struct coo_cmd_request *cmd);

/** @brief Build the standard unknown-command error response. */
struct coo_cmd_response coo_cmd_unknown_response(const struct coo_cmd_request *cmd);

/** @brief Build the standard unsupported-operation error response. */
struct coo_cmd_response coo_cmd_unsupported_response(const struct coo_cmd_request *cmd);

/** @brief Build the standard busy error response. */
struct coo_cmd_response coo_cmd_busy_response(const struct coo_cmd_request *cmd);

/** @brief Build the standard serial-guard-active error response. */
struct coo_cmd_response coo_cmd_serial_active_response(const struct coo_cmd_request *cmd);

/**
 * @brief Build a best-effort warning publication.
 *
 * The caller supplies the already formatted warning topic, usually a telemetry
 * topic such as `dt/<device>/warning`. The response target is
 * COO_CMD_OUT_MQTT_BEST_EFFORT and the payload is a compact warning JSON
 * object with severity, code, msg, context, and uptime_ms.
 */
int coo_cmd_build_warning(struct coo_cmd_response *out,
			  const char *topic,
			  const char *code,
			  const char *msg,
			  const char *context);

/**
 * @brief Log and enqueue one best-effort warning without blocking.
 *
 * Warnings are lossy by design. This helper never publishes MQTT directly and
 * returns an error if the payload cannot be built or the queue is full.
 */
int coo_cmd_warning_emit(struct k_msgq *outbound_queue,
			 const char *topic,
			 const char *code,
			 const char *msg,
			 const char *context);

/** Publish a formatted MQTT response/publication. May block in the socket layer. */
int coo_cmd_publish_mqtt(struct mqtt_client *client,
			 const struct coo_cmd_response *out,
			 uint16_t *message_id);

/** Execute commands from runtime->inbound_queue and enqueue one response each. */
void coo_cmd_runtime_executor_thread(void *p1, void *p2, void *p3);

/** Read Zephyr console lines and pass them to runtime->serial_line_handler. */
void coo_cmd_runtime_serial_thread(void *p1, void *p2, void *p3);

/** Drain outbound serial/MQTT responses with bounded retry behavior. */
void coo_cmd_runtime_drain_outbound(struct coo_cmd_runtime *runtime,
				    struct mqtt_client *client,
				    bool mqtt_available);

/** Print a serial response as topic then tab-indented wrapped payload. */
void coo_cmd_print_serial_response(const struct coo_cmd_response *out,
				   uint16_t wrap_column);

#endif /* COO_COMMONS_COMMAND_DISPATCH_H */
