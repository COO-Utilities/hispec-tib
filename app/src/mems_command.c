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

#include <zephyr/data/json.h>
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

static struct coo_cmd_response route_loss_query_response(const struct coo_cmd_request *cmd,
                                               const char *route)
{
    char payload[MAX_PAYLOAD_LEN] = {0};
    size_t offset = 0U;

    if (coo_json_append(payload, sizeof(payload), &offset,
                        "{\"route\":\"%s\",", route) != 0) {
        return coo_cmd_error(cmd, "route_loss response too large");
    }

    if (route_loss_route_is_split(route)) {
        if (coo_json_append(payload, sizeof(payload), &offset, "\"split\":[") != 0) {
            return coo_cmd_error(cmd, "route_loss response too large");
        }
        for (uint8_t i = 0U; i < MEMS_SPLIT_OUTPUT_COUNT; ++i) {
            const char *loss_key = mems_split_output_loss_key(i);
            double tx = 1.0;

            (void)app_settings_get_route_loss(route, loss_key, &tx);
            if (i > 0U &&
                coo_json_append(payload, sizeof(payload), &offset, ",") != 0) {
                return coo_cmd_error(cmd, "route_loss response too large");
            }
            if (route_loss_append_tx(payload, sizeof(payload), &offset, tx) != 0) {
                return coo_cmd_error(cmd, "route_loss response too large");
            }
        }
        if (coo_json_append(payload, sizeof(payload), &offset, "]}") != 0) {
            return coo_cmd_error(cmd, "route_loss response too large");
        }
        return coo_cmd_reply(cmd, COO_CMD_RESP_OK, payload);
    }

    if (coo_json_append(payload, sizeof(payload), &offset, "\"lasers\":{") != 0) {
        return coo_cmd_error(cmd, "route_loss response too large");
    }
    for (uint8_t i = 0U; i < ARRAY_SIZE(route_loss_laser_names); ++i) {
        double tx = 1.0;

        (void)app_settings_get_route_loss(route, route_loss_laser_names[i], &tx);
        if (i > 0U &&
            coo_json_append(payload, sizeof(payload), &offset, ",") != 0) {
            return coo_cmd_error(cmd, "route_loss response too large");
        }
        if (coo_json_append(payload, sizeof(payload), &offset, "\"%s\":",
                            route_loss_laser_names[i]) != 0 ||
            route_loss_append_tx(payload, sizeof(payload), &offset, tx) != 0) {
            return coo_cmd_error(cmd, "route_loss response too large");
        }
    }
    if (coo_json_append(payload, sizeof(payload), &offset, "}}") != 0) {
        return coo_cmd_error(cmd, "route_loss response too large");
    }

    return coo_cmd_reply(cmd, COO_CMD_RESP_OK, payload);
}

static struct coo_cmd_response route_loss_handle(const struct coo_cmd_request *cmd, bool set_request)
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
        return coo_cmd_error(cmd, "missing or invalid route");
    }

    parse_rc = coo_json_extract_bool(cmd->payload, "persistent", &persist);
    if (parse_rc == COO_JSON_EXTRACT_ERR) {
        return coo_cmd_error(cmd, "invalid persistent");
    }

    split_rc = route_loss_extract_split_tuple(cmd, split_tx);
    laser_rc = route_loss_extract_value(cmd, laser, sizeof(laser), &tx);

    if (!set_request) {
        if (split_rc != -ENOENT || laser_rc != -ENOENT ||
            parse_rc == COO_JSON_EXTRACT_OK) {
            return coo_cmd_error(cmd, "route_loss query uses route only");
        }
        return route_loss_query_response(cmd, route);
    }

    if (split_rc == 0 && laser_rc == 0) {
        return coo_cmd_error(cmd, "route_loss uses split or laser value");
    }
    if (split_rc == -ERANGE || laser_rc == -ERANGE) {
        return coo_cmd_error(cmd, "route_loss out of range");
    }
    if (split_rc != 0 && split_rc != -ENOENT) {
        return coo_cmd_error(cmd, "invalid split route_loss");
    }
    if (laser_rc != 0 && laser_rc != -ENOENT) {
        return coo_cmd_error(cmd, "invalid route_loss value");
    }

    if (split_rc == 0) {
        if (!route_loss_route_is_split(route)) {
            return coo_cmd_error(cmd, "route_loss split route invalid");
        }
        for (uint8_t i = 0U; i < MEMS_SPLIT_OUTPUT_COUNT; ++i) {
            const char *loss_key = mems_split_output_loss_key(i);

            parse_rc = app_settings_set_route_loss(route, loss_key, split_tx[i], persist);
            if (parse_rc == -ENOSPC) {
                return coo_cmd_error(cmd, "route_loss table full");
            }
            if (parse_rc != 0) {
                return coo_cmd_error(cmd, "invalid route_loss key");
            }
        }
        return coo_cmd_ok(cmd);
    }

    if (laser_rc == -ENOENT) {
        return coo_cmd_error(cmd, "missing route_loss value");
    }
    parse_rc = app_settings_set_route_loss(route, laser, tx, persist);
    if (parse_rc == -ENOSPC) {
        return coo_cmd_error(cmd, "route_loss table full");
    }
    if (parse_rc != 0) {
        return coo_cmd_error(cmd, "invalid route_loss key");
    }

    return coo_cmd_ok(cmd);
}

