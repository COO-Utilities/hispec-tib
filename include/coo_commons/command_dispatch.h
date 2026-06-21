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
#include <zephyr/sys/atomic.h>

struct nvs_fs;

/**
 * @file command_dispatch.h
 * @brief Fixed-buffer command request, dispatch, and response helpers.
 *
 * This utility is intentionally small. Applications own their command table and
 * domain handlers. The helper owns reusable fixed-buffer MQTT/serial topic
 * handling, built-in command execution, bounded serial payload normalization,
 * optional lastcommand persistence, warning publication, and transport-shaped
 * response handling. Applications may still provide a custom execute callback
 * for app-owned commands; library built-ins run first so an app extension cannot
 * accidentally remove help, serialguard, or reboot behavior.
 */

#define COO_CMD_TOPIC_MAX 96
#define COO_CMD_KEY_MAX 48
#define COO_CMD_REQID_MAX 32
#define COO_CMD_SESSION_ID_MAX 48
#define COO_CMD_CORRELATION_MAX 16
#define COO_CMD_SERIAL_WRAP_COLUMN 80U

#define COO_CMD_HELP_QUERY (1u << 0)
#define COO_CMD_HELP_EFFECT (1u << 1)
#define COO_CMD_HELP_SERIAL_GUARD_QUERY (1u << 2)
#define COO_CMD_HELP_BUILTIN (1u << 3)
#define COO_CMD_LASTCOMMAND_NVS_OVERHEAD 16U

#if defined(CONFIG_CONSOLE_INPUT_MAX_LINE_LEN)
#define COO_CMD_SERIAL_LINE_MAX CONFIG_CONSOLE_INPUT_MAX_LINE_LEN
#else
#define COO_CMD_SERIAL_LINE_MAX 128
#endif

#if defined(CONFIG_COO_CMD_PAYLOAD_SIZE)
#define COO_CMD_PAYLOAD_MAX CONFIG_COO_CMD_PAYLOAD_SIZE
#else
#define COO_CMD_PAYLOAD_MAX 1024
#endif

/**
 * @brief Request/response class used by the command executor.
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

enum coo_cmd_runtime_emit_type {
	COO_CMD_RUNTIME_EMIT_DATA = 0,
	COO_CMD_RUNTIME_EMIT_WARNING,
};

enum coo_cmd_runtime_emit_delivery {
	COO_CMD_RUNTIME_EMIT_BEST_EFFORT = 0,
	COO_CMD_RUNTIME_EMIT_REQUIRED,
};

enum coo_cmd_class_policy {
	COO_CMD_CLASS_DEFAULT = 0,
	COO_CMD_CLASS_ALWAYS_QUERY,
	COO_CMD_CLASS_ALWAYS_EFFECT,
	COO_CMD_CLASS_SUFFIX_OR_PAYLOAD_EFFECT,
	COO_CMD_CLASS_CUSTOM,
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

struct coo_cmd_runtime_emit_args {
	enum coo_cmd_runtime_emit_type type;
	enum coo_cmd_runtime_emit_delivery delivery;
	const char *suffix;
	struct coo_cmd_response *out;
	const char *code;
	const char *msg;
	const char *context;
};

struct coo_cmd_spec;

typedef int (*coo_cmd_handler_fn)(const struct coo_cmd_request *cmd,
				  struct coo_cmd_response *out);

typedef int (*coo_cmd_format_response_topic_fn)(const char *key,
						char *out,
						size_t out_len,
						void *user_data);

typedef int (*coo_cmd_serial_shorthand_fn)(const char *key,
					   const char *payload,
					   char *out,
					   size_t out_len,
					   void *user_data);

typedef enum coo_cmd_msg_type (*coo_cmd_classify_fn)(
	const struct coo_cmd_request *cmd,
	const struct coo_cmd_spec *spec,
	void *user_data);

typedef bool (*coo_cmd_supported_fn)(const struct coo_cmd_spec *spec,
				     void *user_data);

typedef void (*coo_cmd_reboot_prepare_fn)(bool erase_non_ip_settings,
					  void *user_data);

struct coo_cmd_lastcommand {
	bool valid;
	int64_t time_ms;
	struct coo_cmd_request request;
};

struct coo_cmd_help_entry {
	const char *key;
	const char *usage;
	const char *args;
	const char *values;
	const char *notes;
	uint32_t flags;
};

#define COO_CMD_SERIAL_POSITIONAL_MAX 3U

struct coo_cmd_serial_positional {
	const char *field[COO_CMD_SERIAL_POSITIONAL_MAX];
	uint8_t required_count;
	uint8_t numeric_mask;
};

struct coo_cmd_spec {
	const char *key;
	coo_cmd_handler_fn query_handler;
	coo_cmd_handler_fn effect_handler;
	enum coo_cmd_class_policy class_policy;
	coo_cmd_classify_fn custom_classify;
	coo_cmd_serial_shorthand_fn serial_shorthand;
	struct coo_cmd_serial_positional serial_positional;
	coo_cmd_supported_fn supported;
	/*
	 * Exact key matching is the default. Set key_prefix_match only for
	 * intentionally parameterized endpoint families such as atten/<name>/...
	 * so typos like laser/angstatus do not silently dispatch to laser.
	 */
	bool key_prefix_match;
	/*
	 * Comma-separated top-level payload keys accepted by this endpoint.
	 * NULL or "" means no payload keys are accepted.
	 */
	const char *allowed_payload_keys;
	const struct coo_cmd_help_entry *help;
	bool mqtt_query_allowed_during_serial_guard;
};

