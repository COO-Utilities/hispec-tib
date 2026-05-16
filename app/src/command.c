/**
 * @file command.c
 * @brief Command normalization, execution, and outbound response publication.
 *
 * The module owns the static command table and the two Zephyr message queues
 * that connect ingress, command execution, and MQTT/serial output. Hardware
 * side effects are still delegated to the domain modules where practical.
 */

#include "command.h"
// #include "devices.h"
#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <strings.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/sys/reboot.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/sys/util.h>
#include <app_version.h>
#include <time.h>
#include <zephyr/net/net_ip.h>
#include <zephyr/console/console.h>

#include "devices.h"
#include "laserbank_control.h"
#include "lasers.h"
#include "app_identity.h"
#include "app_settings.h"
#include "app_scheduled_actions.h"
#include "app_warning.h"
#include "attenuator.h"
#include "maiman.h"
#include "mems_switching.h"
#include "photodiode.h"
#include "throughput_monitor.h"
#if defined(CONFIG_SNTP)
#include "sntp_sync.h"
#endif
#include "tempsense.h"
#include <coo_commons/json_utils.h>
#include <coo_commons/mqtt_client.h>
#include <coo_commons/network.h>

LOG_MODULE_REGISTER(command, LOG_LEVEL_DBG);

#define SERIAL_LINE_MAX 220
#define SERIAL_WRAP_COLUMN 80U
#define LASERBANK_FAULT_CLEAR_OFF_MS 250U

static uint16_t mqtt_msg_id = 1;
static atomic_t serial_network_ignore_active;
static char last_command_name[MAX_KEY_LEN];
static char last_command_source[8] = "unknown";
static int64_t last_command_time_ms;

static void print_serial_response(const struct OutMsg *out);


/* MQTT and serial ingress use k_msgq so callbacks never execute hardware work.
 * Depth is intentionally small: clients should retry instead of letting stale
 * hardware commands pile up.
 */
K_MSGQ_DEFINE(inbound_queue,
              sizeof(struct Command),
              MAX_PENDING_COMMANDS,      /* depth */
              4);     /* 4‐byte align */

/* Responses, warnings, and telemetry leave the executor through this bounded
 * queue. The main loop owns MQTT publish retries and serial printing.
 */
K_MSGQ_DEFINE(outbound_queue,
              sizeof(struct OutMsg),
              8,
              4);

extern struct mems_switch mems_switches[MEMS_ROUTER_MAX_SWITCHES];
extern struct mems_router router;
// extern struct attenuator attenuators[NUM_ATTENUATORS];

static bool attenuator_channel_available(uint8_t attenuator_index)
{
    enum hispec_board_type board = devices_board_type();

    if (board == HISPEC_BOARD_TIB) {
        return attenuator_index < NUM_ATTENUATORS;
    }

    if (board == HISPEC_BOARD_CAL_YJ || board == HISPEC_BOARD_CAL_HK) {
        uint8_t cal_attenuator_index;

        return attenuator_index_from_laser_id(HISPEC_LASER_1510_H,
                                              &cal_attenuator_index) == 0 &&
               attenuator_index == cal_attenuator_index;
    }

    return false;
}




const struct DispatchEntry dispatch_table[] = {
    { "help",      help_get,         NULL             },
    { "ip",        ip_get,           ip_set           },
    { "mqtt",      mqtt_get,         mqtt_set         },
    { "time",      time_get,         time_set         },
    { "reboot",    NULL,             reboot_set       },
    { "serialguard", serial_guard_get, serial_guard_set },
    { "memsroute",  memsroute_get,    memsroute_set    },
    { "mems",       mems_get,         mems_set         },
    { "split",      splitting_get,    splitting_set    },
    { "measure_throughput", NULL, measure_throughput_set },
    { "laserbank/power", laserbank_power, laserbank_power },
    { "laserbank/clearfaults", laserbank_clearfaults, laserbank_clearfaults },
    { "laserbank/heater", laserbank_heater, laserbank_heater },
    { "laser/engstatus", laser_engstatus_get, NULL },
    { "laser/status", laser_status_get, NULL },
    { "laser/settings", laser_settings_get, laser_settings_set },
    { "laser/tune", laser_tune_get, laser_tune_set },
    { "laser",      laser_get, laser_set },
    { "atten",      atten_setting_get,  atten_setting_set  },
    { "pdsettings", pd_settings_get, pd_settings_set },
    { "pd",         pd_get,          pd_set          },
    { "temp",       temp_get,         NULL             },
    { "status",     status_get,       NULL  },
};


const struct DispatchEntry *find_dispatch(const char *key)
{
    const struct DispatchEntry *best = NULL;
    size_t best_len = 0;

    for (size_t i = 0; i < ARRAY_SIZE(dispatch_table); ++i) {
        const char *candidate = dispatch_table[i].key;
        size_t len = strlen(candidate);

        if (strncmp(key, candidate, len) != 0) {
            continue;
        }

        if (key[len] != '\0' && key[len] != '/') {
            continue;
        }

        if (len > best_len) {
            best = &dispatch_table[i];
            best_len = len;
        }
    }

    return best;
}

static bool command_pd_dark_status_query(const struct Command *cmd)
{
    char action[20] = {0};

    return cmd != NULL &&
           coo_json_extract_string(cmd->payload, "action",
                                   action, sizeof(action)) == COO_JSON_EXTRACT_OK &&
           strcasecmp(action, "dark_status") == 0;
}

static bool command_should_record_lastcommand(const struct Command *cmd)
{
    const struct DispatchEntry *entry;

    if (cmd == NULL) {
        return false;
    }

    entry = find_dispatch(cmd->key);
    if (entry == NULL) {
        return false;
    }

    if (strcmp(entry->key, "laserbank/clearfaults") == 0) {
        return true;
    }

    if (cmd->msg_type != MSG_SET || entry->set_handler == NULL) {
        return false;
    }

    if (strcmp(entry->key, "pd") == 0 && command_pd_dark_status_query(cmd)) {
        return false;
    }

    return true;
}

static void record_lastcommand(const struct Command *cmd)
{
    strncpy(last_command_name, cmd->key, sizeof(last_command_name) - 1);
    last_command_name[sizeof(last_command_name) - 1] = '\0';
    snprintk(last_command_source, sizeof(last_command_source), "%s",
             cmd->source == CMD_SRC_SERIAL ? "serial" : "mqtt");
    last_command_time_ms = k_uptime_get();
}


struct OutMsg dispatch_command(const struct Command *cmd) {
    LOG_INF("Dispatching: %s", cmd->key);
    struct OutMsg r;

    if (command_should_record_lastcommand(cmd)) {
        record_lastcommand(cmd);
    }

    const struct DispatchEntry *entry = find_dispatch(cmd->key);
    if (!entry) {
        r = unknown_response(cmd);
    } else {
        DispatchFunc func = (cmd->msg_type == MSG_SET) ? entry->set_handler : entry->get_handler;
        r = func==NULL ? unsupported_response(cmd) : func(cmd);
    }
    return r;
}


int parse_key_pair(const char *key,
                   char *out_name, size_t max_name,
                   char *out_setting, size_t max_setting)
{
    /* Find the first slash */
    const char *slash = strchr(key, '/');
    if (!slash) {
        return -1;
    }

    size_t name_len = slash - key;
    if (name_len == 0 || name_len >= max_name) {
        /* Name empty or too long for buffer (including null) */
        return -2;
    }

    /* Copy name */
    memcpy(out_name, key, name_len);
    out_name[name_len] = '\0';

    /* Copy setting, up to max_setting-1 characters, null terminated */
    const char *setting_start = slash + 1;
    size_t setting_len = strcspn(setting_start, "/"); /* Up to next '/', or full string */
    if (setting_len == 0 || setting_len >= max_setting) {
        /* Setting empty or too long for buffer */
        return -3;
    }
    memcpy(out_setting, setting_start, setting_len);
    out_setting[setting_len] = '\0';

    return 0;

}



static int parse_atten_key(const char *key,
                           char *laser_name, size_t laser_name_len,
                           char *setting, size_t setting_len)
{
    const char prefix[] = "atten/";
    const char *laser_start;
    const char *slash;
    size_t laser_len;
    size_t parsed_setting_len;

    if (key == NULL || laser_name == NULL || setting == NULL ||
        strncmp(key, prefix, strlen(prefix)) != 0) {
        return -EINVAL;
    }

    laser_start = key + strlen(prefix);
    slash = strchr(laser_start, '/');
    if (slash == NULL) {
        return -EINVAL;
    }

    laser_len = (size_t)(slash - laser_start);
    parsed_setting_len = strcspn(slash + 1, "/");
    if (laser_len == 0U || laser_len >= laser_name_len ||
        parsed_setting_len == 0U || parsed_setting_len >= setting_len ||
        (slash + 1)[parsed_setting_len] != '\0') {
        return -EINVAL;
    }

    memcpy(laser_name, laser_start, laser_len);
    laser_name[laser_len] = '\0';
    memcpy(setting, slash + 1, parsed_setting_len);
    setting[parsed_setting_len] = '\0';
    return 0;
}

struct OutMsg _msg_builder(const struct Command *cmd, enum MsgType msgtyp, const char *msg) {
    struct OutMsg r = { 0 };
    r.msg_type = msgtyp;
    r.target = (cmd && cmd->source == CMD_SRC_SERIAL) ? OUT_TARGET_SERIAL : OUT_TARGET_MQTT;
    r.qos = MQTT_QOS_1_AT_LEAST_ONCE;

    //        snprintf(r.payload, MAX_PAYLOAD_LEN, "{\"error\":\"Invalid route\"}");


    /* MQTT 5 response_topic is authoritative when supplied; otherwise the
     * firmware derives cmd/<device>/resp/<key> during ingress.
     */
    (void)app_mqtt_format_response_topic(cmd != NULL ? cmd->key : "",
                                         r.topic, sizeof(r.topic));
    if (cmd && strlen(cmd->response_topic) > 0 && strlen(cmd->response_topic) < sizeof(r.topic)) {
        strncpy(r.topic, cmd->response_topic, sizeof(r.topic) - 1);
    }

    /* MQTT 5 correlation_data is opaque requester state and must be echoed
     * exactly so clients can match command responses.
     */
    if (cmd && cmd->corr_len > 0 && cmd->corr_len <= sizeof(r.correlation_data)) {
        memcpy(r.correlation_data, cmd->correlation_data, cmd->corr_len);
        r.corr_len = cmd->corr_len;
    }

    if (msg != NULL && strlen(msg) >= sizeof(r.payload)) {
        static const char overflow_msg[] = "{\"error\":\"response too large\"}";

        r.msg_type = RESP_ERROR;
        snprintk(r.payload, sizeof(r.payload), "%s", overflow_msg);
        r.payload_len = strlen(r.payload);
        return r;
    }

    snprintk(r.payload, sizeof(r.payload), "%s", msg != NULL ? msg : "");
    r.payload_len = strlen(r.payload);
    return r;
}

static struct OutMsg ok_response(const struct Command *cmd)
{
    return _msg_builder(cmd, RESP_OK, "{\"status\":\"ok\"}");
}

static struct OutMsg error_response(const struct Command *cmd, const char *msg)
{
    char payload[MAX_PAYLOAD_LEN];

    snprintk(payload, sizeof(payload), "{\"error\":\"%s\"}", msg);
    return _msg_builder(cmd, RESP_ERROR, payload);
}

static struct OutMsg error_response_rc(const struct Command *cmd, const char *msg, int rc)
{
    char payload[MAX_PAYLOAD_LEN];

    snprintk(payload, sizeof(payload), "{\"error\":\"%s\",\"rc\":%d}", msg, rc);
    return _msg_builder(cmd, RESP_ERROR, payload);
}

static bool copy_topic(const struct mqtt_utf8 *topic, char *out, size_t out_len)
{
    if (topic == NULL || out == NULL || topic->size == 0U || topic->size >= out_len) {
        return false;
    }

    memcpy(out, topic->utf8, topic->size);
    out[topic->size] = '\0';
    return true;
}

static bool mqtt_get_allowed_during_serial_guard(const char *key)
{
    const struct DispatchEntry *entry = find_dispatch(key);

    if (entry == NULL || entry->get_handler == NULL) {
        return false;
    }

    /* Some legacy GET handlers currently have side effects. Keep those blocked
     * under serial guard until their command shape is corrected.
     */
    if (strncmp(entry->key, "laserbank/", strlen("laserbank/")) == 0 ||
        strcmp(entry->key, "laser") == 0) {
        return false;
    }

    return true;
}

static bool derive_default_response_topic(const char *key, char *topic_out, size_t topic_out_len)
{
    return app_mqtt_format_response_topic(key, topic_out, topic_out_len) == 0;
}

static bool command_payload_empty(const struct Command *cmd)
{
    return cmd == NULL || cmd->payload_len == 0U || strcmp(cmd->payload, "{}") == 0;
}

static enum MsgType command_infer_msg_type(const struct Command *cmd)
{
    float fval;
    char text[32];

    if (cmd == NULL) {
        return MSG_GET;
    }

    if (command_payload_empty(cmd)) {
        if (strcmp(cmd->key, "reboot") == 0 ||
            strcmp(cmd->key, "laserbank/clearfaults") == 0 ||
            strncmp(cmd->key, "laserbank/power/", strlen("laserbank/power/")) == 0 ||
            strncmp(cmd->key, "laserbank/heater/", strlen("laserbank/heater/")) == 0) {
            return MSG_SET;
        }
        return MSG_GET;
    }

    if (strcmp(cmd->key, "status") == 0 ||
        strcmp(cmd->key, "laser/status") == 0 ||
        strcmp(cmd->key, "laser/engstatus") == 0) {
        return MSG_GET;
    }

    if (strcmp(cmd->key, "memsroute/route_loss") == 0) {
        return coo_json_extract_string(cmd->payload, "laser",
                                       text, sizeof(text)) != COO_JSON_EXTRACT_MISSING ?
               MSG_GET : MSG_SET;
    }

    if (strcmp(cmd->key, "laser") == 0) {
        return coo_json_extract_float(cmd->payload, "level", &fval) != COO_JSON_EXTRACT_MISSING ?
               MSG_SET : MSG_GET;
    }

    if (strcmp(cmd->key, "laser/tune") == 0) {
        return coo_json_extract_float(cmd->payload, "tune_nm", &fval) != COO_JSON_EXTRACT_MISSING ||
               coo_json_extract_float(cmd->payload, "delta_nm", &fval) != COO_JSON_EXTRACT_MISSING ?
               MSG_SET : MSG_GET;
    }

    if (strcmp(cmd->key, "laser/settings") == 0) {
        char settings_json[MAX_PAYLOAD_LEN];

        return coo_json_extract_object(cmd->payload, "settings",
                                       settings_json, sizeof(settings_json)) != COO_JSON_EXTRACT_MISSING ?
               MSG_SET : MSG_GET;
    }

    return MSG_SET;
}

static void enqueue_serial_error(const char *msg)
{
    struct OutMsg out = {0};

    out.target = OUT_TARGET_SERIAL;
    out.msg_type = RESP_ERROR;
    (void)app_mqtt_format_response_topic("serial", out.topic, sizeof(out.topic));
    out.payload_len = snprintk(out.payload, sizeof(out.payload),
                               "{\"error\":\"%s\"}", msg);
    (void)k_msgq_put(&outbound_queue, &out, K_NO_WAIT);
}

static const char *skip_serial_space(const char *s)
{
    while (s != NULL && (*s == ' ' || *s == '\t')) {
        s++;
    }
    return s;
}

static bool next_serial_token(const char **cursor, char *out, size_t out_len)
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

static bool serial_token_has_extra(const char *cursor)
{
    cursor = skip_serial_space(cursor);
    return cursor != NULL && *cursor != '\0';
}

