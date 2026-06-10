/*
 * Copyright (c) 2026 Caltech Optical Observatories
 * SPDX-License-Identifier: Apache-2.0
 */

#include "mems_command.h"

#include <ctype.h>
#include <errno.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include <zephyr/logging/log.h>
#include <zephyr/sys/util.h>

#include "app_settings.h"
#include "command.h"
#include "mems_switching.h"

#include <coo_commons/command_dispatch.h>
#include <coo_commons/json_utils.h>

LOG_MODULE_REGISTER(mems_command, LOG_LEVEL_DBG);

extern struct mems_router router;

static bool memsroute_output_seen(const char *const *outputs, uint8_t count,
                                  const char *output_name)
{
    for (uint8_t i = 0U; i < count; ++i) {
        if (strcmp(outputs[i], output_name) == 0) {
            return true;
        }
    }

    return false;
}

static int memsroute_append_sources_for_output(char *buf, size_t buf_len, size_t *offset,
                                               const struct mems_route_key *active,
                                               uint8_t active_count,
                                               const char *output_name)
{
    uint8_t n_sources = 0U;

    if (coo_json_append(buf, buf_len, offset, "\"%s\":[", output_name) != 0) {
        return -ENOSPC;
    }

    for (uint8_t i = 0U; i < active_count; ++i) {
        if (strcmp(active[i].output_name, output_name) != 0) {
            continue;
        }

        if (n_sources > 0U && coo_json_append(buf, buf_len, offset, ",") != 0) {
            return -ENOSPC;
        }
        if (coo_json_append(buf, buf_len, offset, "\"%s\"",
                            active[i].input_name) != 0) {
            return -ENOSPC;
        }
        n_sources++;
    }

    if (n_sources == 0U &&
        coo_json_append(buf, buf_len, offset, "\"no source\"") != 0) {
        return -ENOSPC;
    }

    return coo_json_append(buf, buf_len, offset, "]");
}

static const char *const route_loss_laser_names[] = {
    "1028y", "1270j", "1430yj", "1430hk", "1510h", "2330k",
};

static const struct coo_json_string_choice mems_switch_state_choices[] = {
    { "A", 'A' },
    { "B", 'B' },
    { "a", 'A' },
    { "b", 'B' },
};

static bool memsroute_is_route_loss_key(const char *key)
{
    return strcmp(key, "memsroute/route_loss") == 0;
}

static const char *route_loss_json_value_for_key(const char *json, const char *key)
{
    char pattern[40];
    const char *match;
    const char *colon;
    int written;

    if (json == NULL || key == NULL) {
        return NULL;
    }

    written = snprintk(pattern, sizeof(pattern), "\"%s\"", key);
    if (written < 0 || written >= (int)sizeof(pattern)) {
        return NULL;
    }

    match = strstr(json, pattern);
    if (match == NULL) {
        return NULL;
    }

    colon = strchr(match + strlen(pattern), ':');
    if (colon == NULL) {
        return NULL;
    }

    return coo_json_skip_ws(colon + 1);
}

static int route_loss_parse_db_string(const char *text, double *transmission)
{
    char *end = NULL;
    double loss_db;

    if (text == NULL || transmission == NULL) {
        return -EINVAL;
    }

    errno = 0;
    loss_db = strtod(text, &end);
    if (errno != 0 || end == text) {
        return -EINVAL;
    }

    while (*end != '\0' && isspace((unsigned char)*end)) {
        end++;
    }
    if (strcasecmp(end, "db") != 0) {
        return -EINVAL;
    }
    if (loss_db < 0.0) {
        return -ERANGE;
    }

    *transmission = pow(10.0, -loss_db / 10.0);
    return (*transmission > 0.0 && *transmission <= 1.0) ? 0 : -ERANGE;
}

static int route_loss_parse_scalar_token(const char *start, const char **end,
                                         double *transmission)
{
    char db_text[24] = {0};
    char *parse_end = NULL;
    double tx;
    size_t len;

    if (start == NULL || end == NULL || transmission == NULL) {
        return -EINVAL;
    }

    start = coo_json_skip_ws(start);
    if (*start == '"') {
        start++;
        len = strcspn(start, "\"");
        if (start[len] != '"' || len == 0U || len >= sizeof(db_text)) {
            return -EINVAL;
        }
        memcpy(db_text, start, len);
        db_text[len] = '\0';
        *end = start + len + 1;
        return route_loss_parse_db_string(db_text, transmission);
    }

    errno = 0;
    tx = strtod(start, &parse_end);
    if (errno != 0 || parse_end == start || !(tx > 0.0 && tx <= 1.0)) {
        return -ERANGE;
    }

    *transmission = tx;
    *end = parse_end;
    return 0;
}