struct coo_cmd_response memsroute_get(const struct coo_cmd_request *cmd)
{
    struct mems_route_key active[MEMS_ROUTER_MAX_ROUTES];
    const char *outputs[MEMS_ROUTER_MAX_ROUTES];
    uint8_t n_active = mems_router_active_routes(&router, active, MEMS_ROUTER_MAX_ROUTES);
    uint8_t n_outputs = 0U;
    char buf[MAX_PAYLOAD_LEN] = {0};
    size_t offset = 0U;

    if (memsroute_is_route_loss_key(cmd->key)) {
        return route_loss_handle(cmd, false);
    }

    if (coo_json_append(buf, sizeof(buf), &offset, "{\"active_routes\":{") != 0) {
        return coo_cmd_error(cmd, "response too large");
    }

    for (uint8_t i = 0U; router.routes != NULL && i < router.num_routes; ++i) {
        const char *output_name = router.routes[i].key.output_name;

        if (memsroute_output_seen(outputs, n_outputs, output_name)) {
            continue;
        }

        outputs[n_outputs++] = output_name;
        if (n_outputs > 1U &&
            coo_json_append(buf, sizeof(buf), &offset, ",") != 0) {
            return coo_cmd_error(cmd, "response too large");
        }
        if (memsroute_append_sources_for_output(buf, sizeof(buf), &offset,
                                                active, n_active, output_name) != 0) {
            return coo_cmd_error(cmd, "response too large");
        }
    }

    if (coo_json_append(buf, sizeof(buf), &offset, "}}") != 0) {
        return coo_cmd_error(cmd, "response too large");
    }

    return coo_cmd_reply(cmd, COO_CMD_RESP_OK, buf);
}

static float split_abs_float(float value)
{
    return value < 0.0f ? -value : value;
}

static struct coo_cmd_response split_channel_response(const struct coo_cmd_request *cmd,
                                            const struct mems_split_state *state,
                                            uint8_t channel_index)
{
    const char *channel_name = mems_split_channel_name(channel_index);
    char payload[MAX_PAYLOAD_LEN];
    int written;

    if (state == NULL || channel_name == NULL) {
        return coo_cmd_error(cmd, "split route invalid");
    }

    written = snprintk(payload, sizeof(payload),
             "{\"channel\":\"%s\","
             "\"ratio_ask\":[%.4f,%.4f,%.4f],"
             "\"ratio_actual\":[%.4f,%.4f,%.4f],"
             "\"ratio_out\":[%.4f,%.4f,%.4f],"
             "\"split_transmission\":[%.6f,%.6f,%.6f],"
             "\"switches\":["
             "{\"name\":\"%s\",\"state\":\"%c\",\"duty_cycle\":%.4f,"
             "\"numerator\":%u,\"denominator\":%u,\"tick_ms\":%u},"
             "{\"name\":\"%s\",\"state\":\"%c\",\"duty_cycle\":%.4f,"
             "\"numerator\":%u,\"denominator\":%u,\"tick_ms\":%u},"
             "{\"name\":\"%s\",\"state\":\"%c\",\"duty_cycle\":%.4f,"
             "\"numerator\":%u,\"denominator\":%u,\"tick_ms\":%u}],"
             "\"stopsin_s\":%u}",
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
             state->switches[0].name,
             state->switches[0].state,
             (double)state->switches[0].duty_cycle,
             state->switches[0].numerator,
             state->switches[0].denominator,
             state->switches[0].tick_ms,
             state->switches[1].name,
             state->switches[1].state,
             (double)state->switches[1].duty_cycle,
             state->switches[1].numerator,
             state->switches[1].denominator,
             state->switches[1].tick_ms,
             state->switches[2].name,
             state->switches[2].state,
             (double)state->switches[2].duty_cycle,
             state->switches[2].numerator,
             state->switches[2].denominator,
             state->switches[2].tick_ms,
             state->stopsin_s);

    if (written < 0 || written >= (int)sizeof(payload)) {
        return coo_cmd_error(cmd, "split response too large");
    }

    return coo_cmd_reply(cmd, COO_CMD_RESP_OK, payload);
}