static bool serial_token_is_number(const char *token)
{
    char *end = NULL;

    if (token == NULL || token[0] == '\0') {
        return false;
    }

    (void)strtod(token, &end);
    return end != token && end != NULL && *end == '\0';
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

static int append_serial_json_value(char *out, size_t out_len, size_t *off,
                                    const char *token)
{
    const char *bool_json = serial_token_bool_json(token);
    int written;

    if (out == NULL || off == NULL || token == NULL) {
        return -EINVAL;
    }

    if (bool_json != NULL) {
        written = snprintk(out + *off, out_len - *off, "%s", bool_json);
    } else if (serial_token_is_number(token) || strcasecmp(token, "null") == 0) {
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

static int append_serial_json_field(char *out, size_t out_len, size_t *off,
                                    const char *key, const char *token,
                                    bool comma)
{
    int written;
    int rc;

    if (key == NULL || token == NULL || key[0] == '\0' ||
        strchr(key, '"') != NULL || strchr(key, '\\') != NULL) {
        return -EINVAL;
    }

    written = snprintk(out + *off, out_len - *off, "%s\"%s\":", comma ? "," : "", key);
    if (written < 0 || written >= (int)(out_len - *off)) {
        return -ENOSPC;
    }
    *off += (size_t)written;

    rc = append_serial_json_value(out, out_len, off, token);
    return rc;
}

/* Convert a serial payload like "state=A stopafter_s=30" into a compact JSON
 * object. This function does not validate command-specific meaning; handlers
 * still parse and validate the resulting JSON in the normal dispatch path.
 */
static int serial_payload_from_key_values(const char *payload, char *out, size_t out_len)
{
    const char *cursor = payload;
    char token[128];
    bool first = true;
    size_t off = 0;
    int written;

    written = snprintk(out, out_len, "{");
    if (written < 0 || written >= (int)out_len) {
        return -ENOSPC;
    }
    off = (size_t)written;

    while (next_serial_token(&cursor, token, sizeof(token))) {
        char *eq = strchr(token, '=');

        if (eq == NULL || eq == token || eq[1] == '\0') {
            return -EINVAL;
        }
        *eq = '\0';

        if (append_serial_json_field(out, out_len, &off, token, eq + 1, !first) != 0) {
            return -EINVAL;
        }
        first = false;
    }

    written = snprintk(out + off, out_len - off, "}");
    if (written < 0 || written >= (int)(out_len - off)) {
        return -ENOSPC;
    }
    return 0;
}

/* Convert a few common human serial shorthands into the same JSON payloads MQTT
 * uses. This is deliberately a small translation table, not another dispatcher.
 * Examples: "power on", "serialguard off", "mems/foo A 0.5 30".
 */
static int serial_payload_from_shorthand(const char *key, const char *payload,
                                         char *out, size_t out_len)
{
    const char *cursor = payload;
    char t0[96] = {0};
    char t1[96] = {0};
    char t2[96] = {0};
    size_t off = 0;
    int written;

    if (!next_serial_token(&cursor, t0, sizeof(t0))) {
        return -EINVAL;
    }
    (void)next_serial_token(&cursor, t1, sizeof(t1));
    (void)next_serial_token(&cursor, t2, sizeof(t2));
    if (serial_token_has_extra(cursor)) {
        return -EINVAL;
    }

    if (strncmp(key, "mems/", 5) == 0) {
        written = snprintk(out, out_len, "{\"state\":");
        if (written < 0 || written >= (int)out_len) {
            return -ENOSPC;
        }
        off = (size_t)written;
        if (append_serial_json_value(out, out_len, &off, t0) != 0) {
            return -EINVAL;
        }
        if (t1[0] != '\0' &&
            append_serial_json_field(out, out_len, &off, "duty_cycle", t1, true) != 0) {
            return -EINVAL;
        }
        if (t2[0] != '\0' &&
            append_serial_json_field(out, out_len, &off, "stopafter_s", t2, true) != 0) {
            return -EINVAL;
        }
        written = snprintk(out + off, out_len - off, "}");
        return (written < 0 || written >= (int)(out_len - off)) ? -ENOSPC : 0;
    }

    if (strcmp(key, "serialguard") == 0) {
        const char *seconds = (strcasecmp(t0, "off") == 0) ? "0" : t0;

        written = snprintk(out, out_len, "{\"seconds\":");
        if (written < 0 || written >= (int)out_len) {
            return -ENOSPC;
        }
        off = (size_t)written;
        if (append_serial_json_value(out, out_len, &off, seconds) != 0) {
            return -EINVAL;
        }
        if (t1[0] != '\0' &&
            append_serial_json_field(out, out_len, &off, "persistent", t1, true) != 0) {
            return -EINVAL;
        }
        written = snprintk(out + off, out_len - off, "}");
        return (written < 0 || written >= (int)(out_len - off)) ? -ENOSPC : 0;
    }

    if (strcmp(key, "mqtt") == 0) {
        written = snprintk(out, out_len, "{\"broker\":");
        if (written < 0 || written >= (int)out_len) {
            return -ENOSPC;
        }
        off = (size_t)written;
        if (append_serial_json_value(out, out_len, &off, t0) != 0) {
            return -EINVAL;
        }
        if (t1[0] != '\0' &&
            append_serial_json_field(out, out_len, &off, "persistent", t1, true) != 0) {
            return -EINVAL;
        }
        if (t2[0] != '\0') {
            return -EINVAL;
        }
        written = snprintk(out + off, out_len - off, "}");
        return (written < 0 || written >= (int)(out_len - off)) ? -ENOSPC : 0;
    }

    if (strcmp(key, "time") == 0) {
        if (!serial_token_is_number(t0)) {
            return -EINVAL;
        }
        written = snprintk(out, out_len, "{\"linuxtime_ms\":%s}", t0);
        return (written < 0 || written >= (int)out_len) ? -ENOSPC : 0;
    }

    if (t1[0] != '\0' || t2[0] != '\0') {
        return -EINVAL;
    }

    written = snprintk(out, out_len, "{\"value\":");
    if (written < 0 || written >= (int)out_len) {
        return -ENOSPC;
    }
    off = (size_t)written;
    if (append_serial_json_value(out, out_len, &off, t0) != 0) {
        return -EINVAL;
    }
    written = snprintk(out + off, out_len - off, "}");
    return (written < 0 || written >= (int)(out_len - off)) ? -ENOSPC : 0;
}

/* Top-level serial payload policy:
 * - no payload becomes "{}" and is dispatched as MSG_GET;
 * - raw JSON beginning with "{" is copied unchanged, not parsed or rebuilt;
 * - key=value tokens are wrapped into a JSON object;
 * - selected shorthands are translated by serial_payload_from_shorthand().
 */
static int normalize_serial_payload(const char *key, const char *payload,
                                    char *out, size_t out_len)
{
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

    return serial_payload_from_shorthand(key, payload, out, out_len);
}

void command_handle_mqtt_publish(const struct mqtt_publish_param *pub)
{
    struct Command cmd = {0};
    char req_topic[MAX_TOPIC_LEN];
    const char *suffix;
    char cmd_prefix[MAX_TOPIC_LEN];
    size_t prefix_len;
    size_t suffix_len;

    if (pub == NULL || !copy_topic(&pub->message.topic.topic, req_topic, sizeof(req_topic))) {
        return;
    }

    if (app_mqtt_format_request_prefix(cmd_prefix, sizeof(cmd_prefix)) != 0) {
        return;
    }
    prefix_len = strlen(cmd_prefix);
    if (strncmp(req_topic, cmd_prefix, prefix_len) != 0) {
        return;
    }

    suffix = req_topic + prefix_len;
    suffix_len = strlen(suffix);
    if (suffix_len == 0U || suffix_len >= sizeof(cmd.key)) {
        LOG_WRN("Invalid MQTT command topic suffix");
        return;
    }

    cmd.source = CMD_SRC_MQTT;
    memcpy(cmd.key, suffix, suffix_len);
    cmd.key[suffix_len] = '\0';

    if (pub->message.payload.len >= MAX_PAYLOAD_LEN) {
        struct OutMsg r = invalid_command_response(&cmd);
        (void)k_msgq_put(&outbound_queue, &r, K_NO_WAIT);
        return;
    }

    if (pub->message.payload.len > 0U) {
        memcpy(cmd.payload, pub->message.payload.data, pub->message.payload.len);
        cmd.payload[pub->message.payload.len] = '\0';
        cmd.payload_len = pub->message.payload.len;
    } else {
        snprintk(cmd.payload, sizeof(cmd.payload), "{}");
        cmd.payload_len = strlen(cmd.payload);
    }
    cmd.msg_type = command_infer_msg_type(&cmd);

    if (pub->prop.response_topic.utf8 != NULL &&
        pub->prop.response_topic.size > 0U &&
        pub->prop.response_topic.size < sizeof(cmd.response_topic)) {
        memcpy(cmd.response_topic, pub->prop.response_topic.utf8, pub->prop.response_topic.size);
        cmd.response_topic[pub->prop.response_topic.size] = '\0';
    } else if (!derive_default_response_topic(cmd.key, cmd.response_topic, sizeof(cmd.response_topic))) {
        struct OutMsg r = invalid_command_response(&cmd);
        (void)k_msgq_put(&outbound_queue, &r, K_NO_WAIT);
        return;
    }

    if (pub->prop.correlation_data.len > 0U &&
        pub->prop.correlation_data.len <= sizeof(cmd.correlation_data)) {
        memcpy(cmd.correlation_data,
               pub->prop.correlation_data.data,
               pub->prop.correlation_data.len);
        cmd.corr_len = pub->prop.correlation_data.len;
    } else if (pub->prop.correlation_data.len > sizeof(cmd.correlation_data)) {
        LOG_WRN("MQTT correlation_data too long (%zu > %zu); response will not echo it",
                pub->prop.correlation_data.len, sizeof(cmd.correlation_data));
    }

    if (!command_network_mqtt_allowed() &&
        (cmd.msg_type != MSG_GET || !mqtt_get_allowed_during_serial_guard(cmd.key))) {
        struct OutMsg r = serial_active_response(&cmd);

        LOG_WRN("Rejecting MQTT command '%s': local serial control is active", cmd.key);
        (void)k_msgq_put(&outbound_queue, &r, K_NO_WAIT);
        app_warning_emit("serial_guard_active",
                         "MQTT command rejected while serial command guard is active",
                         cmd.key);
        return;
    }

    if (k_msgq_put(&inbound_queue, &cmd, K_NO_WAIT) != 0) {
        struct OutMsg r = busy_response(&cmd);
        (void)k_msgq_put(&outbound_queue, &r, K_NO_WAIT);
    }
}

void command_serial_note_activity(void)
{
    const uint32_t holdoff_s = app_settings_get_serial_holdoff_s();
    int rc;

    if (holdoff_s == 0U) {
        (void)atomic_clear(&serial_network_ignore_active);
        (void)app_scheduled_action_cancel(APP_SCHEDULED_ACTION_SERIAL_GUARD_EXPIRE);
        return;
    }

    (void)atomic_set(&serial_network_ignore_active, 1);
    rc = app_scheduled_action_schedule(APP_SCHEDULED_ACTION_SERIAL_GUARD_EXPIRE,
                                       K_SECONDS(holdoff_s));
    if (rc < 0) {
        (void)atomic_clear(&serial_network_ignore_active);
        LOG_ERR("Failed to schedule serial guard expiration (%d)", rc);
    }
}

bool command_network_mqtt_allowed(void)
{
    return atomic_get(&serial_network_ignore_active) == 0;
}

void command_parse_serial_line(char *line)
{
    struct Command cmd = {0};
    char *cursor = line;
    char *key;
    char *payload = NULL;
    char *sep;

    while (*cursor == ' ' || *cursor == '\t') {
        cursor++;
    }

    if (*cursor == '\0') {
        return;
    }

    command_serial_note_activity();

    /* Serial syntax is one line: "<key> [payload]". Payload text is normalized
     * to JSON, then classified with the same documented request shapes as MQTT.
     */
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
        enqueue_serial_error("missing command key");
        return;
    }

    cmd.source = CMD_SRC_SERIAL;
    strncpy(cmd.key, key, sizeof(cmd.key) - 1);
    cmd.key[sizeof(cmd.key) - 1] = '\0';
    if (!derive_default_response_topic(cmd.key, cmd.response_topic, sizeof(cmd.response_topic))) {
        enqueue_serial_error("invalid command key");
        return;
    }

    if (normalize_serial_payload(cmd.key, payload, cmd.payload, sizeof(cmd.payload)) != 0) {
        enqueue_serial_error("invalid serial payload");
        return;
    }
    cmd.payload_len = strlen(cmd.payload);
    cmd.msg_type = command_infer_msg_type(&cmd);

    if (k_msgq_put(&inbound_queue, &cmd, K_NO_WAIT) != 0) {
        struct OutMsg r = busy_response(&cmd);
        (void)k_msgq_put(&outbound_queue, &r, K_NO_WAIT);
    }
}

void command_executor_thread(void *p1, void *p2, void *p3)
{
    struct Command cmd;
    struct OutMsg out;

    ARG_UNUSED(p1);
    ARG_UNUSED(p2);
    ARG_UNUSED(p3);

    while (1) {
        /* K_FOREVER sleeps this thread until ingress queues a complete command. */
        k_msgq_get(&inbound_queue, &cmd, K_FOREVER);
        out = dispatch_command(&cmd);
        if (k_msgq_put(&outbound_queue, &out, K_NO_WAIT) != 0) {
            LOG_WRN("Outbound queue full; dropping command response");
        }
    }
}

void command_serial_thread(void *p1, void *p2, void *p3)
{

    char *line;

    ARG_UNUSED(p1);
    ARG_UNUSED(p2);
    ARG_UNUSED(p3);

    /* Initialize Zephyr's line-oriented console input once for this thread. */
    console_getline_init();

    while (1) {
        /* Blocks until a full line is available from the configured console. */
        line = console_getline();
        if (line != NULL && line[0] != '\0') {
            command_parse_serial_line(line);
        }
    }

}

static int publish_outmsg(struct mqtt_client *client, const struct OutMsg *out)
{
    struct mqtt_publish_param param;

    if (client == NULL || out == NULL) {
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
    param.message_id = mqtt_msg_id++;
    param.dup_flag = 0U;
    param.retain_flag = 0U;

    /* mqtt_publish() may block in the socket layer and is kept out of timing
     * sensitive work items and sampler threads.
     */
    return mqtt_publish(client, &param);
}

static void build_outbound_queue_full_warning(struct OutMsg *out)
{
    if (out == NULL) {
        return;
    }

    memset(out, 0, sizeof(*out));
    out->msg_type = RESP_OK;
    out->target = OUT_TARGET_MQTT_BEST_EFFORT;
    out->qos = 0U;
    (void)app_mqtt_format_data_topic("warning", out->topic, sizeof(out->topic));
    snprintk(out->payload, sizeof(out->payload),
             "{\"severity\":\"warning\",\"code\":\"outbound_queue_full\","
             "\"msg\":\"outbound queue reached capacity\",\"context\":\"command_drain\","
             "\"uptime_ms\":%lld}",
             (long long)k_uptime_get());
    out->payload_len = strlen(out->payload);
}

static void publish_outbound_queue_full_warning(struct mqtt_client *client,
                                                bool mqtt_available)
{
    struct OutMsg warning;

    build_outbound_queue_full_warning(&warning);
    print_serial_response(&warning);

    if (mqtt_available && publish_outmsg(client, &warning) != 0) {
        LOG_WRN("Failed to publish outbound_queue_full warning");
    }
}

/* Serial responses intentionally reuse the OutMsg generated for MQTT. The
 * topic is printed first, then the payload is wrapped at print time with tab
 * indentation so response builders do not need serial-specific formatting.
 */
static void print_serial_response(const struct OutMsg *out)
{
    size_t len;
    uint16_t col = 0U;

    if (out == NULL) {
        return;
    }

    printk("%s\n\t", out->topic[0] != '\0' ? out->topic : "serial");
    col = 8U;
    len = out->payload_len > 0U ? out->payload_len : strlen(out->payload);

    for (size_t i = 0; i < len && out->payload[i] != '\0'; ++i) {
        const char ch = out->payload[i];

        if (ch == '\n' || col >= SERIAL_WRAP_COLUMN) {
            printk("\n\t");
            col = 8U;
            if (ch == '\n') {
                continue;
            }
        }

        printk("%c", ch);
        col++;

        if ((ch == ',' || ch == '}') && col >= (SERIAL_WRAP_COLUMN - 8U) &&
            i + 1U < len) {
            printk("\n\t");
            col = 8U;
        }
    }

    printk("\n");
}

void command_drain_outbound_queue(struct mqtt_client *client, bool mqtt_available)
{
    struct OutMsg out;
    int budget = 8;
    static bool full_warning_seen;
    static bool full_warning_mqtt_seen;
    bool outbound_full;

    outbound_full = (k_msgq_num_free_get(&outbound_queue) == 0U);
    if (outbound_full) {
        if (!full_warning_seen || (mqtt_available && !full_warning_mqtt_seen)) {
            publish_outbound_queue_full_warning(client, mqtt_available);
            full_warning_seen = true;
            if (mqtt_available) {
                full_warning_mqtt_seen = true;
            }
        }
    } else {
        full_warning_seen = false;
        full_warning_mqtt_seen = false;
    }

    while (budget-- > 0 && k_msgq_get(&outbound_queue, &out, K_NO_WAIT) == 0) {
        const bool best_effort = (out.target == OUT_TARGET_MQTT_BEST_EFFORT);

        if (out.target == OUT_TARGET_SERIAL) {
            print_serial_response(&out);
            continue;
        }

        if (!mqtt_available) {
            if (best_effort) {
                LOG_DBG("Dropping best-effort MQTT msg while MQTT unavailable");
                continue;
            }
            if (k_msgq_put(&outbound_queue, &out, K_NO_WAIT) != 0) {
                LOG_WRN("Dropping MQTT msg (queue full while requeueing)");
            }
            continue;
        }

        if (publish_outmsg(client, &out) != 0) {
            if (best_effort) {
                LOG_WRN("Best-effort MQTT publish failed; dropping msg");
                continue;
            }
            LOG_WRN("MQTT publish failed; will retry");
            if (k_msgq_put(&outbound_queue, &out, K_NO_WAIT) != 0) {
                LOG_WRN("Dropping MQTT msg (queue full after publish failure)");
            }
            break;
        }
    }
}

static bool power_enabled(void) {
    return hispec_laser_bank_power_is_enabled();
}

static const char *command_suffix_after(const struct Command *cmd, const char *prefix);

static bool parse_laserbank_power_mode_text(const char *text,
                                            enum hispec_laser_bank_power_mode *mode)
{
    if (text == NULL || mode == NULL) {
        return false;
    }
    if (strcasecmp(text, "auto") == 0) {
        *mode = HISPEC_LASER_BANK_POWER_AUTO;
        return true;
    }
    if (strcasecmp(text, "override_on") == 0 || strcasecmp(text, "on") == 0) {
        *mode = HISPEC_LASER_BANK_POWER_OVERRIDE_ON;
        return true;
    }
    if (strcasecmp(text, "override_off") == 0 || strcasecmp(text, "off") == 0) {
        *mode = HISPEC_LASER_BANK_POWER_OVERRIDE_OFF;
        return true;
    }
    return false;
}

static bool parse_laserbank_power_request(const struct Command *cmd,
                                          enum hispec_laser_bank_power_mode *mode)
{
    const char *suffix = command_suffix_after(cmd, "laserbank/power");
    char text[20] = {0};

    if (parse_laserbank_power_mode_text(suffix, mode)) {
        return true;
    }
    if (cmd == NULL || cmd->payload_len == 0U || strcmp(cmd->payload, "{}") == 0) {
        return false;
    }
    if (coo_json_extract_string(cmd->payload, "override", text, sizeof(text)) ==
        COO_JSON_EXTRACT_OK ||
        coo_json_extract_string(cmd->payload, "mode", text, sizeof(text)) ==
        COO_JSON_EXTRACT_OK) {
        return parse_laserbank_power_mode_text(text, mode);
    }
    return parse_laserbank_power_mode_text(cmd->payload, mode);
}

struct OutMsg laserbank_power(const struct Command *cmd)
{
    enum hispec_laser_bank_power_mode mode;
    char payload[MAX_PAYLOAD_LEN] = {0};
    int rc;

    if (devices_board_type() != HISPEC_BOARD_TIB) {
        return error_response(cmd, "laser bank unavailable on this board");
    }

    if (cmd != NULL &&
        (cmd->msg_type == MSG_SET ||
         command_suffix_after(cmd, "laserbank/power")[0] != '\0')) {
        if (!parse_laserbank_power_request(cmd, &mode)) {
            return error_response(cmd, "override must be auto, override_on, or override_off");
        }
        rc = hispec_laser_bank_power_mode_set(mode);
        if (rc != 0) {
            return error_response_rc(cmd, "laser bank power mode failed", rc);
        }
    }

    mode = hispec_laser_bank_power_mode_get();
    snprintk(payload, sizeof(payload),
             "{\"mode\":\"%s\",\"powered\":%s}",
             hispec_laser_bank_power_mode_name(mode),
             hispec_laser_bank_power_is_enabled() ? "true" : "false");
    return _msg_builder(cmd, RESP_OK, payload);
}

struct OutMsg laserbank_clearfaults(const struct Command *cmd)
{
    bool fault = false;
    uint32_t off_ms = 0U;
    char payload[MAX_PAYLOAD_LEN] = {0};
    int rc;

    if (devices_board_type() != HISPEC_BOARD_TIB) {
        return error_response(cmd, "laser bank unavailable on this board");
    }

    if (!power_enabled()) {
        return _msg_builder(cmd, RESP_OK, "{\"off_ms\":0}");
    }
    rc = hispec_laser_bank_any_overcurrent_fault(&fault);
    if (rc != 0) {
        return error_response_rc(cmd, "overcurrent status unavailable", rc);
    }
    if (!fault) {
        return _msg_builder(cmd, RESP_OK, "{\"off_ms\":0}");
    }
    if (hispec_laser_bank_clear_faults(LASERBANK_FAULT_CLEAR_OFF_MS) != 0) {
        return error_response(cmd, "laser bank power cycle failed");
    }
    off_ms = LASERBANK_FAULT_CLEAR_OFF_MS;

    if (!power_enabled()) {
        return error_response(cmd, "laser bank power cycle could not turn on");
    }

    snprintf(payload, sizeof(payload),
             "{\"off_ms\":%u}", off_ms);
    return _msg_builder(cmd, RESP_OK, payload);
}

static const char *command_suffix_after(const struct Command *cmd, const char *prefix)
{
    const char *suffix;
    size_t prefix_len;

    if (cmd == NULL || prefix == NULL) {
        return "";
    }

    prefix_len = strlen(prefix);
    if (strncmp(cmd->key, prefix, prefix_len) != 0) {
        return "";
    }

    suffix = cmd->key + prefix_len;
    return suffix[0] == '/' ? suffix + 1 : suffix;
}

static void laserbank_control_status_payload(char *payload, size_t payload_len)
{
    struct laserbank_control_status status = {0};

    laserbank_control_get_status(&status);
    snprintk(payload, payload_len,
             "{\"heater_mode\":\"%s\","
             "\"heater_on\":%s,\"bank_power\":%s,"
             "\"ambient_valid\":%s,\"ambient_c\":%.2f,"
             "\"valid_temps\":%u,\"stale_temps\":%u,"
             "\"any_disabled_below_15c\":%s,"
             "\"any_disabled_above_off_threshold\":%s,"
             "\"all_tecs_enabled\":%s,\"all_tecs_enabled_ms\":%u,"
             "\"last_error\":%d,\"last_poll_age_ms\":%u}",
             laserbank_heater_mode_name(status.heater_mode),
             status.heater_on ? "true" : "false",
             status.bank_powered ? "true" : "false",
             status.ambient_valid ? "true" : "false",
             (double)status.ambient_c,
             status.valid_temp_count,
             status.stale_temp_count,
             status.any_disabled_below_15c ? "true" : "false",
             status.any_disabled_above_off_threshold ? "true" : "false",
             status.all_tecs_enabled ? "true" : "false",
             status.all_tecs_enabled_ms,
             status.last_error,
             status.last_poll_age_ms);
}

static bool parse_heater_mode_text(const char *text,
                                   enum laserbank_heater_mode *mode)
{
    if (text == NULL || mode == NULL) {
        return false;
    }

    if (strcasecmp(text, "auto") == 0) {
        *mode = LASERBANK_HEATER_MODE_AUTO;
        return true;
    }
    if (strcasecmp(text, "override_on") == 0 ||
        strcasecmp(text, "overide_on") == 0 ||
        strcasecmp(text, "on") == 0) {
        *mode = LASERBANK_HEATER_MODE_OVERRIDE_ON;
        return true;
    }
    if (strcasecmp(text, "override_off") == 0 ||
        strcasecmp(text, "overide_off") == 0 ||
        strcasecmp(text, "off") == 0) {
        *mode = LASERBANK_HEATER_MODE_OVERRIDE_OFF;
        return true;
    }

    return false;
}

static bool parse_heater_request(const struct Command *cmd,
                                 enum laserbank_heater_mode *mode)
{
    const char *suffix = command_suffix_after(cmd, "laserbank/heater");
    char state[20] = {0};
    bool flag;
    int rc;

    if (parse_heater_mode_text(suffix, mode)) {
        return true;
    }

    if (cmd == NULL || cmd->payload_len == 0U || strcmp(cmd->payload, "{}") == 0) {
        return false;
    }

    if (coo_json_extract_string(cmd->payload, "override", state, sizeof(state)) ==
        COO_JSON_EXTRACT_OK ||
        coo_json_extract_string(cmd->payload, "state", state, sizeof(state)) ==
        COO_JSON_EXTRACT_OK) {
        return parse_heater_mode_text(state, mode);
    }

    rc = coo_json_extract_bool(cmd->payload, "override_on", &flag);
    if (rc == COO_JSON_EXTRACT_OK && flag) {
        *mode = LASERBANK_HEATER_MODE_OVERRIDE_ON;
        return true;
    }
    rc = coo_json_extract_bool(cmd->payload, "override_off", &flag);
    if (rc == COO_JSON_EXTRACT_OK && flag) {
        *mode = LASERBANK_HEATER_MODE_OVERRIDE_OFF;
        return true;
    }

    return parse_heater_mode_text(cmd->payload, mode);
}

struct OutMsg laserbank_heater(const struct Command *cmd)
{
    enum laserbank_heater_mode mode;
    char payload[MAX_PAYLOAD_LEN] = {0};

    if (devices_board_type() != HISPEC_BOARD_TIB) {
        return error_response(cmd, "laser bank unavailable on this board");
    }

    if (cmd != NULL &&
        (cmd->msg_type == MSG_SET ||
         command_suffix_after(cmd, "laserbank/heater")[0] != '\0')) {
        if (!parse_heater_request(cmd, &mode)) {
            return _msg_builder(cmd, RESP_ERROR,
                                "{\"error\":\"Use laserbank/heater auto|override_on|override_off\"}");
        }
        int rc = laserbank_control_set_heater_mode(mode, true);
        if (rc != 0) {
            return error_response_rc(cmd, "laser bank heater relay unavailable", rc);
        }
    }

    laserbank_control_status_payload(payload, sizeof(payload));
    return _msg_builder(cmd, RESP_OK, payload);
}

static void serial_guard_expire_handler(enum app_scheduled_action_id id, void *user_data)
{
    ARG_UNUSED(id);
    ARG_UNUSED(user_data);

    (void)atomic_clear(&serial_network_ignore_active);
    LOG_INF("Serial guard expired; MQTT command execution is enabled");
}

static void reboot_action_handler(enum app_scheduled_action_id id, void *user_data)
{
    ARG_UNUSED(id);
    ARG_UNUSED(user_data);

    sys_reboot(SYS_REBOOT_COLD);
}

int command_runtime_init(void)
{
    int rc;

    rc = app_scheduled_actions_init();
    if (rc != 0) {
        return rc;
    }

    rc = app_scheduled_action_register(APP_SCHEDULED_ACTION_SERIAL_GUARD_EXPIRE,
                                       serial_guard_expire_handler, NULL);
    if (rc != 0) {
        return rc;
    }

    return app_scheduled_action_register(APP_SCHEDULED_ACTION_REBOOT,
                                         reboot_action_handler, NULL);
}





/* COMMAND HANDLERS */


struct OutMsg invalid_command_response(const struct Command *cmd) {
    const char *err = "{\"error\":\"Invalid or unrecognized command\"}";
    return _msg_builder(cmd, RESP_ERROR, err);
}

struct OutMsg unknown_response(const struct Command *cmd) {
    const char *err = "{\"error\":\"Unknown request\"}";
    return _msg_builder(cmd, RESP_ERROR, err);
}

struct OutMsg unsupported_response(const struct Command *cmd) {
    const char *err = "{\"error\":\"Unsupported operation\"}";
    return _msg_builder(cmd, RESP_ERROR, err);
}

struct OutMsg busy_response(const struct Command *cmd) {
    const char *err = "{\"error\":\"busy\"}";
    return _msg_builder(cmd, RESP_ERROR,  err);
}

struct OutMsg serial_active_response(const struct Command *cmd) {
    const char *err = "{\"error\":\"try later. local serial commands active\"}";
    return _msg_builder(cmd, RESP_ERROR,  err);
}

struct OutMsg help_get(const struct Command *cmd)
{
    return _msg_builder(cmd, RESP_OK,
                        "{\"help\":\"help,ip,mqtt,time,temp,status,reboot,serialguard,"
                        "memsroute,mems,split,measure_throughput,laser,laserbank,"
                        "atten,pd,pdsettings\"}");
}

struct OutMsg ip_get(const struct Command *cmd)
{
    struct app_ip_settings ip_cfg;
    struct network_ipv4_info net = {0};
#if defined(CONFIG_SNTP)
    struct sntp_sync_status sntp = {0};
    const char *ntp_source;
    const char *ntp_server;
#else
    const char *ntp_source = "unsupported";
    const char *ntp_server = "";
#endif
    char payload[MAX_PAYLOAD_LEN];

    app_settings_get_ip(&ip_cfg);
    (void)network_get_ipv4_info(&net);
#if defined(CONFIG_SNTP)
    sntp_sync_get_status(&sntp);
    ntp_source = sntp_sync_source_str(sntp.source);
    ntp_server = sntp.server;
#endif

    snprintk(payload, sizeof(payload),
             "{\"source\":\"%s\",\"trydhcpfirst\":%s,"
             "\"preferdhcpdns\":%s,\"preferdhcpntp\":%s,"
             "\"manual\":{\"ip\":\"%s\",\"subnet\":\"%s\",\"gateway\":\"%s\",\"dns\":\"%s\",\"ntp\":\"%s\"},"
             "\"active\":{\"ready\":%s,\"ip\":\"%s\"},"
             "\"ntp\":{\"source\":\"%s\",\"server\":\"%s\"}}",
             network_ipv4_source_str(net.source),
             ip_cfg.try_dhcp_first ? "true" : "false",
             ip_cfg.prefer_dhcp_dns ? "true" : "false",
             ip_cfg.prefer_dhcp_ntp ? "true" : "false",
             ip_cfg.ip, ip_cfg.subnet, ip_cfg.gateway, ip_cfg.dns, ip_cfg.ntp,
             net.link_ready ? "true" : "false",
             net.ip,
             ntp_source,
             ntp_server);

    return _msg_builder(cmd, RESP_OK, payload);
}

static void network_config_from_app_ip(const struct app_ip_settings *ip_cfg,
                                       struct network_config *net_cfg)
{
#if defined(CONFIG_NET_DHCPV4)
    const bool dhcp_supported = true;
#else
    const bool dhcp_supported = false;
#endif

    if (ip_cfg == NULL || net_cfg == NULL) {
        return;
    }

    network_config_defaults(net_cfg);
    net_cfg->try_dhcp_first = ip_cfg->try_dhcp_first && dhcp_supported;
    net_cfg->prefer_dhcp_dns = ip_cfg->prefer_dhcp_dns;
    net_cfg->prefer_dhcp_ntp = ip_cfg->prefer_dhcp_ntp;

    strncpy(net_cfg->static_profile.ip, ip_cfg->ip,
            sizeof(net_cfg->static_profile.ip) - 1U);
    net_cfg->static_profile.ip[sizeof(net_cfg->static_profile.ip) - 1U] = '\0';
    strncpy(net_cfg->static_profile.subnet, ip_cfg->subnet,
            sizeof(net_cfg->static_profile.subnet) - 1U);
    net_cfg->static_profile.subnet[sizeof(net_cfg->static_profile.subnet) - 1U] = '\0';
    strncpy(net_cfg->static_profile.gateway, ip_cfg->gateway,
            sizeof(net_cfg->static_profile.gateway) - 1U);
    net_cfg->static_profile.gateway[sizeof(net_cfg->static_profile.gateway) - 1U] = '\0';

#if defined(CONFIG_DNS_RESOLVER)
    strncpy(net_cfg->static_profile.dns, ip_cfg->dns,
            sizeof(net_cfg->static_profile.dns) - 1U);
    net_cfg->static_profile.dns[sizeof(net_cfg->static_profile.dns) - 1U] = '\0';
#endif

#if defined(CONFIG_SNTP)
    strncpy(net_cfg->static_profile.ntp, ip_cfg->ntp,
            sizeof(net_cfg->static_profile.ntp) - 1U);
    net_cfg->static_profile.ntp[sizeof(net_cfg->static_profile.ntp) - 1U] = '\0';
#endif
}

struct OutMsg ip_set(const struct Command *cmd)
{
    struct app_ip_settings ip_cfg;
    char response[MAX_PAYLOAD_LEN];
#if defined(CONFIG_NET_DHCPV4)
    const bool dhcp_supported = true;
#else
    const bool dhcp_supported = false;
#endif
#if defined(CONFIG_DNS_RESOLVER)
    const bool dns_supported = true;
#else
    const bool dns_supported = false;
#endif
#if defined(CONFIG_SNTP)
    const bool ntp_supported = true;
#else
    const bool ntp_supported = false;
#endif
    bool persist = false;
    bool changed = false;
    bool network_changed = false;
    bool ntp_changed = false;
    bool unsupported_dhcp = false;
    bool unsupported_dns = false;
    bool unsupported_ntp = false;
    int parse_rc;
    char buf[NET_IPV4_ADDR_LEN];

    app_settings_get_ip(&ip_cfg);

    parse_rc = coo_json_extract_bool(cmd->payload, "trydhcpfirst", &ip_cfg.try_dhcp_first);
    if (!dhcp_supported) {
        if (parse_rc != COO_JSON_EXTRACT_MISSING) {
            unsupported_dhcp = true;
        }
    } else {
        if (parse_rc == COO_JSON_EXTRACT_OK) {
            changed = true;
            network_changed = true;
        } else if (parse_rc == COO_JSON_EXTRACT_ERR) {
            return error_response(cmd, "invalid trydhcpfirst");
        }
    }

    parse_rc = coo_json_extract_bool(cmd->payload, "preferdhcpdns", &ip_cfg.prefer_dhcp_dns);
    if (!dns_supported) {
        if (parse_rc != COO_JSON_EXTRACT_MISSING) {
            unsupported_dns = true;
        }
    } else {
        if (parse_rc == COO_JSON_EXTRACT_OK) {
            changed = true;
            network_changed = true;
        } else if (parse_rc == COO_JSON_EXTRACT_ERR) {
            return error_response(cmd, "invalid preferdhcpdns");
        }
    }

    parse_rc = coo_json_extract_bool(cmd->payload, "preferdhcpntp", &ip_cfg.prefer_dhcp_ntp);
    if (!ntp_supported) {
        if (parse_rc != COO_JSON_EXTRACT_MISSING) {
            unsupported_ntp = true;
        }
    } else {
        if (parse_rc == COO_JSON_EXTRACT_OK) {
            changed = true;
            ntp_changed = true;
        } else if (parse_rc == COO_JSON_EXTRACT_ERR) {
            return error_response(cmd, "invalid preferdhcpntp");
        }
    }

    parse_rc = coo_json_extract_string(cmd->payload, "ip", buf, sizeof(buf));
    if (parse_rc == COO_JSON_EXTRACT_OK) {
        strncpy(ip_cfg.ip, buf, sizeof(ip_cfg.ip) - 1);
        ip_cfg.ip[sizeof(ip_cfg.ip) - 1] = '\0';
        changed = true;
        network_changed = true;
    } else if (parse_rc == COO_JSON_EXTRACT_ERR) {
        return error_response(cmd, "invalid ip");
    }

    parse_rc = coo_json_extract_string(cmd->payload, "subnet", buf, sizeof(buf));
    if (parse_rc == COO_JSON_EXTRACT_OK) {
        strncpy(ip_cfg.subnet, buf, sizeof(ip_cfg.subnet) - 1);
        ip_cfg.subnet[sizeof(ip_cfg.subnet) - 1] = '\0';
        changed = true;
        network_changed = true;
    } else if (parse_rc == COO_JSON_EXTRACT_ERR) {
        return error_response(cmd, "invalid subnet");
    }

    parse_rc = coo_json_extract_string(cmd->payload, "gateway", buf, sizeof(buf));
    if (parse_rc == COO_JSON_EXTRACT_OK) {
        strncpy(ip_cfg.gateway, buf, sizeof(ip_cfg.gateway) - 1);
        ip_cfg.gateway[sizeof(ip_cfg.gateway) - 1] = '\0';
        changed = true;
        network_changed = true;
    } else if (parse_rc == COO_JSON_EXTRACT_ERR) {
        return error_response(cmd, "invalid gateway");
    }

    parse_rc = coo_json_extract_string(cmd->payload, "dns", buf, sizeof(buf));
    if (!dns_supported) {
        if (parse_rc != COO_JSON_EXTRACT_MISSING) {
            unsupported_dns = true;
        }
    } else {
        if (parse_rc == COO_JSON_EXTRACT_OK) {
            strncpy(ip_cfg.dns, buf, sizeof(ip_cfg.dns) - 1);
            ip_cfg.dns[sizeof(ip_cfg.dns) - 1] = '\0';
            changed = true;
            network_changed = true;
        } else if (parse_rc == COO_JSON_EXTRACT_ERR) {
            return error_response(cmd, "invalid dns");
        }
    }

    parse_rc = coo_json_extract_string(cmd->payload, "ntp", buf, sizeof(buf));
    if (!ntp_supported) {
        if (parse_rc != COO_JSON_EXTRACT_MISSING) {
            unsupported_ntp = true;
        }
    } else {
        if (parse_rc == COO_JSON_EXTRACT_OK) {
            strncpy(ip_cfg.ntp, buf, sizeof(ip_cfg.ntp) - 1);
            ip_cfg.ntp[sizeof(ip_cfg.ntp) - 1] = '\0';
            changed = true;
            ntp_changed = true;
        } else if (parse_rc == COO_JSON_EXTRACT_ERR) {
            return error_response(cmd, "invalid ntp");
        }
    }

    parse_rc = coo_json_extract_bool(cmd->payload, "persistent", &persist);
    if (parse_rc == COO_JSON_EXTRACT_ERR) {
        return error_response(cmd, "invalid persistent");
    }

    if (!changed && !(unsupported_dhcp || unsupported_dns || unsupported_ntp)) {
        return error_response(cmd, "no recognized ip fields");
    }

    if (network_changed) {
        struct network_config net_cfg;
        int rc;

        network_config_from_app_ip(&ip_cfg, &net_cfg);
        rc = network_reconfigure(&net_cfg);
        if (rc != 0) {
            return error_response_rc(cmd, "network reconfigure failed", rc);
        }
    }

    if (changed) {
        app_settings_update_ip(&ip_cfg, persist);
#if defined(CONFIG_SNTP)
        if (ntp_changed) {
            sntp_sync_schedule_now();
        }
#endif
    }

    if (unsupported_dhcp || unsupported_dns || unsupported_ntp) {
        snprintk(response, sizeof(response),
                 "{\"dhcp\":\"%s\",\"dns\":\"%s\",\"ntp\":\"%s\"}",
                 unsupported_dhcp ? "unsupported" : "ok",
                 unsupported_dns ? "unsupported" : "ok",
                 unsupported_ntp ? "unsupported" : "ok");
        return _msg_builder(cmd, RESP_OK, response);
    }

    return ok_response(cmd);
}

struct OutMsg mqtt_get(const struct Command *cmd)
{
    struct app_mqtt_settings mqtt_cfg = {0};
    struct coo_mqtt_broker_config broker_cfg = {0};
    char endpoint[160] = {0};
    char payload[MAX_PAYLOAD_LEN] = {0};
#if defined(CONFIG_DNS_RESOLVER)
    const bool dns_supported = true;
#else
    const bool dns_supported = false;
#endif

    app_settings_get_mqtt(&mqtt_cfg);
    strncpy(broker_cfg.host, mqtt_cfg.broker_host, sizeof(broker_cfg.host) - 1U);
    broker_cfg.host[sizeof(broker_cfg.host) - 1U] = '\0';
    broker_cfg.port = mqtt_cfg.broker_port;
    (void)coo_mqtt_format_broker_endpoint(&broker_cfg, endpoint, sizeof(endpoint));
    snprintk(payload, sizeof(payload),
             "{\"broker\":\"%s\",\"dns_supported\":%s}",
             endpoint,
             dns_supported ? "true" : "false");
    return _msg_builder(cmd, RESP_OK, payload);
}

struct OutMsg mqtt_set(const struct Command *cmd)
{
    struct app_mqtt_settings mqtt_cfg = {0};
    struct coo_mqtt_broker_config broker_cfg = {0};
    char endpoint[160] = {0};
    char resolved_ip[NET_IPV4_ADDR_LEN] = {0};
    bool persist = false;
    int parse_rc;
    int rc;

    app_settings_get_mqtt(&mqtt_cfg);

    parse_rc = coo_json_extract_string(cmd->payload, "broker", endpoint, sizeof(endpoint));
    if (parse_rc == COO_JSON_EXTRACT_MISSING) {
        return error_response(cmd, "missing broker");
    }
    if (parse_rc == COO_JSON_EXTRACT_ERR) {
        return error_response(cmd, "invalid broker");
    }
    if (!coo_mqtt_parse_broker_endpoint(endpoint, &broker_cfg)) {
        return error_response(cmd, "broker must be host-or-ip:port");
    }
    rc = coo_mqtt_resolve_broker_config(&broker_cfg, resolved_ip, sizeof(resolved_ip));
    if (rc == -ENOTSUP) {
        return error_response(cmd, "broker hostname requires DNS");
    }
    if (rc != 0) {
        return error_response(cmd, "broker host did not resolve");
    }
    strncpy(mqtt_cfg.broker_host, broker_cfg.host, sizeof(mqtt_cfg.broker_host) - 1U);
    mqtt_cfg.broker_host[sizeof(mqtt_cfg.broker_host) - 1U] = '\0';
    mqtt_cfg.broker_port = broker_cfg.port;

    parse_rc = coo_json_extract_bool(cmd->payload, "persistent", &persist);
    if (parse_rc == COO_JSON_EXTRACT_ERR) {
        return error_response(cmd, "invalid persistent");
    }

    app_settings_update_mqtt(&mqtt_cfg, persist);
    return ok_response(cmd);
}

struct OutMsg time_get(const struct Command *cmd)
{
    struct timespec ts = {0};
    uint64_t utc_ms;
    char payload[MAX_PAYLOAD_LEN];

    clock_gettime(CLOCK_REALTIME, &ts);
    utc_ms = ((uint64_t)ts.tv_sec * 1000ULL) + ((uint64_t)ts.tv_nsec / 1000000ULL);

    snprintk(payload, sizeof(payload),
             "{\"utc\":%llu,\"uptime\":%lld}",
             (unsigned long long)utc_ms, (long long)k_uptime_get());

    return _msg_builder(cmd, RESP_OK, payload);
}

struct OutMsg time_set(const struct Command *cmd)
{
    uint64_t utc_ms = 0;
    struct timespec ts = {0};
    int parse_rc;

    parse_rc = coo_json_extract_u64(cmd->payload, "linuxtime_ms", &utc_ms);
    if (parse_rc == COO_JSON_EXTRACT_MISSING) {
        return error_response(cmd, "missing linuxtime_ms");
    }
    if (parse_rc == COO_JSON_EXTRACT_ERR) {
        return error_response(cmd, "invalid linuxtime_ms");
    }

    ts.tv_sec = utc_ms / 1000ULL;
    ts.tv_nsec = (utc_ms % 1000ULL) * 1000000ULL;

    if (clock_settime(CLOCK_REALTIME, &ts) != 0) {
        return error_response(cmd, "clock_settime failed");
    }

    return ok_response(cmd);
}

struct OutMsg reboot_set(const struct Command *cmd)
{
    int rc;

    rc = app_scheduled_action_schedule(APP_SCHEDULED_ACTION_REBOOT, K_MSEC(250));
    if (rc < 0) {
        return error_response(cmd, "failed to schedule reboot");
    }

    return ok_response(cmd);
}

struct OutMsg serial_guard_get(const struct Command *cmd)
{
    char payload[MAX_PAYLOAD_LEN];
    int64_t remaining_ms = 0;

    (void)app_scheduled_action_remaining_ms(APP_SCHEDULED_ACTION_SERIAL_GUARD_EXPIRE,
                                            &remaining_ms);
    snprintk(payload, sizeof(payload),
             "{\"serialguard_s\":%u,\"active\":%s,\"remaining_ms\":%lld}",
             app_settings_get_serial_holdoff_s(),
             command_network_mqtt_allowed() ? "false" : "true",
             (long long)remaining_ms);
    return _msg_builder(cmd, RESP_OK, payload);
}

struct OutMsg serial_guard_set(const struct Command *cmd)
{
    uint32_t holdoff_s = 0;
    bool persist = false;
    int parse_rc_seconds;
    int parse_rc_value;
    int parse_rc_persist;

    parse_rc_seconds = coo_json_extract_u32(cmd->payload, "seconds", &holdoff_s);
    parse_rc_value = coo_json_extract_u32(cmd->payload, "value", &holdoff_s);
    if (parse_rc_seconds == COO_JSON_EXTRACT_ERR || parse_rc_value == COO_JSON_EXTRACT_ERR) {
        return error_response(cmd, "invalid seconds");
    }
    if (parse_rc_seconds == COO_JSON_EXTRACT_MISSING &&
        parse_rc_value == COO_JSON_EXTRACT_MISSING) {
        return error_response(cmd, "missing seconds");
    }

    parse_rc_persist = coo_json_extract_bool(cmd->payload, "persistent", &persist);
    if (parse_rc_persist == COO_JSON_EXTRACT_ERR) {
        return error_response(cmd, "invalid persistent");
    }
    app_settings_set_serial_holdoff_s(holdoff_s, persist);
    if (cmd->source == CMD_SRC_SERIAL) {
        command_serial_note_activity();
    }
    return ok_response(cmd);
}


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

static bool memsroute_is_route_loss_key(const char *key)
{
    return strcmp(key, "memsroute/route_loss") == 0;
}

static bool route_loss_laser_name_is_known(const char *laser)
{
    for (uint8_t i = 0U; i < ARRAY_SIZE(route_loss_laser_names); ++i) {
        if (strcmp(route_loss_laser_names[i], laser) == 0) {
            return true;
        }
    }

    return false;
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

static int route_loss_extract_value(const struct Command *cmd,
                                    char *laser, size_t laser_len,
                                    double *transmission)
{
    for (uint8_t i = 0U; i < ARRAY_SIZE(route_loss_laser_names); ++i) {
        const char *candidate = route_loss_laser_names[i];
        double tx = 0.0;
        char db_text[24] = {0};
        int rc;

        rc = coo_json_extract_double(cmd->payload, candidate, &tx);
        if (rc == COO_JSON_EXTRACT_OK) {
            if (!(tx > 0.0 && tx <= 1.0)) {
                return -ERANGE;
            }
            snprintk(laser, laser_len, "%s", candidate);
            *transmission = tx;
            return 0;
        }
        if (rc != COO_JSON_EXTRACT_MISSING &&
            coo_json_extract_string(cmd->payload, candidate, db_text, sizeof(db_text)) ==
            COO_JSON_EXTRACT_OK) {
            int parse_rc = route_loss_parse_db_string(db_text, &tx);

            if (parse_rc != 0) {
                return parse_rc;
            }
            snprintk(laser, laser_len, "%s", candidate);
            *transmission = tx;
            return 0;
        }
    }

    return -ENOENT;
}

static struct OutMsg route_loss_query_response(const struct Command *cmd,
                                               const char *route,
                                               const char *laser)
{
    char payload[MAX_PAYLOAD_LEN] = {0};
    double tx = 1.0;
    double loss_db = 0.0;
    bool configured = false;
    int rc;

    rc = app_settings_get_route_loss(route, laser, &tx, &configured);
    if (rc != 0) {
        return error_response(cmd, "invalid route_loss key");
    }

    loss_db = -10.0 * log10(tx);
    snprintk(payload, sizeof(payload),
             "{\"tx\":%.9f,\"loss_db\":%.6f,\"configured\":%s}",
             tx, loss_db, configured ? "true" : "false");
    return _msg_builder(cmd, RESP_OK, payload);
}

static struct OutMsg route_loss_handle(const struct Command *cmd, bool set_request)
{
    char route[APP_ROUTE_LOSS_ROUTE_MAX_LEN] = {0};
    char laser[APP_ROUTE_LOSS_LASER_MAX_LEN] = {0};
    double tx = 1.0;
    bool persist = false;
    int parse_rc;

    parse_rc = coo_json_extract_string(cmd->payload, "route", route, sizeof(route));
    if (parse_rc != COO_JSON_EXTRACT_OK) {
        return error_response(cmd, "missing or invalid route");
    }

    parse_rc = route_loss_extract_value(cmd, laser, sizeof(laser), &tx);
    if (parse_rc == -ENOENT) {
        parse_rc = coo_json_extract_string(cmd->payload, "laser", laser, sizeof(laser));
        if (parse_rc == COO_JSON_EXTRACT_OK) {
            if (!route_loss_laser_name_is_known(laser)) {
                return error_response(cmd, "invalid route_loss laser");
            }
            return route_loss_query_response(cmd, route, laser);
        }
        return error_response(cmd, "missing route_loss laser value");
    }
    if (!set_request) {
        return error_response(cmd, "route_loss query uses laser field");
    }
    if (parse_rc == -ERANGE) {
        return error_response(cmd, "route_loss out of range");
    }
    if (parse_rc != 0) {
        return error_response(cmd, "invalid route_loss value");
    }

    parse_rc = coo_json_extract_bool(cmd->payload, "persistent", &persist);
    if (parse_rc == COO_JSON_EXTRACT_ERR) {
        return error_response(cmd, "invalid persistent");
    }

    parse_rc = app_settings_set_route_loss(route, laser, tx, persist);
    if (parse_rc == -ENOSPC) {
        return error_response(cmd, "route_loss table full");
    }
    if (parse_rc != 0) {
        return error_response(cmd, "invalid route_loss key");
    }

    return ok_response(cmd);
}

struct OutMsg memsroute_get(const struct Command *cmd)
{
    struct mems_route_key active[MEMS_ROUTER_MAX_ROUTES];
    const char *outputs[MEMS_ROUTER_MAX_ROUTES];
    uint8_t n_active = mems_router_active_routes(&router, active, MEMS_ROUTER_MAX_ROUTES);
    uint8_t n_outputs = 0U;
    char buf[MAX_PAYLOAD_LEN] = {0};
    size_t offset = 0;

    if (memsroute_is_route_loss_key(cmd->key)) {
        return route_loss_handle(cmd, false);
    }

    if (coo_json_append(buf, sizeof(buf), &offset, "{\"active_routes\":{") != 0) {
        return error_response(cmd, "response too large");
    }

    for (uint8_t i = 0U; router.routes != NULL && i < router.num_routes; ++i) {
        const char *output_name = router.routes[i].key.output_name;

        if (memsroute_output_seen(outputs, n_outputs, output_name)) {
            continue;
        }

        outputs[n_outputs++] = output_name;
        if (n_outputs > 1U &&
            coo_json_append(buf, sizeof(buf), &offset, ",") != 0) {
            return error_response(cmd, "response too large");
        }
        if (memsroute_append_sources_for_output(buf, sizeof(buf), &offset,
                                                active, n_active, output_name) != 0) {
            return error_response(cmd, "response too large");
        }
    }

    if (coo_json_append(buf, sizeof(buf), &offset, "}}") != 0) {
        return error_response(cmd, "response too large");
    }

    return _msg_builder(cmd, RESP_OK, buf);
}

enum {
    SPLIT_CHANNEL_COUNT = 2,
    SPLIT_OUTPUT_COUNT = 3,
    SPLIT_ROUTE_SWITCH_COUNT = 3,
};

struct split_switch_duty {
    char name[MEMS_SWITCH_NAME_LEN];
    char state;
    float duty_cycle;
    uint32_t numerator;
    uint32_t denominator;
    uint32_t tick_ms;
};

struct split_state {
    float requested[SPLIT_OUTPUT_COUNT];
    float actual[SPLIT_OUTPUT_COUNT];
    struct split_switch_duty switches[SPLIT_ROUTE_SWITCH_COUNT];
    uint32_t stopsin_s;
};

static const char *split_channel_names[SPLIT_CHANNEL_COUNT] = {"yj", "hk"};
static const char *split_route_inputs[SPLIT_CHANNEL_COUNT] = {"yj_calin", "hk_calin"};
static const char *split_route_outputs[SPLIT_CHANNEL_COUNT] = {"yj_split", "hk_split"};

/* Command-level cache of each channel's last requested and measured split.
 * The MEMS router lock protects switch hardware state; this mutex only keeps
 * command responses coherent across serial/MQTT callers.
 */
static struct split_state g_split_state[SPLIT_CHANNEL_COUNT];
static K_MUTEX_DEFINE(split_state_lock);

static int split_channel_index(const char *channel, uint8_t *index)
{
    if (channel == NULL || index == NULL) {
        return -EINVAL;
    }

    for (uint8_t i = 0; i < SPLIT_CHANNEL_COUNT; ++i) {
        if (strcmp(channel, split_channel_names[i]) == 0) {
            *index = i;
            return 0;
        }
    }

    return -ENOENT;
}

static int split_channel_index_from_key(const char *key, uint8_t *index)
{
    const char *slash = strchr(key, '/');

    if (slash == NULL || slash[1] == '\0') {
        return -ENOENT;
    }

    return split_channel_index(slash + 1, index);
}

static const struct mems_route *split_route_for_channel(uint8_t channel_index)
{
    return mems_router_get_route(&router,
                                 split_route_inputs[channel_index],
                                 split_route_outputs[channel_index]);
}

static uint32_t split_period_ticks(void)
{
    const float ticks = 1000.0f /
                        (MEMS_SWITCH_MAX_TOGGLE_HZ *
                         (float)MEMS_SWITCH_ELECTRICAL_PULSE_MS);
    uint32_t period_ticks = (uint32_t)ticks;

    if ((float)period_ticks < ticks) {
        period_ticks += 1U;
    }
    if (period_ticks < 2U) {
        period_ticks = 2U;
    }

    return period_ticks;
}

static uint32_t split_ratio_to_ticks(float ratio, uint32_t period_ticks)
{
    uint32_t ticks = (uint32_t)(ratio * (float)period_ticks + 0.5f);

    return MIN(ticks, period_ticks);
}

static float split_abs_float(float value)
{
    return value < 0.0f ? -value : value;
}

static uint32_t split_selected_numerator(const struct mems_switch_status *status,
                                         char state)
{
    if (state == 'A') {
        return status->duty_numerator;
    }

    return status->duty_denominator - status->duty_numerator;
}

static void split_clamp_actual(float actual[SPLIT_OUTPUT_COUNT])
{
    float used;

    for (uint8_t i = 0; i < SPLIT_OUTPUT_COUNT; ++i) {
        if (actual[i] < 0.0f) {
            actual[i] = 0.0f;
        }
        if (actual[i] > 1.0f) {
            actual[i] = 1.0f;
        }
    }

    used = actual[0] + actual[1];
    if (used > 1.0f) {
        actual[1] = 1.0f - actual[0];
        used = 1.0f;
    }
    actual[2] = 1.0f - used;
}

static int split_read_channel_state(uint8_t channel_index,
                                    const struct mems_route *route,
                                    const float requested[SPLIT_OUTPUT_COUNT])
{
    struct split_state next = {0};
    float sw1_duty;
    float sw3_duty;

    if (route == NULL || route->num_steps != SPLIT_ROUTE_SWITCH_COUNT) {
        return -EINVAL;
    }

    if (requested != NULL) {
        memcpy(next.requested, requested, sizeof(next.requested));
    } else {
        k_mutex_lock(&split_state_lock, K_FOREVER);
        next = g_split_state[channel_index];
        k_mutex_unlock(&split_state_lock);
    }

    for (uint8_t i = 0; i < SPLIT_ROUTE_SWITCH_COUNT; ++i) {
        const struct mems_route_step *step = &route->steps[i];
        struct mems_switch *sw = mems_router_find_switch(&router, step->switch_name);
        struct mems_switch_status status = {0};
        uint32_t selected_ticks;

        if (sw == NULL) {
            LOG_ERR("Split route %s->%s references missing switch %s",
                    route->key.input_name, route->key.output_name,
                    step->switch_name);
            return -EINVAL;
        }

        mems_switch_get_status(sw, &status);
        selected_ticks = split_selected_numerator(&status, step->state);

        snprintk(next.switches[i].name, sizeof(next.switches[i].name),
                 "%s", step->switch_name);
        next.switches[i].state = step->state;
        next.switches[i].numerator = selected_ticks;
        next.switches[i].denominator = status.duty_denominator;
        next.switches[i].tick_ms = status.tick_duration_ms;
        next.switches[i].duty_cycle =
            status.duty_denominator == 0U ? 0.0f :
            (float)selected_ticks / (float)status.duty_denominator;
        next.stopsin_s = MAX(next.stopsin_s, status.stopafter_s);
    }

    sw1_duty = next.switches[0].duty_cycle;
    sw3_duty = next.switches[2].duty_cycle;
    next.actual[0] = sw1_duty;
    next.actual[1] = sw3_duty > sw1_duty ? sw3_duty - sw1_duty : 0.0f;
    split_clamp_actual(next.actual);

    k_mutex_lock(&split_state_lock, K_FOREVER);
    g_split_state[channel_index] = next;
    k_mutex_unlock(&split_state_lock);

    LOG_INF("Split %s actual %.4f %.4f %.4f",
            split_channel_names[channel_index],
            (double)next.actual[0],
            (double)next.actual[1],
            (double)next.actual[2]);

    return 0;
}

static struct OutMsg split_channel_response(const struct Command *cmd,
                                            uint8_t channel_index)
{
    struct split_state state;
    char payload[MAX_PAYLOAD_LEN];
    int written;

    k_mutex_lock(&split_state_lock, K_FOREVER);
    state = g_split_state[channel_index];
    k_mutex_unlock(&split_state_lock);

    written = snprintk(payload, sizeof(payload),
             "{\"channel\":\"%s\","
             "\"requested_ratio\":[%.4f,%.4f,%.4f],"
             "\"actual_ratio\":[%.4f,%.4f,%.4f],"
             "\"switches\":["
             "{\"name\":\"%s\",\"state\":\"%c\",\"duty_cycle\":%.4f,"
             "\"numerator\":%u,\"denominator\":%u,\"tick_ms\":%u},"
             "{\"name\":\"%s\",\"state\":\"%c\",\"duty_cycle\":%.4f,"
             "\"numerator\":%u,\"denominator\":%u,\"tick_ms\":%u},"
             "{\"name\":\"%s\",\"state\":\"%c\",\"duty_cycle\":%.4f,"
             "\"numerator\":%u,\"denominator\":%u,\"tick_ms\":%u}],"
             "\"stopsin_s\":%u}",
             split_channel_names[channel_index],
             (double)state.requested[0],
             (double)state.requested[1],
             (double)state.requested[2],
             (double)state.actual[0],
             (double)state.actual[1],
             (double)state.actual[2],
             state.switches[0].name,
             state.switches[0].state,
             (double)state.switches[0].duty_cycle,
             state.switches[0].numerator,
             state.switches[0].denominator,
             state.switches[0].tick_ms,
             state.switches[1].name,
             state.switches[1].state,
             (double)state.switches[1].duty_cycle,
             state.switches[1].numerator,
             state.switches[1].denominator,
             state.switches[1].tick_ms,
             state.switches[2].name,
             state.switches[2].state,
             (double)state.switches[2].duty_cycle,
             state.switches[2].numerator,
             state.switches[2].denominator,
             state.switches[2].tick_ms,
             state.stopsin_s);

    if (written < 0 || written >= (int)sizeof(payload)) {
        return error_response(cmd, "split response too large");
    }

    return _msg_builder(cmd, RESP_OK, payload);
}

static void split_emit_quantization_warning(uint8_t channel_index,
                                            const struct split_state *state)
{
    char context[144];

    if (split_abs_float(state->actual[0] - state->requested[0]) <= 0.0005f &&
        split_abs_float(state->actual[1] - state->requested[1]) <= 0.0005f &&
        split_abs_float(state->actual[2] - state->requested[2]) <= 0.0005f) {
        return;
    }

    snprintk(context, sizeof(context),
             "channel=%s requested=%.4f/%.4f/%.4f actual=%.4f/%.4f/%.4f",
             split_channel_names[channel_index],
             (double)state->requested[0],
             (double)state->requested[1],
             (double)state->requested[2],
             (double)state->actual[0],
             (double)state->actual[1],
             (double)state->actual[2]);
    app_warning_emit("split_ratio_quantized",
                     "requested split ratio was quantized to MEMS ticks",
                     context);
}

static int split_parse_channel(const struct Command *cmd, uint8_t *channel_index)
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

    return split_channel_index(channel, channel_index);
}

struct OutMsg splitting_get(const struct Command *cmd)
{
    uint8_t channel_index;
    const struct mems_route *route;
    int rc;

    rc = split_parse_channel(cmd, &channel_index);
    if (rc != 0) {
        return error_response(cmd, "channel required: yj or hk");
    }

    route = split_route_for_channel(channel_index);
    if (route == NULL) {
        return error_response(cmd, "split route unavailable");
    }

    rc = split_read_channel_state(channel_index, route, NULL);
    if (rc != 0) {
        return error_response(cmd, "split route invalid");
    }

    return split_channel_response(cmd, channel_index);
}

/** Apply one AS-PCB split route.
 *
 * The user provides ratio1 and ratio2 for one channel. ratio3 is the remaining
 * fraction. SW3 is intentionally held through SW1's output-1 interval, so its
 * selected-state numerator is ratio1 + ratio2 rather than ratio2 alone.
 */
struct OutMsg splitting_set(const struct Command *cmd)
{
    uint8_t channel_index;
    const struct mems_route *route;
    struct split_state stored;
    float requested[SPLIT_OUTPUT_COUNT] = {0};
    float ratio3_probe = 0.0f;
    uint32_t stopafter_s = 0U;
    uint32_t period_ticks;
    uint32_t output_ticks[SPLIT_OUTPUT_COUNT];
    uint32_t switch_ticks[SPLIT_ROUTE_SWITCH_COUNT];
    int parse_rc;
    int rc;

    rc = split_parse_channel(cmd, &channel_index);
    if (rc != 0) {
        return error_response(cmd, "channel must be yj or hk");
    }

    parse_rc = coo_json_extract_float(cmd->payload, "ratio1", &requested[0]);
    if (parse_rc == COO_JSON_EXTRACT_MISSING) {
        return error_response(cmd, "missing ratio1");
    }
    if (parse_rc == COO_JSON_EXTRACT_ERR) {
        return error_response(cmd, "invalid ratio1");
    }

    parse_rc = coo_json_extract_float(cmd->payload, "ratio2", &requested[1]);
    if (parse_rc == COO_JSON_EXTRACT_MISSING) {
        return error_response(cmd, "missing ratio2");
    }
    if (parse_rc == COO_JSON_EXTRACT_ERR) {
        return error_response(cmd, "invalid ratio2");
    }

    parse_rc = coo_json_extract_float(cmd->payload, "ratio3", &ratio3_probe);
    if (parse_rc != COO_JSON_EXTRACT_MISSING) {
        return error_response(cmd, "ratio3 is computed internally");
    }

    if (requested[0] < 0.0f || requested[0] > 1.0f ||
        requested[1] < 0.0f || requested[1] > 1.0f ||
        requested[0] + requested[1] > 1.000001f) {
        return error_response(cmd, "ratios must be 0.0-1.0 and sum <= 1.0");
    }
    requested[2] = 1.0f - requested[0] - requested[1];

    parse_rc = coo_json_extract_u32(cmd->payload, "stopafter_s", &stopafter_s);
    if (parse_rc == COO_JSON_EXTRACT_ERR ||
        stopafter_s > MEMS_SWITCH_MAX_TOGGLE_DURATION_S) {
        return error_response(cmd, "invalid stopafter_s");
    }

    parse_rc = coo_json_extract_float(cmd->payload, "toggle_rate_hz", &ratio3_probe);
    if (parse_rc != COO_JSON_EXTRACT_MISSING) {
        return error_response(cmd, "toggle_rate_hz is automatic");
    }

    route = split_route_for_channel(channel_index);
    if (route == NULL || route->num_steps != SPLIT_ROUTE_SWITCH_COUNT) {
        return error_response(cmd, "split route unavailable");
    }

    period_ticks = split_period_ticks();
    output_ticks[0] = split_ratio_to_ticks(requested[0], period_ticks);
    output_ticks[1] = split_ratio_to_ticks(requested[1], period_ticks);
    if (output_ticks[0] + output_ticks[1] > period_ticks) {
        output_ticks[1] = period_ticks - output_ticks[0];
    }
    output_ticks[2] = period_ticks - output_ticks[0] - output_ticks[1];

    switch_ticks[0] = output_ticks[0];
    switch_ticks[1] = period_ticks;
    switch_ticks[2] = output_ticks[0] + output_ticks[1];

    for (uint8_t i = 0; i < SPLIT_ROUTE_SWITCH_COUNT; ++i) {
        const struct mems_route_step *step = &route->steps[i];
        struct mems_switch *sw = mems_router_find_switch(&router, step->switch_name);

        if (sw == NULL) {
            return error_response(cmd, "split route references missing switch");
        }

        rc = mems_switch_set_state_ticks(sw, step->state, switch_ticks[i],
                                         period_ticks,
                                         i == 1U ? 0U : stopafter_s);
        if (rc != 0) {
            char payload[MAX_PAYLOAD_LEN];

            snprintk(payload, sizeof(payload),
                     "{\"error\":\"failed setting %s\"}",
                     step->switch_name);
            return _msg_builder(cmd, RESP_ERROR, payload);
        }
    }

    rc = split_read_channel_state(channel_index, route, requested);
    if (rc != 0) {
        return error_response(cmd, "split readback failed");
    }

    k_mutex_lock(&split_state_lock, K_FOREVER);
    stored = g_split_state[channel_index];
    k_mutex_unlock(&split_state_lock);
    split_emit_quantization_warning(channel_index, &stored);

    return split_channel_response(cmd, channel_index);
}

struct OutMsg memsroute_set(const struct Command *cmd) {
    if (memsroute_is_route_loss_key(cmd->key)) {
        return route_loss_handle(cmd, true);
    }

    // Parse {"input":"...","output":"..."}
    struct mems_route_id route_id = {0};
    struct json_obj_descr d[] = {
        JSON_OBJ_DESCR_PRIM(struct mems_route_id, input, JSON_TOK_STRING),
        JSON_OBJ_DESCR_PRIM(struct mems_route_id, output, JSON_TOK_STRING),
    };
    if (json_obj_parse((char *) cmd->payload, cmd->payload_len, d, ARRAY_SIZE(d), &route_id) < 0) {
        return _msg_builder(cmd, RESP_ERROR, "{\"error\":\"Failed to parse JSON input or output\"}");
    }

    const struct mems_route *route = mems_router_get_route(&router, route_id.input, route_id.output);
    if (!route) {
        return _msg_builder(cmd, RESP_ERROR, "{\"error\":\"Invalid Route\"}");
    }

    for (uint8_t i = 0; i < route->num_steps; ++i) {
        const struct mems_route_step *step = &route->steps[i];
        struct mems_switch *sw = mems_router_find_switch(&router, step->switch_name);
        int rc;

        if (sw==NULL) {
            return error_response(cmd, "route references missing switch");
        }

        rc = mems_switch_set_state(sw, step->state, 1, 0, 0.0f);

        if (rc != 0) {
            char payload[MAX_PAYLOAD_LEN]={0};
            snprintf(payload, MAX_PAYLOAD_LEN, "{\"error\":\"Setting switch %s to %c failed\"}",
                step->switch_name,  step->state);
            return _msg_builder(cmd, RESP_ERROR, payload);
        }
        LOG_INF("Set switch %s to %c\n", step->switch_name, step->state);
    }

    return ok_response(cmd);
}

struct OutMsg measure_throughput_set(const struct Command *cmd)
{
    char stop[8] = {0};
    char laser_name[16] = {0};
    char fiber_text[4] = "M";
    char format[8] = "json";
    struct throughput_monitor_request request = {0};
    struct throughput_monitor_status status = {0};
    uint32_t stopafter_s = 0U;
    bool autolevel = true;
    int parse_rc;
    int rc;

    if (devices_board_type() != HISPEC_BOARD_TIB) {
        return error_response(cmd, "measure_throughput unavailable on this board");
    }

    parse_rc = coo_json_extract_string(cmd->payload, "stop", stop, sizeof(stop));
    if (parse_rc == COO_JSON_EXTRACT_OK) {
        uint8_t channel;

        if (strcasecmp(stop, "all") == 0) {
            rc = throughput_monitor_stop(PHOTODIODE_CHANNEL_COUNT, &status);
            return rc == 0 ?
                ok_response(cmd) :
                error_response(cmd, "stop failed");
        }

        if (strcasecmp(stop, "yj") == 0) {
            channel = PHOTODIODE_CHANNEL_YJ;
        } else if (strcasecmp(stop, "hk") == 0) {
            channel = PHOTODIODE_CHANNEL_HK;
        } else {
            return error_response(cmd, "stop must be yj, hk, or all");
        }

        rc = throughput_monitor_stop(channel, &status);
        if (rc != 0) {
            return error_response(cmd, "stop failed");
        }

        return ok_response(cmd);
    }
    if (parse_rc == COO_JSON_EXTRACT_ERR) {
        return error_response(cmd, "invalid stop");
    }

    parse_rc = coo_json_extract_string(cmd->payload, "laser", laser_name, sizeof(laser_name));
    if (parse_rc != COO_JSON_EXTRACT_OK ||
        hispec_laser_id_from_name(laser_name, &request.laser) != 0) {
        return error_response(cmd, "missing or invalid laser");
    }

    parse_rc = coo_json_extract_string(cmd->payload, "fiber", fiber_text, sizeof(fiber_text));
    if (parse_rc == COO_JSON_EXTRACT_ERR || fiber_text[0] == '\0' ||
        fiber_text[1] != '\0') {
        return error_response(cmd, "fiber must be M or S");
    }
    fiber_text[0] = (char)toupper((unsigned char)fiber_text[0]);
    if (fiber_text[0] != 'M' && fiber_text[0] != 'S') {
        return error_response(cmd, "fiber must be M or S");
    }

    parse_rc = coo_json_extract_bool(cmd->payload, "autolevel", &autolevel);
    if (parse_rc == COO_JSON_EXTRACT_ERR) {
        return error_response(cmd, "invalid autolevel");
    }

    parse_rc = coo_json_extract_u32(cmd->payload, "stopafter_s", &stopafter_s);
    if (parse_rc == COO_JSON_EXTRACT_ERR) {
        return error_response(cmd, "invalid stopafter_s");
    }

    parse_rc = coo_json_extract_string(cmd->payload, "format", format, sizeof(format));
    if (parse_rc == COO_JSON_EXTRACT_ERR ||
        (strcasecmp(format, "json") != 0 && strcasecmp(format, "binary") != 0)) {
        return error_response(cmd, "format must be json or binary");
    }

    request.autolevel = autolevel;
    request.binary = strcasecmp(format, "binary") == 0;
    request.fiber = fiber_text[0];
    request.stopafter_s = stopafter_s;

    rc = throughput_monitor_start(&request, &status);
    if (rc != 0) {
        LOG_ERR("measure_throughput start failed: %d", rc);
        return error_response(cmd, "measure_throughput start failed");
    }

    return ok_response(cmd);
}


static void mems_format_state(const struct mems_switch_status *status, char *out, size_t out_len)
{
    if (status->state == 'A' || status->state == 'B') {
        snprintk(out, out_len, status->state_known_this_boot ? "%c": "%c?", status->state);
        return;
    }

    snprintk(out, out_len, "?");
}


static struct OutMsg mems_response_for_switch(const struct Command *cmd, const struct mems_switch *sw)
{
    struct mems_switch_status status = {0};
    char state_buf[4] = {0};
    char payload[MAX_PAYLOAD_LEN] = {0};

    mems_switch_get_status(sw, &status);
    mems_format_state(&status, state_buf, sizeof(state_buf));

    snprintk(payload, sizeof(payload),
             "{\"state\":\"%s\",\"duty_cycle\":%.3f,"
             "\"requested_toggle_rate_hz\":%.3f,\"toggle_rate_hz\":%.3f,"
             "\"stopafter_s\":%u}",
             state_buf,
             (double)status.duty_cycle,
             (double)status.requested_toggle_rate_hz,
             (double)status.toggle_rate_hz,
             status.stopafter_s);

    return _msg_builder(cmd, RESP_OK, payload);
}


struct OutMsg mems_get(const struct Command *cmd) {


    if (strcmp(cmd->key, "mems") == 0) {
        char payload[MAX_PAYLOAD_LEN] = {0};
        size_t off = 0;
        int written;
        struct mems_switch_status status = {0};
        char state_buf[4] = {0};

        written = snprintk(payload + off, sizeof(payload) - off, "{");
        off += (size_t)written;

        for (uint8_t i = 0; i < router.num_switches; ++i) {
            if (i > 0U) {
                written = snprintk(payload + off, sizeof(payload) - off, ",");
                if (written < 0 || written >= (int)(sizeof(payload) - off)) {
                    return _msg_builder(cmd, RESP_ERROR,
                                        "{\"error\":\"mems response too large; query mems/<switchname>\"}");
                }
                off += (size_t)written;
            }

            mems_switch_get_status(router.switches[i], &status);
            mems_format_state(&status, state_buf, sizeof(state_buf));
            written = snprintk(payload + off, sizeof(payload) - off,
                               "\"%s\":{\"state\":\"%s\",\"duty_cycle\":%.3f}",
                               router.switches[i]->name,
                               state_buf,
                               (double)status.duty_cycle);
            if (written < 0 || written >= (int)(sizeof(payload) - off)) {
                return _msg_builder(cmd, RESP_ERROR,
                                    "{\"error\":\"mems response too large; query mems/<switchname>\"}");
            }
            off += (size_t)written;
        }

        written = snprintk(payload + off, sizeof(payload) - off, "}");
        if (written < 0 || written >= (int)(sizeof(payload) - off)) {
            return _msg_builder(cmd, RESP_ERROR, "{\"error\":\"mems response too large\"}");
        }
        return _msg_builder(cmd, RESP_OK, payload);
    }

    char name[16], mems_switch[16];
    if  (parse_key_pair(cmd->key, name, 15, mems_switch, 15)!=0) {
        return _msg_builder(cmd, RESP_ERROR,"{\"error\":\"Failed to parse mems switch name\"}");
    }

    struct mems_switch *sw = mems_router_find_switch(&router, mems_switch);

    if (sw==NULL) {
        return _msg_builder(cmd, RESP_ERROR,"{\"error\":\"Invalid switch name\"}");
    }

    return mems_response_for_switch(cmd, sw);
}


struct OutMsg mems_set(const struct Command *cmd) {
    char requested_state[8] = {0};
    float duty_cycle = 0.0f;
    float stopafter_s = 0.0f;
    float toggle_rate_hz = 0.0f;
    uint32_t stopafter_s_u32 = 0U;
    bool has_duty_cycle = false;
    bool has_stopafter_s = false;
    bool has_toggle_rate_hz = false;
    int parse_rc;
    int rc;

    char name[16], mems_switch[16];
    if  (parse_key_pair(cmd->key, name, 15, mems_switch, 15)!=0) {
        return _msg_builder(cmd, RESP_ERROR,"{\"error\":\"Failed to parse mems switch name\"}");
    }

    parse_rc = coo_json_extract_string(cmd->payload, "state", requested_state, sizeof(requested_state));
    if (parse_rc == COO_JSON_EXTRACT_MISSING) {
        return _msg_builder(cmd, RESP_ERROR, "{\"error\":\"Missing state\"}");
    }
    if (parse_rc == COO_JSON_EXTRACT_ERR || requested_state[0] == '\0' || requested_state[1] != '\0') {
        return _msg_builder(cmd, RESP_ERROR, "{\"error\":\"Failed to parse switch state\"}");
    }

    requested_state[0] = (char)toupper((unsigned char)requested_state[0]);
    if (requested_state[0] != 'A' && requested_state[0] != 'B') {
        return _msg_builder(cmd, RESP_ERROR, "{\"error\":\"Invalid switch state\"}");
    }

    parse_rc = coo_json_extract_float(cmd->payload, "duty_cycle", &duty_cycle);
    if (parse_rc == COO_JSON_EXTRACT_ERR) {
        return _msg_builder(cmd, RESP_ERROR, "{\"error\":\"Invalid duty_cycle\"}");
    }
    has_duty_cycle = (parse_rc == COO_JSON_EXTRACT_OK);

    parse_rc = coo_json_extract_float(cmd->payload, "stopafter_s", &stopafter_s);
    if (parse_rc == COO_JSON_EXTRACT_ERR) {
        return _msg_builder(cmd, RESP_ERROR, "{\"error\":\"Invalid stopafter_s\"}");
    }
    has_stopafter_s = (parse_rc == COO_JSON_EXTRACT_OK);

    parse_rc = coo_json_extract_float(cmd->payload, "toggle_rate_hz", &toggle_rate_hz);
    if (parse_rc == COO_JSON_EXTRACT_ERR) {
        return _msg_builder(cmd, RESP_ERROR, "{\"error\":\"Invalid toggle_rate_hz\"}");
    }
    has_toggle_rate_hz = (parse_rc == COO_JSON_EXTRACT_OK);
    if (has_toggle_rate_hz && toggle_rate_hz <= 0.0f) {
        return _msg_builder(cmd, RESP_ERROR, "{\"error\":\"toggle_rate_hz must be > 0\"}");
    }

    if (has_duty_cycle && requested_state[0] == 'B') {
        return _msg_builder(cmd, RESP_ERROR, "{\"error\":\"duty_cycle only valid with state A\"}");
    }
    if (has_stopafter_s) {
        if (stopafter_s < 0.0f || stopafter_s > (float)MEMS_SWITCH_MAX_TOGGLE_DURATION_S) {
            return _msg_builder(cmd, RESP_ERROR, "{\"error\":\"stopafter_s out of range\"}");
        }
        stopafter_s_u32 = (uint32_t)(stopafter_s + 0.5f);
        if (stopafter_s_u32 == 0U && duty_cycle > 0.0f && duty_cycle < 1.0f) {
            return _msg_builder(cmd, RESP_ERROR, "{\"error\":\"stopafter_s must be > 0 for toggling\"}");
        }
    }

    struct mems_switch *sw = mems_router_find_switch(&router, mems_switch);

    if (sw==NULL) {
        return _msg_builder(cmd, RESP_ERROR,"{\"error\":\"Invalid switch name\"}");
    }

    if (has_duty_cycle) {
        rc = mems_switch_set_state(sw, requested_state[0], duty_cycle,
                                   stopafter_s_u32,
                                   has_toggle_rate_hz ? toggle_rate_hz : 0.0f);
    } else {
        rc = mems_switch_set_state(sw, requested_state[0], 1, 0,
                                   has_toggle_rate_hz ? toggle_rate_hz : 0.0f);
    }

    if (rc == -ERANGE) {
        return _msg_builder(cmd, RESP_ERROR, "{\"error\":\"MEMS setting out of range\"}");
    }
    if (rc != 0) {
        return _msg_builder(cmd, RESP_ERROR, "{\"error\":\"Invalid MEMS setting\"}");
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
            app_warning_emit("mems_rate_quantized",
                             "requested MEMS toggle rate was quantized",
                             context);
        }
    }

    return mems_response_for_switch(cmd, sw);
}

static int command_laser_id_from_payload(const struct Command *cmd,
                                         enum hispec_laser_id *id,
                                         char *name,
                                         size_t name_len)
{
    int parse_rc;

    if (cmd == NULL || id == NULL || name == NULL || name_len == 0U) {
        return -EINVAL;
    }

    parse_rc = coo_json_extract_string(cmd->payload, "name", name, name_len);
    if (parse_rc != COO_JSON_EXTRACT_OK) {
        return -EINVAL;
    }
    return hispec_laser_id_from_name(name, id);
}

static struct OutMsg laser_unavailable(const struct Command *cmd)
{
    return error_response(cmd, "laser bank unavailable on this board");
}

static struct OutMsg laser_error_response(const struct Command *cmd,
                                          const char *msg,
                                          int rc)
{
    return error_response_rc(cmd, msg, rc);
}

static float laser_status_level(const struct hispec_laser_status *status)
{
    const laserprops_t *props;
    float range;

    if (status == NULL || status->properties == NULL ||
        status->current_set_ma != status->current_set_ma) {
        return LASERPROP_NA;
    }
    props = status->properties;
    range = props->nominal_current_ma - props->threshold_current_ma;
    if (range <= 0.0f) {
        return LASERPROP_NA;
    }
    if (status->current_set_ma <= 0.0f) {
        return 0.0f;
    }
    return 100.0f * (status->current_set_ma - props->threshold_current_ma) / range;
}

static int laser_append_compact_status(char *payload, size_t payload_len,
                                       const struct hispec_laser_status *status)
{
    size_t off = 0U;
    float level = laser_status_level(status);
    const laserprops_t *props = status->properties;

    if (coo_json_append(payload, payload_len, &off,
                        "{\"name\":\"%s\",\"powered\":%s,"
                        "\"tec_on_s\":%.1f,\"emit_on_s\":%.1f,"
                        "\"emit_total_s\":%.1f,\"temp_c\":",
                        status->name,
                        status->bank_powered ? "true" : "false",
                        (double)status->tec_on_time_s,
                        (double)status->current_on_time_s,
                        status->total_emitting_s) != 0 ||
        coo_json_append_float_or_null(payload, payload_len, &off,
                                      status->tec_temperature_measured_c, 2) != 0 ||
        coo_json_append(payload, payload_len, &off,
                        ",\"current_ma\":") != 0 ||
        coo_json_append_float_or_null(payload, payload_len, &off,
                                      status->current_set_ma, 2) != 0 ||
        coo_json_append(payload, payload_len, &off,
                        ",\"level\":") != 0 ||
        coo_json_append_float_or_null(payload, payload_len, &off, level, 2) != 0 ||
        coo_json_append(payload, payload_len, &off,
                        ",\"power_mw\":") != 0 ||
        coo_json_append_float_or_null(payload, payload_len, &off,
                                      status->estimated_power_mw, 3) != 0 ||
        coo_json_append(payload, payload_len, &off,
                        ",\"nominal_nm\":%.3f,\"tuned_nm\":",
                        props != NULL ? (double)props->wavelength_nm : 0.0) != 0 ||
        coo_json_append_float_or_null(payload, payload_len, &off,
                                      status->estimated_wavelength_nm, 3) != 0 ||
        coo_json_append(payload, payload_len, &off,
        ",\"tune_nm\":%.3f,\"tec_ma\":",
                        (double)status->tune_delta_nm) != 0 ||
        coo_json_append_float_or_null(payload, payload_len, &off,
                                      (double)status->tec_current_measured_a * 1000.0, 2) != 0 ||
        coo_json_append(payload, payload_len, &off,
                        ",\"diode_v\":") != 0 ||
        coo_json_append_float_or_null(payload, payload_len, &off,
                                      status->voltage_v, 3) != 0 ||
        coo_json_append(payload, payload_len, &off,
                        ",\"tec_v\":") != 0 ||
        coo_json_append_float_or_null(payload, payload_len, &off,
                                      status->tec_voltage_v, 3) != 0 ||
        coo_json_append(payload, payload_len, &off,
                        ",\"offin_s\":%lld,\"oc_fault\":%s}",
                        (long long)status->off_in_s,
                        status->lock_ld_overcurrent ? "true" : "false") != 0) {
        return -ENOSPC;
    }

    return 0;
}

struct OutMsg laser_get(const struct Command *cmd)
{
    enum hispec_laser_id id;
    struct hispec_laser_status status = {0};
    char name[16] = {0};
    char payload[MAX_PAYLOAD_LEN] = {0};
    int rc;

    if (devices_board_type() != HISPEC_BOARD_TIB) {
        return laser_unavailable(cmd);
    }
    if (command_laser_id_from_payload(cmd, &id, name, sizeof(name)) != 0) {
        return error_response(cmd, "missing or invalid laser name");
    }

    rc = hispec_laser_get_status(id, &status);
    if (rc != 0 && !status.bank_powered) {
        return laser_error_response(cmd, "laser status failed", rc);
    }
    if (laser_append_compact_status(payload, sizeof(payload), &status) != 0) {
        return error_response(cmd, "laser response too large");
    }
    return rc == 0 ? _msg_builder(cmd, RESP_OK, payload) :
           laser_error_response(cmd, "laser status failed", rc);
}

struct OutMsg laser_set(const struct Command *cmd)
{
    enum hispec_laser_id id;
    struct app_laser_channel_settings settings;
    char name[16] = {0};
    float level = 0.0f;
    uint32_t autooff_s;
    int parse_rc;
    int rc;

    if (devices_board_type() != HISPEC_BOARD_TIB) {
        return laser_unavailable(cmd);
    }
    if (command_laser_id_from_payload(cmd, &id, name, sizeof(name)) != 0) {
        return error_response(cmd, "missing or invalid laser name");
    }
    parse_rc = coo_json_extract_float(cmd->payload, "level", &level);
    if (parse_rc != COO_JSON_EXTRACT_OK || level < 0.0f || level > 100.0f) {
        return error_response(cmd, "level must be 0..100");
    }
    rc = hispec_laser_get_channel_settings(id, &settings);
    if (rc != 0) {
        return laser_error_response(cmd, "laser settings unavailable", rc);
    }
    autooff_s = settings.autooff_s;
    parse_rc = coo_json_extract_u32(cmd->payload, "autooff_s", &autooff_s);
    if (parse_rc == COO_JSON_EXTRACT_ERR) {
        return error_response(cmd, "invalid autooff_s");
    }

    throughput_monitor_note_laser_changed(id);
    rc = hispec_laser_set_output_percent_autooff(id, level, autooff_s);
    if (rc != 0) {
        return laser_error_response(cmd, "laser level failed", rc);
    }
    return ok_response(cmd);
}

struct OutMsg laser_tune_get(const struct Command *cmd)
{
    enum hispec_laser_id id;
    char name[16] = {0};
    char payload[MAX_PAYLOAD_LEN];

    if (devices_board_type() != HISPEC_BOARD_TIB) {
        return laser_unavailable(cmd);
    }
    if (command_laser_id_from_payload(cmd, &id, name, sizeof(name)) != 0) {
        return error_response(cmd, "missing or invalid laser name");
    }
    snprintk(payload, sizeof(payload),
             "{\"name\":\"%s\",\"tune_nm\":%.4f}",
             hispec_laser_name(id),
             (double)hispec_laser_get_tune_delta_nm(id));
    return _msg_builder(cmd, RESP_OK, payload);
}

struct OutMsg laser_tune_set(const struct Command *cmd)
{
    enum hispec_laser_id id;
    char name[16] = {0};
    float delta_nm = 0.0f;
    int parse_rc;
    int rc;

    if (devices_board_type() != HISPEC_BOARD_TIB) {
        return laser_unavailable(cmd);
    }
    if (command_laser_id_from_payload(cmd, &id, name, sizeof(name)) != 0) {
        return error_response(cmd, "missing or invalid laser name");
    }
    parse_rc = coo_json_extract_float(cmd->payload, "tune_nm", &delta_nm);
    if (parse_rc == COO_JSON_EXTRACT_MISSING) {
        parse_rc = coo_json_extract_float(cmd->payload, "delta_nm", &delta_nm);
    }
    if (parse_rc != COO_JSON_EXTRACT_OK) {
        return error_response(cmd, "missing tune_nm");
    }
    throughput_monitor_note_laser_changed(id);
    rc = hispec_laser_set_tune_delta_nm(id, delta_nm, true);
    if (rc != 0) {
        return laser_error_response(cmd, "laser tune failed", rc);
    }
    return ok_response(cmd);
}

static int laser_settings_payload(char *payload, size_t payload_len,
                                  enum hispec_laser_id id,
                                  const struct app_laser_channel_settings *settings)
{
    const laserprops_t *p = &settings->properties;
    int written;

    written = snprintk(payload, payload_len,
        "{\"name\":\"%s\",\"settings\":{"
        "\"model\":\"%s\",\"nominal_current_ma\":%.3f,"
        "\"max_current_ma\":%.3f,\"current_set_calibration_pct\":%.3f,"
        "\"threshold_current_ma\":%.3f,\"efficiency_mw_per_ma\":%.6f,"
        "\"wavelength_nm\":%.3f,\"operating_temp_range_c\":[%.2f,%.2f],"
        "\"default_operating_temp_c\":%.2f,\"thermistor_kohm\":%.2f,"
        "\"isolation_db\":%.2f,\"tec_max_current_a\":%.3f,"
        "\"tec_pid\":{\"p\":%u,\"i\":%u,\"d\":%u},"
        "\"disable_tec_at_autooff\":%s,"
        "\"ntc_t_coefficient_per_c\":%.6f,"
        "\"dlambda_dT_nm_per_k\":%.6f,"
        "\"dlambda_dA_nm_per_ma\":%.6f,"
        "\"autooff_s\":%u,\"tune_nm\":%.4f,"
        "\"emit_total_s\":%.1f}}",
        hispec_laser_name(id), p->model_number,
        (double)p->nominal_current_ma,
        (double)p->max_current_ma,
        (double)settings->current_set_calibration_pct,
        (double)p->threshold_current_ma,
        (double)p->efficiency_mw_per_ma,
        (double)p->wavelength_nm,
        (double)p->operating_temp_range_c.min_c,
        (double)p->operating_temp_range_c.max_c,
        (double)p->operating_temp_c,
        (double)p->thermistor_kohm,
        (double)p->isolation_db,
        (double)p->tec_max_current_a,
        p->tec_pid.kp, p->tec_pid.ki, p->tec_pid.kd,
        settings->disable_tec_at_autooff ? "true" : "false",
        (double)p->ntc_t_coefficient_per_c,
        (double)p->dlambda_dT_nm_per_k,
        (double)p->dlambda_dA_nm_per_ma,
        settings->autooff_s,
        (double)settings->tune_delta_nm,
        settings->total_emitting_s);

    return written >= 0 && written < (int)payload_len ? 0 : -ENOSPC;
}

struct OutMsg laser_settings_get(const struct Command *cmd)
{
    enum hispec_laser_id id;
    struct app_laser_channel_settings settings;
    char name[16] = {0};
    char payload[MAX_PAYLOAD_LEN] = {0};
    int rc;

    if (devices_board_type() != HISPEC_BOARD_TIB) {
        return laser_unavailable(cmd);
    }
    if (command_laser_id_from_payload(cmd, &id, name, sizeof(name)) != 0) {
        return error_response(cmd, "missing or invalid laser name");
    }
    rc = hispec_laser_get_channel_settings(id, &settings);
    if (rc != 0) {
        return laser_error_response(cmd, "laser settings unavailable", rc);
    }
    if (laser_settings_payload(payload, sizeof(payload), id, &settings) != 0) {
        return error_response(cmd, "laser settings response too large");
    }
    return _msg_builder(cmd, RESP_OK, payload);
}

static int laser_parse_settings_update(const char *json,
                                       struct app_laser_channel_settings *settings,
                                       bool *driver_changed,
                                       bool *changed)
{
    double range[2] = {0};
    size_t range_len = 0U;
    char pid_json[128] = {0};
    float fval;
    uint32_t uval;
    bool bval;
    int rc;

    if (json == NULL || settings == NULL || driver_changed == NULL || changed == NULL) {
        return -EINVAL;
    }

#define LASER_PARSE_FLOAT(key, field, minv, maxv, driver) do { \
        rc = coo_json_extract_float(json, key, &fval); \
        if (rc == COO_JSON_EXTRACT_ERR) return -EINVAL; \
        if (rc == COO_JSON_EXTRACT_OK) { \
            if (!(fval >= (minv) && fval <= (maxv))) return -ERANGE; \
            (field) = fval; \
            *changed = true; \
            if (driver) *driver_changed = true; \
        } \
    } while (0)

    LASER_PARSE_FLOAT("nominal_current_ma", settings->properties.nominal_current_ma,
                      0.0f, 1000.0f, false);
    LASER_PARSE_FLOAT("max_current_ma", settings->properties.max_current_ma,
                      0.0f, 1000.0f, true);
    LASER_PARSE_FLOAT("threshold_current_ma", settings->properties.threshold_current_ma,
                      0.0f, 1000.0f, false);
    LASER_PARSE_FLOAT("efficiency_mw_per_ma", settings->properties.efficiency_mw_per_ma,
                      0.0f, 100.0f, false);
    LASER_PARSE_FLOAT("wavelength_nm", settings->properties.wavelength_nm,
                      1.0f, 10000.0f, false);
    LASER_PARSE_FLOAT("current_set_calibration_pct",
                      settings->current_set_calibration_pct,
                      95.0f, 105.0f, true);
    LASER_PARSE_FLOAT("current_set_calibration_%",
                      settings->current_set_calibration_pct,
                      95.0f, 105.0f, true);
    LASER_PARSE_FLOAT("default_operating_temp_c", settings->properties.operating_temp_c,
                      15.0f, 40.0f, false);
    LASER_PARSE_FLOAT("dlambda_dT_nm_per_k", settings->properties.dlambda_dT_nm_per_k,
                      -10.0f, 10.0f, false);
    LASER_PARSE_FLOAT("dlambda_dA_nm_per_ma", settings->properties.dlambda_dA_nm_per_ma,
                      -10.0f, 10.0f, false);

#undef LASER_PARSE_FLOAT

    rc = coo_json_extract_double_array(json, "operating_temp_range_c",
                                       range, ARRAY_SIZE(range), &range_len);
    if (rc == COO_JSON_EXTRACT_ERR) {
        return -EINVAL;
    }
    if (rc == COO_JSON_EXTRACT_OK) {
        if (range_len != 2U || range[0] < 15.0 || range[1] > 40.0 || range[0] > range[1]) {
            return -ERANGE;
        }
        settings->properties.operating_temp_range_c.min_c = (float)range[0];
        settings->properties.operating_temp_range_c.max_c = (float)range[1];
        *changed = true;
    }

    rc = coo_json_extract_object(json, "tec_pid", pid_json, sizeof(pid_json));
    if (rc == COO_JSON_EXTRACT_ERR) {
        return -EINVAL;
    }
    if (rc == COO_JSON_EXTRACT_OK) {
        if (coo_json_extract_u32(pid_json, "p", &uval) == COO_JSON_EXTRACT_OK &&
            uval <= UINT16_MAX) {
            settings->properties.tec_pid.kp = (uint16_t)uval;
            *changed = true;
            *driver_changed = true;
        }
        if (coo_json_extract_u32(pid_json, "i", &uval) == COO_JSON_EXTRACT_OK &&
            uval <= UINT16_MAX) {
            settings->properties.tec_pid.ki = (uint16_t)uval;
            *changed = true;
            *driver_changed = true;
        }
        if (coo_json_extract_u32(pid_json, "d", &uval) == COO_JSON_EXTRACT_OK &&
            uval <= UINT16_MAX) {
            settings->properties.tec_pid.kd = (uint16_t)uval;
            *changed = true;
            *driver_changed = true;
        }
    }

    rc = coo_json_extract_bool(json, "disable_tec_at_autooff", &bval);
    if (rc == COO_JSON_EXTRACT_ERR) {
        return -EINVAL;
    }
    if (rc == COO_JSON_EXTRACT_OK) {
        settings->disable_tec_at_autooff = bval;
        *changed = true;
    }

    rc = coo_json_extract_u32(json, "autooff_s", &uval);
    if (rc == COO_JSON_EXTRACT_ERR) {
        return -EINVAL;
    }
    if (rc == COO_JSON_EXTRACT_OK) {
        settings->autooff_s = uval;
        *changed = true;
    }

    return 0;
}

struct OutMsg laser_settings_set(const struct Command *cmd)
{
    enum hispec_laser_id id;
    struct app_laser_channel_settings settings;
    char name[16] = {0};
    char settings_json[MAX_PAYLOAD_LEN] = {0};
    const char *json;
    bool changed = false;
    bool driver_changed = false;
    int rc;

    if (devices_board_type() != HISPEC_BOARD_TIB) {
        return laser_unavailable(cmd);
    }
    if (command_laser_id_from_payload(cmd, &id, name, sizeof(name)) != 0) {
        return error_response(cmd, "missing or invalid laser name");
    }
    rc = hispec_laser_get_channel_settings(id, &settings);
    if (rc != 0) {
        return laser_error_response(cmd, "laser settings unavailable", rc);
    }

    rc = coo_json_extract_object(cmd->payload, "settings", settings_json, sizeof(settings_json));
    if (rc == COO_JSON_EXTRACT_ERR) {
        return error_response(cmd, "invalid settings object");
    }
    json = rc == COO_JSON_EXTRACT_OK ? settings_json : cmd->payload;

    rc = laser_parse_settings_update(json, &settings, &driver_changed, &changed);
    if (rc != 0) {
        return laser_error_response(cmd, "invalid laser settings", rc);
    }
    if (!changed) {
        return error_response(cmd, "no laser settings fields supplied");
    }

    throughput_monitor_note_laser_changed(id);
    rc = hispec_laser_update_channel_settings(id, &settings, driver_changed, true);
    if (rc != 0) {
        return laser_error_response(cmd, "laser settings update failed", rc);
    }
    return ok_response(cmd);
}

struct OutMsg laser_status_get(const struct Command *cmd)
{
    return laser_get(cmd);
}

static int json_append_named_float(char *payload, size_t payload_len,
                                   size_t *off, const char *name,
                                   double value, int precision)
{
    if (coo_json_append(payload, payload_len, off, ",\"%s\":", name) != 0) {
        return -ENOSPC;
    }
    return coo_json_append_float_or_null(payload, payload_len, off,
                                         value, precision);
}

struct OutMsg laser_engstatus_get(const struct Command *cmd)
{
    enum hispec_laser_id id;
    struct hispec_laser_status s = {0};
    char name[16] = {0};
    char payload[MAX_PAYLOAD_LEN] = {0};
    size_t off = 0U;
    int rc;

    if (devices_board_type() != HISPEC_BOARD_TIB) {
        return laser_unavailable(cmd);
    }
    if (command_laser_id_from_payload(cmd, &id, name, sizeof(name)) != 0) {
        return error_response(cmd, "missing or invalid laser name");
    }

    rc = hispec_laser_get_status(id, &s);
    if (coo_json_append(payload, sizeof(payload), &off,
                        "{\"name\":\"%s\",\"read_rc\":%d,\"powered\":%s,"
                        "\"dev_id\":%u,\"serial\":%u,\"serial_ok\":%s,"
                        "\"raw_state\":%u,\"raw_lock\":%u,\"raw_tec\":%u,"
                        "\"op_started\":%s,\"ready\":%s,"
                        "\"curr_set_internal\":%s,\"enable_internal\":%s,"
                        "\"ext_ntc_denied\":%s,\"interlock_denied\":%s,"
                        "\"interlock\":%s,\"ext_ntc_interlock\":%s,"
                        "\"ld_overcurrent\":%s,\"ld_overheat\":%s,"
                        "\"tec_started\":%s,\"tec_set_internal\":%s,"
                        "\"tec_enable_internal\":%s,\"tec_error\":%s,"
                        "\"tec_selfheat\":%s",
                        s.name, rc, s.bank_powered ? "true" : "false",
                        s.device_id, s.serial_number,
                        s.serial_matches ? "true" : "false",
                        s.device_state, s.lock_status, s.tec_state,
                        s.operation_started ? "true" : "false",
                        s.ready_to_operate ? "true" : "false",
                        s.current_set_internal ? "true" : "false",
                        s.enable_internal ? "true" : "false",
                        s.external_ntc_denied ? "true" : "false",
                        s.interlock_denied ? "true" : "false",
                        s.lock_interlock ? "true" : "false",
                        s.lock_external_ntc_interlock ? "true" : "false",
                        s.lock_ld_overcurrent ? "true" : "false",
                        s.lock_ld_overheat ? "true" : "false",
                        s.tec_started ? "true" : "false",
                        s.tec_set_internal ? "true" : "false",
                        s.tec_enable_internal ? "true" : "false",
                        s.lock_tec_error ? "true" : "false",
                        s.lock_tec_selfheat ? "true" : "false") != 0 ||
        json_append_named_float(payload, sizeof(payload), &off,
                                "curr_ma", s.current_set_ma, 3) != 0 ||
        json_append_named_float(payload, sizeof(payload), &off,
                                "curr_meas_ma", s.current_measured_ma, 3) != 0 ||
        json_append_named_float(payload, sizeof(payload), &off,
                                "curr_min_ma", s.current_min_ma, 3) != 0 ||
        json_append_named_float(payload, sizeof(payload), &off,
                                "curr_max_ma", s.current_max_ma, 3) != 0 ||
        json_append_named_float(payload, sizeof(payload), &off,
                                "drv_max_ma", s.current_max_limit_ma, 3) != 0 ||
        json_append_named_float(payload, sizeof(payload), &off,
                                "ocp_ma", s.current_protection_threshold_ma, 3) != 0 ||
        json_append_named_float(payload, sizeof(payload), &off,
                                "curr_cal_pct", s.current_set_calibration_pct, 3) != 0 ||
        json_append_named_float(payload, sizeof(payload), &off,
                                "diode_v", s.voltage_v, 3) != 0 ||
        json_append_named_float(payload, sizeof(payload), &off,
                                "tec_temp_set_c", s.tec_temperature_set_c, 3) != 0 ||
        json_append_named_float(payload, sizeof(payload), &off,
                                "tec_temp_c", s.tec_temperature_measured_c, 3) != 0 ||
        json_append_named_float(payload, sizeof(payload), &off,
                                "pcb_temp_c", s.pcb_temperature_c, 3) != 0 ||
        json_append_named_float(payload, sizeof(payload), &off,
                                "tec_curr_a", s.tec_current_measured_a, 3) != 0 ||
        json_append_named_float(payload, sizeof(payload), &off,
                                "tec_curr_lim_a", s.tec_current_limit_a, 3) != 0 ||
        json_append_named_float(payload, sizeof(payload), &off,
                                "tec_v", s.tec_voltage_v, 3) != 0 ||
        coo_json_append(payload, sizeof(payload), &off,
                        ",\"pid\":[%u,%u,%u]",
                        s.tec_pid.kp, s.tec_pid.ki, s.tec_pid.kd) != 0 ||
        json_append_named_float(payload, sizeof(payload), &off,
                                "ntc_t_coeff", s.ntc_t_coefficient_per_c, 6) != 0 ||
        coo_json_append(payload, sizeof(payload), &off, "}") != 0) {
        return error_response(cmd, "laser engineering status response too large");
    }
    return rc == 0 ? _msg_builder(cmd, RESP_OK, payload) :
           laser_error_response(cmd, "laser engineering status failed", rc);
}

struct OutMsg atten_setting_get(const struct Command *cmd) {

    char laser_name[16], setting[16];
    if (parse_atten_key(cmd->key, laser_name, sizeof(laser_name),
                        setting, sizeof(setting)) != 0) {
        return _msg_builder(cmd, RESP_ERROR,"{\"error\":\"Failed to parse atten/setting\"}");
    }

    enum hispec_laser_id laser_id;
    uint8_t attenuator_index;

    if (hispec_laser_id_from_name(laser_name, &laser_id) != 0 ||
        attenuator_index_from_laser_id(laser_id, &attenuator_index) != 0) {
        return _msg_builder(cmd, RESP_ERROR,"{\"error\":\"Invalid attenuator\"}");
    }
    if (!attenuator_channel_available(attenuator_index)) {
        return _msg_builder(cmd, RESP_ERROR,
                            "{\"error\":\"Attenuator unavailable on this board\"}");
    }

    char payload[MAX_PAYLOAD_LEN]={0};
    if (strcasecmp(setting, "coeff") == 0) {
        snprintf(payload, MAX_PAYLOAD_LEN,
                 "{\"dac1\":[%.8f,%.8f],\"dac2\":[%.8f,%.8f]}",
                 attenuators[attenuator_index].coeff1.slope,
                 attenuators[attenuator_index].coeff1.offset,
                 attenuators[attenuator_index].coeff2.slope,
                 attenuators[attenuator_index].coeff2.offset);
    } else if (strcasecmp(setting, "value") == 0 || strcasecmp(setting, "valuedb") == 0) {
        struct attenuator_status status = {0};

        if (!attenuator_get(&attenuators[attenuator_index], &status)) {
            return _msg_builder(cmd, RESP_ERROR,
                                "{\"error\":\"Failed to read attenuator\"}");
        }
        snprintf(payload, MAX_PAYLOAD_LEN,
                 "{\"db\":%.4f,\"linear\":%.6f,"
                 "\"voltage1\":%.4f,\"voltage2\":%.4f,"
                 "\"db1\":%.4f,\"db2\":%.4f}",
                 status.attenuation_db,
                 status.linear,
                 status.voltage1,
                 status.voltage2,
                 status.attenuation_db1,
                 status.attenuation_db2);
    } else {
        return _msg_builder(cmd, RESP_ERROR,"{\"error\":\"Invalid setting\"}");
    }

    return _msg_builder(cmd, RESP_OK, payload);
}

struct OutMsg atten_setting_set(const struct Command *cmd) {

    char laser_name[16], setting[16];
    if (parse_atten_key(cmd->key, laser_name, sizeof(laser_name),
                        setting, sizeof(setting)) != 0) {
        return _msg_builder(cmd, RESP_ERROR,"{\"error\":\"Failed to parse laser/setting\"}");
    }

    enum hispec_laser_id laser_id;
    uint8_t attenuator_index;

    if (hispec_laser_id_from_name(laser_name, &laser_id) != 0 ||
        attenuator_index_from_laser_id(laser_id, &attenuator_index) != 0) {
        return _msg_builder(cmd, RESP_ERROR,"{\"error\":\"Invalid attenuator\"}");
    }
    if (!attenuator_channel_available(attenuator_index)) {
        return _msg_builder(cmd, RESP_ERROR,
                            "{\"error\":\"Attenuator unavailable on this board\"}");
    }

    if (strcasecmp(setting, "coeff") == 0) {

        double dac1_coeffs[ATTENUATOR_COEFF_COUNT] = {0};
        double dac2_coeffs[ATTENUATOR_COEFF_COUNT] = {0};
        size_t dac1_len = 0U;
        size_t dac2_len = 0U;
        struct app_attenuator_channel_settings stored_coeffs = {0};
        bool persist = false;
        int parse_rc;
        struct attenuator_status status = {0};

        parse_rc = coo_json_extract_double_array(cmd->payload, "dac1",
                                                 dac1_coeffs,
                                                 ATTENUATOR_COEFF_COUNT,
                                                 &dac1_len);
        if (parse_rc != COO_JSON_EXTRACT_OK || dac1_len != ATTENUATOR_COEFF_COUNT) {
            return _msg_builder(cmd, RESP_ERROR,"{\"error\":\"Improper arguments\"}");
        }

        parse_rc = coo_json_extract_double_array(cmd->payload, "dac2",
                                                 dac2_coeffs,
                                                 ATTENUATOR_COEFF_COUNT,
                                                 &dac2_len);
        if (parse_rc != COO_JSON_EXTRACT_OK || dac2_len != ATTENUATOR_COEFF_COUNT) {
            return _msg_builder(cmd, RESP_ERROR,"{\"error\":\"Improper arguments\"}");
        }

        parse_rc = coo_json_extract_bool(cmd->payload, "persistent", &persist);
        if (parse_rc == COO_JSON_EXTRACT_ERR) {
            return _msg_builder(cmd, RESP_ERROR,"{\"error\":\"Invalid persistent flag\"}");
        }

        if (!attenuator_get(&attenuators[attenuator_index], &status)) {
            return _msg_builder(cmd, RESP_ERROR,
                                "{\"error\":\"Failed to read attenuator\"}");
        }

        attenuators[attenuator_index].coeff1.slope = dac1_coeffs[0];
        attenuators[attenuator_index].coeff1.offset = dac1_coeffs[1];
        attenuators[attenuator_index].coeff2.slope = dac2_coeffs[0];
        attenuators[attenuator_index].coeff2.offset = dac2_coeffs[1];
        stored_coeffs.physical[0].slope = dac1_coeffs[0];
        stored_coeffs.physical[0].offset = dac1_coeffs[1];
        stored_coeffs.physical[1].slope = dac2_coeffs[0];
        stored_coeffs.physical[1].offset = dac2_coeffs[1];

        if (!attenuator_set_db(&attenuators[attenuator_index], status.attenuation_db)) {
            return _msg_builder(cmd, RESP_ERROR,
                                "{\"error\":\"Failed to apply coefficients\"}");
        }
        app_settings_update_attenuator_channel(attenuator_index, &stored_coeffs, persist);

    } else if (strcasecmp(setting, "value") == 0 || strcasecmp(setting, "valuedb") == 0) {

        double value;

        if (coo_json_extract_double(cmd->payload, "value", &value) !=
            COO_JSON_EXTRACT_OK) {
            return _msg_builder(cmd, RESP_ERROR,"{\"error\":\"Missing setting value\"}");
        }

        if (strcasecmp(setting, "value") == 0) {
            if (!attenuator_set_linear(&attenuators[attenuator_index], value)) {
                return _msg_builder(cmd, RESP_ERROR,
                                    "{\"error\":\"Invalid linear transmission\"}");
            }
        } else {
            if (!attenuator_set_db(&attenuators[attenuator_index], value)) {
                return _msg_builder(cmd, RESP_ERROR,
                                    "{\"error\":\"Invalid dB attenuation\"}");
            }
        }

    } else {
        return _msg_builder(cmd, RESP_ERROR,"{\"error\":\"Invalid setting\"}");
    }

    throughput_monitor_note_attenuator_changed(attenuator_index);

    return ok_response(cmd);
}

static int pd_parse_channel_name(const char *name, enum photodiode_channel *channel)
{
    if (name == NULL || channel == NULL) {
        return -EINVAL;
    }
    if (strcasecmp(name, "yj") == 0) {
        *channel = PHOTODIODE_CHANNEL_YJ;
        return 0;
    }
    if (strcasecmp(name, "hk") == 0) {
        *channel = PHOTODIODE_CHANNEL_HK;
        return 0;
    }

    return -ENOENT;
}

static int pd_parse_channel_from_key(const struct Command *cmd,
                                     enum photodiode_channel *channel)
{
    const char *slash = strchr(cmd->key, '/');

    if (slash == NULL || slash[1] == '\0') {
        return -ENOENT;
    }

    return pd_parse_channel_name(slash + 1, channel);
}

static int pd_parse_channel_from_payload_or_key(const struct Command *cmd,
                                                enum photodiode_channel *channel)
{
    char channel_name[8] = {0};
    int parse_rc;

    parse_rc = pd_parse_channel_from_key(cmd, channel);
    if (parse_rc == 0) {
        return 0;
    }

    parse_rc = coo_json_extract_string(cmd->payload, "channel",
                                       channel_name, sizeof(channel_name));
    if (parse_rc == COO_JSON_EXTRACT_MISSING) {
        return -ENOENT;
    }
    if (parse_rc == COO_JSON_EXTRACT_ERR) {
        return -EINVAL;
    }

    return pd_parse_channel_name(channel_name, channel);
}

struct OutMsg pd_get(const struct Command *cmd)
{
    struct photodiode_status status;
    char payload[MAX_PAYLOAD_LEN] = {0};
    struct app_photodiode_settings settings;
    float yj_value;
    float hk_value;
    float yj_err;
    float hk_err;
    float yj_gain;
    float hk_gain;

    if (devices_board_type() != HISPEC_BOARD_TIB) {
        return error_response(cmd, "photodiodes unavailable on this board");
    }

    photodiode_get_status(&status);
    app_settings_get_photodiode(&settings);

    yj_value = status.channel[PHOTODIODE_CHANNEL_YJ].power_uw;
    hk_value = status.channel[PHOTODIODE_CHANNEL_HK].power_uw;
    yj_gain = settings.channel[PHOTODIODE_CHANNEL_YJ].gain_v_per_uw;
    hk_gain = settings.channel[PHOTODIODE_CHANNEL_HK].gain_v_per_uw;

    yj_err = (yj_gain > 0.0f) ?
        status.channel[PHOTODIODE_CHANNEL_YJ].noise_rms_mv / (yj_gain * 1000.0f) :
        0.0f;

    hk_err = (hk_gain > 0.0f) ?
        status.channel[PHOTODIODE_CHANNEL_HK].noise_rms_mv / (hk_gain * 1000.0f) :
        0.0f;

    snprintk(payload, sizeof(payload),
             "{\"yjvalue\":%.6f,\"yjvalue_err\":%.6f,"
             "\"hkvalue\":%.6f,\"hkvalue_err\":%.6f,"
             "\"yj_raw\":%d,\"hk_raw\":%d,\"yj_mv\":%.3f,\"hk_mv\":%.3f,"
             "\"yj_noise_rms_mv\":%.3f,\"hk_noise_rms_mv\":%.3f,"
             "\"yj_mean_mv_1s\":%.3f,\"hk_mean_mv_1s\":%.3f,"
             "\"yj_rms_mv_0p5s\":%.3f,\"hk_rms_mv_0p5s\":%.3f,"
             "\"uptime\":%lld}",
             (double)yj_value,
             (double)yj_err,
             (double)hk_value,
             (double)hk_err,
             status.channel[PHOTODIODE_CHANNEL_YJ].raw,
             status.channel[PHOTODIODE_CHANNEL_HK].raw,
             (double)status.channel[PHOTODIODE_CHANNEL_YJ].mv,
             (double)status.channel[PHOTODIODE_CHANNEL_HK].mv,
             (double)status.channel[PHOTODIODE_CHANNEL_YJ].noise_rms_mv,
             (double)status.channel[PHOTODIODE_CHANNEL_HK].noise_rms_mv,
             (double)status.channel[PHOTODIODE_CHANNEL_YJ].mean_mv_1s,
             (double)status.channel[PHOTODIODE_CHANNEL_HK].mean_mv_1s,
             (double)status.channel[PHOTODIODE_CHANNEL_YJ].rms_mv_0p5s,
             (double)status.channel[PHOTODIODE_CHANNEL_HK].rms_mv_0p5s,
             status.uptime_ms);
    return _msg_builder(cmd, RESP_OK, payload);
}

static struct OutMsg pd_dark_status_response(const struct Command *cmd,
                                             const struct photodiode_dark_status *status)
{
    char payload[MAX_PAYLOAD_LEN] = {0};
    const struct photodiode_dark_result *result = &status->result;
    const char *state_name = photodiode_dark_state_name(status->state);

    if (status->state == PHOTODIODE_DARK_COMPLETE) {
        snprintk(payload, sizeof(payload),
                 "{\"state\":\"%s\",\"channel\":\"%s\",\"stored\":%s,"
                 "\"duration_ms\":%u,\"samples\":%u,\"target_samples\":%u,"
                 "\"mean_dark_mv\":%.3f,\"rms_mv\":%.3f,"
                 "\"min_mv\":%.3f,\"max_mv\":%.3f,"
                 "\"previous_dark_mv\":%.3f,\"configured_dark_mv\":%.3f,"
                 "\"lowest_dark_mv\":%.3f,\"lowest_dark_valid\":%s}",
                 state_name,
                 photodiode_channel_names[status->channel],
                 result->stored ? "true" : "false",
                 status->duration_ms,
                 status->samples,
                 status->target_samples,
                 (double)result->mean_mv,
                 (double)result->rms_mv,
                 (double)result->min_mv,
                 (double)result->max_mv,
                 (double)result->previous_dark_mv,
                 (double)result->configured_dark_mv,
                 (double)result->lowest_dark_mv,
                 result->lowest_dark_valid ? "true" : "false");
        return _msg_builder(cmd, RESP_OK, payload);
    }

    if (status->state == PHOTODIODE_DARK_ERROR) {
        snprintk(payload, sizeof(payload),
                 "{\"error\":\"dark measurement failed\",\"channel\":\"%s\",\"rc\":%d,"
                 "\"duration_ms\":%u,\"samples\":%u,\"target_samples\":%u}",
                 photodiode_channel_names[status->channel],
                 status->last_error,
                 status->duration_ms,
                 status->samples,
                 status->target_samples);
        return _msg_builder(cmd, RESP_ERROR, payload);
    }

    snprintk(payload, sizeof(payload),
             "{\"state\":\"%s\",\"channel\":\"%s\",\"stored_on_complete\":%s,"
             "\"duration_ms\":%u,\"samples\":%u,\"target_samples\":%u}",
             state_name,
             photodiode_channel_names[status->channel],
             status->store ? "true" : "false",
             status->duration_ms,
             status->samples,
             status->target_samples);
    return _msg_builder(cmd, RESP_OK, payload);
}

struct OutMsg pd_set(const struct Command *cmd)
{
    char action[32] = {0};
    enum photodiode_channel channel;
    uint32_t duration_ms = 0U;
    bool store = false;
    bool persist = true;
    int parse_rc;
    int rc;

    if (devices_board_type() != HISPEC_BOARD_TIB) {
        return error_response(cmd, "photodiodes unavailable on this board");
    }

    parse_rc = coo_json_extract_string(cmd->payload, "action", action, sizeof(action));
    if (parse_rc == COO_JSON_EXTRACT_MISSING) {
        return error_response(cmd, "missing action");
    }
    if (parse_rc == COO_JSON_EXTRACT_ERR) {
        return error_response(cmd, "invalid action");
    }

    rc = pd_parse_channel_from_payload_or_key(cmd, &channel);
    if (rc != 0) {
        return error_response(cmd, "channel must be yj or hk");
    }

    if (strcasecmp(action, "measure_dark") == 0) {
        struct photodiode_dark_status status;

        if (throughput_monitor_autolevel_active(channel)) {
            return error_response(cmd, "dark measurement blocked by autolevel throughput monitor");
        }

        parse_rc = coo_json_extract_u32(cmd->payload, "duration_ms", &duration_ms);
        if (parse_rc == COO_JSON_EXTRACT_ERR) {
            return error_response(cmd, "invalid duration_ms");
        }

        parse_rc = coo_json_extract_bool(cmd->payload, "store", &store);
        if (parse_rc == COO_JSON_EXTRACT_ERR) {
            return error_response(cmd, "invalid store");
        }

        rc = photodiode_start_dark_measurement(channel, duration_ms, store, &status);
        if (rc != 0) {
            return error_response_rc(cmd, "dark measurement failed", rc);
        }
        return pd_dark_status_response(cmd, &status);
    }

    if (strcasecmp(action, "dark_status") == 0) {
        struct photodiode_dark_status status;

        rc = photodiode_get_dark_status(channel, &status);
        if (rc != 0) {
            return error_response(cmd, "dark status unavailable");
        }

        return pd_dark_status_response(cmd, &status);
    }

    if (strcasecmp(action, "reset_lowest_dark") == 0) {
        parse_rc = coo_json_extract_bool(cmd->payload, "persistent", &persist);
        if (parse_rc == COO_JSON_EXTRACT_ERR) {
            return error_response(cmd, "invalid persistent");
        }

        rc = photodiode_reset_lowest_dark(channel, persist);
        if (rc != 0) {
            return error_response(cmd, "reset failed");
        }
        return ok_response(cmd);
    }

    return error_response(cmd, "unknown action");
}

static int pd_settings_channel_json(char *payload, size_t payload_len,
                                    enum photodiode_channel channel,
                                    const struct app_pd_channel_settings *ch)
{
    struct photodiode_dark_status dark = {0};
    int written;

    (void)photodiode_get_dark_status(channel, &dark);

    written = snprintk(payload, payload_len,
                       "{\"channel\":\"%s\",\"dark_mv\":%.3f,"
                       "\"lowest_dark_mv\":%.3f,"
                       "\"lowest_dark_valid\":%s,"
                       "\"dark_measurement\":\"%s\","
                       "\"dark_measurement_duration_ms\":%u,"
                       "\"dark_measurement_samples\":%u,"
                       "\"dark_measurement_target_samples\":%u,"
                       "\"noise_rms_mV\":%.3f,"
                       "\"gain_v_p_uw\":%.6f}",
                       photodiode_channel_names[channel],
                       (double)ch->dark_mv,
                       (double)ch->lowest_dark_mv,
                       ch->lowest_dark_valid ? "true" : "false",
                       photodiode_dark_state_name(dark.state),
                       dark.duration_ms,
                       dark.samples,
                       dark.target_samples,
                       (double)ch->noise_warn_rms_mv,
                       (double)ch->gain_v_per_uw);

    return (written >= 0 && written < (int)payload_len) ? 0 : -ENOSPC;
}

struct OutMsg pd_settings_get(const struct Command *cmd)
{
    struct app_photodiode_settings settings;
    char payload[MAX_PAYLOAD_LEN] = {0};
    enum photodiode_channel channel;
    int rc;

    if (devices_board_type() != HISPEC_BOARD_TIB) {
        return error_response(cmd, "photodiodes unavailable on this board");
    }

    rc = pd_parse_channel_from_key(cmd, &channel);
    if (rc != 0) {
        return error_response(cmd, "pdsettings key must be pdsettings/yj or pdsettings/hk");
    }

    app_settings_get_photodiode(&settings);
    rc = pd_settings_channel_json(payload, sizeof(payload), channel,
                                  &settings.channel[channel]);
    if (rc != 0) {
        return error_response(cmd, "pdsettings response too large");
    }

    return _msg_builder(cmd, RESP_OK, payload);
}

struct OutMsg pd_settings_set(const struct Command *cmd)
{
    struct app_photodiode_settings settings;
    struct app_pd_channel_settings channel_settings;
    enum photodiode_channel channel;
    bool persist = false;
    bool changed = false;
    int parse_rc;
    int rc;

    if (devices_board_type() != HISPEC_BOARD_TIB) {
        return error_response(cmd, "photodiodes unavailable on this board");
    }

    rc = pd_parse_channel_from_key(cmd, &channel);
    if (rc != 0) {
        return error_response(cmd, "pdsettings key must be pdsettings/yj or pdsettings/hk");
    }

    app_settings_get_photodiode(&settings);
    channel_settings = settings.channel[channel];

    parse_rc = coo_json_extract_bool(cmd->payload, "persistent", &persist);
    if (parse_rc == COO_JSON_EXTRACT_ERR) {
        return error_response(cmd, "invalid persistent");
    }

    if (coo_json_extract_optional_float_range(cmd->payload, "dark_mv",
                                              &channel_settings.dark_mv,
                                              &changed, -5000.0f, 5000.0f) != 0 ||
        coo_json_extract_optional_float_range(cmd->payload, "noise_rms_mV",
                                              &channel_settings.noise_warn_rms_mv,
                                              &changed, 0.0f, 5000.0f) != 0 ||
        coo_json_extract_optional_float_range(cmd->payload, "gain_v_p_uw",
                                              &channel_settings.gain_v_per_uw,
                                              &changed, 0.000001f, 1000000000.0f) != 0) {
        return error_response(cmd, "invalid pdsettings value");
    }

    if (!changed) {
        return error_response(cmd, "no pdsettings fields supplied");
    }

    app_settings_update_photodiode_channel((uint8_t)channel,
                                           &channel_settings,
                                           persist);
    return ok_response(cmd);
}


struct OutMsg status_get(const struct Command *cmd)
{
    struct tempsense_status ts = {0};
    struct laserbank_control_status bank = {0};
    bool include_ip = false;
    bool include_lasers = false;
    bool include_attens = false;
    char payload[MAX_PAYLOAD_LEN] = {0};
    size_t off = 0U;
    int parse_rc;

    parse_rc = coo_json_extract_bool(cmd->payload, "ip", &include_ip);
    if (parse_rc == COO_JSON_EXTRACT_ERR) {
        return error_response(cmd, "invalid ip");
    }
    parse_rc = coo_json_extract_bool(cmd->payload, "lasers", &include_lasers);
    if (parse_rc == COO_JSON_EXTRACT_ERR) {
        return error_response(cmd, "invalid lasers");
    }
    parse_rc = coo_json_extract_bool(cmd->payload, "attens", &include_attens);
    if (parse_rc == COO_JSON_EXTRACT_ERR) {
        return error_response(cmd, "invalid attens");
    }

    tempsense_get_status(&ts);
    laserbank_control_get_status(&bank);
    if (coo_json_append(payload, sizeof(payload), &off,
                        "{\"fwversion\":\"%s\",\"bootcount\":%u,"
                        "\"board_type\":\"%s\",\"board_valid\":%s,"
                        "\"mems_switches\":%u,\"relay_gpio_error\":%d,"
                        "\"temp_c\":",
                        APP_VERSION_STRING,
                        app_settings_get_boot_count(),
                        devices_board_type_name(),
                        devices_board_type() != HISPEC_BOARD_UNKNOWN ? "true" : "false",
                        router.num_switches,
                        devices_relay_gpio_last_error()) != 0 ||
        coo_json_append_float_or_null(payload, sizeof(payload), &off,
                                      ts.valid ? ts.ambient_c : NAN, 3) != 0 ||
        coo_json_append(payload, sizeof(payload), &off,
                        ",\"pd_ontime\":%.1f,"
                        "\"laserbank_ontime\":%u",
                        (double)MAX(hispec_laser_aux_power_on_time_s(HISPEC_LASER_AUX_YJ_PHOTODIODE),
                                    hispec_laser_aux_power_on_time_s(HISPEC_LASER_AUX_HK_PHOTODIODE)),
                        bank.bank_power_on_time_s) != 0) {
        return error_response(cmd, "status response too large");
    }

    if (include_ip) {
        struct OutMsg ip = ip_get(cmd);

        if (ip.msg_type != RESP_OK ||
            coo_json_append(payload, sizeof(payload), &off,
                            ",\"ip\":%s", ip.payload) != 0) {
            return error_response(cmd, "status response too large");
        }
    }

    if (include_lasers) {
        if (coo_json_append(payload, sizeof(payload), &off, ",\"lasers\":{") != 0) {
            return error_response(cmd, "status response too large");
        }
        for (uint8_t i = 0U; i < HISPEC_LASER_COUNT; ++i) {
            struct hispec_laser_status laser = {0};
            int rc = hispec_laser_get_status((enum hispec_laser_id)i, &laser);

            if (coo_json_append(payload, sizeof(payload), &off,
                                "%s\"%s\":{\"power_mw\":",
                                i == 0U ? "" : ",",
                                hispec_laser_name((enum hispec_laser_id)i)) != 0 ||
                coo_json_append_float_or_null(payload, sizeof(payload), &off,
                                              rc == 0 ? laser.estimated_power_mw : NAN, 3) != 0 ||
                coo_json_append(payload, sizeof(payload), &off,
                                ",\"tec_on_time_s\":%.1f,\"offin_s\":%lld}",
                                rc == 0 ? (double)laser.tec_on_time_s : 0.0,
                                rc == 0 ? (long long)laser.off_in_s : 0LL) != 0) {
                return error_response(cmd, "status response too large");
            }
        }
        if (coo_json_append(payload, sizeof(payload), &off, "}") != 0) {
            return error_response(cmd, "status response too large");
        }
    }

    if (include_attens) {
        bool first = true;

        if (coo_json_append(payload, sizeof(payload), &off, ",\"attens\":{") != 0) {
            return error_response(cmd, "status response too large");
        }
        for (uint8_t i = 0U; i < HISPEC_LASER_COUNT; ++i) {
            uint8_t atten_index;
            struct attenuator_status atten = {0};
            bool valid;

            if (attenuator_index_from_laser_id((enum hispec_laser_id)i, &atten_index) != 0 ||
                !attenuator_channel_available(atten_index)) {
                continue;
            }

            valid = attenuator_get(&attenuators[atten_index], &atten);

            if (coo_json_append(payload, sizeof(payload), &off,
                                "%s\"%s\":{\"level_%%\":",
                                first ? "" : ",",
                                hispec_laser_name((enum hispec_laser_id)i)) != 0 ||
                coo_json_append_float_or_null(payload, sizeof(payload), &off,
                                              valid ? atten.linear * 100.0 : (double)NAN, 3) != 0 ||
                coo_json_append(payload, sizeof(payload), &off, "}") != 0) {
                return error_response(cmd, "status response too large");
            }
            first = false;
        }
        if (coo_json_append(payload, sizeof(payload), &off, "}") != 0) {
            return error_response(cmd, "status response too large");
        }
    }

    if (coo_json_append(payload, sizeof(payload), &off,
                        ",\"lastcommand\":{\"name\":\"%s\",\"source\":\"%s\","
                        "\"time\":%lld}}",
                        last_command_name,
                        last_command_source,
                        (long long)last_command_time_ms) != 0) {
        return error_response(cmd, "status response too large");
    }

    return _msg_builder(cmd, RESP_OK, payload);
}

struct OutMsg temp_get(const struct Command *cmd)
{
    struct tempsense_status ts = {0};
    struct hispec_laser_bank_temperature_status bank = {0};
    char payload[MAX_PAYLOAD_LEN] = {0};
    size_t off = 0U;
    double bank_sum = 0.0;
    uint8_t bank_count = 0U;
    int laser_rc = 0;

    tempsense_get_status(&ts);
    if (devices_board_type() == HISPEC_BOARD_TIB) {
        laser_rc = hispec_laser_bank_read_temperatures(&bank);
        if (laser_rc == 0) {
            for (uint8_t i = 0U; i < HISPEC_LASER_COUNT; ++i) {
                if (bank.channel[i].valid) {
                    bank_sum += (double)bank.channel[i].tec_temperature_c;
                    bank_count++;
                }
            }
        }
    }

    if (coo_json_append(payload, sizeof(payload), &off,
                        "{\"ambient_c\":") != 0 ||
        coo_json_append_float_or_null(payload, sizeof(payload), &off,
                                      ts.valid ? ts.ambient_c : NAN, 3) != 0) {
        return error_response(cmd, "temp response too large");
    }

    if (coo_json_append(payload, sizeof(payload), &off,
                        ",\"laserbank_c\":") != 0 ||
        coo_json_append_float_or_null(payload, sizeof(payload), &off,
                                      bank_count > 0U ? bank_sum / (double)bank_count : (double)NAN,
                                      3) != 0 ||
        coo_json_append(payload, sizeof(payload), &off,
                        ",\"laser\":{") != 0) {
        return error_response(cmd, "temp response too large");
    }
    for (uint8_t i = 0U; i < HISPEC_LASER_COUNT; ++i) {
        if (coo_json_append(payload, sizeof(payload), &off,
                            "%s\"%s\":",
                            i == 0U ? "" : ",",
                            hispec_laser_name((enum hispec_laser_id)i)) != 0 ||
            coo_json_append_float_or_null(payload, sizeof(payload), &off,
                                          laser_rc == 0 && bank.channel[i].valid ?
                                          bank.channel[i].tec_temperature_c : NAN,
                                          3) != 0) {
            return error_response(cmd, "temp response too large");
        }
    }
    if (coo_json_append(payload, sizeof(payload), &off, "}}") != 0) {
        return error_response(cmd, "temp response too large");
    }

    return _msg_builder(cmd, RESP_OK, payload);
}