static int route_loss_extract_field_transmission(const struct coo_cmd_request *cmd,
                                                 const char *field,
                                                 double *transmission)
{
    char db_text[24] = {0};
    double tx = 0.0;
    int rc_num;
    int rc_str;
    int parse_rc;

    rc_num = coo_json_extract_double(cmd->payload, field, &tx);
    if (rc_num == COO_JSON_EXTRACT_OK) {
        if (!(tx > 0.0 && tx <= 1.0)) {
            return -ERANGE;
        }
        *transmission = tx;
        return 0;
    }

    rc_str = coo_json_extract_string(cmd->payload, field, db_text, sizeof(db_text));
    if (rc_str == COO_JSON_EXTRACT_OK) {
        parse_rc = route_loss_parse_db_string(db_text, &tx);
        if (parse_rc != 0) {
            return parse_rc;
        }
        *transmission = tx;
        return 0;
    }

    if (rc_num == COO_JSON_EXTRACT_MISSING && rc_str == COO_JSON_EXTRACT_MISSING) {
        return -ENOENT;
    }

    return -EINVAL;
}

static int route_loss_extract_value(const struct coo_cmd_request *cmd,
                                    char *laser, size_t laser_len,
                                    double *transmission)
{
    for (uint8_t i = 0U; i < ARRAY_SIZE(route_loss_laser_names); ++i) {
        const char *candidate = route_loss_laser_names[i];
        int rc;

        rc = route_loss_extract_field_transmission(cmd, candidate, transmission);
        if (rc == 0) {
            snprintk(laser, laser_len, "%s", candidate);
            return 0;
        }
        if (rc != -ENOENT) {
            return rc;
        }
    }

    return -ENOENT;
}

static int route_loss_extract_split_tuple(const struct coo_cmd_request *cmd,
                                          double transmission[MEMS_SPLIT_OUTPUT_COUNT])
{
    const char *cursor;
    int rc;

    cursor = route_loss_json_value_for_key(cmd->payload, "split");
    if (cursor == NULL) {
        return -ENOENT;
    }
    if (*cursor != '[') {
        return -EINVAL;
    }
    cursor++;

    for (uint8_t i = 0U; i < MEMS_SPLIT_OUTPUT_COUNT; ++i) {
        cursor = coo_json_skip_ws(cursor);
        rc = route_loss_parse_scalar_token(cursor, &cursor, &transmission[i]);
        if (rc != 0) {
            return rc;
        }

        cursor = coo_json_skip_ws(cursor);
        if (i + 1U < MEMS_SPLIT_OUTPUT_COUNT) {
            if (*cursor != ',') {
                return -EINVAL;
            }
            cursor++;
        }
    }

    cursor = coo_json_skip_ws(cursor);
    return *cursor == ']' ? 0 : -EINVAL;
}

static bool route_loss_route_is_split(const char *route)
{
    char split_route[APP_ROUTE_LOSS_ROUTE_MAX_LEN] = {0};

    for (uint8_t i = 0U; i < MEMS_SPLIT_CHANNEL_COUNT; ++i) {
        (void)mems_split_route_name(i, split_route, sizeof(split_route));
        if (strcmp(route, split_route) == 0) {
            return true;
        }
    }

    return false;
}

static int route_loss_append_tx(char *payload, size_t payload_len, size_t *offset,
                                double tx)
{
    return coo_json_append(payload, payload_len, offset, "%.6f", tx);
}

static int route_loss_query_response(const struct coo_cmd_request *cmd,
				     const char *route,
				     struct coo_cmd_response *out)
{
    char payload[MAX_PAYLOAD_LEN] = {0};
    size_t offset = 0U;

    if (coo_json_append(payload, sizeof(payload), &offset,
                        "{\"route\":\"%s\",", route) != 0) {
        return coo_cmd_error(out, cmd, "route_loss response too large");
    }

    if (route_loss_route_is_split(route)) {
        if (coo_json_append(payload, sizeof(payload), &offset, "\"split\":[") != 0) {
            return coo_cmd_error(out, cmd, "route_loss response too large");
        }
        for (uint8_t i = 0U; i < MEMS_SPLIT_OUTPUT_COUNT; ++i) {
            const char *loss_key = mems_split_output_loss_key(i);
            double tx = 1.0;

            (void)app_settings_get_route_loss(route, loss_key, &tx);
            if (i > 0U &&
                coo_json_append(payload, sizeof(payload), &offset, ",") != 0) {
                return coo_cmd_error(out, cmd, "route_loss response too large");
            }
            if (route_loss_append_tx(payload, sizeof(payload), &offset, tx) != 0) {
                return coo_cmd_error(out, cmd, "route_loss response too large");
            }
        }
        if (coo_json_append(payload, sizeof(payload), &offset, "]}") != 0) {
            return coo_cmd_error(out, cmd, "route_loss response too large");
        }
        return coo_cmd_reply(out, cmd, COO_CMD_RESP_OK, payload);
    }

