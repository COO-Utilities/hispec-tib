/*
 * Copyright (c) 2026 Caltech Optical Observatories
 * SPDX-License-Identifier: Apache-2.0
 */

#include <coo_commons/command_dispatch.h>

#include <coo_commons/json_utils.h>

#include <ctype.h>
#include <errno.h>
#include <math.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <zephyr/console/console.h>
#include <zephyr/kvss/nvs.h>
#include <zephyr/logging/log.h>
#if defined(CONFIG_COO_CMD_REBOOT)
#include <zephyr/sys/reboot.h>
#endif
#include <zephyr/sys/util.h>

LOG_MODULE_REGISTER(coo_command_dispatch, LOG_LEVEL_INF);

#define SERIAL_POLL_CHAR_BUDGET 64
#define COO_CMD_SERIAL_LINE_END "\n"
#define COO_CMD_LASTCOMMAND_MAGIC 0x434c4344U /* "CLCD" */
#define COO_CMD_LASTCOMMAND_VERSION 1U
#define COO_CMD_REBOOT_DEFAULT_DELAY_MS 3000U

static void serial_reset_line(struct coo_cmd_runtime *runtime);
static int runtime_init_serial_console(struct coo_cmd_runtime *runtime);
static void runtime_enqueue_response(struct coo_cmd_runtime *runtime,
				     const struct coo_cmd_response *out);
static void runtime_load_lastcommand(struct coo_cmd_runtime *runtime);
static int runtime_execute_default(struct coo_cmd_runtime *runtime,
				   const struct coo_cmd_request *cmd,
				   struct coo_cmd_response *out);
#if defined(CONFIG_COO_CMD_REBOOT)
static void reboot_work_handler(struct k_work *work);
#endif
#if defined(CONFIG_COO_CMD_SERIAL_GUARD)
static void serial_guard_expire_work_handler(struct k_work *work);
#endif

struct coo_cmd_lastcommand_nvs_record {
	uint32_t magic;
	uint16_t version;
	uint16_t size;
	int64_t time_ms;
	struct coo_cmd_request request;
};

int coo_cmd_runtime_configure(struct coo_cmd_runtime *runtime,
			      const struct coo_cmd_runtime_config *cfg)
{
	int rc;

	if (runtime == NULL || cfg == NULL || cfg->device_id == NULL ||
	    cfg->device_id[0] == '\0' || cfg->inbound_queue == NULL ||
	    cfg->outbound_queue == NULL || cfg->mqtt_msg_id == NULL ||
	    strlen(cfg->device_id) >= sizeof(runtime->device_id)) {
		return -EINVAL;
	}

	memset(runtime, 0, sizeof(*runtime));
	strncpy(runtime->device_id, cfg->device_id, sizeof(runtime->device_id) - 1U);
	rc = coo_cmd_format_request_prefix(runtime->device_id,
					   runtime->request_prefix,
					   sizeof(runtime->request_prefix));
	if (rc != 0) {
		return rc;
	}
	rc = coo_cmd_format_data_topic(runtime->device_id, "warning",
				       runtime->warning_topic,
				       sizeof(runtime->warning_topic));
	if (rc != 0) {
		return rc;
	}

	runtime->inbound_queue = cfg->inbound_queue;
	runtime->outbound_queue = cfg->outbound_queue;
	runtime->execute_handler = cfg->execute_handler;
	runtime->mqtt_msg_id = cfg->mqtt_msg_id;
	runtime->serial_wrap_column = cfg->serial_wrap_column != 0U ?
				      cfg->serial_wrap_column :
				      COO_CMD_SERIAL_WRAP_COLUMN;
	runtime->command_specs = cfg->command_specs;
	runtime->command_spec_count = cfg->command_spec_count;
	runtime->lastcommand_nvs = cfg->lastcommand_nvs;
	runtime->lastcommand_nvs_id = cfg->lastcommand_nvs_id;
	runtime->user_data = cfg->user_data;
#if defined(CONFIG_COO_CMD_REBOOT)
	runtime->reboot_delay_ms = cfg->reboot_delay_ms != 0U ?
				   cfg->reboot_delay_ms :
				   COO_CMD_REBOOT_DEFAULT_DELAY_MS;
	runtime->reboot_prepare = cfg->reboot_prepare;
	k_work_init_delayable(&runtime->reboot_work, reboot_work_handler);
	(void)atomic_clear(&runtime->reboot_pending);
#endif
#if defined(CONFIG_COO_CMD_SERIAL_GUARD)
	runtime->serial_guard_seconds = CONFIG_COO_CMD_SERIAL_GUARD_DEFAULT_SECONDS;
	k_work_init_delayable(&runtime->serial_guard_work,
			      serial_guard_expire_work_handler);
	(void)atomic_clear(&runtime->serial_guard_active);
#endif
	runtime_load_lastcommand(runtime);

	return runtime_init_serial_console(runtime);
}

static bool coo_cmd_spec_key_matches(const struct coo_cmd_spec *spec,
				     const char *key)
{
	if (spec == NULL || spec->key == NULL || spec->key[0] == '\0' ||
	    key == NULL) {
		return false;
	}

	if (spec->key_prefix_match) {
		return coo_cmd_key_matches_prefix(key, spec->key);
	}

	return strcmp(key, spec->key) == 0;
}

const struct coo_cmd_spec *
coo_cmd_runtime_find_spec(const struct coo_cmd_runtime *runtime,
			  const char *key)
{
	const struct coo_cmd_spec *best = NULL;
	size_t best_len = 0U;

	if (runtime == NULL || key == NULL || runtime->command_specs == NULL) {
		return NULL;
	}

	for (size_t i = 0U; i < runtime->command_spec_count; ++i) {
		const struct coo_cmd_spec *spec = &runtime->command_specs[i];
		const size_t len = spec->key != NULL ? strlen(spec->key) : 0U;

		if (len == 0U || !coo_cmd_spec_key_matches(spec, key)) {
			continue;
		}
		if (len > best_len) {
			best = spec;
			best_len = len;
		}
	}

	return best;
}

bool coo_cmd_runtime_spec_supported(const struct coo_cmd_runtime *runtime,
				    const struct coo_cmd_spec *spec)
{
	if (spec == NULL) {
		return false;
	}

	return spec->supported == NULL || spec->supported(spec, runtime != NULL ?
							  runtime->user_data : NULL);
}

const char *coo_cmd_source_name(enum coo_cmd_source source)
{
	switch (source) {
	case COO_CMD_SOURCE_SERIAL:
		return "serial";
	case COO_CMD_SOURCE_MQTT:
		return "mqtt";
	default:
		return "unknown";
	}
}

static bool fixed_string_terminated(const char *text, size_t text_len)
{
	return text != NULL && memchr(text, '\0', text_len) != NULL;
}

static bool lastcommand_record_valid(const struct coo_cmd_lastcommand_nvs_record *record)
{
	const struct coo_cmd_request *request;

	if (record == NULL ||
	    record->magic != COO_CMD_LASTCOMMAND_MAGIC ||
	    record->version != COO_CMD_LASTCOMMAND_VERSION ||
	    record->size != sizeof(*record)) {
		return false;
	}

	request = &record->request;
	return request->payload_len < sizeof(request->payload) &&
	       request->corr_len <= sizeof(request->correlation_data) &&
	       fixed_string_terminated(request->key, sizeof(request->key)) &&
	       fixed_string_terminated(request->session_id, sizeof(request->session_id)) &&
	       fixed_string_terminated(request->response_topic, sizeof(request->response_topic)) &&
	       fixed_string_terminated(request->payload, sizeof(request->payload));
}

static void runtime_load_lastcommand(struct coo_cmd_runtime *runtime)
{
	struct coo_cmd_lastcommand_nvs_record record;
	int rc;

	if (runtime == NULL || runtime->lastcommand_nvs == NULL ||
	    runtime->lastcommand_nvs_id == 0U) {
		return;
	}

	rc = nvs_read(runtime->lastcommand_nvs, runtime->lastcommand_nvs_id,
		      &record, sizeof(record));
	if (rc == -ENOENT) {
		return;
	}
	if (rc != (int)sizeof(record) || !lastcommand_record_valid(&record)) {
		LOG_WRN("Ignoring invalid persisted command lastcommand (%d)", rc);
		return;
	}

	runtime->lastcommand.valid = true;
	runtime->lastcommand.time_ms = record.time_ms;
	runtime->lastcommand.request = record.request;
}

static void runtime_record_lastcommand(struct coo_cmd_runtime *runtime,
				       const struct coo_cmd_request *cmd)
{
	struct coo_cmd_lastcommand_nvs_record record = {
		.magic = COO_CMD_LASTCOMMAND_MAGIC,
		.version = COO_CMD_LASTCOMMAND_VERSION,
		.size = sizeof(record),
	};
	int rc;

	if (runtime == NULL || cmd == NULL || cmd->msg_type != COO_CMD_EFFECT) {
		return;
	}

	record.time_ms = k_uptime_get();
	record.request = *cmd;
	runtime->lastcommand.valid = true;
	runtime->lastcommand.time_ms = record.time_ms;
	runtime->lastcommand.request = record.request;

	if (runtime->lastcommand_nvs == NULL || runtime->lastcommand_nvs_id == 0U) {
		return;
	}

	rc = nvs_write(runtime->lastcommand_nvs, runtime->lastcommand_nvs_id,
		       &record, sizeof(record));
	if (rc < 0) {
		LOG_WRN("NVS lastcommand write failed (%d)", rc);
	}
}

bool coo_cmd_runtime_get_lastcommand(const struct coo_cmd_runtime *runtime,
				     struct coo_cmd_lastcommand *out)
{
	if (runtime == NULL || out == NULL || !runtime->lastcommand.valid) {
		return false;
	}

	*out = runtime->lastcommand;
	return true;
}

static int format_device_topic(const char *device_id, char *buf, size_t buf_len,
			       const char *prefix, const char *suffix)
{
	int written;

	if (device_id == NULL || device_id[0] == '\0' || buf == NULL ||
	    buf_len == 0U || prefix == NULL) {
		return -EINVAL;
	}

	written = snprintk(buf, buf_len, "%s%s%s",
			   prefix, device_id, suffix != NULL ? suffix : "");
	return (written < 0 || written >= (int)buf_len) ? -ENOSPC : 0;
}

int coo_cmd_format_request_prefix(const char *device_id,
				  char *buf,
				  size_t buf_len)
{
	return format_device_topic(device_id, buf, buf_len, "cmd/", "/req/");
}

int coo_cmd_format_response_topic(const char *device_id,
				  const char *key,
				  char *buf,
				  size_t buf_len)
{
	char suffix[96];
	int written;

	written = snprintk(suffix, sizeof(suffix), "/resp/%s",
			   key != NULL ? key : "");
	if (written < 0 || written >= (int)sizeof(suffix)) {
		return -ENOSPC;
	}

	return format_device_topic(device_id, buf, buf_len, "cmd/", suffix);
}