static void split_emit_quantization_warning(uint8_t channel_index,
                                            const struct mems_split_state *state)
{
    const char *channel_name = mems_split_channel_name(channel_index);
    char context[144];

    if (channel_name == NULL || state == NULL) {
        return;
    }

    if (split_abs_float(state->output[0] - state->requested[0]) <= 0.0005f &&
        split_abs_float(state->output[1] - state->requested[1]) <= 0.0005f &&
        split_abs_float(state->output[2] - state->requested[2]) <= 0.0005f) {
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
    coo_cmd_runtime_warning_emit(command_runtime_get(), "split_ratio_quantized",
                     "requested split ratio was quantized to MEMS ticks",
                     context);
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

struct coo_cmd_response splitting_get(const struct coo_cmd_request *cmd)
{
    struct mems_split_state state = {0};
    uint8_t channel_index;
    int rc;

    rc = split_parse_channel(cmd, &channel_index);
    if (rc != 0) {
        return coo_cmd_error(cmd, "channel required: yj or hk");
    }

    rc = mems_split_read_channel_state(&router, channel_index, NULL, &state);
    if (rc != 0) {
        return coo_cmd_error(cmd, "split route invalid");
    }

    return split_channel_response(cmd, &state, channel_index);
}

struct coo_cmd_response splitting_set(const struct coo_cmd_request *cmd)
{
    struct mems_split_state state = {0};
    uint8_t channel_index;
    float requested[MEMS_SPLIT_OUTPUT_COUNT] = {0};
    float ratio3_probe = 0.0f;
    uint32_t stopafter_s = 0U;
    const char *failed_switch = NULL;
    int parse_rc;
    int rc;

    rc = split_parse_channel(cmd, &channel_index);
    if (rc != 0) {
        return coo_cmd_error(cmd, "channel must be yj or hk");
    }

    parse_rc = coo_json_extract_float(cmd->payload, "ratio1", &requested[0]);
    if (parse_rc == COO_JSON_EXTRACT_MISSING) {
        return coo_cmd_error(cmd, "missing ratio1");
    }
    if (parse_rc == COO_JSON_EXTRACT_ERR) {
        return coo_cmd_error(cmd, "invalid ratio1");
    }

    parse_rc = coo_json_extract_float(cmd->payload, "ratio2", &requested[1]);
    if (parse_rc == COO_JSON_EXTRACT_MISSING) {
        return coo_cmd_error(cmd, "missing ratio2");
    }
    if (parse_rc == COO_JSON_EXTRACT_ERR) {
        return coo_cmd_error(cmd, "invalid ratio2");
    }

    parse_rc = coo_json_extract_float(cmd->payload, "ratio3", &ratio3_probe);
    if (parse_rc != COO_JSON_EXTRACT_MISSING) {
        return coo_cmd_error(cmd, "ratio3 is computed internally");
    }

    if (requested[0] < 0.0f || requested[0] > 1.0f ||
        requested[1] < 0.0f || requested[1] > 1.0f ||
        requested[0] + requested[1] > 1.000001f) {
        return coo_cmd_error(cmd, "ratios must be 0.0-1.0 and sum <= 1.0");
    }
    requested[2] = 1.0f - requested[0] - requested[1];

    if (coo_json_extract_optional_u32(cmd->payload, "stopafter_s",
                                      &stopafter_s, NULL) != 0 ||
        stopafter_s > MEMS_SWITCH_MAX_TOGGLE_DURATION_S) {
        return coo_cmd_error(cmd, "invalid stopafter_s");
    }

    parse_rc = coo_json_extract_float(cmd->payload, "toggle_rate_hz", &ratio3_probe);
    if (parse_rc != COO_JSON_EXTRACT_MISSING) {
        return coo_cmd_error(cmd, "toggle_rate_hz is automatic");
    }

    rc = mems_split_apply_channel(&router, channel_index, requested, stopafter_s,
                                  &state, &failed_switch);
    if (rc == -ENOENT) {
        return coo_cmd_error(cmd, "split route references missing switch");
    }
    if (rc != 0) {
        char payload[MAX_PAYLOAD_LEN];

        if (failed_switch != NULL) {
            snprintk(payload, sizeof(payload),
                     "{\"error\":\"failed setting %s\"}", failed_switch);
            return coo_cmd_reply(cmd, COO_CMD_RESP_ERROR, payload);
        }
        return coo_cmd_error(cmd, "split route unavailable");
    }

    split_emit_quantization_warning(channel_index, &state);
    return split_channel_response(cmd, &state, channel_index);
}

struct coo_cmd_response memsroute_set(const struct coo_cmd_request *cmd)
{
    struct mems_route_id route_id = {0};
    const struct mems_route *route;
    const char *failed_switch = NULL;
    char failed_state = '\0';
    struct json_obj_descr d[] = {
        JSON_OBJ_DESCR_PRIM(struct mems_route_id, input, JSON_TOK_STRING),
        JSON_OBJ_DESCR_PRIM(struct mems_route_id, output, JSON_TOK_STRING),
    };
    int rc;

    if (memsroute_is_route_loss_key(cmd->key)) {
        return route_loss_handle(cmd, true);
    }

    if (json_obj_parse((char *)cmd->payload, cmd->payload_len,
                       d, ARRAY_SIZE(d), &route_id) < 0) {
        return coo_cmd_error(cmd, "Failed to parse JSON input or output");
    }

    route = mems_router_get_route(&router, route_id.input, route_id.output);
    if (route == NULL) {
        return coo_cmd_error(cmd, "Invalid Route");
    }

    rc = mems_router_apply_route(&router, route, &failed_switch, &failed_state);
    if (rc == -ENOENT) {
        return coo_cmd_error(cmd, "route references missing switch");
    }
    if (rc != 0) {
        char payload[MAX_PAYLOAD_LEN] = {0};

        snprintk(payload, sizeof(payload),
                 "{\"error\":\"Setting switch %s to %c failed\"}",
                 failed_switch == NULL ? "unknown" : failed_switch,
                 failed_state == '\0' ? '?' : failed_state);
        return coo_cmd_reply(cmd, COO_CMD_RESP_ERROR, payload);
    }

    LOG_INF("Set route %s -> %s", route_id.input, route_id.output);
    return coo_cmd_ok(cmd);
}

static void mems_format_state(const struct mems_switch_status *status,
                              char *out,
                              size_t out_len)
{
    char state = status->state;

    if (status->duty_denominator != 0U) {
        if (status->duty_numerator == 0U) {
            state = 'B';
        } else if (status->duty_numerator >= status->duty_denominator) {
            state = 'A';
        }
    }

    if (state == 'A' || state == 'B') {
        snprintk(out, out_len, status->state_known_this_boot ? "%c" : "%c?",
                 state);
        return;
    }

    snprintk(out, out_len, "?");
}

static bool mems_status_has_nonconstant_duty(const struct mems_switch_status *status)
{
    return status->duty_denominator != 0U &&
           status->duty_numerator > 0U &&
           status->duty_numerator < status->duty_denominator;
}

static int mems_append_duty_field(char *buf, size_t buf_len, size_t *offset,
                                  const struct mems_switch_status *status)
{
    int written;

    written = snprintk(buf + *offset, buf_len - *offset,
                       ",\"duty_cycle\":%.3f",
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
                       ",\"requested_toggle_rate_hz\":%.3f,"
                       "\"toggle_rate_hz\":%.3f,\"stopafter_s\":%u",
                       (double)status->requested_toggle_rate_hz,
                       (double)status->toggle_rate_hz,
                       status->stopafter_s);
    if (written < 0 || written >= (int)(buf_len - *offset)) {
        return -ENOSPC;
    }

    *offset += (size_t)written;
    return 0;
}

static struct coo_cmd_response mems_response_for_switch(const struct coo_cmd_request *cmd,
                                              const struct mems_switch *sw)
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
        return coo_cmd_error(cmd, "mems response too large");
    }
    off += (size_t)written;

    if (mems_status_has_nonconstant_duty(&status) &&
        mems_append_timing_fields(payload, sizeof(payload), &off, &status) != 0) {
        return coo_cmd_error(cmd, "mems response too large");
    }

    written = snprintk(payload + off, sizeof(payload) - off, "}");
    if (written < 0 || written >= (int)(sizeof(payload) - off)) {
        return coo_cmd_error(cmd, "mems response too large");
    }

    return coo_cmd_reply(cmd, COO_CMD_RESP_OK, payload);
}