    if (coo_json_append(payload, sizeof(payload), &offset, "\"lasers\":{") != 0) {
        return coo_cmd_error(out, cmd, "route_loss response too large");
    }
    for (uint8_t i = 0U; i < ARRAY_SIZE(route_loss_laser_names); ++i) {
        double tx = 1.0;

        (void)app_settings_get_route_loss(route, route_loss_laser_names[i], &tx);
        if (i > 0U &&
            coo_json_append(payload, sizeof(payload), &offset, ",") != 0) {
            return coo_cmd_error(out, cmd, "route_loss response too large");
        }
        if (coo_json_append(payload, sizeof(payload), &offset, "\"%s\":",
                            route_loss_laser_names[i]) != 0 ||
            route_loss_append_tx(payload, sizeof(payload), &offset, tx) != 0) {
            return coo_cmd_error(out, cmd, "route_loss response too large");
        }
    }
    if (coo_json_append(payload, sizeof(payload), &offset, "}}") != 0) {
        return coo_cmd_error(out, cmd, "route_loss response too large");
    }

    return coo_cmd_reply(out, cmd, COO_CMD_RESP_OK, payload);
}

static int route_loss_handle(const struct coo_cmd_request *cmd, bool set_request,
			     struct coo_cmd_response *out)
{
    char route[APP_ROUTE_LOSS_ROUTE_MAX_LEN] = {0};
    char laser[APP_ROUTE_LOSS_LASER_MAX_LEN] = {0};
    double split_tx[MEMS_SPLIT_OUTPUT_COUNT] = {0};
    double tx = 1.0;
    bool persist = false;
    int split_rc;
    int laser_rc;
    int parse_rc;

    parse_rc = coo_json_extract_string(cmd->payload, "route", route, sizeof(route));
    if (parse_rc != COO_JSON_EXTRACT_OK) {
        return coo_cmd_error(out, cmd, "missing or invalid route");
    }

    parse_rc = coo_json_extract_bool(cmd->payload, "persistent", &persist);
    if (parse_rc == COO_JSON_EXTRACT_ERR) {
        return coo_cmd_error(out, cmd, "invalid persistent");
    }

    split_rc = route_loss_extract_split_tuple(cmd, split_tx);
    laser_rc = route_loss_extract_value(cmd, laser, sizeof(laser), &tx);

    if (!set_request) {
        if (split_rc != -ENOENT || laser_rc != -ENOENT ||
            parse_rc == COO_JSON_EXTRACT_OK) {
            return coo_cmd_error(out, cmd, "route_loss query uses route only");
        }
        return route_loss_query_response(cmd, route, out);
    }

    if (split_rc == 0 && laser_rc == 0) {
        return coo_cmd_error(out, cmd, "route_loss uses split or laser value");
    }
    if (split_rc == -ERANGE || laser_rc == -ERANGE) {
        return coo_cmd_error(out, cmd, "route_loss out of range");
    }
    if (split_rc != 0 && split_rc != -ENOENT) {
        return coo_cmd_error(out, cmd, "invalid split route_loss");
    }
    if (laser_rc != 0 && laser_rc != -ENOENT) {
        return coo_cmd_error(out, cmd, "invalid route_loss value");
    }

    if (split_rc == 0) {
        if (!route_loss_route_is_split(route)) {
            return coo_cmd_error(out, cmd, "route_loss split route invalid");
        }
        for (uint8_t i = 0U; i < MEMS_SPLIT_OUTPUT_COUNT; ++i) {
            const char *loss_key = mems_split_output_loss_key(i);

            parse_rc = app_settings_set_route_loss(route, loss_key, split_tx[i], persist);
            if (parse_rc == -ENOSPC) {
                return coo_cmd_error(out, cmd, "route_loss table full");
            }
            if (parse_rc != 0) {
                return coo_cmd_error(out, cmd, "invalid route_loss key");
            }
        }
        return coo_cmd_ok(out, cmd);
    }

    if (laser_rc == -ENOENT) {
        return coo_cmd_error(out, cmd, "missing route_loss value");
    }
    parse_rc = app_settings_set_route_loss(route, laser, tx, persist);
    if (parse_rc == -ENOSPC) {
        return coo_cmd_error(out, cmd, "route_loss table full");
    }
    if (parse_rc != 0) {
        return coo_cmd_error(out, cmd, "invalid route_loss key");
    }

    return coo_cmd_ok(out, cmd);
}