int coo_cmd_format_data_topic(const char *device_id,
			      const char *suffix,
			      char *buf,
			      size_t buf_len)
{
	char topic_suffix[64];
	int written;

	if (suffix == NULL || suffix[0] == '\0') {
		return -EINVAL;
	}

	written = snprintk(topic_suffix, sizeof(topic_suffix), "/%s", suffix);
	if (written < 0 || written >= (int)sizeof(topic_suffix)) {
		return -ENOSPC;
	}

	return format_device_topic(device_id, buf, buf_len, "dt/", topic_suffix);
}

bool coo_cmd_key_matches_prefix(const char *key, const char *prefix)
{
	size_t len;

	if (key == NULL || prefix == NULL) {
		return false;
	}

	len = strlen(prefix);
	if (strncmp(key, prefix, len) != 0) {
		return false;
	}

	return key[len] == '\0' || key[len] == '/';
}

const char *coo_cmd_key_suffix_after(const char *key, const char *prefix)
{
	size_t len;

	if (!coo_cmd_key_matches_prefix(key, prefix)) {
		return "";
	}

	len = strlen(prefix);
	return key[len] == '/' ? key + len + 1U : "";
}

int coo_cmd_key_suffix_segment_copy(const char *key,
				    const char *prefix,
				    char *suffix,
				    size_t suffix_len)
{
	const char *start;
	size_t prefix_len;
	size_t parsed_len;

	if (key == NULL || prefix == NULL || suffix == NULL || suffix_len == 0U) {
		return -EINVAL;
	}
	suffix[0] = '\0';

	if (!coo_cmd_key_matches_prefix(key, prefix)) {
		return -EINVAL;
	}

	prefix_len = strlen(prefix);
	if (key[prefix_len] != '/') {
		return -ENOENT;
	}

	start = key + prefix_len + 1U;
	parsed_len = strcspn(start, "/");
	if (parsed_len == 0U || start[parsed_len] != '\0') {
		return -EINVAL;
	}
	if (parsed_len >= suffix_len) {
		return -ENOSPC;
	}

	memcpy(suffix, start, parsed_len);
	suffix[parsed_len] = '\0';
	return 0;
}

int coo_cmd_key_suffix_pair_copy(const char *key,
				 const char *prefix,
				 char *first,
				 size_t first_len,
				 char *second,
				 size_t second_len)
{
	const char *start;
	const char *slash;
	size_t prefix_len;
	size_t first_parsed_len;
	size_t second_parsed_len;

	if (key == NULL || prefix == NULL || first == NULL || second == NULL ||
	    first_len == 0U || second_len == 0U) {
		return -EINVAL;
	}
	first[0] = '\0';
	second[0] = '\0';

	if (!coo_cmd_key_matches_prefix(key, prefix)) {
		return -EINVAL;
	}

	prefix_len = strlen(prefix);
	if (key[prefix_len] != '/') {
		return -ENOENT;
	}

	start = key + prefix_len + 1U;
	slash = strchr(start, '/');
	if (slash == NULL) {
		return -EINVAL;
	}

	first_parsed_len = (size_t)(slash - start);
	second_parsed_len = strcspn(slash + 1, "/");
	if (first_parsed_len == 0U ||
	    second_parsed_len == 0U ||
	    (slash + 1)[second_parsed_len] != '\0') {
		return -EINVAL;
	}
	if (first_parsed_len >= first_len || second_parsed_len >= second_len) {
		return -ENOSPC;
	}

	memcpy(first, start, first_parsed_len);
	first[first_parsed_len] = '\0';
	memcpy(second, slash + 1, second_parsed_len);
	second[second_parsed_len] = '\0';
	return 0;
}

bool coo_cmd_payload_empty(const struct coo_cmd_request *cmd)
{
	return cmd == NULL || cmd->payload_len == 0U || strcmp(cmd->payload, "{}") == 0;
}

bool coo_cmd_copy_mqtt_utf8(const struct mqtt_utf8 *topic,
			    char *out,
			    size_t out_len)
{
	if (topic == NULL || out == NULL || topic->size == 0U ||
	    topic->size >= out_len) {
		return false;
	}

	memcpy(out, topic->utf8, topic->size);
	out[topic->size] = '\0';
	return true;
}

static const char *skip_serial_space(const char *s)
{
	while (s != NULL && (*s == ' ' || *s == '\t')) {
		s++;
	}

	return s;
}

bool coo_cmd_serial_next_token(const char **cursor, char *out, size_t out_len)
{
	const char *start;
	size_t len;

	if (cursor == NULL || *cursor == NULL || out == NULL || out_len == 0U) {
		return false;
	}

	start = skip_serial_space(*cursor);
	if (*start == '\0') {
		*cursor = start;
		return false;
	}

	len = strcspn(start, " \t");
	if (len >= out_len) {
		len = out_len - 1U;
	}

	memcpy(out, start, len);
	out[len] = '\0';
	*cursor = start + strcspn(start, " \t");
	return true;
}

bool coo_cmd_serial_has_extra(const char *cursor)
{
	cursor = skip_serial_space(cursor);
	return cursor != NULL && *cursor != '\0';
}

bool coo_cmd_serial_token_is_number(const char *token)
{
	char *end = NULL;
	double value;

	if (token == NULL || token[0] == '\0') {
		return false;
	}

	value = strtod(token, &end);
	return end != token && end != NULL && *end == '\0' && isfinite(value);
}

static bool serial_token_has_control(const char *token)
{
	if (token == NULL) {
		return true;
	}

	for (const char *p = token; *p != '\0'; ++p) {
		if (iscntrl((unsigned char)*p)) {
			return true;
		}
	}

	return false;
}

static bool serial_token_is_json_number(const char *token)
{
	const char *p = token;

	if (p == NULL || *p == '\0') {
		return false;
	}

	if (*p == '-') {
		p++;
	}

	if (*p == '0') {
		p++;
	} else if (*p >= '1' && *p <= '9') {
		do {
			p++;
		} while (isdigit((unsigned char)*p));
	} else {
		return false;
	}

	if (*p == '.') {
		p++;
		if (!isdigit((unsigned char)*p)) {
			return false;
		}
		while (isdigit((unsigned char)*p)) {
			p++;
		}
	}

	if (*p == 'e' || *p == 'E') {
		p++;
		if (*p == '+' || *p == '-') {
			p++;
		}
		if (!isdigit((unsigned char)*p)) {
			return false;
		}
		while (isdigit((unsigned char)*p)) {
			p++;
		}
	}

	return *p == '\0';
}

static int serial_append_json_number(char *out, size_t out_len, size_t *off,
				     const char *token)
{
	char *end = NULL;
	double value;
	int written;

	value = strtod(token, &end);
	if (end == token || end == NULL || *end != '\0' || !isfinite(value)) {
		return -EINVAL;
	}

	if (serial_token_is_json_number(token)) {
		written = snprintk(out + *off, out_len - *off, "%s", token);
	} else {
		/* Serial accepts human shorthand such as .5; normalize it before
		 * handlers parse the generated JSON.
		 */
		written = snprintk(out + *off, out_len - *off, "%.17g", value);
	}

	if (written < 0 || written >= (int)(out_len - *off)) {
		return -ENOSPC;
	}

	*off += (size_t)written;
	return 0;
}

static const char *serial_token_bool_json(const char *token)
{
	if (token == NULL) {
		return NULL;
	}

	if (strcasecmp(token, "true") == 0 || strcasecmp(token, "on") == 0 ||
	    strcasecmp(token, "yes") == 0) {
		return "true";
	}
	if (strcasecmp(token, "false") == 0 || strcasecmp(token, "off") == 0 ||
	    strcasecmp(token, "no") == 0) {
		return "false";
	}

	return NULL;
}

int coo_cmd_serial_append_json_value(char *out, size_t out_len, size_t *off,
				    const char *token)
{
	const char *bool_json = serial_token_bool_json(token);
	int written;

	if (out == NULL || off == NULL || token == NULL || *off >= out_len) {
		return -EINVAL;
	}
	if (serial_token_has_control(token)) {
		return -EINVAL;
	}

	if (bool_json != NULL) {
		written = snprintk(out + *off, out_len - *off, "%s", bool_json);
	} else if (coo_cmd_serial_token_is_number(token)) {
		return serial_append_json_number(out, out_len, off, token);
	} else if (strcasecmp(token, "null") == 0) {
		written = snprintk(out + *off, out_len - *off, "%s", token);
	} else {
		if (strchr(token, '"') != NULL || strchr(token, '\\') != NULL) {
			return -EINVAL;
		}
		written = snprintk(out + *off, out_len - *off, "\"%s\"", token);
	}

	if (written < 0 || written >= (int)(out_len - *off)) {
		return -ENOSPC;
	}
	*off += (size_t)written;
	return 0;
}

int coo_cmd_serial_append_json_field(char *out, size_t out_len, size_t *off,
				    const char *key, const char *token,
				    bool comma)
{
	int written;

	if (key == NULL || token == NULL || key[0] == '\0' ||
	    strchr(key, '"') != NULL || strchr(key, '\\') != NULL ||
	    serial_token_has_control(key) || serial_token_has_control(token) ||
	    *off >= out_len) {
		return -EINVAL;
	}

	written = snprintk(out + *off, out_len - *off,
			   "%s\"%s\":", comma ? "," : "", key);
	if (written < 0 || written >= (int)(out_len - *off)) {
		return -ENOSPC;
	}
	*off += (size_t)written;

	return coo_cmd_serial_append_json_value(out, out_len, off, token);
}

static int serial_payload_from_key_values(const char *payload, char *out,
					  size_t out_len)
{
	const char *cursor = payload;
	char token[128];
	bool first = true;
	size_t off = 0U;
	int written;

	written = snprintk(out, out_len, "{");
	if (written < 0 || written >= (int)out_len) {
		return -ENOSPC;
	}
	off = (size_t)written;

	while (coo_cmd_serial_next_token(&cursor, token, sizeof(token))) {
		char *eq = strchr(token, '=');

		if (eq == NULL || eq == token || eq[1] == '\0') {
			return -EINVAL;
		}
		*eq = '\0';

		if (coo_cmd_serial_append_json_field(out, out_len, &off, token, eq + 1,
					     !first) != 0) {
			return -EINVAL;
		}
		first = false;
	}

	written = snprintk(out + off, out_len - off, "}");
	return (written < 0 || written >= (int)(out_len - off)) ? -ENOSPC : 0;
}