/**
 * @brief Runtime wiring for a simple command executor and output drain.
 *
 * The application owns the queues, optional app-command execute callback, and
 * MQTT message-id storage. The runtime owns the copied device identity, topic
 * formatting derived from it, library built-ins, and scratch buffers used to
 * keep large command payload storage off thread stacks. The runtime helpers do
 * not allocate memory; they block only in the executor queue wait, optional NVS
 * lastcommand persistence, reboot prepare callback, and MQTT publish path used
 * by the outbound drain.
 */
struct coo_cmd_runtime {
	struct k_msgq *inbound_queue;
	struct k_msgq *outbound_queue;
	char device_id[32];
	char request_prefix[COO_CMD_TOPIC_MAX];
	char warning_topic[COO_CMD_TOPIC_MAX];
	coo_cmd_handler_fn execute_handler;
	uint16_t *mqtt_msg_id;
	uint16_t serial_wrap_column;
	void *user_data;
	const struct coo_cmd_spec *command_specs;
	size_t command_spec_count;
	struct nvs_fs *lastcommand_nvs;
	uint16_t lastcommand_nvs_id;
	struct coo_cmd_lastcommand lastcommand;
#if defined(CONFIG_COO_CMD_REBOOT)
	struct k_work_delayable reboot_work;
	atomic_t reboot_pending;
	uint32_t reboot_delay_ms;
	bool reboot_erase_non_ip_settings;
	coo_cmd_reboot_prepare_fn reboot_prepare;
#endif
#if defined(CONFIG_COO_CMD_SERIAL_GUARD)
	struct k_work_delayable serial_guard_work;
	atomic_t serial_guard_active;
	uint32_t serial_guard_seconds;
#endif
	bool outbound_full_warning_seen;
	bool serial_initialized;
	bool serial_line_overflow;
	size_t serial_line_len;
	char serial_line[COO_CMD_SERIAL_LINE_MAX];
	struct coo_cmd_request ingress_cmd;
	/* Executor-owned buffers keep large request/response payload storage off
	 * the command thread stack.
	 */
	struct coo_cmd_request executor_cmd;
	struct coo_cmd_response executor_out;
	struct coo_cmd_response outbound_scratch;
	/* Runtime warning builders can share one buffer; if it is busy,
	 * best-effort warnings stay local rather than allocating another full
	 * response on a producer thread stack.
	 */
	atomic_t emit_scratch_busy;
	struct coo_cmd_response emit_scratch;
};