int memsroute_get(const struct coo_cmd_request *cmd, struct coo_cmd_response *out)
{
    struct mems_route_key active[MEMS_ROUTER_MAX_ROUTES];
    const char *outputs[MEMS_ROUTER_MAX_ROUTES];
    uint8_t n_active = mems_router_active_routes(&router, active, MEMS_ROUTER_MAX_ROUTES);
    uint8_t n_outputs = 0U;
    char buf[MAX_PAYLOAD_LEN] = {0};
    size_t offset = 0U;

    if (memsroute_is_route_loss_key(cmd->key)) {
        return route_loss_handle(cmd, false, out);
    }

    if (coo_json_append(buf, sizeof(buf), &offset, "{\"active_routes\":{") != 0) {
        return coo_cmd_error(out, cmd, "response too large");
    }

    for (uint8_t i = 0U; router.routes != NULL && i < router.num_routes; ++i) {
        const char *output_name = router.routes[i].key.output_name;

        if (memsroute_output_seen(outputs, n_outputs, output_name)) {
            continue;
        }

        outputs[n_outputs++] = output_name;
        if (n_outputs > 1U &&
            coo_json_append(buf, sizeof(buf), &offset, ",") != 0) {
            return coo_cmd_error(out, cmd, "response too large");
        }
        if (memsroute_append_sources_for_output(buf, sizeof(buf), &offset,
                                                active, n_active, output_name) != 0) {
            return coo_cmd_error(out, cmd, "response too large");
        }
    }

    if (coo_json_append(buf, sizeof(buf), &offset, "}}") != 0) {
        return coo_cmd_error(out, cmd, "response too large");
    }

    return coo_cmd_reply(out, cmd, COO_CMD_RESP_OK, buf);
}

static double split_abs_float(double value)
{
    return value < 0.0 ? -value : value;
}

static int split_channel_response(const struct coo_cmd_request *cmd,
				  const struct mems_split_state *state,
				  uint8_t channel_index,
				  struct coo_cmd_response *out)
{
    const char *channel_name = mems_split_channel_name(channel_index);
    char payload[MAX_PAYLOAD_LEN];
    int written;

    if (state == NULL || channel_name == NULL) {
        return coo_cmd_error(out, cmd, "split route invalid");
    }

    written = snprintk(payload, sizeof(payload),
             "{\"channel\":\"%s\","
             "\"ratio_ask\":[%.4f,%.4f,%.4f],"
             "\"ratio_actual\":[%.4f,%.4f,%.4f],"
             "\"ratio_out\":[%.4f,%.4f,%.4f],"
             "\"split_transmission\":[%.6f,%.6f,%.6f],"
             "\"cycle_ms\":%u,"
             "\"switches\":["
             "{\"name\":\"%s\",\"state\":\"%c\",\"duty_cycle\":%.4f,\"a_ms\":%u,\"b_ms\":%u},"
             "{\"name\":\"%s\",\"state\":\"%c\",\"duty_cycle\":%.4f,\"a_ms\":%u,\"b_ms\":%u},"
             "{\"name\":\"%s\",\"state\":\"%c\",\"duty_cycle\":%.4f,\"a_ms\":%u,\"b_ms\":%u}],"
             "\"stop_in_s\":%u}",
             channel_name,
             (double)state->requested[0],
             (double)state->requested[1],
             (double)state->requested[2],
             (double)state->actual[0],
             (double)state->actual[1],
             (double)state->actual[2],
             (double)state->output[0],
             (double)state->output[1],
             (double)state->output[2],
             (double)state->transmission[0],
             (double)state->transmission[1],
             (double)state->transmission[2],
             state->cycle_ms,
             state->switches[0].name,
             state->switches[0].state,
             (double)state->switches[0].duty_cycle,
             state->switches[0].a_ms,
             state->switches[0].b_ms,
             state->switches[1].name,
             state->switches[1].state,
             (double)state->switches[1].duty_cycle,
             state->switches[1].a_ms,
             state->switches[1].b_ms,
             state->switches[2].name,
             state->switches[2].state,
             (double)state->switches[2].duty_cycle,
             state->switches[2].a_ms,
             state->switches[2].b_ms,
             state->stop_in_s);

    if (written < 0 || written >= (int)sizeof(payload)) {
        return coo_cmd_error(out, cmd, "split response too large");
    }

    return coo_cmd_reply(out, cmd, COO_CMD_RESP_OK, payload);
}

static void split_emit_quantization_warning(uint8_t channel_index,
                                            const struct mems_split_state *state)
{
    const char *channel_name = mems_split_channel_name(channel_index);
    char context[144];

    if (channel_name == NULL || state == NULL) {
        return;
    }

    if (split_abs_float(state->output[0] - state->requested[0]) <= 0.0005 &&
        split_abs_float(state->output[1] - state->requested[1]) <= 0.0005 &&
        split_abs_float(state->output[2] - state->requested[2]) <= 0.0005) {
        return;
    }

    snprintk(context, sizeof(context),
             "channel=%s ask=%.4f/%.4f/%.4f out=%.4f/%.4f/%.4f",
             channel_name,
             (double)state->requested[0],
             (double)state->requested[1],
             (double)state->requested[2],
             (double)state->output[0],
             (double)state->output[1],
             (double)state->output[2]);
	coo_cmd_runtime_emit(command_runtime_get(),
			     &(const struct coo_cmd_runtime_emit_args){
				     .type = COO_CMD_RUNTIME_EMIT_WARNING,
				     .delivery = COO_CMD_RUNTIME_EMIT_BEST_EFFORT,
				     .code = "split_ratio_quantized",
				     .msg = "requested split ratio was quantized to MEMS ticks",
				     .context = context,
			     });
}