static int serial_payload_from_value(const char *payload, char *out, size_t out_len)
{
	const char *cursor = payload;
	char token[128] = {0};
	size_t off = 0U;
	int written;

	if (!coo_cmd_serial_next_token(&cursor, token, sizeof(token)) ||
	    coo_cmd_serial_has_extra(cursor)) {
		return -EINVAL;
	}

	written = snprintk(out, out_len, "{\"value\":");
	if (written < 0 || written >= (int)out_len) {
		return -ENOSPC;
	}
	off = (size_t)written;
	if (coo_cmd_serial_append_json_value(out, out_len, &off, token) != 0) {
		return -EINVAL;
	}
	written = snprintk(out + off, out_len - off, "}");
	return (written < 0 || written >= (int)(out_len - off)) ? -ENOSPC : 0;
}

static int serial_payload_from_positional(const char *payload,
					  const struct coo_cmd_serial_positional *pos,
					  char *out,
					  size_t out_len)
{
	const char *cursor = payload;
	char token[COO_CMD_SERIAL_POSITIONAL_MAX][128] = {{0}};
	uint8_t count = 0U;
	size_t off = 0U;
	int written;

	if (pos == NULL || pos->field[0] == NULL ||
	    pos->required_count > COO_CMD_SERIAL_POSITIONAL_MAX) {
		return -EINVAL;
	}

	while (count < COO_CMD_SERIAL_POSITIONAL_MAX &&
	       coo_cmd_serial_next_token(&cursor, token[count], sizeof(token[count]))) {
		count++;
	}
	if (coo_cmd_serial_has_extra(cursor) || count < pos->required_count) {
		return -EINVAL;
	}

	written = snprintk(out, out_len, "{");
	if (written < 0 || written >= (int)out_len) {
		return -ENOSPC;
	}
	off = (size_t)written;

	for (uint8_t i = 0U; i < count; ++i) {
		if (pos->field[i] == NULL || token[i][0] == '\0') {
			return -EINVAL;
		}
		if ((pos->numeric_mask & BIT(i)) != 0U &&
		    !coo_cmd_serial_token_is_number(token[i])) {
			return -EINVAL;
		}
		if (coo_cmd_serial_append_json_field(out, out_len, &off,
						     pos->field[i], token[i],
						     i != 0U) != 0) {
			return -EINVAL;
		}
	}

	written = snprintk(out + off, out_len - off, "}");
	return (written < 0 || written >= (int)(out_len - off)) ? -ENOSPC : 0;
}

#if defined(CONFIG_COO_CMD_SERIAL_GUARD)
static int serial_payload_from_serial_guard(const char *payload, char *out, size_t out_len)
{
	const char *cursor = payload;
	const char *seconds;
	char token[32] = {0};
	size_t off = 0U;
	int written;

	if (!coo_cmd_serial_next_token(&cursor, token, sizeof(token)) ||
	    coo_cmd_serial_has_extra(cursor)) {
		return -EINVAL;
	}

	seconds = (strcasecmp(token, "off") == 0) ? "0" : token;
	written = snprintk(out, out_len, "{\"seconds\":");
	if (written < 0 || written >= (int)out_len) {
		return -ENOSPC;
	}
	off = (size_t)written;
	if (coo_cmd_serial_append_json_value(out, out_len, &off, seconds) != 0) {
		return -EINVAL;
	}
	written = snprintk(out + off, out_len - off, "}");
	return (written < 0 || written >= (int)(out_len - off)) ? -ENOSPC : 0;
}
#endif

int coo_cmd_normalize_serial_payload(const char *key,
				     const char *payload,
				     coo_cmd_serial_shorthand_fn shorthand,
				     void *user_data,
				     char *out,
				     size_t out_len)
{
	if (out == NULL || out_len == 0U) {
		return -EINVAL;
	}

	payload = skip_serial_space(payload);
	if (payload == NULL || payload[0] == '\0') {
		int written = snprintk(out, out_len, "{}");

		return (written < 0 || written >= (int)out_len) ? -ENOSPC : 0;
	}

	if (payload[0] == '{') {
		if (strlen(payload) >= out_len) {
			return -ENOSPC;
		}
		strncpy(out, payload, out_len - 1U);
		out[out_len - 1U] = '\0';
		return 0;
	}

	if (strchr(payload, '=') != NULL) {
		return serial_payload_from_key_values(payload, out, out_len);
	}

#if defined(CONFIG_COO_CMD_SERIAL_GUARD)
	if (key != NULL && strcmp(key, "serialguard") == 0) {
		return serial_payload_from_serial_guard(payload, out, out_len);
	}
#endif

	if (shorthand != NULL) {
		return shorthand(key, payload, out, out_len, user_data);
	}

	return serial_payload_from_value(payload, out, out_len);
}

static int runtime_normalize_serial_payload(const struct coo_cmd_spec *spec,
					    const char *key,
					    const char *payload,
					    void *user_data,
					    char *out,
					    size_t out_len)
{
	payload = skip_serial_space(payload);
	if (payload != NULL && payload[0] != '\0' &&
	    payload[0] != '{' && strchr(payload, '=') == NULL &&
	    spec != NULL && spec->serial_positional.field[0] != NULL) {
		return serial_payload_from_positional(payload,
						      &spec->serial_positional,
						      out, out_len);
	}

	return coo_cmd_normalize_serial_payload(key, payload,
						spec != NULL ? spec->serial_shorthand : NULL,
						user_data, out, out_len);
}

int coo_cmd_make_response(struct coo_cmd_response *out,
			  const struct coo_cmd_request *cmd,
			  enum coo_cmd_msg_type msg_type,
			  const char *payload,
			  coo_cmd_format_response_topic_fn format_topic,
			  void *user_data)
{
	static const char overflow_msg[] = "{\"error\":\"response too large\"}";
	bool payload_in_out;
	size_t payload_len = 0U;

	if (out == NULL) {
		return -EINVAL;
	}

	payload_in_out = payload != NULL &&
			 payload >= out->payload &&
			 payload < out->payload + sizeof(out->payload);
	if (payload != NULL) {
		payload_len = strlen(payload);
	}

	if (!payload_in_out) {
		memset(out->payload, 0, sizeof(out->payload));
	} else if (payload_len >= sizeof(out->payload)) {
		payload_len = sizeof(out->payload) - 1U;
		out->payload[payload_len] = '\0';
	}
	out->topic[0] = '\0';
	out->payload_len = 0U;
	out->corr_len = 0U;
	memset(out->correlation_data, 0, sizeof(out->correlation_data));

	out->msg_type = msg_type;
	out->target = (cmd != NULL && cmd->source == COO_CMD_SOURCE_SERIAL) ?
		   COO_CMD_OUT_SERIAL : COO_CMD_OUT_MQTT;
	out->qos = MQTT_QOS_1_AT_LEAST_ONCE;

	if (format_topic != NULL) {
		(void)format_topic(cmd != NULL ? cmd->key : "",
				   out->topic, sizeof(out->topic), user_data);
	}

	if (cmd != NULL && cmd->response_topic[0] != '\0' &&
	    strlen(cmd->response_topic) < sizeof(out->topic)) {
		strncpy(out->topic, cmd->response_topic, sizeof(out->topic) - 1U);
	}

	if (cmd != NULL && cmd->corr_len > 0U &&
	    cmd->corr_len <= sizeof(out->correlation_data)) {
		memcpy(out->correlation_data, cmd->correlation_data, cmd->corr_len);
		out->corr_len = cmd->corr_len;
	}

	if (payload != NULL && payload_len >= sizeof(out->payload)) {
		out->msg_type = COO_CMD_RESP_ERROR;
		snprintk(out->payload, sizeof(out->payload), "%s", overflow_msg);
		out->payload_len = strlen(out->payload);
		return -ENOSPC;
	}

	if (payload != NULL && !payload_in_out) {
		snprintk(out->payload, sizeof(out->payload), "%s", payload);
	}
	out->payload_len = strlen(out->payload);
	return 0;
}

int coo_cmd_reply(struct coo_cmd_response *out,
		  const struct coo_cmd_request *cmd,
		  enum coo_cmd_msg_type msg_type,
		  const char *payload)
{
	return coo_cmd_make_response(out, cmd, msg_type, payload, NULL, NULL);
}

int coo_cmd_ok(struct coo_cmd_response *out, const struct coo_cmd_request *cmd)
{
	return coo_cmd_reply(out, cmd, COO_CMD_RESP_OK, "{\"status\":\"ok\"}");
}

int coo_cmd_error(struct coo_cmd_response *out,
		  const struct coo_cmd_request *cmd,
		  const char *msg)
{
	int rc;

	if (out == NULL) {
		return -EINVAL;
	}

	rc = coo_cmd_reply(out, cmd, COO_CMD_RESP_ERROR, NULL);
	if (rc != 0) {
		return rc;
	}
	snprintk(out->payload, sizeof(out->payload), "{\"error\":\"%s\"}",
		 msg != NULL ? msg : "Unspecified error");
	out->payload_len = strlen(out->payload);
	return 0;
}

int coo_cmd_error_rc(struct coo_cmd_response *out,
		     const struct coo_cmd_request *cmd,
		     const char *msg,
		     int rc)
{
	int build_rc;

	if (out == NULL) {
		return -EINVAL;
	}

	build_rc = coo_cmd_reply(out, cmd, COO_CMD_RESP_ERROR, NULL);
	if (build_rc != 0) {
		return build_rc;
	}
	snprintk(out->payload, sizeof(out->payload), "{\"error\":\"%s\",\"rc\":%d}",
		 msg != NULL ? msg : "", rc);
	out->payload_len = strlen(out->payload);
	return 0;
}

int coo_cmd_invalid_response(struct coo_cmd_response *out,
			     const struct coo_cmd_request *cmd)
{
	return coo_cmd_reply(out, cmd, COO_CMD_RESP_ERROR,
			     "{\"error\":\"Invalid or unrecognized command\"}");
}

int coo_cmd_unknown_response(struct coo_cmd_response *out,
			     const struct coo_cmd_request *cmd)
{
	return coo_cmd_reply(out, cmd, COO_CMD_RESP_ERROR,
			     "{\"error\":\"Unknown request\"}");
}

int coo_cmd_unsupported_response(struct coo_cmd_response *out,
				 const struct coo_cmd_request *cmd)
{
	return coo_cmd_reply(out, cmd, COO_CMD_RESP_ERROR,
			     "{\"error\":\"Unsupported operation\"}");
}

int coo_cmd_busy_response(struct coo_cmd_response *out,
			  const struct coo_cmd_request *cmd)
{
	return coo_cmd_reply(out, cmd, COO_CMD_RESP_ERROR, "{\"error\":\"busy\"}");
}

int coo_cmd_serial_active_response(struct coo_cmd_response *out,
				   const struct coo_cmd_request *cmd)
{
	return coo_cmd_reply(out, cmd, COO_CMD_RESP_ERROR,
			     "{\"error\":\"try later. local serial commands active\"}");
}