struct coo_cmd_response mems_get(const struct coo_cmd_request *cmd)
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
                    return coo_cmd_error(cmd,
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
                return coo_cmd_error(cmd,
                                      "mems response too large; query mems/<switchname>");
            }
            off += (size_t)written;

            if (mems_status_has_nonconstant_duty(&status) &&
                mems_append_duty_field(payload, sizeof(payload), &off, &status) != 0) {
                return coo_cmd_error(cmd,
                                      "mems response too large; query mems/<switchname>");
            }

            written = snprintk(payload + off, sizeof(payload) - off, "}");
            if (written < 0 || written >= (int)(sizeof(payload) - off)) {
                return coo_cmd_error(cmd,
                                      "mems response too large; query mems/<switchname>");
            }
            off += (size_t)written;
        }

        written = snprintk(payload + off, sizeof(payload) - off, "}");
        if (written < 0 || written >= (int)(sizeof(payload) - off)) {
            return coo_cmd_error(cmd, "mems response too large");
        }
        return coo_cmd_reply(cmd, COO_CMD_RESP_OK, payload);
    }

    char mems_switch[MEMS_SWITCH_NAME_LEN] = {0};
    if (coo_cmd_key_suffix_segment_copy(cmd->key, "mems", mems_switch,
                                        sizeof(mems_switch)) != 0) {
        return coo_cmd_error(cmd, "Failed to parse mems switch name");
    }

    struct mems_switch *sw = mems_router_find_switch(&router, mems_switch);

    if (sw == NULL) {
        return coo_cmd_error(cmd, "Invalid switch name");
    }

    return mems_response_for_switch(cmd, sw);
}