static int split_channel_index_from_key(const char *key, uint8_t *index)
{
    char channel[8] = {0};

    if (coo_cmd_key_suffix_segment_copy(key, "split", channel, sizeof(channel)) != 0) {
        return -ENOENT;
    }

    return mems_split_channel_index(channel, index);
}

static int split_parse_channel(const struct coo_cmd_request *cmd, uint8_t *channel_index)
{
    char channel[8] = {0};
    int parse_rc;

    if (split_channel_index_from_key(cmd->key, channel_index) == 0) {
        return 0;
    }

    parse_rc = coo_json_extract_string(cmd->payload, "channel",
                                       channel, sizeof(channel));
    if (parse_rc == COO_JSON_EXTRACT_MISSING) {
        return -ENOENT;
    }
    if (parse_rc == COO_JSON_EXTRACT_ERR) {
        return -EINVAL;
    }

    return mems_split_channel_index(channel, channel_index);
}

int splitting_get(const struct coo_cmd_request *cmd, struct coo_cmd_response *out)
{
    struct mems_split_state state = {0};
    uint8_t channel_index;
    int rc;

    rc = split_parse_channel(cmd, &channel_index);
    if (rc != 0) {
        return coo_cmd_error(out, cmd, "channel required: yj or hk");
    }

    rc = mems_split_read_channel_state(&router, channel_index, NULL, &state);
    if (rc != 0) {
        return coo_cmd_error(out, cmd, "split route invalid");
    }

    return split_channel_response(cmd, &state, channel_index, out);
}

int splitting_set(const struct coo_cmd_request *cmd, struct coo_cmd_response *out)
{
    struct mems_split_state state = {0};
    uint8_t channel_index;
    double requested[MEMS_SPLIT_OUTPUT_COUNT] = {0};
    double ratio3_probe = 0.0;
    uint32_t cycle_ms = 0U;
    uint32_t off_in_s = 0U;
    const char *failed_switch = NULL;
    bool has_cycle_ms = false;
    int parse_rc;
    int rc;

    rc = split_parse_channel(cmd, &channel_index);
    if (rc != 0) {
        return coo_cmd_error(out, cmd, "channel must be yj or hk");
    }

    parse_rc = coo_json_extract_double(cmd->payload, "ratio1", &requested[0]);
    if (parse_rc == COO_JSON_EXTRACT_MISSING) {
        return coo_cmd_error(out, cmd, "missing ratio1");
    }
    if (parse_rc == COO_JSON_EXTRACT_ERR) {
        return coo_cmd_error(out, cmd, "invalid ratio1");
    }

    parse_rc = coo_json_extract_double(cmd->payload, "ratio2", &requested[1]);
    if (parse_rc == COO_JSON_EXTRACT_MISSING) {
        return coo_cmd_error(out, cmd, "missing ratio2");
    }
    if (parse_rc == COO_JSON_EXTRACT_ERR) {
        return coo_cmd_error(out, cmd, "invalid ratio2");
    }

    parse_rc = coo_json_extract_double(cmd->payload, "ratio3", &ratio3_probe);
    if (parse_rc != COO_JSON_EXTRACT_MISSING) {
        return coo_cmd_error(out, cmd, "ratio3 is computed internally");
    }

    if (requested[0] < 0.0 || requested[0] > 1.0 ||
        requested[1] < 0.0 || requested[1] > 1.0 ||
        requested[0] + requested[1] > 1.000001) {
        return coo_cmd_error(out, cmd, "ratios must be 0.0-1.0 and sum <= 1.0");
    }
    requested[2] = 1.0 - requested[0] - requested[1];

    if (coo_json_extract_optional_u32(cmd->payload, "off_in_s",
                                      &off_in_s, NULL) != 0 ||
        off_in_s > MEMS_SWITCH_MAX_TOGGLE_DURATION_S) {
        return coo_cmd_error(out, cmd, "invalid off_in_s");
    }

    if (coo_json_extract_optional_u32(cmd->payload, "cycle_ms",
                                      &cycle_ms, &has_cycle_ms) != 0 ||
        (has_cycle_ms && cycle_ms == 0U)) {
        return coo_cmd_error(out, cmd, "invalid cycle_ms");
    }

    parse_rc = coo_json_extract_double(cmd->payload, "toggle_rate_hz", &ratio3_probe);
    if (parse_rc != COO_JSON_EXTRACT_MISSING) {
        return coo_cmd_error(out, cmd, "cycle_ms replaces toggle_rate_hz");
    }

    rc = mems_split_apply_channel(&router, channel_index, requested, cycle_ms, off_in_s,
                                  &state, &failed_switch);
    if (rc == -ENOENT) {
        return coo_cmd_error(out, cmd, "split route references missing switch");
    }
    if (rc != 0) {
        char payload[MAX_PAYLOAD_LEN];

        if (failed_switch != NULL) {
            snprintk(payload, sizeof(payload),
                     "{\"error\":\"failed setting %s\"}", failed_switch);
            return coo_cmd_reply(out, cmd, COO_CMD_RESP_ERROR, payload);
        }
        return coo_cmd_error(out, cmd, "split route unavailable");
    }

    split_emit_quantization_warning(channel_index, &state);
    if (has_cycle_ms && state.cycle_ms != cycle_ms) {
        const char *channel_name = mems_split_channel_name(channel_index);
        char context[96];

        snprintk(context, sizeof(context),
                 "channel=%s requested_cycle_ms=%u actual_cycle_ms=%u",
                 channel_name == NULL ? "?" : channel_name,
                 cycle_ms, state.cycle_ms);
		coo_cmd_runtime_emit(command_runtime_get(),
				     &(const struct coo_cmd_runtime_emit_args){
					     .type = COO_CMD_RUNTIME_EMIT_WARNING,
					     .delivery = COO_CMD_RUNTIME_EMIT_BEST_EFFORT,
					     .code = "mems_timing_quantized",
					     .msg = "requested MEMS cycle was quantized",
					     .context = context,
				     });
    }
    return split_channel_response(cmd, &state, channel_index, out);
}