static int runtime_unknown_argument_response(struct coo_cmd_response *out,
					     const struct coo_cmd_request *cmd,
					     const char *key)
{
	int rc;

	rc = coo_cmd_reply(out, cmd, COO_CMD_RESP_ERROR, NULL);
	if (rc != 0) {
		return rc;
	}
	snprintk(out->payload, sizeof(out->payload),
		 "{\"error\":\"unknown argument\",\"arg\":\"%s\"}",
		 key != NULL ? key : "");
	out->payload_len = strlen(out->payload);
	return 0;
}

static int runtime_validation_reply(int reply_rc)
{
	return reply_rc == 0 ? 1 : reply_rc;
}

static int runtime_validate_payload_keys(const struct coo_cmd_spec *spec,
					 const struct coo_cmd_request *cmd,
					 struct coo_cmd_response *out)
{
	char unknown_key[64] = {0};
	int rc;

	if (spec == NULL || cmd == NULL || coo_cmd_payload_empty(cmd)) {
		return 0;
	}

	rc = coo_json_validate_top_level_keys(cmd->payload,
					      spec->allowed_payload_keys,
					      unknown_key,
					      sizeof(unknown_key));
	if (rc == 0) {
		return 0;
	}
	if (rc == -ENOENT) {
		return runtime_validation_reply(
			runtime_unknown_argument_response(out, cmd, unknown_key));
	}
	if (rc == -ENOSPC) {
		return runtime_validation_reply(
			coo_cmd_error(out, cmd, "argument name too long"));
	}

	return runtime_validation_reply(coo_cmd_error(out, cmd, "invalid payload"));
}

static int append_format(char *buf, size_t buf_len, size_t *off, const char *fmt, ...)
{
	va_list args;
	int written;

	if (buf == NULL || off == NULL || fmt == NULL || *off >= buf_len) {
		return -EINVAL;
	}

	va_start(args, fmt);
	written = vsnprintk(buf + *off, buf_len - *off, fmt, args);
	va_end(args);

	if (written < 0 || written >= (int)(buf_len - *off)) {
		return -ENOSPC;
	}
	*off += (size_t)written;
	return 0;
}

static int append_json_string(char *buf, size_t buf_len, size_t *off,
			      const char *text)
{
	const char *s = text != NULL ? text : "";

	if (buf == NULL || off == NULL || *off >= buf_len) {
		return -EINVAL;
	}

	for (; *s != '\0'; ++s) {
		int written;

		if (*s == '"' || *s == '\\') {
			written = snprintk(buf + *off, buf_len - *off, "\\%c", *s);
		} else if ((unsigned char)*s < 0x20U) {
			written = snprintk(buf + *off, buf_len - *off, "?");
		} else {
			written = snprintk(buf + *off, buf_len - *off, "%c", *s);
		}

		if (written < 0 || written >= (int)(buf_len - *off)) {
			return -ENOSPC;
		}
		*off += (size_t)written;
	}

	return 0;
}

static enum coo_cmd_out_target runtime_emit_target(
	enum coo_cmd_runtime_emit_delivery delivery)
{
	return delivery == COO_CMD_RUNTIME_EMIT_REQUIRED ?
	       COO_CMD_OUT_MQTT : COO_CMD_OUT_MQTT_BEST_EFFORT;
}

static int runtime_build_warning(struct coo_cmd_response *out,
				 const char *topic,
				 enum coo_cmd_runtime_emit_delivery delivery,
				 const char *code,
				 const char *msg,
				 const char *context)
{
	size_t off;
	int written;

	if (out == NULL || topic == NULL ||
	    strlen(topic) >= sizeof(out->topic)) {
		return -EINVAL;
	}

	memset(out, 0, sizeof(*out));
	out->msg_type = COO_CMD_RESP_OK;
	out->target = runtime_emit_target(delivery);
	out->qos = 0U;
	strncpy(out->topic, topic, sizeof(out->topic) - 1U);

	written = snprintk(out->payload, sizeof(out->payload),
			   "{\"severity\":\"warning\",\"code\":\"");
	if (written < 0 || written >= (int)sizeof(out->payload)) {
		return -ENOSPC;
	}
	off = (size_t)written;

	if (append_json_string(out->payload, sizeof(out->payload), &off, code) != 0) {
		return -ENOSPC;
	}

	written = snprintk(out->payload + off, sizeof(out->payload) - off,
			   "\",\"msg\":\"");
	if (written < 0 || written >= (int)(sizeof(out->payload) - off)) {
		return -ENOSPC;
	}
	off += (size_t)written;

	if (append_json_string(out->payload, sizeof(out->payload), &off, msg) != 0) {
		return -ENOSPC;
	}

	written = snprintk(out->payload + off, sizeof(out->payload) - off,
			   "\",\"context\":\"");
	if (written < 0 || written >= (int)(sizeof(out->payload) - off)) {
		return -ENOSPC;
	}
	off += (size_t)written;

	if (append_json_string(out->payload, sizeof(out->payload), &off, context) != 0) {
		return -ENOSPC;
	}

	written = snprintk(out->payload + off, sizeof(out->payload) - off,
			   "\",\"uptime_ms\":%lld}",
			   (long long)k_uptime_get());
	if (written < 0 || written >= (int)(sizeof(out->payload) - off)) {
		return -ENOSPC;
	}
	off += (size_t)written;
	out->payload_len = off;
	return 0;
}

static bool runtime_emit_scratch_take(struct coo_cmd_runtime *runtime)
{
	return runtime != NULL && atomic_cas(&runtime->emit_scratch_busy, 0, 1);
}

static void runtime_emit_scratch_release(struct coo_cmd_runtime *runtime)
{
	if (runtime != NULL) {
		(void)atomic_clear(&runtime->emit_scratch_busy);
	}
}

static int runtime_emit_queue(struct coo_cmd_runtime *runtime,
			      const struct coo_cmd_response *out)
{
	if (runtime == NULL || runtime->outbound_queue == NULL || out == NULL) {
		return -EINVAL;
	}
	if (k_msgq_put(runtime->outbound_queue, out, K_NO_WAIT) != 0) {
		return -ENOSPC;
	}
	return 0;
}

static int runtime_emit_data(struct coo_cmd_runtime *runtime,
			     const struct coo_cmd_runtime_emit_args *args)
{
	struct coo_cmd_response *out;
	int rc;

	if (runtime == NULL || args == NULL || args->suffix == NULL ||
	    args->out == NULL) {
		return -EINVAL;
	}

	out = args->out;
	if (out->payload_len > sizeof(out->payload)) {
		return -ENOSPC;
	}
	out->msg_type = COO_CMD_RESP_OK;
	out->target = runtime_emit_target(args->delivery);
	out->qos = 0U;
	out->corr_len = 0U;
	rc = coo_cmd_format_data_topic(runtime->device_id, args->suffix,
				       out->topic, sizeof(out->topic));
	if (rc != 0) {
		return rc;
	}

	return runtime_emit_queue(runtime, out);
}

static int runtime_emit_warning(struct coo_cmd_runtime *runtime,
				const struct coo_cmd_runtime_emit_args *args)
{
	struct coo_cmd_response *out;
	bool scratch = false;
	int rc;

	if (runtime == NULL || args == NULL || runtime->warning_topic[0] == '\0') {
		return -EINVAL;
	}

	LOG_WRN("%s: %s%s%s",
		args->code != NULL ? args->code : "warning",
		args->msg != NULL ? args->msg : "",
		args->context != NULL && args->context[0] != '\0' ? " context=" : "",
		args->context != NULL ? args->context : "");

	if (args->out != NULL) {
		out = args->out;
	} else if (runtime_emit_scratch_take(runtime)) {
		out = &runtime->emit_scratch;
		scratch = true;
	} else {
		LOG_WRN("warning scratch busy; warning was only logged locally");
		return -EAGAIN;
	}

	rc = runtime_build_warning(out, runtime->warning_topic, args->delivery,
				   args->code, args->msg, args->context);
	if (rc != 0) {
		LOG_WRN("warning payload too large; MQTT warning dropped");
		goto out;
	}

	rc = runtime_emit_queue(runtime, out);
	if (rc != 0) {
		LOG_WRN("warning MQTT queue full; warning was only logged locally");
	}

out:
	if (scratch) {
		runtime_emit_scratch_release(runtime);
	}
	return rc;
}

int coo_cmd_runtime_emit(struct coo_cmd_runtime *runtime,
			 const struct coo_cmd_runtime_emit_args *args)
{
	if (args == NULL) {
		return -EINVAL;
	}
	switch (args->type) {
	case COO_CMD_RUNTIME_EMIT_DATA:
		return runtime_emit_data(runtime, args);
	case COO_CMD_RUNTIME_EMIT_WARNING:
		return runtime_emit_warning(runtime, args);
	default:
		return -EINVAL;
	}
}

static bool payload_has_text(const char *payload)
{
	payload = skip_serial_space(payload);
	return payload != NULL && payload[0] != '\0';
}

static const struct coo_cmd_help_entry builtin_help_entries[] = {
	{
		.key = "help",
		.usage = "help",
		.args = "none",
		.values = NULL,
		.notes = "serial prints full command help directly; MQTT returns compact endpoints",
		.flags = COO_CMD_HELP_QUERY | COO_CMD_HELP_SERIAL_GUARD_QUERY |
			 COO_CMD_HELP_BUILTIN,
	},
#if defined(CONFIG_COO_CMD_SERIAL_GUARD)
	{
		.key = "serialguard",
		.usage = "serialguard [seconds=<s>|off]",
		.args = "[seconds=<s>] or [off]",
		.values = "seconds: 0 disables MQTT holdoff until changed again",
		.notes = "runtime-only local serial guard; not persisted across reboot",
		.flags = COO_CMD_HELP_QUERY | COO_CMD_HELP_EFFECT |
			 COO_CMD_HELP_SERIAL_GUARD_QUERY | COO_CMD_HELP_BUILTIN,
	},
#endif
#if defined(CONFIG_COO_CMD_REBOOT)
	{
		.key = "reboot",
		.usage = "reboot [erase_non_ip_settings]",
		.args = "optional erase_non_ip_settings flag",
		.values = "erase_non_ip_settings: true deletes persisted non-IP settings before reboot",
		.notes = "schedules a non-cancelable reboot after the response window",
		.flags = COO_CMD_HELP_EFFECT | COO_CMD_HELP_BUILTIN,
	},
#endif
};

static bool runtime_key_is_help(const char *key)
{
	return key != NULL && strcmp(key, "help") == 0;
}

static bool runtime_key_is_serial_guard(const char *key)
{
#if defined(CONFIG_COO_CMD_SERIAL_GUARD)
	return key != NULL && strcmp(key, "serialguard") == 0;
#else
	ARG_UNUSED(key);
	return false;
#endif
}