struct coo_cmd_response mems_set(const struct coo_cmd_request *cmd)
{
    char requested_state[8] = {0};
    char mems_switch[MEMS_SWITCH_NAME_LEN] = {0};
    float duty_cycle = 0.0f;
    float stopafter_s = 0.0f;
    float toggle_rate_hz = 0.0f;
    uint32_t stopafter_s_u32 = 0U;
    bool has_duty_cycle = false;
    bool has_stopafter_s = false;
    bool has_toggle_rate_hz = false;
    int state_value;
    int parse_rc;
    int rc;

    if (coo_cmd_key_suffix_segment_copy(cmd->key, "mems", mems_switch,
                                        sizeof(mems_switch)) != 0) {
        return coo_cmd_error(cmd, "Failed to parse mems switch name");
    }

    parse_rc = coo_json_extract_string(cmd->payload, "state",
                                       requested_state, sizeof(requested_state));
    if (parse_rc == COO_JSON_EXTRACT_MISSING) {
        return coo_cmd_error(cmd, "Missing state");
    }
    if (parse_rc == COO_JSON_EXTRACT_ERR ||
        requested_state[0] == '\0' || requested_state[1] != '\0') {
        return coo_cmd_error(cmd, "state must be A or B");
    }

    if (coo_json_match_string_choice(requested_state, mems_switch_state_choices,
                                     ARRAY_SIZE(mems_switch_state_choices),
                                     &state_value) != 0) {
        return coo_cmd_error(cmd, "state must be A or B");
    }
    requested_state[0] = (char)state_value;