struct coo_cmd_runtime_config {
	const char *device_id;
	struct k_msgq *inbound_queue;
	struct k_msgq *outbound_queue;
	coo_cmd_handler_fn execute_handler;
	uint16_t *mqtt_msg_id;
	uint16_t serial_wrap_column;
	const struct coo_cmd_spec *command_specs;
	size_t command_spec_count;
	struct nvs_fs *lastcommand_nvs;
	uint16_t lastcommand_nvs_id;
#if defined(CONFIG_COO_CMD_REBOOT)
	uint32_t reboot_delay_ms;
	coo_cmd_reboot_prepare_fn reboot_prepare;
#endif
	void *user_data;
};

/**
 * @brief Initialize a command runtime with stable app identity and callbacks.
 *
 * Copies @p cfg->device_id, preformats the request and warning topics, and
 * initializes nonblocking Zephyr console input for serial commands.
 * Applications normally call this once after board identity is known and before
 * starting the runtime executor.
 */
int coo_cmd_runtime_configure(struct coo_cmd_runtime *runtime,
			      const struct coo_cmd_runtime_config *cfg);

/** Find the longest exact-or-slash-prefix command spec for @p key. */
const struct coo_cmd_spec *
coo_cmd_runtime_find_spec(const struct coo_cmd_runtime *runtime,
			  const char *key);

/** Return whether a command spec is currently supported by the app/board. */
bool coo_cmd_runtime_spec_supported(const struct coo_cmd_runtime *runtime,
				    const struct coo_cmd_spec *spec);

/**
 * @brief Copy the last effect command recorded by the runtime.
 *
 * The record is loaded from NVS during runtime configuration when an NVS
 * backend and ID are supplied. Dispatch records only commands that resolve to a
 * supported effect handler or accepted built-in effect.
 */
bool coo_cmd_runtime_get_lastcommand(const struct coo_cmd_runtime *runtime,
				     struct coo_cmd_lastcommand *out);

/** Return a stable text label for a request source enum. */
const char *coo_cmd_source_name(enum coo_cmd_source source);

/** Return true when @p key starts with @p prefix and is exact or slash-delimited. */
bool coo_cmd_key_matches_prefix(const char *key, const char *prefix);

/**
 * Return the suffix after an exact or slash-delimited command-key prefix.
 *
 * Returns an empty string for exact matches, missing inputs, or non-matches.
 * For `laserbank/power/auto` with prefix `laserbank/power`, this returns
 * `auto`.
 */
const char *coo_cmd_key_suffix_after(const char *key, const char *prefix);

/**
 * Copy one slash-delimited suffix segment after a command-key prefix.
 *
 * Returns 0 for keys like `mems/yj_cal_laser` with prefix `mems`. Exact
 * matches, nested suffixes, missing inputs, and too-small output buffers fail
 * with a negative errno value.
 */
int coo_cmd_key_suffix_segment_copy(const char *key,
				    const char *prefix,
				    char *suffix,
				    size_t suffix_len);

/**
 * Copy two slash-delimited suffix segments after a command-key prefix.
 *
 * Returns 0 for keys like `atten/1028y/coeff` with prefix `atten`. Exact
 * matches, missing segments, extra nested segments, missing inputs, and
 * too-small output buffers fail with a negative errno value.
 */
int coo_cmd_key_suffix_pair_copy(const char *key,
				 const char *prefix,
				 char *first,
				 size_t first_len,
				 char *second,
				 size_t second_len);

/** True for a missing, empty, or "{}" payload. */
bool coo_cmd_payload_empty(const struct coo_cmd_request *cmd);

/** Copy one Zephyr MQTT UTF-8 field into a C string. */
bool coo_cmd_copy_mqtt_utf8(const struct mqtt_utf8 *topic,
			    char *out,
			    size_t out_len);

/** Format `cmd/<device_id>/req/` for MQTT command subscription and parsing. */
int coo_cmd_format_request_prefix(const char *device_id,
				  char *buf,
				  size_t buf_len);

/** Format `cmd/<device_id>/resp/<key>` for default command responses. */
int coo_cmd_format_response_topic(const char *device_id,
				  const char *key,
				  char *buf,
				  size_t buf_len);