static bool runtime_key_is_reboot(const char *key)
{
#if defined(CONFIG_COO_CMD_REBOOT)
	return key != NULL && strcmp(key, "reboot") == 0;
#else
	ARG_UNUSED(key);
	return false;
#endif
}

static void serial_line_end(void)
{
	printk(COO_CMD_SERIAL_LINE_END);
}

static uint16_t serial_print_prefix(const char *prefix)
{
	uint16_t col = 0U;

	for (const char *s = prefix != NULL ? prefix : ""; *s != '\0'; ++s) {
		printk("%c", *s);
		col++;
	}

	return col;
}

static void serial_print_wrapped_text(const char *prefix,
				      const char *text,
				      uint16_t wrap_column)
{
	uint16_t col;
	size_t word_len = 0U;

	if (text == NULL || text[0] == '\0') {
		return;
	}

	col = serial_print_prefix(prefix);
	for (const char *s = text; *s != '\0'; ++s) {
		const bool at_space = (*s == ' ' || *s == '\t');

		if (!at_space) {
			word_len++;
		}

		if (wrap_column != 0U && col >= wrap_column && at_space) {
			serial_line_end();
			col = serial_print_prefix("      ");
			word_len = 0U;
			continue;
		}
		if (wrap_column != 0U && col + word_len >= wrap_column &&
		    word_len > 0U && at_space) {
			serial_line_end();
			col = serial_print_prefix("      ");
			word_len = 0U;
			continue;
		}

		printk("%c", at_space ? ' ' : *s);
		col++;
		if (at_space) {
			word_len = 0U;
		}
	}
	serial_line_end();
}

static void serial_print_help_entry(const struct coo_cmd_runtime *runtime,
				    const char *key,
				    const struct coo_cmd_help_entry *entry,
				    const struct coo_cmd_spec *spec,
				    uint16_t wrap_column)
{
	if (entry == NULL || key == NULL) {
		return;
	}

	printk("  %s", key);
	if (spec != NULL && !coo_cmd_runtime_spec_supported(runtime, spec)) {
		printk(" [unsupported]");
	}
	if ((entry->flags & COO_CMD_HELP_QUERY) != 0U &&
	    (entry->flags & COO_CMD_HELP_EFFECT) != 0U) {
		printk(" query/effect");
	} else if ((entry->flags & COO_CMD_HELP_QUERY) != 0U) {
		printk(" query");
	} else if ((entry->flags & COO_CMD_HELP_EFFECT) != 0U) {
		printk(" effect");
	}
	serial_line_end();

	serial_print_wrapped_text("    use: ", entry->usage, wrap_column);
	serial_print_wrapped_text("    args: ", entry->args, wrap_column);
	serial_print_wrapped_text("    values: ", entry->values, wrap_column);
	serial_print_wrapped_text("    notes: ", entry->notes, wrap_column);
}

static void runtime_print_serial_help(const struct coo_cmd_runtime *runtime,
				      uint16_t wrap_column)
{
	if (wrap_column == 0U) {
		wrap_column = COO_CMD_SERIAL_WRAP_COLUMN;
	}

	printk("serial help");
	serial_line_end();
	printk("  device: %s", runtime != NULL ? runtime->device_id : "");
	serial_line_end();
	printk("  request prefix: %s", runtime != NULL ? runtime->request_prefix : "");
	serial_line_end();
	printk("  [] marks optional payload fields or serial tokens");
	serial_line_end();

	for (size_t i = 0U; i < ARRAY_SIZE(builtin_help_entries); ++i) {
		serial_print_help_entry(runtime, builtin_help_entries[i].key,
					&builtin_help_entries[i], NULL,
					wrap_column);
	}
	if (runtime != NULL) {
		for (size_t i = 0U; i < runtime->command_spec_count; ++i) {
			const struct coo_cmd_spec *spec = &runtime->command_specs[i];

			if (spec->help != NULL) {
				serial_print_help_entry(runtime, spec->key,
							spec->help, spec,
							wrap_column);
			}
		}
	}
}

static int append_help_key(char *payload, size_t payload_len, size_t *off,
			   const char *key, bool *first)
{
	if (key == NULL) {
		return 0;
	}

	if (append_format(payload, payload_len, off, "%s\"",
			  *first ? "" : ",") != 0 ||
	    append_json_string(payload, payload_len, off, key) != 0 ||
	    append_format(payload, payload_len, off, "\"") != 0) {
		return -ENOSPC;
	}
	*first = false;
	return 0;
}

static int runtime_help_response(struct coo_cmd_runtime *runtime,
				 const struct coo_cmd_request *cmd,
				 struct coo_cmd_response *out)
{
	char response_prefix[COO_CMD_TOPIC_MAX];
	size_t off = 0U;
	bool first = true;

	if (runtime == NULL || out == NULL ||
	    coo_cmd_format_response_topic(runtime->device_id, "",
					  response_prefix,
					  sizeof(response_prefix)) != 0) {
		return coo_cmd_error(out, cmd, "help topic formatting failed");
	}

	if (append_format(out->payload, sizeof(out->payload),
			  &off,
			  "{\"device\":\"%s\",\"request_prefix\":\"%s\","
			  "\"response_prefix\":\"%s\",\"commands\":[",
			  runtime->device_id,
			  runtime->request_prefix,
			  response_prefix) != 0) {
		return coo_cmd_error(out, cmd, "help response too large");
	}
	for (size_t i = 0U; i < ARRAY_SIZE(builtin_help_entries); ++i) {
		if (append_help_key(out->payload, sizeof(out->payload), &off,
				    builtin_help_entries[i].key, &first) != 0) {
			return coo_cmd_error(out, cmd, "help response too large");
		}
	}
	for (size_t i = 0U; i < runtime->command_spec_count; ++i) {
		const struct coo_cmd_spec *spec = &runtime->command_specs[i];

		if (spec->help != NULL &&
		    append_help_key(out->payload, sizeof(out->payload), &off,
				    spec->key, &first) != 0) {
			return coo_cmd_error(out, cmd, "help response too large");
		}
	}
	if (append_format(out->payload, sizeof(out->payload), &off, "]}") != 0) {
		return coo_cmd_error(out, cmd, "help response too large");
	}

	return coo_cmd_reply(out, cmd, COO_CMD_RESP_OK, out->payload);
}

#if defined(CONFIG_COO_CMD_SERIAL_GUARD)
static bool runtime_serial_guard_active(const struct coo_cmd_runtime *runtime)
{
	return runtime != NULL && atomic_get(&runtime->serial_guard_active) != 0;
}

static void runtime_clear_serial_guard(struct coo_cmd_runtime *runtime)
{
	if (runtime == NULL) {
		return;
	}

	(void)atomic_clear(&runtime->serial_guard_active);
	(void)k_work_cancel_delayable(&runtime->serial_guard_work);
}

static void runtime_note_serial_guard_activity(struct coo_cmd_runtime *runtime)
{
	int rc;

	if (runtime == NULL) {
		return;
	}
	if (runtime->serial_guard_seconds == 0U) {
		runtime_clear_serial_guard(runtime);
		return;
	}

	(void)atomic_set(&runtime->serial_guard_active, 1);
	rc = k_work_reschedule(&runtime->serial_guard_work,
			       K_SECONDS(runtime->serial_guard_seconds));
	if (rc < 0) {
		(void)atomic_clear(&runtime->serial_guard_active);
		LOG_ERR("Failed to schedule serial guard expiration (%d)", rc);
	}
}

static void serial_guard_expire_work_handler(struct k_work *work)
{
	struct k_work_delayable *dwork = k_work_delayable_from_work(work);
	struct coo_cmd_runtime *runtime =
		CONTAINER_OF(dwork, struct coo_cmd_runtime, serial_guard_work);

	(void)atomic_clear(&runtime->serial_guard_active);
	LOG_INF("Serial guard expired; MQTT command execution is enabled");
}

static bool runtime_mqtt_allowed_during_serial_guard(struct coo_cmd_runtime *runtime,
						     const struct coo_cmd_request *cmd)
{
	const struct coo_cmd_spec *spec;

	if (!runtime_serial_guard_active(runtime)) {
		return true;
	}

	if (cmd == NULL) {
		return false;
	}

	if (runtime_key_is_help(cmd->key) || runtime_key_is_serial_guard(cmd->key)) {
		return true;
	}

	if (cmd->msg_type != COO_CMD_QUERY) {
		return false;
	}

	spec = coo_cmd_runtime_find_spec(runtime, cmd->key);
	return spec != NULL &&
	       spec->query_handler != NULL &&
	       spec->mqtt_query_allowed_during_serial_guard;
}

static int runtime_serial_guard_get(struct coo_cmd_runtime *runtime,
				    const struct coo_cmd_request *cmd,
				    struct coo_cmd_response *out)
{
	int64_t remaining_ms = 0;
	int64_t remaining_s = 0;
	k_ticks_t remaining_ticks;

	if (runtime == NULL || out == NULL) {
		return coo_cmd_error(out, cmd, "serial guard unavailable");
	}

	remaining_ticks = k_work_delayable_remaining_get(&runtime->serial_guard_work);
	remaining_ms = k_ticks_to_ms_floor64(remaining_ticks);
	if (remaining_ms > 0) {
		remaining_s = (remaining_ms + (int64_t)MSEC_PER_SEC - 1LL) /
			      (int64_t)MSEC_PER_SEC;
	}
	snprintk(out->payload, sizeof(out->payload),
		 "{\"serialguard_s\":%u,\"active\":%s,\"remaining_s\":%lld}",
		 runtime->serial_guard_seconds,
		 runtime_serial_guard_active(runtime) ? "true" : "false",
		 (long long)remaining_s);
	return coo_cmd_reply(out, cmd, COO_CMD_RESP_OK, out->payload);
}

static int runtime_serial_guard_set(struct coo_cmd_runtime *runtime,
				    const struct coo_cmd_request *cmd,
				    struct coo_cmd_response *out)
{
	uint32_t holdoff_s = 0U;
	bool was_active;
	bool persist = false;
	int parse_rc_seconds;
	int parse_rc_value;
	int parse_rc_persist;

	if (runtime == NULL || cmd == NULL) {
		return coo_cmd_error(out, cmd, "serial guard unavailable");
	}

	parse_rc_seconds = coo_json_extract_u32(cmd->payload, "seconds", &holdoff_s);
	parse_rc_value = coo_json_extract_u32(cmd->payload, "value", &holdoff_s);
	if (parse_rc_seconds == COO_JSON_EXTRACT_ERR ||
	    parse_rc_value == COO_JSON_EXTRACT_ERR) {
		return coo_cmd_error(out, cmd, "invalid seconds");
	}
	if (parse_rc_seconds == COO_JSON_EXTRACT_MISSING &&
	    parse_rc_value == COO_JSON_EXTRACT_MISSING) {
		return coo_cmd_error(out, cmd, "missing seconds");
	}