    parse_rc = coo_json_extract_float(cmd->payload, "duty_cycle", &duty_cycle);
    if (parse_rc == COO_JSON_EXTRACT_ERR) {
        return coo_cmd_error(cmd, "duty_cycle must be a number from 0.0 to 1.0");
    }
    has_duty_cycle = (parse_rc == COO_JSON_EXTRACT_OK);
    if (has_duty_cycle && (duty_cycle < 0.0f || duty_cycle > 1.0f)) {
        return coo_cmd_error(cmd, "duty_cycle must be a number from 0.0 to 1.0");
    }

    parse_rc = coo_json_extract_float(cmd->payload, "stopafter_s", &stopafter_s);
    if (parse_rc == COO_JSON_EXTRACT_ERR) {
        return coo_cmd_error(cmd, "Invalid stopafter_s");
    }
    has_stopafter_s = (parse_rc == COO_JSON_EXTRACT_OK);

    parse_rc = coo_json_extract_float(cmd->payload, "toggle_rate_hz", &toggle_rate_hz);
    if (parse_rc == COO_JSON_EXTRACT_ERR) {
        return coo_cmd_error(cmd, "Invalid toggle_rate_hz");
    }
    has_toggle_rate_hz = (parse_rc == COO_JSON_EXTRACT_OK);
    if (has_toggle_rate_hz && toggle_rate_hz <= 0.0f) {
        return coo_cmd_error(cmd, "toggle_rate_hz must be > 0");
    }

    if (has_duty_cycle && requested_state[0] == 'B') {
        return coo_cmd_error(cmd, "duty_cycle only valid with state A");
    }
    if (has_stopafter_s) {
        if (stopafter_s < 0.0f ||
            stopafter_s > (float)MEMS_SWITCH_MAX_TOGGLE_DURATION_S) {
            return coo_cmd_error(cmd, "stopafter_s out of range");
        }
        stopafter_s_u32 = (uint32_t)(stopafter_s + 0.5f);
        if (stopafter_s_u32 == 0U && duty_cycle > 0.0f && duty_cycle < 1.0f) {
            return coo_cmd_error(cmd, "stopafter_s must be > 0 for toggling");
        }
    }

    struct mems_switch *sw = mems_router_find_switch(&router, mems_switch);

    if (sw == NULL) {
        return coo_cmd_error(cmd, "Invalid switch name");
    }

    if (has_duty_cycle) {
        rc = mems_switch_set_state(sw, requested_state[0], duty_cycle,
                                   stopafter_s_u32,
                                   has_toggle_rate_hz ? toggle_rate_hz : 0.0f);
    } else {
        rc = mems_switch_set_state(sw, requested_state[0], 1.0f, 0U,
                                   has_toggle_rate_hz ? toggle_rate_hz : 0.0f);
    }

    if (rc == -ERANGE) {
        return coo_cmd_error(cmd, "MEMS setting out of range");
    }
    if (rc != 0) {
        return coo_cmd_error(cmd, "Invalid MEMS setting");
    }

    if (has_toggle_rate_hz) {
        struct mems_switch_status status = {0};
        char context[96];
        float diff;

        mems_switch_get_status(sw, &status);
        diff = status.toggle_rate_hz - toggle_rate_hz;
        if (diff < 0.0f) {
            diff = -diff;
        }
        if (diff > 0.001f) {
            snprintk(context, sizeof(context),
                     "switch=%s requested=%.3f actual=%.3f",
                     sw->name, (double)toggle_rate_hz,
                     (double)status.toggle_rate_hz);
            coo_cmd_runtime_warning_emit(command_runtime_get(), "mems_rate_quantized",
                             "requested MEMS toggle rate was quantized",
                             context);
        }
    }

    return mems_response_for_switch(cmd, sw);
}