int memsroute_set(const struct coo_cmd_request *cmd, struct coo_cmd_response *out)
{
    char input[MEMS_SOURCEDEST_MAX_LEN] = {0};
    char output[MEMS_SOURCEDEST_MAX_LEN] = {0};
    const struct mems_route *route;
    const char *failed_switch = NULL;
    char failed_state = '\0';
    bool force = false;
    int rc;

    if (memsroute_is_route_loss_key(cmd->key)) {
        return route_loss_handle(cmd, true, out);
    }

    if (coo_json_extract_string(cmd->payload, "input", input, sizeof(input)) !=
        COO_JSON_EXTRACT_OK ||
        coo_json_extract_string(cmd->payload, "output", output, sizeof(output)) !=
        COO_JSON_EXTRACT_OK) {
        return coo_cmd_error(out, cmd, "missing or invalid input or output");
    }
    if (coo_json_extract_optional_bool(cmd->payload, "force", &force, NULL) != 0) {
        return coo_cmd_error(out, cmd, "invalid force");
    }

    route = mems_router_get_route(&router, input, output);
    if (route == NULL) {
        return coo_cmd_error(out, cmd, "Invalid Route");
    }

    rc = mems_router_apply_route(&router, route, force, &failed_switch, &failed_state);
    if (rc == -ENOENT) {
        return coo_cmd_error(out, cmd, "route references missing switch");
    }
    if (rc != 0) {
        char payload[MAX_PAYLOAD_LEN] = {0};

        snprintk(payload, sizeof(payload),
                 "{\"error\":\"Setting switch %s to %c failed\"}",
                 failed_switch == NULL ? "unknown" : failed_switch,
                 failed_state == '\0' ? '?' : failed_state);
        return coo_cmd_reply(out, cmd, COO_CMD_RESP_ERROR, payload);
    }

    LOG_INF("Set route %s -> %s", input, output);
    return coo_cmd_ok(out, cmd);
}

static void mems_format_state(const struct mems_switch_status *status,
                              char *out,
                              size_t out_len)
{
    char state = status->state;

    if (status->cycle_ms != 0U) {
        if (status->a_ms == 0U) {
            state = 'B';
        } else if (status->b_ms == 0U) {
            state = 'A';
        }
    }

    if (state == 'A' || state == 'B') {
        snprintk(out, out_len, "%c", state);
        return;
    }

    snprintk(out, out_len, "?");
}

static bool mems_status_has_nonconstant_duty(const struct mems_switch_status *status)
{
    return status->cycle_ms != 0U && status->a_ms > 0U && status->b_ms > 0U;
}

static int mems_append_duty_field(char *buf, size_t buf_len, size_t *offset,
                                  const struct mems_switch_status *status)
{
    int written;

    written = snprintk(buf + *offset, buf_len - *offset,
                       ",\"duty_cycle\":%.6f",
                       (double)status->duty_cycle);
    if (written < 0 || written >= (int)(buf_len - *offset)) {
        return -ENOSPC;
    }

    *offset += (size_t)written;
    return 0;
}

/* Command responses include timing fields only while the requested MEMS profile
 * is mixed duty. Static A/B replies stay compact and unambiguous.
 */
static int mems_append_timing_fields(char *buf, size_t buf_len, size_t *offset,
                                     const struct mems_switch_status *status)
{
    int written;

    if (mems_append_duty_field(buf, buf_len, offset, status) != 0) {
        return -ENOSPC;
    }

    written = snprintk(buf + *offset, buf_len - *offset,
                       ",\"cycle_ms\":%u,\"a_ms\":%u,\"b_ms\":%u,\"stop_in_s\":%u",
                       status->cycle_ms,
                       status->a_ms,
                       status->b_ms,
                       status->stop_in_s);
    if (written < 0 || written >= (int)(buf_len - *offset)) {
        return -ENOSPC;
    }

    *offset += (size_t)written;
    return 0;
}