	parse_rc_persist = coo_json_extract_bool(cmd->payload, "persist", &persist);
	if (parse_rc_persist != COO_JSON_EXTRACT_MISSING) {
		return coo_cmd_error(out, cmd, "serialguard persistence unsupported");
	}

	was_active = runtime_serial_guard_active(runtime);
	runtime->serial_guard_seconds = holdoff_s;
	if (holdoff_s == 0U) {
		runtime_clear_serial_guard(runtime);
	} else if (cmd->source == COO_CMD_SOURCE_SERIAL || was_active) {
		runtime_note_serial_guard_activity(runtime);
	}

	return coo_cmd_ok(out, cmd);
}
#endif

#if defined(CONFIG_COO_CMD_REBOOT)
static int runtime_parse_reboot_options(const struct coo_cmd_request *cmd,
					bool *erase_non_ip_settings)
{
	bool changed = false;
	char value[32] = {0};
	int rc;

	if (cmd == NULL || erase_non_ip_settings == NULL) {
		return -EINVAL;
	}

	*erase_non_ip_settings = false;
	if (coo_cmd_payload_empty(cmd)) {
		return 0;
	}

	rc = coo_json_extract_optional_bool(cmd->payload,
					    "erase_non_ip_settings",
					    erase_non_ip_settings,
					    &changed);
	if (rc != 0) {
		return rc;
	}
	if (changed) {
		return 0;
	}

	rc = coo_json_extract_string(cmd->payload, "value", value, sizeof(value));
	if (rc == COO_JSON_EXTRACT_OK &&
	    strcasecmp(value, "erase_non_ip_settings") == 0) {
		*erase_non_ip_settings = true;
		return 0;
	}

	return -EINVAL;
}

static void reboot_work_handler(struct k_work *work)
{
	struct k_work_delayable *dwork = k_work_delayable_from_work(work);
	struct coo_cmd_runtime *runtime =
		CONTAINER_OF(dwork, struct coo_cmd_runtime, reboot_work);

	if (runtime->reboot_prepare != NULL) {
		runtime->reboot_prepare(runtime->reboot_erase_non_ip_settings,
					runtime->user_data);
	}

	LOG_WRN("Executing scheduled reboot");
	sys_reboot(SYS_REBOOT_COLD);
}

static bool runtime_reboot_pending(const struct coo_cmd_runtime *runtime)
{
	return runtime != NULL && atomic_get(&runtime->reboot_pending) != 0;
}

static int runtime_reboot_set(struct coo_cmd_runtime *runtime,
			      const struct coo_cmd_request *cmd,
			      struct coo_cmd_response *out)
{
	bool erase_non_ip_settings = false;
	int rc;

	if (runtime == NULL || cmd == NULL) {
		return coo_cmd_error(out, cmd, "reboot unavailable");
	}
	if (runtime_parse_reboot_options(cmd, &erase_non_ip_settings) != 0) {
		return coo_cmd_error(out, cmd, "invalid reboot options");
	}
	if (!atomic_cas(&runtime->reboot_pending, 0, 1)) {
		return coo_cmd_error(out, cmd, "reboot already pending");
	}

	runtime->reboot_erase_non_ip_settings = erase_non_ip_settings;
	LOG_WRN("Reboot command accepted; rebooting in %u ms%s",
		runtime->reboot_delay_ms,
		erase_non_ip_settings ? " after erasing non-IP settings" : "");
	rc = k_work_schedule(&runtime->reboot_work,
			     K_MSEC(runtime->reboot_delay_ms));
	if (rc < 0) {
		(void)atomic_clear(&runtime->reboot_pending);
		runtime->reboot_erase_non_ip_settings = false;
		return coo_cmd_error(out, cmd, "failed to schedule reboot");
	}

	runtime_record_lastcommand(runtime, cmd);
	if (erase_non_ip_settings) {
		snprintk(out->payload, sizeof(out->payload),
			 "{\"status\":\"ok\",\"rebooting_in_ms\":%u,"
			 "\"erase_non_ip_settings\":true}",
			 runtime->reboot_delay_ms);
	} else {
		snprintk(out->payload, sizeof(out->payload),
			 "{\"status\":\"ok\",\"rebooting_in_ms\":%u}",
			 runtime->reboot_delay_ms);
	}
	return coo_cmd_reply(out, cmd, COO_CMD_RESP_OK, out->payload);
}
#endif

static bool runtime_handle_builtin_request(struct coo_cmd_runtime *runtime,
					   const struct coo_cmd_request *cmd,
					   struct coo_cmd_response *out)
{
	if (runtime == NULL || cmd == NULL || out == NULL) {
		return false;
	}

#if defined(CONFIG_COO_CMD_REBOOT)
	if (runtime_reboot_pending(runtime) && !runtime_key_is_reboot(cmd->key)) {
		(void)coo_cmd_error(out, cmd, "reboot pending");
		return true;
	}
#endif

	if (runtime_key_is_help(cmd->key)) {
		(void)runtime_help_response(runtime, cmd, out);
		return true;
	}

#if defined(CONFIG_COO_CMD_SERIAL_GUARD)
	if (runtime_key_is_serial_guard(cmd->key)) {
		if (cmd->msg_type == COO_CMD_EFFECT) {
			(void)runtime_serial_guard_set(runtime, cmd, out);
		} else {
			(void)runtime_serial_guard_get(runtime, cmd, out);
		}
		return true;
	}
#endif

#if defined(CONFIG_COO_CMD_REBOOT)
	if (runtime_key_is_reboot(cmd->key)) {
		(void)runtime_reboot_set(runtime, cmd, out);
		return true;
	}
#endif

	return false;
}

int coo_cmd_publish_mqtt(struct mqtt_client *client,
			 const struct coo_cmd_response *out,
			 uint16_t *message_id)
{
	struct mqtt_publish_param param;

	if (client == NULL || out == NULL || message_id == NULL) {
		return -EINVAL;
	}

	memset(&param, 0, sizeof(param));
	param.message.topic.qos = out->qos;
	param.message.topic.topic.utf8 = (uint8_t *)out->topic;
	param.message.topic.topic.size = strlen(out->topic);
	param.message.payload.data = (uint8_t *)out->payload;
	param.message.payload.len = out->payload_len;
	param.prop.correlation_data.data = (uint8_t *)out->correlation_data;
	param.prop.correlation_data.len = out->corr_len;
	param.message_id = (*message_id)++;
	param.dup_flag = 0U;
	param.retain_flag = 0U;

	return mqtt_publish(client, &param);
}

static int runtime_execute_default(struct coo_cmd_runtime *runtime,
				   const struct coo_cmd_request *cmd,
				   struct coo_cmd_response *out)
{
	const struct coo_cmd_spec *spec;
	coo_cmd_handler_fn handler;

	if (runtime == NULL || cmd == NULL) {
		return coo_cmd_invalid_response(out, cmd);
	}

#if defined(CONFIG_COO_CMD_REBOOT)
	if (runtime_reboot_pending(runtime)) {
		return coo_cmd_error(out, cmd, "reboot pending");
	}
#endif

	spec = coo_cmd_runtime_find_spec(runtime, cmd->key);
	LOG_INF("Dispatching: %s", cmd->key);
	if (spec == NULL) {
		return coo_cmd_unknown_response(out, cmd);
	}
	if (!coo_cmd_runtime_spec_supported(runtime, spec)) {
		return coo_cmd_error(out, cmd, "command unavailable on this board");
	}
	{
		int validate_rc = runtime_validate_payload_keys(spec, cmd, out);

		if (validate_rc > 0) {
			return 0;
		}
		if (validate_rc < 0) {
			return validate_rc;
		}
	}

	handler = cmd->msg_type == COO_CMD_EFFECT ?
		  spec->effect_handler : spec->query_handler;
	if (handler == NULL) {
		return coo_cmd_unsupported_response(out, cmd);
	}

	if (cmd->msg_type == COO_CMD_EFFECT) {
		runtime_record_lastcommand(runtime, cmd);
	}

	return handler(cmd, out);
}

void coo_cmd_runtime_executor_thread(void *p1, void *p2, void *p3)
{
	struct coo_cmd_runtime *runtime = p1;
	struct coo_cmd_request *cmd;
	struct coo_cmd_response *out;

	ARG_UNUSED(p2);
	ARG_UNUSED(p3);

	if (runtime == NULL || runtime->inbound_queue == NULL ||
	    runtime->outbound_queue == NULL) {
		LOG_ERR("command runtime executor missing queues");
		return;
	}

	cmd = &runtime->executor_cmd;
	out = &runtime->executor_out;

	while (1) {
		/* K_FOREVER sleeps until ingress queues a complete command. */
		k_msgq_get(runtime->inbound_queue, cmd, K_FOREVER);
		if (runtime_handle_builtin_request(runtime, cmd, out)) {
			/* Built-ins stay library-owned even when the app provides a
			 * custom executor hook.
			 */
		} else if (runtime->execute_handler != NULL) {
			(void)runtime->execute_handler(cmd, out);
		} else {
			(void)runtime_execute_default(runtime, cmd, out);
		}
		if (k_msgq_put(runtime->outbound_queue, out, K_NO_WAIT) != 0) {
			LOG_WRN("Outbound queue full; dropping command response");
		}
	}
}

static enum coo_cmd_msg_type runtime_classify(struct coo_cmd_runtime *runtime,
					      const struct coo_cmd_request *cmd)
{
	const struct coo_cmd_spec *spec;

	if (cmd == NULL) {
		return COO_CMD_QUERY;
	}

	if (runtime_key_is_reboot(cmd->key)) {
		return COO_CMD_EFFECT;
	}

	spec = coo_cmd_runtime_find_spec(runtime, cmd->key);
	if (spec != NULL) {
		switch (spec->class_policy) {
		case COO_CMD_CLASS_ALWAYS_QUERY:
			return COO_CMD_QUERY;
		case COO_CMD_CLASS_ALWAYS_EFFECT:
			return COO_CMD_EFFECT;
		case COO_CMD_CLASS_SUFFIX_OR_PAYLOAD_EFFECT:
			return (!coo_cmd_payload_empty(cmd) ||
				coo_cmd_key_suffix_after(cmd->key, spec->key)[0] != '\0') ?
			       COO_CMD_EFFECT : COO_CMD_QUERY;
		case COO_CMD_CLASS_CUSTOM:
			if (spec->custom_classify != NULL) {
				return spec->custom_classify(cmd, spec,
							    runtime != NULL ?
							    runtime->user_data : NULL);
			}
			break;
		case COO_CMD_CLASS_DEFAULT:
		default:
			break;
		}
	}

	return coo_cmd_payload_empty(cmd) ? COO_CMD_QUERY : COO_CMD_EFFECT;
}