/** Format `dt/<device_id>/<suffix>` for warning and telemetry publications. */
int coo_cmd_format_data_topic(const char *device_id,
			      const char *suffix,
			      char *buf,
			      size_t buf_len);

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
int coo_cmd_make_response(struct coo_cmd_response *out,
			  const struct coo_cmd_request *cmd,
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
int coo_cmd_reply(struct coo_cmd_response *out,
		  const struct coo_cmd_request *cmd,
		  enum coo_cmd_msg_type msg_type,
		  const char *payload);

/** @brief Build the standard data-less success response: {"status":"ok"}. */
int coo_cmd_ok(struct coo_cmd_response *out, const struct coo_cmd_request *cmd);

/** @brief Build a structured error response with one error string. */
int coo_cmd_error(struct coo_cmd_response *out,
		  const struct coo_cmd_request *cmd,
		  const char *msg);

/** @brief Build a structured error response with one error string and rc. */
int coo_cmd_error_rc(struct coo_cmd_response *out,
		     const struct coo_cmd_request *cmd,
		     const char *msg,
		     int rc);

/** @brief Build the standard malformed-command error response. */
int coo_cmd_invalid_response(struct coo_cmd_response *out,
			     const struct coo_cmd_request *cmd);

/** @brief Build the standard unknown-command error response. */
int coo_cmd_unknown_response(struct coo_cmd_response *out,
			     const struct coo_cmd_request *cmd);

/** @brief Build the standard unsupported-operation error response. */
int coo_cmd_unsupported_response(struct coo_cmd_response *out,
				 const struct coo_cmd_request *cmd);

/** @brief Build the standard busy error response. */
int coo_cmd_busy_response(struct coo_cmd_response *out,
			  const struct coo_cmd_request *cmd);

/** @brief Build the standard serial-guard-active error response. */
int coo_cmd_serial_active_response(struct coo_cmd_response *out,
				   const struct coo_cmd_request *cmd);

/** Publish a formatted MQTT response/publication. May block in the socket layer. */
int coo_cmd_publish_mqtt(struct mqtt_client *client,
			 const struct coo_cmd_response *out,
			 uint16_t *message_id);

/** Execute commands from runtime->inbound_queue and enqueue one response each. */
void coo_cmd_runtime_executor_thread(void *p1, void *p2, void *p3);

/** Poll buffered console characters and queue completed serial commands. */
void coo_cmd_runtime_serial_poll(struct coo_cmd_runtime *runtime);

/** Copy and queue one MQTT publish as a normalized command request. */
void coo_cmd_runtime_handle_mqtt_publish(struct coo_cmd_runtime *runtime,
					 const struct mqtt_publish_param *pub);

/** MQTT wrapper callback adapter; @p user_data must be a coo_cmd_runtime. */
void coo_cmd_runtime_mqtt_callback(const struct mqtt_publish_param *pub,
				   void *user_data);

/** Parse one console line and queue a normalized serial command request. */
void coo_cmd_runtime_handle_serial_line(struct coo_cmd_runtime *runtime,
					char *line);

/** Drain outbound serial/MQTT responses with bounded retry behavior. */
void coo_cmd_runtime_drain_outbound(struct coo_cmd_runtime *runtime,
				    struct mqtt_client *client,
				    bool mqtt_available);

/**
 * Queue one runtime data or warning publication.
 *
 * DATA requires args->suffix and args->out with payload/payload_len already
 * populated. WARNING builds the compact warning JSON into args->out, or into a
 * guarded runtime scratch response when args->out is NULL. The helper owns
 * topic formatting, delivery target, QoS, and outbound queue insertion; it
 * never publishes MQTT directly and never allocates a full response on its
 * stack. REQUIRED delivery means the outbound drain will retry after successful
 * enqueue; enqueue can still fail if the bounded queue is full.
 */
int coo_cmd_runtime_emit(struct coo_cmd_runtime *runtime,
			 const struct coo_cmd_runtime_emit_args *args);

/** Print a serial response as topic then space-indented wrapped payload. */
void coo_cmd_print_serial_response(const struct coo_cmd_response *out,
				   uint16_t wrap_column);

/**
 * Print a serial response with JSON-aware indentation.
 *
 * This is presentation only. It does not change MQTT payloads or validate
 * command responses before publication.
 */
void coo_cmd_print_serial_response_pretty(const struct coo_cmd_response *out,
					  uint16_t wrap_column);

#endif /* COO_COMMONS_COMMAND_DISPATCH_H */