static int mems_response_for_switch(const struct coo_cmd_request *cmd,
				    const struct mems_switch *sw,
				    struct coo_cmd_response *out)
{
    struct mems_switch_status status = {0};
    char state_buf[4] = {0};
    char payload[MAX_PAYLOAD_LEN] = {0};
    size_t off = 0U;
    int written;

    mems_switch_get_status(sw, &status);
    mems_format_state(&status, state_buf, sizeof(state_buf));

    written = snprintk(payload + off, sizeof(payload) - off,
                       "{\"state\":\"%s\"", state_buf);
    if (written < 0 || written >= (int)(sizeof(payload) - off)) {
        return coo_cmd_error(out, cmd, "mems response too large");
    }
    off += (size_t)written;

    if (mems_status_has_nonconstant_duty(&status) &&
        mems_append_timing_fields(payload, sizeof(payload), &off, &status) != 0) {
        return coo_cmd_error(out, cmd, "mems response too large");
    }

    written = snprintk(payload + off, sizeof(payload) - off, "}");
    if (written < 0 || written >= (int)(sizeof(payload) - off)) {
        return coo_cmd_error(out, cmd, "mems response too large");
    }

    return coo_cmd_reply(out, cmd, COO_CMD_RESP_OK, payload);
}

int mems_get(const struct coo_cmd_request *cmd, struct coo_cmd_response *out)
{
    if (strcmp(cmd->key, "mems") == 0) {
        char payload[MAX_PAYLOAD_LEN] = {0};
        size_t off = 0U;
        int written;
        struct mems_switch_status status = {0};
        char state_buf[4] = {0};

        written = snprintk(payload + off, sizeof(payload) - off, "{");
        off += (size_t)written;

        for (uint8_t i = 0U; i < router.num_switches; ++i) {
            if (i > 0U) {
                written = snprintk(payload + off, sizeof(payload) - off, ",");
                if (written < 0 || written >= (int)(sizeof(payload) - off)) {
                    return coo_cmd_error(out, cmd,
                                          "mems response too large; query mems/<switchname>");
                }
                off += (size_t)written;
            }

            mems_switch_get_status(router.switches[i], &status);
            mems_format_state(&status, state_buf, sizeof(state_buf));
            written = snprintk(payload + off, sizeof(payload) - off,
                               "\"%s\":{\"state\":\"%s\"",
                               router.switches[i]->name,
                               state_buf);
            if (written < 0 || written >= (int)(sizeof(payload) - off)) {
                return coo_cmd_error(out, cmd,
                                      "mems response too large; query mems/<switchname>");
            }
            off += (size_t)written;

            if (mems_status_has_nonconstant_duty(&status) &&
                mems_append_duty_field(payload, sizeof(payload), &off, &status) != 0) {
                return coo_cmd_error(out, cmd,
                                      "mems response too large; query mems/<switchname>");
            }

            written = snprintk(payload + off, sizeof(payload) - off, "}");
            if (written < 0 || written >= (int)(sizeof(payload) - off)) {
                return coo_cmd_error(out, cmd,
                                      "mems response too large; query mems/<switchname>");
            }
            off += (size_t)written;
        }

        written = snprintk(payload + off, sizeof(payload) - off, "}");
        if (written < 0 || written >= (int)(sizeof(payload) - off)) {
            return coo_cmd_error(out, cmd, "mems response too large");
        }
        return coo_cmd_reply(out, cmd, COO_CMD_RESP_OK, payload);
    }

    char mems_switch[MEMS_SWITCH_NAME_LEN] = {0};
    if (coo_cmd_key_suffix_segment_copy(cmd->key, "mems", mems_switch,
                                        sizeof(mems_switch)) != 0) {
        return coo_cmd_error(out, cmd, "Failed to parse mems switch name");
    }

    struct mems_switch *sw = mems_router_find_switch(&router, mems_switch);

    if (sw == NULL) {
        return coo_cmd_error(out, cmd, "Invalid switch name");
    }

    return mems_response_for_switch(cmd, sw, out);
}