static void runtime_enqueue_response(struct coo_cmd_runtime *runtime,
				     const struct coo_cmd_response *out)
{
	if (runtime == NULL || runtime->outbound_queue == NULL || out == NULL) {
		return;
	}

	if (k_msgq_put(runtime->outbound_queue, out, K_NO_WAIT) != 0) {
		LOG_WRN("Outbound queue full; dropping immediate command response");
	}
}

static void runtime_enqueue_serial_error(struct coo_cmd_runtime *runtime, const char *msg)
{
	struct coo_cmd_response *out;

	if (runtime == NULL) {
		return;
	}

	out = &runtime->outbound_scratch;
	memset(out, 0, sizeof(*out));
	out->target = COO_CMD_OUT_SERIAL;
	out->msg_type = COO_CMD_RESP_ERROR;
	out->qos = MQTT_QOS_1_AT_LEAST_ONCE;
	(void)coo_cmd_format_response_topic(runtime->device_id, "serial",
					    out->topic, sizeof(out->topic));
	out->payload_len = snprintk(out->payload, sizeof(out->payload),
				    "{\"error\":\"%s\"}", msg);
	runtime_enqueue_response(runtime, out);
}

void coo_cmd_runtime_handle_serial_line(struct coo_cmd_runtime *runtime, char *line)
{
	struct coo_cmd_request *cmd;
	const struct coo_cmd_spec *spec;
	char *cursor = line;
	char *key;
	char *payload = NULL;
	char *sep;

	if (runtime == NULL || line == NULL) {
		return;
	}

	while (*cursor == ' ' || *cursor == '\t') {
		cursor++;
	}
	if (*cursor == '\0') {
		return;
	}

#if defined(CONFIG_COO_CMD_SERIAL_GUARD)
	runtime_note_serial_guard_activity(runtime);
#endif

	sep = strpbrk(cursor, " \t");
	if (sep == NULL) {
		key = cursor;
	} else {
		*sep = '\0';
		key = cursor;
		cursor = sep + 1;
		while (*cursor == ' ' || *cursor == '\t') {
			cursor++;
		}
		payload = cursor;
	}

	if (key == NULL || *key == '\0') {
		runtime_enqueue_serial_error(runtime, "missing command key");
		return;
	}
	spec = coo_cmd_runtime_find_spec(runtime, key);

	if (runtime_key_is_help(key)) {
		if (payload_has_text(payload)) {
			runtime_enqueue_serial_error(runtime, "help takes no arguments");
		} else {
			runtime_print_serial_help(runtime, runtime->serial_wrap_column);
		}
		return;
	}

	cmd = &runtime->ingress_cmd;
	memset(cmd, 0, sizeof(*cmd));
	cmd->source = COO_CMD_SOURCE_SERIAL;
	strncpy(cmd->key, key, sizeof(cmd->key) - 1U);
	if (coo_cmd_format_response_topic(runtime->device_id, cmd->key,
					  cmd->response_topic,
					  sizeof(cmd->response_topic)) != 0) {
		runtime_enqueue_serial_error(runtime, "invalid command key");
		return;
	}

	if (runtime_normalize_serial_payload(spec, cmd->key, payload,
					     runtime->user_data, cmd->payload,
					     sizeof(cmd->payload)) != 0) {
		runtime_enqueue_serial_error(runtime, "invalid serial payload");
		return;
	}
	cmd->payload_len = strlen(cmd->payload);
	cmd->msg_type = runtime_classify(runtime, cmd);

	if (k_msgq_put(runtime->inbound_queue, cmd, K_NO_WAIT) != 0) {
		struct coo_cmd_response *out = &runtime->outbound_scratch;

		(void)coo_cmd_busy_response(out, cmd);
		runtime_enqueue_response(runtime, out);
	}
}

static void serial_reset_line(struct coo_cmd_runtime *runtime)
{
	runtime->serial_line_len = 0U;
	runtime->serial_line[0] = '\0';
	runtime->serial_line_overflow = false;
}

static void serial_accept_char(struct coo_cmd_runtime *runtime, char ch)
{
	if (ch == '\r' || ch == '\n') {
		if (runtime->serial_line_overflow) {
			runtime_enqueue_serial_error(runtime, "serial line too long");
		} else if (runtime->serial_line_len > 0U) {
			runtime->serial_line[runtime->serial_line_len] = '\0';
			coo_cmd_runtime_handle_serial_line(runtime, runtime->serial_line);
		}
		serial_reset_line(runtime);
		return;
	}

	if (ch == '\b' || ch == 0x7f) {
		if (runtime->serial_line_len > 0U) {
			runtime->serial_line_len--;
			runtime->serial_line[runtime->serial_line_len] = '\0';
		}
		return;
	}

	if ((unsigned char)ch < 0x20U && ch != '\t') {
		return;
	}

	if (runtime->serial_line_len + 1U >= sizeof(runtime->serial_line)) {
		runtime->serial_line_overflow = true;
		return;
	}

	runtime->serial_line[runtime->serial_line_len++] = ch;
	runtime->serial_line[runtime->serial_line_len] = '\0';
}

static int runtime_init_serial_console(struct coo_cmd_runtime *runtime)
{
	int rc;

	if (runtime == NULL) {
		return -EINVAL;
	}

	rc = console_init();
	if (rc != 0) {
		return rc;
	}

	console_set_rx_timeout(K_NO_WAIT);
	serial_reset_line(runtime);
	runtime->serial_initialized = true;
	return 0;
}

void coo_cmd_runtime_serial_poll(struct coo_cmd_runtime *runtime)
{
	int budget = SERIAL_POLL_CHAR_BUDGET;

	if (runtime == NULL || !runtime->serial_initialized) {
		return;
	}

	while (budget-- > 0) {
		char ch;
		ssize_t read_len = console_read(NULL, &ch, sizeof(ch));

		if (read_len == 1) {
			serial_accept_char(runtime, ch);
			continue;
		}

		if (read_len < 0 && read_len != -EAGAIN) {
			LOG_WRN("Serial console read failed (%zd)", read_len);
		}
		break;
	}
}

void coo_cmd_runtime_handle_mqtt_publish(struct coo_cmd_runtime *runtime,
					 const struct mqtt_publish_param *pub)
{
	struct coo_cmd_request *cmd;
	char req_topic[COO_CMD_TOPIC_MAX];
	const char *suffix;
	size_t prefix_len;
	size_t suffix_len;

	if (runtime == NULL || pub == NULL ||
	    !coo_cmd_copy_mqtt_utf8(&pub->message.topic.topic,
				    req_topic, sizeof(req_topic))) {
		return;
	}
	cmd = &runtime->ingress_cmd;
	memset(cmd, 0, sizeof(*cmd));

	prefix_len = strlen(runtime->request_prefix);
	if (prefix_len == 0U ||
	    strncmp(req_topic, runtime->request_prefix, prefix_len) != 0) {
		return;
	}

	suffix = req_topic + prefix_len;
	suffix_len = strlen(suffix);
	if (suffix_len == 0U || suffix_len >= sizeof(cmd->key)) {
		LOG_WRN("Invalid MQTT command topic suffix");
		return;
	}

	cmd->source = COO_CMD_SOURCE_MQTT;
	memcpy(cmd->key, suffix, suffix_len);
	cmd->key[suffix_len] = '\0';

		if (coo_cmd_format_response_topic(runtime->device_id, cmd->key,
						  cmd->response_topic,
						  sizeof(cmd->response_topic)) != 0) {
			struct coo_cmd_response *out = &runtime->outbound_scratch;

			(void)coo_cmd_invalid_response(out, cmd);
			runtime_enqueue_response(runtime, out);
			return;
		}

		if (pub->retain_flag != 0U) {
			struct coo_cmd_response *out = &runtime->outbound_scratch;

			(void)coo_cmd_reply(out, cmd, COO_CMD_RESP_ERROR,
					    "{\"error\":\"retained MQTT command ignored\"}");
			LOG_WRN("Ignoring retained MQTT command '%s'", cmd->key);
			runtime_enqueue_response(runtime, out);
			return;
		}

	if (pub->prop.response_topic.utf8 != NULL &&
	    pub->prop.response_topic.size > 0U &&
	    pub->prop.response_topic.size < sizeof(cmd->response_topic)) {
		memcpy(cmd->response_topic, pub->prop.response_topic.utf8,
		       pub->prop.response_topic.size);
		cmd->response_topic[pub->prop.response_topic.size] = '\0';
	}

		if (pub->message.payload.len >= sizeof(cmd->payload)) {
			struct coo_cmd_response *out = &runtime->outbound_scratch;

			(void)coo_cmd_invalid_response(out, cmd);
			runtime_enqueue_response(runtime, out);
			return;
		}

	if (pub->message.payload.len > 0U) {
		memcpy(cmd->payload, pub->message.payload.data,
		       pub->message.payload.len);
		cmd->payload[pub->message.payload.len] = '\0';
		cmd->payload_len = pub->message.payload.len;
	} else {
		snprintk(cmd->payload, sizeof(cmd->payload), "{}");
		cmd->payload_len = strlen(cmd->payload);
	}
	cmd->msg_type = runtime_classify(runtime, cmd);

	if (pub->prop.correlation_data.len > 0U &&
	    pub->prop.correlation_data.len <= sizeof(cmd->correlation_data)) {
		memcpy(cmd->correlation_data, pub->prop.correlation_data.data,
		       pub->prop.correlation_data.len);
		cmd->corr_len = pub->prop.correlation_data.len;
	} else if (pub->prop.correlation_data.len > sizeof(cmd->correlation_data)) {
		LOG_WRN("MQTT correlation_data too long (%zu > %zu); response will not echo it",
			pub->prop.correlation_data.len, sizeof(cmd->correlation_data));
	}

#if defined(CONFIG_COO_CMD_SERIAL_GUARD)
		if (!runtime_mqtt_allowed_during_serial_guard(runtime, cmd)) {
			struct coo_cmd_response *out = &runtime->outbound_scratch;

			(void)coo_cmd_serial_active_response(out, cmd);
			LOG_WRN("Rejecting MQTT command '%s': local serial control is active", cmd->key);
			runtime_enqueue_response(runtime, out);
			(void)coo_cmd_runtime_emit(
				runtime,
				&(const struct coo_cmd_runtime_emit_args){
					.type = COO_CMD_RUNTIME_EMIT_WARNING,
					.delivery = COO_CMD_RUNTIME_EMIT_BEST_EFFORT,
					.code = "serial_guard_active",
					.msg = "MQTT command rejected while serial command guard is active",
					.context = cmd->key,
				});
		return;
	}
#endif

	if (k_msgq_put(runtime->inbound_queue, cmd, K_NO_WAIT) != 0) {
		struct coo_cmd_response *out = &runtime->outbound_scratch;

		(void)coo_cmd_busy_response(out, cmd);
		runtime_enqueue_response(runtime, out);
	}
}

void coo_cmd_runtime_mqtt_callback(const struct mqtt_publish_param *pub,
				   void *user_data)
{
	coo_cmd_runtime_handle_mqtt_publish(user_data, pub);
}

void coo_cmd_runtime_drain_outbound(struct coo_cmd_runtime *runtime,
				    struct mqtt_client *client,
				    bool mqtt_available)
{
	struct coo_cmd_response *out;
	int budget = 8;
	bool outbound_full;
	uint16_t wrap_column;

	if (runtime == NULL || runtime->outbound_queue == NULL ||
	    runtime->mqtt_msg_id == NULL) {
		return;
	}
	wrap_column = runtime->serial_wrap_column != 0U ?
		runtime->serial_wrap_column : COO_CMD_SERIAL_WRAP_COLUMN;
	out = &runtime->outbound_scratch;

	outbound_full = (k_msgq_num_free_get(runtime->outbound_queue) == 0U);
	if (outbound_full) {
		if (!runtime->outbound_full_warning_seen) {
			LOG_WRN("outbound_queue_full: outbound queue reached capacity context=command_drain");
			runtime->outbound_full_warning_seen = true;
		}
	} else {
		runtime->outbound_full_warning_seen = false;
	}

	while (budget-- > 0 &&
	       k_msgq_get(runtime->outbound_queue, out, K_NO_WAIT) == 0) {
		const bool best_effort = (out->target == COO_CMD_OUT_MQTT_BEST_EFFORT);

		if (out->target == COO_CMD_OUT_SERIAL) {
			coo_cmd_print_serial_response_pretty(out, wrap_column);
			continue;
		}

		if (!mqtt_available) {
			if (best_effort) {
				LOG_DBG("Dropping best-effort MQTT msg while MQTT unavailable");
				continue;
			}
			if (k_msgq_put(runtime->outbound_queue, out, K_NO_WAIT) != 0) {
				LOG_WRN("Dropping MQTT msg (queue full while requeueing)");
			}
			continue;
		}

		if (coo_cmd_publish_mqtt(client, out, runtime->mqtt_msg_id) != 0) {
			if (best_effort) {
				LOG_WRN("Best-effort MQTT publish failed; dropping msg");
				continue;
			}
			LOG_WRN("MQTT publish failed; will retry");
			if (k_msgq_put(runtime->outbound_queue, out, K_NO_WAIT) != 0) {
				LOG_WRN("Dropping MQTT msg (queue full after publish failure)");
			}
			break;
		}
	}
}

void coo_cmd_print_serial_response(const struct coo_cmd_response *out,
				   uint16_t wrap_column)
{
	size_t len;
	uint16_t col = 0U;

	if (out == NULL) {
		return;
	}

	printk("%s" COO_CMD_SERIAL_LINE_END "        ",
	       out->topic[0] != '\0' ? out->topic : "serial");
	col = 8U;
	len = out->payload_len > 0U ? out->payload_len : strlen(out->payload);

	for (size_t i = 0U; i < len && out->payload[i] != '\0'; ++i) {
		const char ch = out->payload[i];

		if (ch == '\r') {
			continue;
		}

		if (ch == '\n' || (wrap_column != 0U && col >= wrap_column)) {
			printk(COO_CMD_SERIAL_LINE_END "        ");
			col = 8U;
			if (ch == '\n') {
				continue;
			}
		}

		printk("%c", ch);
		col++;

		if (wrap_column != 0U &&
		    (ch == ',' || ch == '}') && col >= (wrap_column - 8U) &&
		    i + 1U < len) {
			printk(COO_CMD_SERIAL_LINE_END "        ");
			col = 8U;
		}
	}

	printk(COO_CMD_SERIAL_LINE_END);
}

static const char *serial_payload_start(const char *payload)
{
	while (payload != NULL && isspace((unsigned char)*payload)) {
		payload++;
	}

	return payload;
}

static void serial_response_newline_indent(uint8_t indent, uint16_t *col)
{
	printk(COO_CMD_SERIAL_LINE_END);
	*col = 8U;
	for (uint8_t i = 0U; i < 8U; ++i) {
		printk(" ");
	}
	for (uint8_t i = 0U; i < indent; ++i) {
		printk("  ");
		*col += 2U;
	}
}

static bool json_stack_push(char *stack, size_t stack_len, uint8_t *depth, char close_ch)
{
	if (*depth >= stack_len) {
		return false;
	}

	stack[*depth] = close_ch;
	(*depth)++;
	return true;
}

static bool json_stack_pop(char *stack, uint8_t *depth, char close_ch)
{
	if (*depth == 0U || stack[*depth - 1U] != close_ch) {
		return false;
	}

	(*depth)--;
	return true;
}

static bool json_scalar_array_end(const char *payload, size_t len,
				  size_t start, size_t *end)
{
	bool in_string = false;
	bool escaped = false;

	if (payload == NULL || end == NULL || start >= len || payload[start] != '[') {
		return false;
	}

	for (size_t i = start + 1U; i < len && payload[i] != '\0'; ++i) {
		const char ch = payload[i];

		if (in_string) {
			if (escaped) {
				escaped = false;
			} else if (ch == '\\') {
				escaped = true;
			} else if (ch == '"') {
				in_string = false;
			} else if ((unsigned char)ch < 0x20U) {
				return false;
			}
			continue;
		}

		if (isspace((unsigned char)ch)) {
			continue;
		}

		switch (ch) {
		case '"':
			in_string = true;
			break;
		case '[':
		case '{':
			return false;
		case ']':
			*end = i;
			return true;
		default:
			if ((unsigned char)ch < 0x20U) {
				return false;
			}
			break;
		}
	}

	return false;
}

static bool serial_print_json_scalar_array(const char *payload, size_t start,
					   size_t end, uint16_t *col)
{
	bool in_string = false;
	bool escaped = false;

	for (size_t i = start; i <= end; ++i) {
		const char ch = payload[i];

		if (in_string) {
			printk("%c", ch);
			(*col)++;
			if (escaped) {
				escaped = false;
			} else if (ch == '\\') {
				escaped = true;
			} else if (ch == '"') {
				in_string = false;
			} else if ((unsigned char)ch < 0x20U) {
				return false;
			}
			continue;
		}

		if (isspace((unsigned char)ch)) {
			continue;
		}

		if (ch == '"') {
			in_string = true;
			printk("%c", ch);
			(*col)++;
		} else if (ch == ',') {
			printk(", ");
			*col += 2U;
		} else {
			if ((unsigned char)ch < 0x20U) {
				return false;
			}
			printk("%c", ch);
			(*col)++;
		}
	}

	return !in_string && !escaped;
}

static bool serial_print_json_payload(const char *payload, size_t len)
{
	char stack[16];
	uint8_t depth = 0U;
	uint16_t col = 8U;
	bool in_string = false;
	bool escaped = false;
	bool saw_token = false;

	for (size_t i = 0U; i < len && payload[i] != '\0'; ++i) {
		const char ch = payload[i];

		if (in_string) {
			printk("%c", ch);
			col++;
			if (escaped) {
				escaped = false;
			} else if (ch == '\\') {
				escaped = true;
			} else if (ch == '"') {
				in_string = false;
			} else if ((unsigned char)ch < 0x20U) {
				return false;
			}
			continue;
		}

		if (isspace((unsigned char)ch)) {
			continue;
		}

		saw_token = true;
		switch (ch) {
		case '"':
			in_string = true;
			printk("%c", ch);
			col++;
			break;
		case '{':
			if (!json_stack_push(stack, sizeof(stack), &depth, '}')) {
				return false;
			}
			printk("%c", ch);
			col++;
			serial_response_newline_indent(depth, &col);
			break;
		case '[':
		{
			size_t array_end;

			if (json_scalar_array_end(payload, len, i, &array_end)) {
				if (!serial_print_json_scalar_array(payload, i, array_end, &col)) {
					return false;
				}
				i = array_end;
				break;
			}
			if (!json_stack_push(stack, sizeof(stack), &depth, ']')) {
				return false;
			}
			printk("%c", ch);
			col++;
			serial_response_newline_indent(depth, &col);
			break;
		}
		case '}':
		case ']':
			if (!json_stack_pop(stack, &depth, ch)) {
				return false;
			}
			serial_response_newline_indent(depth, &col);
			printk("%c", ch);
			col++;
			break;
		case ',':
			printk("%c", ch);
			col++;
			serial_response_newline_indent(depth, &col);
			break;
		case ':':
			printk(": ");
			col += 2U;
			break;
		default:
			if ((unsigned char)ch < 0x20U) {
				return false;
			}
			printk("%c", ch);
			col++;
			break;
		}
	}

	return saw_token && !in_string && !escaped && depth == 0U;
}

void coo_cmd_print_serial_response_pretty(const struct coo_cmd_response *out,
					  uint16_t wrap_column)
{
	const char *payload;
	size_t len;

	if (out == NULL) {
		return;
	}

	payload = out->payload;
	len = out->payload_len > 0U ? out->payload_len : strlen(out->payload);
	printk("%s" COO_CMD_SERIAL_LINE_END "        ",
	       out->topic[0] != '\0' ? out->topic : "serial");

	payload = serial_payload_start(payload);
	if (payload != NULL && (*payload == '{' || *payload == '[')) {
		if (!serial_print_json_payload(payload, len - (size_t)(payload - out->payload))) {
			uint16_t col = 13U;

			printk("{\"error\":\"serial JSON render failed\"}"
			       COO_CMD_SERIAL_LINE_END "        raw: ");
			for (size_t i = 0U; i < len && out->payload[i] != '\0'; ++i) {
				const char ch = out->payload[i];

				if (ch == '\r') {
					continue;
				}
				if (ch == '\n' ||
				    (wrap_column != 0U && col >= wrap_column)) {
					printk(COO_CMD_SERIAL_LINE_END "        ");
					col = 8U;
					if (ch == '\n') {
						continue;
					}
				}
				printk("%c", ch);
				col++;
			}
			printk(COO_CMD_SERIAL_LINE_END);
			return;
		}
		printk(COO_CMD_SERIAL_LINE_END);
		return;
	}

	for (size_t i = 0U; i < len && out->payload[i] != '\0'; ++i) {
		const char ch = out->payload[i];

		if (ch == '\r') {
			continue;
		}
		if (ch == '\n') {
			printk(COO_CMD_SERIAL_LINE_END "        ");
			continue;
		}
		printk("%c", ch);
	}
	printk(COO_CMD_SERIAL_LINE_END);
}