int mems_set(const struct coo_cmd_request *cmd, struct coo_cmd_response *out)
{
    char requested_state[8] = {0};
    char mems_switch[MEMS_SWITCH_NAME_LEN] = {0};
    double duty_cycle = 0.0;
    double off_in_s = 0.0;
    double removed_rate = 0.0;
    uint32_t cycle_ms = 0U;
    uint32_t off_in_s_u32 = 0U;
    bool has_duty_cycle = false;
    bool has_off_in_s = false;
    bool has_cycle_ms = false;
    bool force = false;
    int state_value;
    int parse_rc;
    int rc;

    if (coo_cmd_key_suffix_segment_copy(cmd->key, "mems", mems_switch,
                                        sizeof(mems_switch)) != 0) {
        return coo_cmd_error(out, cmd, "Failed to parse mems switch name");
    }

    parse_rc = coo_json_extract_string(cmd->payload, "state",
                                       requested_state, sizeof(requested_state));
    if (parse_rc == COO_JSON_EXTRACT_MISSING) {
        return coo_cmd_error(out, cmd, "Missing state");
    }
    if (parse_rc == COO_JSON_EXTRACT_ERR ||
        requested_state[0] == '\0' || requested_state[1] != '\0') {
        return coo_cmd_error(out, cmd, "state must be A or B");
    }

    if (coo_json_match_string_choice(requested_state, mems_switch_state_choices,
                                     ARRAY_SIZE(mems_switch_state_choices),
                                     &state_value) != 0) {
        return coo_cmd_error(out, cmd, "state must be A or B");
    }
    requested_state[0] = (char)state_value;

    parse_rc = coo_json_extract_double(cmd->payload, "duty_cycle", &duty_cycle);
    if (parse_rc == COO_JSON_EXTRACT_ERR) {
        return coo_cmd_error(out, cmd, "duty_cycle must be a number from 0.0 to 1.0");
    }
    has_duty_cycle = (parse_rc == COO_JSON_EXTRACT_OK);
    if (has_duty_cycle && (duty_cycle < 0.0 || duty_cycle > 1.0)) {
        return coo_cmd_error(out, cmd, "duty_cycle must be a number from 0.0 to 1.0");
    }

    parse_rc = coo_json_extract_double(cmd->payload, "off_in_s", &off_in_s);
    if (parse_rc == COO_JSON_EXTRACT_ERR) {
        return coo_cmd_error(out, cmd, "Invalid off_in_s");
    }
    has_off_in_s = (parse_rc == COO_JSON_EXTRACT_OK);

    if (coo_json_extract_optional_u32(cmd->payload, "cycle_ms",
                                      &cycle_ms, &has_cycle_ms) != 0 ||
        (has_cycle_ms && cycle_ms == 0U)) {
        return coo_cmd_error(out, cmd, "invalid cycle_ms");
    }
    if (coo_json_extract_optional_bool(cmd->payload, "force", &force, NULL) != 0) {
        return coo_cmd_error(out, cmd, "invalid force");
    }

    parse_rc = coo_json_extract_double(cmd->payload, "toggle_rate_hz", &removed_rate);
    if (parse_rc != COO_JSON_EXTRACT_MISSING) {
        return coo_cmd_error(out, cmd, "cycle_ms replaces toggle_rate_hz");
    }

    if (has_duty_cycle && requested_state[0] == 'B') {
        return coo_cmd_error(out, cmd, "duty_cycle only valid with state A");
    }
    if (has_cycle_ms && (!has_duty_cycle || duty_cycle <= 0.0 || duty_cycle >= 1.0)) {
        return coo_cmd_error(out, cmd, "cycle_ms only valid for toggling");
    }
    if (force && has_duty_cycle && duty_cycle > 0.0 && duty_cycle < 1.0) {
        return coo_cmd_error(out, cmd, "force only valid for static state");
    }
    if (has_off_in_s) {
        if (off_in_s < 0.0 ||
            off_in_s > (double)MEMS_SWITCH_MAX_TOGGLE_DURATION_S) {
            return coo_cmd_error(out, cmd, "off_in_s out of range");
        }
        off_in_s_u32 = (uint32_t)(off_in_s + 0.5);
        if (off_in_s_u32 == 0U && duty_cycle > 0.0 && duty_cycle < 1.0) {
            return coo_cmd_error(out, cmd, "off_in_s must be > 0 for toggling");
        }
    }

    struct mems_switch *sw = mems_router_find_switch(&router, mems_switch);

    if (sw == NULL) {
        return coo_cmd_error(out, cmd, "Invalid switch name");
    }

    if (has_duty_cycle) {
        rc = mems_switch_set_state(sw, requested_state[0], duty_cycle,
                                   off_in_s_u32,
                                   has_cycle_ms ? (double)cycle_ms : 0.0,
                                   force);
    } else {
        rc = mems_switch_set_state(sw, requested_state[0], 1.0, 0U,
                                   0.0, force);
    }

    if (rc == -ERANGE) {
        return coo_cmd_error(out, cmd, "MEMS setting out of range");
    }
    if (rc != 0) {
        return coo_cmd_error(out, cmd, "Invalid MEMS setting");
    }

    if (has_cycle_ms) {
        struct mems_switch_status status = {0};
        char context[96];

        mems_switch_get_status(sw, &status);
        if (status.cycle_ms != cycle_ms) {
            snprintk(context, sizeof(context),
                     "switch=%s requested_cycle_ms=%u actual_cycle_ms=%u",
                     sw->name, cycle_ms, status.cycle_ms);
			coo_cmd_runtime_emit(command_runtime_get(),
					     &(const struct coo_cmd_runtime_emit_args){
						     .type = COO_CMD_RUNTIME_EMIT_WARNING,
						     .delivery = COO_CMD_RUNTIME_EMIT_BEST_EFFORT,
						     .code = "mems_timing_quantized",
						     .msg = "requested MEMS cycle was quantized",
						     .context = context,
					     });
		}
	}

    return mems_response_for_switch(cmd, sw, out);
}
