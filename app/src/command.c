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
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
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
#include "app_identity.h"
#include "app_settings.h"
#include "app_scheduled_actions.h"
#include "app_warning.h"
#include "attenuator.h"
#include "maiman.h"
#include "mems_switching.h"
#include "photodiode.h"
#include "sntp_sync.h"
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

extern const struct gpio_dt_spec laser_power_gpio;
extern struct mems_switch mems_switches[MEMS_ROUTER_MAX_SWITCHES];
extern struct mems_router router;
// extern struct attenuator attenuators[NUM_ATTENUATORS];

struct json_value_uint16 {
    uint16_t value;
};

struct json_value_bool {
    bool value;
};


typedef enum laser_t {
    LASER_1028_Y=1,
    LASER_1270_J=1,
    LASER_1430_YJ=2,
    LASER_1430_HK=3,
    LASER_1510_H=4,
    LASER_2330_K=5,
    LASER_UNKNOWN=6
} laser_t;

static bool attenuator_channel_available(laser_t laser_id)
{
    enum hispec_board_type board = devices_board_type();

    if (board == HISPEC_BOARD_TIB) {
        return laser_id != LASER_UNKNOWN && (uint8_t)laser_id < NUM_ATTENUATORS;
    }

    if (board == HISPEC_BOARD_CAL_YJ || board == HISPEC_BOARD_CAL_HK) {
        return laser_id == LASER_1510_H;
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
    { "laserbank/poweron", laserbank_poweron, laserbank_poweron },
    { "laserbank/poweroff", laserbank_poweroff, laserbank_poweroff },
    { "laserbank/clearfaults", laserbank_clearfaults, laserbank_clearfaults },
    { "laser",      laser_setting_get,laser_setting_set},
    { "atten",      atten_setting_get,  atten_setting_set  },
    { "pdsettings", pd_settings_get, pd_settings_set },
    { "pd",         pd_get,          pd_set          },
    { "temp",       temp_get,         NULL             },
    { "status",     status_get,       NULL  },// GET only
    //todo add reset for system
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


struct OutMsg dispatch_command(const struct Command *cmd) {
    LOG_INF("Dispatching: %s", cmd->key);
    struct OutMsg r;

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



bool parse_msg_type_from_payload(const char *payload, enum MsgType *msg_type_out)
{
    enum coo_msg_type msg_type;
    if (!coo_json_parse_msg_type(payload, &msg_type)) {
        return false;
    }

    if (msg_type == COO_MSG_GET) {
        *msg_type_out = MSG_GET;
        return true;
    }
    if (msg_type == COO_MSG_SET) {
        *msg_type_out = MSG_SET;
        return true;
    }
    return false;
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
    snprintk(r.topic, sizeof(r.topic), APP_MQTT_RESP_PREFIX);
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
        static const char overflow_msg[] = "{\"status\":\"error\",\"msg\":\"response too large\"}";

        r.msg_type = RESP_ERROR;
        snprintk(r.payload, sizeof(r.payload), "%s", overflow_msg);
        r.payload_len = strlen(r.payload);
        return r;
    }

    snprintk(r.payload, sizeof(r.payload), "%s", msg != NULL ? msg : "");
    r.payload_len = strlen(r.payload);
    return r;
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
    const int n = snprintk(topic_out, topic_out_len, "%s%s", APP_MQTT_RESP_PREFIX, key);

    return n > 0 && n < (int)topic_out_len;
}

static void enqueue_serial_error(const char *msg)
{
    struct OutMsg out = {0};

    out.target = OUT_TARGET_SERIAL;
    out.msg_type = RESP_ERROR;
    snprintk(out.topic, sizeof(out.topic), APP_MQTT_RESP_PREFIX "serial");
    out.payload_len = snprintk(out.payload, sizeof(out.payload),
                               "{\"status\":\"error\",\"msg\":\"%s\"}", msg);
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
    size_t prefix_len;
    size_t suffix_len;

    if (pub == NULL || !copy_topic(&pub->message.topic.topic, req_topic, sizeof(req_topic))) {
        return;
    }

    prefix_len = strlen(APP_MQTT_CMD_PREFIX);
    if (strncmp(req_topic, APP_MQTT_CMD_PREFIX, prefix_len) != 0) {
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
        if (!parse_msg_type_from_payload(cmd.payload, &cmd.msg_type)) {
            cmd.msg_type = MSG_SET;
        }
    } else {
        cmd.msg_type = MSG_GET;
        snprintk(cmd.payload, sizeof(cmd.payload), "{}");
        cmd.payload_len = strlen(cmd.payload);
    }

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

    /* Serial syntax is one line: "<key> [payload]". There are no get/set
     * words. An empty payload is a GET; any payload is normalized to JSON and
     * dispatched as a SET through the same handlers MQTT uses.
     */
    sep = strpbrk(cursor, " \t");
    if (sep == NULL) {
        key = cursor;
        cmd.msg_type = MSG_GET;
    } else {
        *sep = '\0';
        key = cursor;
        cursor = sep + 1;
        while (*cursor == ' ' || *cursor == '\t') {
            cursor++;
        }
        payload = cursor;
        cmd.msg_type = (*payload == '\0') ? MSG_GET : MSG_SET;
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





laser_t get_laser_channel(const char *laser_name) {
    // Case-insensitive check for supported types
    if (strncasecmp(laser_name, "1028y", 7) == 0) {
        return LASER_1028_Y;
    }
    if (strncasecmp(laser_name, "1270j", 7) == 0) {
        return LASER_1270_J;
    }
    if (strncasecmp(laser_name, "1430yj", 7) == 0) {
        return LASER_1430_YJ;
    }
    if (strncasecmp(laser_name, "1430hk", 7) == 0) {
        return LASER_1430_HK;
    }
    if (strncasecmp(laser_name, "1510h", 7) == 0) {
        return LASER_1510_H;
    }
    if (strncasecmp(laser_name, "2330k", 7) == 0) {
        return LASER_2330_K;
    }

    return LASER_UNKNOWN;
}

void wait_laser_boot() {
    k_sleep(K_MSEC(1000));
}

bool power_enabled() {
    if (devices_board_type() != HISPEC_BOARD_TIB) {
        return false;
    }
    if (!gpio_is_ready_dt(&laser_power_gpio)) {
        return false;
    }
    int val = gpio_pin_get_dt(&laser_power_gpio);
    return val==1;
}

bool enable_power() {
    if (devices_board_type() != HISPEC_BOARD_TIB) {
        LOG_WRN("Laser power GPIO unavailable on board %s", devices_board_type_name());
        return false;
    }
    if (!gpio_is_ready_dt(&laser_power_gpio)) {
        LOG_ERR("POWER_GPIO not ready");
        return false;
    }
    if (power_enabled())
        return false;
    int err = gpio_pin_set_dt(&laser_power_gpio, 1);
    if (err) {
        LOG_ERR("Failed to set POWER_GPIO high\n");
    }
    return true;
}

bool disable_power() {
    if (devices_board_type() != HISPEC_BOARD_TIB) {
        LOG_WRN("Laser power GPIO unavailable on board %s", devices_board_type_name());
        return false;
    }
    if (!gpio_is_ready_dt(&laser_power_gpio)) {
        LOG_ERR("POWER_GPIO not ready");
        return false;
    }
    if (!power_enabled())
        return false;
    int err = gpio_pin_set_dt(&laser_power_gpio, 0);
    if (err) {
        LOG_ERR("Failed to set POWER_GPIO low\n");
    }
    return true;

}

struct OutMsg laserbank_poweron(const struct Command *cmd)
{
    bool transitioned;
    char payload[MAX_PAYLOAD_LEN] = {0};

    if (devices_board_type() != HISPEC_BOARD_TIB) {
        return _msg_builder(cmd, RESP_ERROR,
                            "{\"error\":\"Laser bank unavailable on this board\"}");
    }

    transitioned = enable_power();
    if (transitioned) {
        wait_laser_boot();
    }

    if (!power_enabled()) {
        return _msg_builder(cmd, RESP_ERROR,
                            "{\"status\":\"error\",\"msg\":\"laser bank power did not turn on\"}");
    }

    snprintf(payload, sizeof(payload),
             "{\"status\":\"OK\",\"laser_power\":true,\"transitioned\":%s}",
             transitioned ? "true" : "false");
    return _msg_builder(cmd, RESP_OK, payload);
}

struct OutMsg laserbank_poweroff(const struct Command *cmd)
{
    bool was_powered;
    bool transitioned;
    char payload[MAX_PAYLOAD_LEN] = {0};

    if (devices_board_type() != HISPEC_BOARD_TIB) {
        return _msg_builder(cmd, RESP_ERROR,
                            "{\"error\":\"Laser bank unavailable on this board\"}");
    }

    was_powered = power_enabled();
    transitioned = disable_power();
    if (power_enabled()) {
        return _msg_builder(cmd, RESP_ERROR,
                            "{\"status\":\"error\",\"msg\":\"laser bank power did not turn off\"}");
    }

    snprintf(payload, sizeof(payload),
             "{\"status\":\"OK\",\"laser_power\":false,\"was_powered\":%s,"
             "\"transitioned\":%s}",
             was_powered ? "true" : "false",
             transitioned ? "true" : "false");
    return _msg_builder(cmd, RESP_OK, payload);
}

struct OutMsg laserbank_clearfaults(const struct Command *cmd)
{
    bool was_powered;
    bool turned_on;
    char payload[MAX_PAYLOAD_LEN] = {0};

    if (devices_board_type() != HISPEC_BOARD_TIB) {
        return _msg_builder(cmd, RESP_ERROR,
                            "{\"error\":\"Laser bank unavailable on this board\"}");
    }

    was_powered = power_enabled();
    if (was_powered) {
        (void)disable_power();
        if (power_enabled()) {
            return _msg_builder(cmd, RESP_ERROR,
                                "{\"status\":\"error\","
                                "\"msg\":\"laser bank power cycle could not turn off\"}");
        }
    }

    /* k_sleep() is a bounded Zephyr delay that gives the laser-bank supply
     * time to drop before re-enabling it for a fault-clear cycle.
     */
    k_sleep(K_MSEC(LASERBANK_FAULT_CLEAR_OFF_MS));
    turned_on = enable_power();
    if (turned_on) {
        wait_laser_boot();
    }

    if (!power_enabled()) {
        return _msg_builder(cmd, RESP_ERROR,
                            "{\"status\":\"error\","
                            "\"msg\":\"laser bank power cycle could not turn on\"}");
    }

    snprintf(payload, sizeof(payload),
             "{\"status\":\"OK\",\"laser_power\":true,\"was_powered\":%s,"
             "\"off_ms\":%u,\"fault_detection\":\"power_cycle_only\"}",
             was_powered ? "true" : "false",
             LASERBANK_FAULT_CLEAR_OFF_MS);
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
                        "memsroute,mems,split,laser,laserbank,atten,pd,pdsettings\"}");
}

struct OutMsg ip_get(const struct Command *cmd)
{
    struct app_ip_settings ip_cfg;
    struct network_ipv4_info net = {0};
    struct sntp_sync_status sntp = {0};
    char payload[MAX_PAYLOAD_LEN];

    app_settings_get_ip(&ip_cfg);
    (void)network_get_ipv4_info(&net);
    sntp_sync_get_status(&sntp);

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
             sntp_sync_source_str(sntp.source),
             sntp.server);

    return _msg_builder(cmd, RESP_OK, payload);
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
            return _msg_builder(cmd, RESP_ERROR, "{\"status\":\"error\",\"msg\":\"invalid trydhcpfirst\"}");
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
            return _msg_builder(cmd, RESP_ERROR, "{\"status\":\"error\",\"msg\":\"invalid preferdhcpdns\"}");
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
            return _msg_builder(cmd, RESP_ERROR, "{\"status\":\"error\",\"msg\":\"invalid preferdhcpntp\"}");
        }
    }

    parse_rc = coo_json_extract_string(cmd->payload, "ip", buf, sizeof(buf));
    if (parse_rc == COO_JSON_EXTRACT_OK) {
        strncpy(ip_cfg.ip, buf, sizeof(ip_cfg.ip) - 1);
        ip_cfg.ip[sizeof(ip_cfg.ip) - 1] = '\0';
        changed = true;
        network_changed = true;
    } else if (parse_rc == COO_JSON_EXTRACT_ERR) {
        return _msg_builder(cmd, RESP_ERROR, "{\"status\":\"error\",\"msg\":\"invalid ip\"}");
    }

    parse_rc = coo_json_extract_string(cmd->payload, "subnet", buf, sizeof(buf));
    if (parse_rc == COO_JSON_EXTRACT_OK) {
        strncpy(ip_cfg.subnet, buf, sizeof(ip_cfg.subnet) - 1);
        ip_cfg.subnet[sizeof(ip_cfg.subnet) - 1] = '\0';
        changed = true;
        network_changed = true;
    } else if (parse_rc == COO_JSON_EXTRACT_ERR) {
        return _msg_builder(cmd, RESP_ERROR, "{\"status\":\"error\",\"msg\":\"invalid subnet\"}");
    }

    parse_rc = coo_json_extract_string(cmd->payload, "gateway", buf, sizeof(buf));
    if (parse_rc == COO_JSON_EXTRACT_OK) {
        strncpy(ip_cfg.gateway, buf, sizeof(ip_cfg.gateway) - 1);
        ip_cfg.gateway[sizeof(ip_cfg.gateway) - 1] = '\0';
        changed = true;
        network_changed = true;
    } else if (parse_rc == COO_JSON_EXTRACT_ERR) {
        return _msg_builder(cmd, RESP_ERROR, "{\"status\":\"error\",\"msg\":\"invalid gateway\"}");
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
            return _msg_builder(cmd, RESP_ERROR, "{\"status\":\"error\",\"msg\":\"invalid dns\"}");
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
            return _msg_builder(cmd, RESP_ERROR, "{\"status\":\"error\",\"msg\":\"invalid ntp\"}");
        }
    }

    parse_rc = coo_json_extract_bool(cmd->payload, "persistent", &persist);
    if (parse_rc == COO_JSON_EXTRACT_ERR) {
        return _msg_builder(cmd, RESP_ERROR, "{\"status\":\"error\",\"msg\":\"invalid persistent\"}");
    }

    if (!changed && !(unsupported_dhcp || unsupported_dns || unsupported_ntp)) {
        return _msg_builder(cmd, RESP_ERROR, "{\"status\":\"error\",\"msg\":\"no recognized ip fields\"}");
    }

    if (changed) {
        app_settings_update_ip(&ip_cfg, persist);
        if (ntp_changed && ntp_supported) {
            sntp_sync_schedule_now();
        }
    }

    if (unsupported_dhcp || unsupported_dns || unsupported_ntp) {
        const char *apply =
            network_changed ? "reboot_required" :
            (ntp_changed ? "immediate" : "none");
        snprintk(response, sizeof(response),
                 "{\"status\":\"partial\",\"dhcp\":\"%s\",\"dns\":\"%s\",\"ntp\":\"%s\",\"apply\":\"%s\"}",
                 unsupported_dhcp ? "unsupported" : "ok",
                 unsupported_dns ? "unsupported" : "ok",
                 unsupported_ntp ? "unsupported" : "ok",
                 apply);
        return _msg_builder(cmd, RESP_OK, response);
    }

    if (network_changed) {
        return _msg_builder(cmd, RESP_OK, "{\"status\":\"success\",\"apply\":\"reboot_required\"}");
    }

    if (ntp_changed) {
        return _msg_builder(cmd, RESP_OK, "{\"status\":\"success\",\"apply\":\"immediate\"}");
    }

    return _msg_builder(cmd, RESP_OK, "{\"status\":\"success\"}");
}

static bool mqtt_host_is_numeric_ipv4(const char *host)
{
    struct in_addr addr = {0};

    if (host == NULL || host[0] == '\0') {
        return false;
    }

    return net_addr_pton(AF_INET, host, &addr) == 0;
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
    bool persist = false;
    int parse_rc;
#if defined(CONFIG_DNS_RESOLVER)
    const bool dns_supported = true;
#else
    const bool dns_supported = false;
#endif

    app_settings_get_mqtt(&mqtt_cfg);

    parse_rc = coo_json_extract_string(cmd->payload, "broker", endpoint, sizeof(endpoint));
    if (parse_rc == COO_JSON_EXTRACT_MISSING) {
        return _msg_builder(cmd, RESP_ERROR, "{\"status\":\"error\",\"msg\":\"missing broker\"}");
    }
    if (parse_rc == COO_JSON_EXTRACT_ERR) {
        return _msg_builder(cmd, RESP_ERROR, "{\"status\":\"error\",\"msg\":\"invalid broker\"}");
    }
    if (!coo_mqtt_parse_broker_endpoint(endpoint, &broker_cfg)) {
        return _msg_builder(cmd, RESP_ERROR,
                            "{\"status\":\"error\",\"msg\":\"broker must be host-or-ip:port\"}");
    }
    if (!mqtt_host_is_numeric_ipv4(broker_cfg.host) && !dns_supported) {
        return _msg_builder(cmd, RESP_ERROR,
                            "{\"status\":\"error\",\"msg\":\"broker hostname requires DNS\"}");
    }
    strncpy(mqtt_cfg.broker_host, broker_cfg.host, sizeof(mqtt_cfg.broker_host) - 1U);
    mqtt_cfg.broker_host[sizeof(mqtt_cfg.broker_host) - 1U] = '\0';
    mqtt_cfg.broker_port = broker_cfg.port;

    parse_rc = coo_json_extract_bool(cmd->payload, "persistent", &persist);
    if (parse_rc == COO_JSON_EXTRACT_ERR) {
        return _msg_builder(cmd, RESP_ERROR, "{\"status\":\"error\",\"msg\":\"invalid persistent\"}");
    }

    app_settings_update_mqtt(&mqtt_cfg, persist);
    return _msg_builder(cmd, RESP_OK, "{\"status\":\"success\",\"apply\":\"reconnect\"}");
}

struct OutMsg time_get(const struct Command *cmd)
{
    struct timespec ts = {0};
    struct sntp_sync_status sntp = {0};
    uint64_t utc_ms;
    char payload[MAX_PAYLOAD_LEN];

    clock_gettime(CLOCK_REALTIME, &ts);
    sntp_sync_get_status(&sntp);
    utc_ms = ((uint64_t)ts.tv_sec * 1000ULL) + ((uint64_t)ts.tv_nsec / 1000000ULL);

    snprintk(payload, sizeof(payload),
             "{\"utc\":%llu,\"ticks\":%u,\"uptime\":%lld,"
             "\"ntp\":{\"source\":\"%s\",\"server\":\"%s\",\"synced\":%s,"
             "\"last_sync_utc\":%llu,\"last_error\":%d}}",
             (unsigned long long)utc_ms, k_cycle_get_32(), (long long)k_uptime_get(),
             sntp_sync_source_str(sntp.source),
             sntp.server,
             sntp.synced ? "true" : "false",
             (unsigned long long)sntp.last_sync_utc_ms,
             sntp.last_error);

    return _msg_builder(cmd, RESP_OK, payload);
}

struct OutMsg time_set(const struct Command *cmd)
{
    uint64_t utc_ms = 0;
    struct timespec ts = {0};
    int parse_rc;

    parse_rc = coo_json_extract_u64(cmd->payload, "linuxtime_ms", &utc_ms);
    if (parse_rc == COO_JSON_EXTRACT_MISSING) {
        return _msg_builder(cmd, RESP_ERROR, "{\"status\":\"error\",\"msg\":\"missing linuxtime_ms\"}");
    }
    if (parse_rc == COO_JSON_EXTRACT_ERR) {
        return _msg_builder(cmd, RESP_ERROR, "{\"status\":\"error\",\"msg\":\"invalid linuxtime_ms\"}");
    }

    ts.tv_sec = utc_ms / 1000ULL;
    ts.tv_nsec = (utc_ms % 1000ULL) * 1000000ULL;

    if (clock_settime(CLOCK_REALTIME, &ts) != 0) {
        return _msg_builder(cmd, RESP_ERROR, "{\"status\":\"error\",\"msg\":\"clock_settime failed\"}");
    }

    return _msg_builder(cmd, RESP_OK, "{\"status\":\"success\"}");
}

struct OutMsg reboot_set(const struct Command *cmd)
{
    int rc;

    rc = app_scheduled_action_schedule(APP_SCHEDULED_ACTION_REBOOT, K_MSEC(250));
    if (rc < 0) {
        return _msg_builder(cmd, RESP_ERROR,
                            "{\"status\":\"error\",\"msg\":\"failed to schedule reboot\"}");
    }

    return _msg_builder(cmd, RESP_OK, "{\"status\":\"success\"}");
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
        return _msg_builder(cmd, RESP_ERROR, "{\"status\":\"error\",\"msg\":\"invalid seconds\"}");
    }
    if (parse_rc_seconds == COO_JSON_EXTRACT_MISSING &&
        parse_rc_value == COO_JSON_EXTRACT_MISSING) {
        return _msg_builder(cmd, RESP_ERROR, "{\"status\":\"error\",\"msg\":\"missing seconds\"}");
    }

    parse_rc_persist = coo_json_extract_bool(cmd->payload, "persistent", &persist);
    if (parse_rc_persist == COO_JSON_EXTRACT_ERR) {
        return _msg_builder(cmd, RESP_ERROR, "{\"status\":\"error\",\"msg\":\"invalid persistent\"}");
    }
    app_settings_set_serial_holdoff_s(holdoff_s, persist);
    if (cmd->source == CMD_SRC_SERIAL) {
        command_serial_note_activity();
    }
    return _msg_builder(cmd, RESP_OK, "{\"status\":\"success\"}");
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

static int memsroute_append_json(char *buf, size_t buf_len, size_t *offset,
                                 const char *fmt, ...)
{
    va_list args;
    int written;

    if (buf == NULL || offset == NULL || *offset >= buf_len) {
        return -ENOSPC;
    }

    va_start(args, fmt);
    written = vsnprintk(buf + *offset, buf_len - *offset, fmt, args);
    va_end(args);

    if (written < 0 || written >= (int)(buf_len - *offset)) {
        return -ENOSPC;
    }

    *offset += (size_t)written;
    return 0;
}

static int memsroute_append_sources_for_output(char *buf, size_t buf_len, size_t *offset,
                                              const struct mems_route_key *active,
                                              uint8_t active_count,
                                              const char *output_name)
{
    uint8_t n_sources = 0U;

    if (memsroute_append_json(buf, buf_len, offset, "\"%s\":[", output_name) != 0) {
        return -ENOSPC;
    }

    for (uint8_t i = 0U; i < active_count; ++i) {
        if (strcmp(active[i].output_name, output_name) != 0) {
            continue;
        }

        if (n_sources > 0U && memsroute_append_json(buf, buf_len, offset, ",") != 0) {
            return -ENOSPC;
        }
        if (memsroute_append_json(buf, buf_len, offset, "\"%s\"",
                                  active[i].input_name) != 0) {
            return -ENOSPC;
        }
        n_sources++;
    }

    if (n_sources == 0U &&
        memsroute_append_json(buf, buf_len, offset, "\"no source\"") != 0) {
        return -ENOSPC;
    }

    return memsroute_append_json(buf, buf_len, offset, "]");
}

struct OutMsg memsroute_get(const struct Command *cmd)
{
    struct mems_route_key active[MEMS_ROUTER_MAX_ROUTES];
    const char *outputs[MEMS_ROUTER_MAX_ROUTES];
    uint8_t n_active = mems_router_active_routes(&router, active, MEMS_ROUTER_MAX_ROUTES);
    uint8_t n_outputs = 0U;
    char buf[MAX_PAYLOAD_LEN] = {0};
    size_t offset = 0;

    if (memsroute_append_json(buf, sizeof(buf), &offset, "{\"active_routes\":{") != 0) {
        return _msg_builder(cmd, RESP_ERROR, "{\"status\":\"error\",\"msg\":\"response too large\"}");
    }

    for (uint8_t i = 0U; router.routes != NULL && i < router.num_routes; ++i) {
        const char *output_name = router.routes[i].key.output_name;

        if (memsroute_output_seen(outputs, n_outputs, output_name)) {
            continue;
        }

        outputs[n_outputs++] = output_name;
        if (n_outputs > 1U &&
            memsroute_append_json(buf, sizeof(buf), &offset, ",") != 0) {
            return _msg_builder(cmd, RESP_ERROR, "{\"status\":\"error\",\"msg\":\"response too large\"}");
        }
        if (memsroute_append_sources_for_output(buf, sizeof(buf), &offset,
                                                active, n_active, output_name) != 0) {
            return _msg_builder(cmd, RESP_ERROR, "{\"status\":\"error\",\"msg\":\"response too large\"}");
        }
    }

    if (memsroute_append_json(buf, sizeof(buf), &offset, "}}") != 0) {
        return _msg_builder(cmd, RESP_ERROR, "{\"status\":\"error\",\"msg\":\"response too large\"}");
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
             "{\"status\":\"success\",\"channel\":\"%s\","
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
        return _msg_builder(cmd, RESP_ERROR,
                            "{\"status\":\"error\",\"msg\":\"split response too large\"}");
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
        return _msg_builder(cmd, RESP_ERROR,
                            "{\"status\":\"error\",\"msg\":\"channel required: yj or hk\"}");
    }

    route = split_route_for_channel(channel_index);
    if (route == NULL) {
        return _msg_builder(cmd, RESP_ERROR,
                            "{\"status\":\"error\",\"msg\":\"split route unavailable\"}");
    }

    rc = split_read_channel_state(channel_index, route, NULL);
    if (rc != 0) {
        return _msg_builder(cmd, RESP_ERROR,
                            "{\"status\":\"error\",\"msg\":\"split route invalid\"}");
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
        return _msg_builder(cmd, RESP_ERROR,
                            "{\"status\":\"error\",\"msg\":\"channel must be yj or hk\"}");
    }

    parse_rc = coo_json_extract_float(cmd->payload, "ratio1", &requested[0]);
    if (parse_rc == COO_JSON_EXTRACT_MISSING) {
        return _msg_builder(cmd, RESP_ERROR, "{\"status\":\"error\",\"msg\":\"missing ratio1\"}");
    }
    if (parse_rc == COO_JSON_EXTRACT_ERR) {
        return _msg_builder(cmd, RESP_ERROR, "{\"status\":\"error\",\"msg\":\"invalid ratio1\"}");
    }

    parse_rc = coo_json_extract_float(cmd->payload, "ratio2", &requested[1]);
    if (parse_rc == COO_JSON_EXTRACT_MISSING) {
        return _msg_builder(cmd, RESP_ERROR, "{\"status\":\"error\",\"msg\":\"missing ratio2\"}");
    }
    if (parse_rc == COO_JSON_EXTRACT_ERR) {
        return _msg_builder(cmd, RESP_ERROR, "{\"status\":\"error\",\"msg\":\"invalid ratio2\"}");
    }

    parse_rc = coo_json_extract_float(cmd->payload, "ratio3", &ratio3_probe);
    if (parse_rc != COO_JSON_EXTRACT_MISSING) {
        return _msg_builder(cmd, RESP_ERROR,
                            "{\"status\":\"error\",\"msg\":\"ratio3 is computed internally\"}");
    }

    if (requested[0] < 0.0f || requested[0] > 1.0f ||
        requested[1] < 0.0f || requested[1] > 1.0f ||
        requested[0] + requested[1] > 1.000001f) {
        return _msg_builder(cmd, RESP_ERROR,
                            "{\"status\":\"error\",\"msg\":\"ratios must be 0.0-1.0 and sum <= 1.0\"}");
    }
    requested[2] = 1.0f - requested[0] - requested[1];

    parse_rc = coo_json_extract_u32(cmd->payload, "stopafter_s", &stopafter_s);
    if (parse_rc == COO_JSON_EXTRACT_ERR ||
        stopafter_s > MEMS_SWITCH_MAX_TOGGLE_DURATION_S) {
        return _msg_builder(cmd, RESP_ERROR, "{\"status\":\"error\",\"msg\":\"invalid stopafter_s\"}");
    }

    parse_rc = coo_json_extract_float(cmd->payload, "toggle_rate_hz", &ratio3_probe);
    if (parse_rc != COO_JSON_EXTRACT_MISSING) {
        return _msg_builder(cmd, RESP_ERROR,
                            "{\"status\":\"error\",\"msg\":\"toggle_rate_hz is automatic\"}");
    }

    route = split_route_for_channel(channel_index);
    if (route == NULL || route->num_steps != SPLIT_ROUTE_SWITCH_COUNT) {
        return _msg_builder(cmd, RESP_ERROR,
                            "{\"status\":\"error\",\"msg\":\"split route unavailable\"}");
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
            return _msg_builder(cmd, RESP_ERROR,
                                "{\"status\":\"error\",\"msg\":\"split route references missing switch\"}");
        }

        rc = mems_switch_set_state_ticks(sw, step->state, switch_ticks[i],
                                         period_ticks,
                                         i == 1U ? 0U : stopafter_s);
        if (rc != 0) {
            char payload[MAX_PAYLOAD_LEN];

            snprintk(payload, sizeof(payload),
                     "{\"status\":\"error\",\"msg\":\"failed setting %s\"}",
                     step->switch_name);
            return _msg_builder(cmd, RESP_ERROR, payload);
        }
    }

    rc = split_read_channel_state(channel_index, route, requested);
    if (rc != 0) {
        return _msg_builder(cmd, RESP_ERROR,
                            "{\"status\":\"error\",\"msg\":\"split readback failed\"}");
    }

    k_mutex_lock(&split_state_lock, K_FOREVER);
    stored = g_split_state[channel_index];
    k_mutex_unlock(&split_state_lock);
    split_emit_quantization_warning(channel_index, &stored);

    return split_channel_response(cmd, channel_index);
}

struct OutMsg memsroute_set(const struct Command *cmd) {

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
            //TODO this should be an impossible error if compiled code is correct
            LOG_ERR("Internal route error: Switch %s not found\n", step->switch_name);
            return _msg_builder(cmd, RESP_ERROR, "{\"error\":\"Internal route error\"}");
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

    return _msg_builder(cmd, RESP_OK, "{\"status\":\"OK\"}");
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
                               "\"%s\":{\"state\":\"%s\",\"duty_cycle\":%.3f,"
                               "\"requested_toggle_rate_hz\":%.3f,\"toggle_rate_hz\":%.3f,"
                               "\"stopafter_s\":%u}",
                               router.switches[i]->name,
                               state_buf,
                               (double)status.duty_cycle,
                               (double)status.requested_toggle_rate_hz,
                               (double)status.toggle_rate_hz,
                               status.stopafter_s);
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

struct OutMsg laser_setting_get(const struct Command *cmd) {

    if (devices_board_type() != HISPEC_BOARD_TIB) {
        return _msg_builder(cmd, RESP_ERROR,
                            "{\"error\":\"Laser bank unavailable on this board\"}");
    }

    // Extract laser### and <setting> from key
    char laser_name[16], setting[16];
    if (parse_key_pair(cmd->key, laser_name, 15, setting, 15)!=0) {
        return _msg_builder(cmd, RESP_ERROR,"{\"error\":\"Failed to parse laser/setting\"}");
    }

    maiman_driver_t driver;
    driver.node_id=get_laser_channel(laser_name+5);
    if (driver.node_id==LASER_UNKNOWN) {
        return _msg_builder(cmd, RESP_ERROR,"{\"error\":\"Invalid laser\"}");
    }

    laser_address_t addr;;
    if (!maiman_get_register_address(setting, &addr)) {
        return _msg_builder(cmd, RESP_ERROR,"{\"error\":\"Invalid laser setting\"}");
    }

    if (enable_power()) {
        wait_laser_boot();
    }

    uint16_t value = 0;
    if (!maiman_read_u16(&driver, addr, &value)) {
        return _msg_builder(cmd, RESP_ERROR,"{\"error\":\"get_driver_setting failed\"}");
    }

    char payload[MAX_PAYLOAD_LEN]={0};
    snprintf(payload, MAX_PAYLOAD_LEN, "{\"%s\":%hd}", setting, value);
    return _msg_builder(cmd, RESP_OK, payload);
}

struct OutMsg laser_setting_set(const struct Command *cmd) {

    if (devices_board_type() != HISPEC_BOARD_TIB) {
        return _msg_builder(cmd, RESP_ERROR,
                            "{\"error\":\"Laser bank unavailable on this board\"}");
    }

    // Extract laser### and <setting>
    char laser_name[16], setting[16];
    if (parse_key_pair(cmd->key, laser_name, 15, setting, 15)!=0) {
        return _msg_builder(cmd, RESP_ERROR,"{\"error\":\"Failed to parse laser/setting\"}");
    }

    // Parse value
    struct json_value_uint16 in_data = {0};
    struct json_obj_descr d[] = {
        JSON_OBJ_DESCR_PRIM(struct json_value_uint16, value, JSON_TOK_NUMBER)
    };
    if (json_obj_parse((char *) cmd->payload, cmd->payload_len, d, 1, &in_data) < 0) {
        return _msg_builder(cmd, RESP_ERROR,"{\"error\":\"Missing setting value\"}");
    }

    maiman_driver_t driver;
    driver.node_id=get_laser_channel(laser_name+5);
    if (driver.node_id==LASER_UNKNOWN) {
        return _msg_builder(cmd, RESP_ERROR,"{\"error\":\"Invalid laser\"}");
    }

    laser_address_t addr;;
    if (!maiman_get_register_address(setting, &addr)) {
        return _msg_builder(cmd, RESP_ERROR,"{\"error\":\"Invalid laser setting\"}");
    }

    if (enable_power()) {
        wait_laser_boot();
    }

    if (!maiman_write_u16(&driver, addr, in_data.value) ) {
        return _msg_builder(cmd, RESP_ERROR,"{\"error\":\"set_driver_setting failed\"}");
    }

    return _msg_builder(cmd, RESP_OK, "{\"status\":\"OK\"}");
}


struct OutMsg atten_setting_get(const struct Command *cmd) {

    char laser_name[16], setting[16];
    if (parse_atten_key(cmd->key, laser_name, sizeof(laser_name),
                        setting, sizeof(setting)) != 0) {
        return _msg_builder(cmd, RESP_ERROR,"{\"error\":\"Failed to parse atten/setting\"}");
    }

    laser_t laser_id = get_laser_channel(laser_name);

    if (laser_id==LASER_UNKNOWN) {
        return _msg_builder(cmd, RESP_ERROR,"{\"error\":\"Invalid attenuator\"}");
    }
    if (!attenuator_channel_available(laser_id)) {
        return _msg_builder(cmd, RESP_ERROR,
                            "{\"error\":\"Attenuator unavailable on this board\"}");
    }

    char payload[MAX_PAYLOAD_LEN]={0};
    if (strcasecmp(setting, "coeff") == 0) {
        snprintf(payload, MAX_PAYLOAD_LEN,
                 "{\"db2volt\":[%.4f,%.4f,%.4f],\"volt2db\":[%.4f,%.4f,%.4f]}",
                 attenuators[laser_id].coeff_db_to_volt[0],
                 attenuators[laser_id].coeff_db_to_volt[1],
                 attenuators[laser_id].coeff_db_to_volt[2],
                 attenuators[laser_id].coeff_volt_to_db[0],
                 attenuators[laser_id].coeff_volt_to_db[1],
                 attenuators[laser_id].coeff_volt_to_db[2]);
    } else if (strcasecmp(setting, "value") == 0 || strcasecmp(setting, "valuedb") == 0) {
        double db, voltage;
        attenuator_get(&attenuators[laser_id], &db, false);
        attenuator_get(&attenuators[laser_id], &voltage, true);
        snprintf(payload, MAX_PAYLOAD_LEN, "{\"voltage\":%.4f,\"db\":%.4f}", voltage, db);
    } else {
        return _msg_builder(cmd, RESP_ERROR,"{\"error\":\"Invalid setting\"}");
    }

    return _msg_builder(cmd, RESP_OK, payload);
}

struct OutMsg atten_setting_set(const struct Command *cmd) {

    char laser_name[16], setting[16];
    char payload[64] = "{\"status\":\"OK\"}";
    if (parse_atten_key(cmd->key, laser_name, sizeof(laser_name),
                        setting, sizeof(setting)) != 0) {
        return _msg_builder(cmd, RESP_ERROR,"{\"error\":\"Failed to parse laser/setting\"}");
    }

    laser_t laser_id = get_laser_channel(laser_name);

    if (laser_id==LASER_UNKNOWN) {
        return _msg_builder(cmd, RESP_ERROR,"{\"error\":\"Invalid attenuator\"}");
    }
    if (!attenuator_channel_available(laser_id)) {
        return _msg_builder(cmd, RESP_ERROR,
                            "{\"error\":\"Attenuator unavailable on this board\"}");
    }

    // Parse value
    struct coeffs {
        float db2volt[ATTENUATOR_COEFF_COUNT];
        size_t db2volt_len;
        float volt2db[ATTENUATOR_COEFF_COUNT];
        size_t volt2db_len;
    };

    // Parse value
    struct json_value_float {
        float value;
    };

    if (strcasecmp(setting, "coeff") == 0) {

        struct coeffs parsed_coeffs = {0};
        struct app_attenuator_channel_settings stored_coeffs = {0};
        bool persist = false;
        int parse_rc;

        const struct json_obj_descr coeff_descr[] = {
            JSON_OBJ_DESCR_ARRAY(struct coeffs, db2volt, ATTENUATOR_COEFF_COUNT,
                                 db2volt_len, JSON_TOK_FLOAT),
            JSON_OBJ_DESCR_ARRAY(struct coeffs, volt2db, ATTENUATOR_COEFF_COUNT,
                                 volt2db_len, JSON_TOK_FLOAT),
        };

        if (json_obj_parse((char *) cmd->payload, cmd->payload_len, coeff_descr,
                                 ARRAY_SIZE(coeff_descr), &parsed_coeffs) < 0 ||
                                 parsed_coeffs.db2volt_len != ATTENUATOR_COEFF_COUNT ||
                                 parsed_coeffs.volt2db_len != ATTENUATOR_COEFF_COUNT) {
            return _msg_builder(cmd, RESP_ERROR,"{\"error\":\"Improper arguments\"}");
        }
        parse_rc = coo_json_extract_bool(cmd->payload, "persistent", &persist);
        if (parse_rc == COO_JSON_EXTRACT_ERR) {
            return _msg_builder(cmd, RESP_ERROR,"{\"error\":\"Invalid persistent flag\"}");
        }

        double db;
        attenuator_get(&attenuators[laser_id], &db, false);

        for (uint8_t i=0; i<ATTENUATOR_COEFF_COUNT; i++) {
            attenuators[laser_id].coeff_db_to_volt[i]=parsed_coeffs.db2volt[i];
            attenuators[laser_id].coeff_volt_to_db[i]=parsed_coeffs.volt2db[i];
            stored_coeffs.db_to_volt[i]=parsed_coeffs.db2volt[i];
            stored_coeffs.volt_to_db[i]=parsed_coeffs.volt2db[i];
        }

        attenuator_set(&attenuators[laser_id], db, false);
        app_settings_update_attenuator_channel((uint8_t)laser_id, &stored_coeffs, persist);
        snprintf(payload, sizeof(payload), "{\"status\":\"OK\",\"persistent\":%s}",
                 persist ? "true" : "false");

    } else if (strcasecmp(setting, "value") == 0 || strcasecmp(setting, "valuedb") == 0) {

        struct json_value_float in_data = {0};
        struct json_obj_descr d[] = {
            JSON_OBJ_DESCR_PRIM(struct json_value_float, value, JSON_TOK_NUMBER)
        };
        if (json_obj_parse((char *) cmd->payload, cmd->payload_len, d, 1, &in_data) < 0) {
            return _msg_builder(cmd, RESP_ERROR,"{\"error\":\"Missing setting value\"}");
        }

        bool raw_voltage = (strcasecmp(setting, "value") == 0);
        attenuator_set(&attenuators[laser_id], in_data.value, raw_voltage);

    } else {
        return _msg_builder(cmd, RESP_ERROR,"{\"error\":\"Invalid setting\"}");
    }

    return _msg_builder(cmd, RESP_OK, payload);
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
    char unit[12] = "power";
    char payload[MAX_PAYLOAD_LEN] = {0};
    float yj_value;
    float hk_value;
    float yj_err;
    float hk_err;
    int parse_rc;

    if (devices_board_type() != HISPEC_BOARD_TIB) {
        return _msg_builder(cmd, RESP_ERROR,
                            "{\"status\":\"error\",\"msg\":\"photodiodes unavailable on this board\"}");
    }

    parse_rc = coo_json_extract_string(cmd->payload, "unit", unit, sizeof(unit));
    if (parse_rc == COO_JSON_EXTRACT_ERR) {
        return _msg_builder(cmd, RESP_ERROR, "{\"status\":\"error\",\"msg\":\"invalid unit\"}");
    }
    if (strcasecmp(unit, "power") != 0 && strcasecmp(unit, "volts") != 0) {
        return _msg_builder(cmd, RESP_ERROR,
                            "{\"status\":\"error\",\"msg\":\"unit must be power or volts\"}");
    }

    photodiode_get_status(&status);

    if (strcasecmp(unit, "volts") == 0) {
        yj_value = status.channel[PHOTODIODE_CHANNEL_YJ].mv / 1000.0f;
        hk_value = status.channel[PHOTODIODE_CHANNEL_HK].mv / 1000.0f;
        yj_err = status.channel[PHOTODIODE_CHANNEL_YJ].noise_rms_mv / 1000.0f;
        hk_err = status.channel[PHOTODIODE_CHANNEL_HK].noise_rms_mv / 1000.0f;
        snprintk(unit, sizeof(unit), "volts");
    } else {
        yj_value = status.channel[PHOTODIODE_CHANNEL_YJ].power_uw;
        hk_value = status.channel[PHOTODIODE_CHANNEL_HK].power_uw;

        struct app_photodiode_settings settings;
        float yj_gain;
        float hk_gain;

        app_settings_get_photodiode(&settings);
        yj_gain = settings.channel[PHOTODIODE_CHANNEL_YJ].gain_v_per_uw;
        hk_gain = settings.channel[PHOTODIODE_CHANNEL_HK].gain_v_per_uw;

        yj_err = (yj_gain > 0.0f) ?
            status.channel[PHOTODIODE_CHANNEL_YJ].noise_rms_mv / (yj_gain * 1000.0f) :
            0.0f;

        hk_err = (hk_gain > 0.0f) ?
            status.channel[PHOTODIODE_CHANNEL_HK].noise_rms_mv / (hk_gain * 1000.0f) :
            0.0f;

        snprintk(unit, sizeof(unit), "power");
    }

    snprintk(payload, sizeof(payload),
             "{\"unit\":\"%s\",\"yjvalue\":%.6f,\"yjvalue_err\":%.6f,"
             "\"hkvalue\":%.6f,\"hkvalue_err\":%.6f,"
             "\"yj_raw\":%d,\"hk_raw\":%d,\"yj_mv\":%.3f,\"hk_mv\":%.3f,"
             "\"yj_noise_rms_mv\":%.3f,\"hk_noise_rms_mv\":%.3f,"
             "\"uptime\":%lld}",
             unit,
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
                 "{\"status\":\"%s\",\"channel\":\"%s\",\"stored\":%s,"
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
                 "{\"status\":\"error\",\"channel\":\"%s\",\"rc\":%d,"
                 "\"duration_ms\":%u,\"samples\":%u,\"target_samples\":%u}",
                 photodiode_channel_names[status->channel],
                 status->last_error,
                 status->duration_ms,
                 status->samples,
                 status->target_samples);
        return _msg_builder(cmd, RESP_ERROR, payload);
    }

    snprintk(payload, sizeof(payload),
             "{\"status\":\"%s\",\"channel\":\"%s\",\"stored_on_complete\":%s,"
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
        return _msg_builder(cmd, RESP_ERROR,
                            "{\"status\":\"error\",\"msg\":\"photodiodes unavailable on this board\"}");
    }

    parse_rc = coo_json_extract_string(cmd->payload, "action", action, sizeof(action));
    if (parse_rc == COO_JSON_EXTRACT_MISSING) {
        return _msg_builder(cmd, RESP_ERROR, "{\"status\":\"error\",\"msg\":\"missing action\"}");
    }
    if (parse_rc == COO_JSON_EXTRACT_ERR) {
        return _msg_builder(cmd, RESP_ERROR, "{\"status\":\"error\",\"msg\":\"invalid action\"}");
    }

    rc = pd_parse_channel_from_payload_or_key(cmd, &channel);
    if (rc != 0) {
        return _msg_builder(cmd, RESP_ERROR,
                            "{\"status\":\"error\",\"msg\":\"channel must be yj or hk\"}");
    }

    if (strcasecmp(action, "measure_dark") == 0) {
        struct photodiode_dark_status status;

        parse_rc = coo_json_extract_u32(cmd->payload, "duration_ms", &duration_ms);
        if (parse_rc == COO_JSON_EXTRACT_ERR) {
            return _msg_builder(cmd, RESP_ERROR,
                                "{\"status\":\"error\",\"msg\":\"invalid duration_ms\"}");
        }

        parse_rc = coo_json_extract_bool(cmd->payload, "store", &store);
        if (parse_rc == COO_JSON_EXTRACT_ERR) {
            return _msg_builder(cmd, RESP_ERROR,
                                "{\"status\":\"error\",\"msg\":\"invalid store\"}");
        }

        rc = photodiode_start_dark_measurement(channel, duration_ms, store, &status);
        if (rc != 0) {
            char payload[MAX_PAYLOAD_LEN];

            snprintk(payload, sizeof(payload),
                     "{\"status\":\"error\",\"msg\":\"dark measurement failed\",\"rc\":%d}",
                     rc);
            return _msg_builder(cmd, RESP_ERROR, payload);
        }
        return pd_dark_status_response(cmd, &status);
    }

    if (strcasecmp(action, "dark_status") == 0) {
        struct photodiode_dark_status status;

        rc = photodiode_get_dark_status(channel, &status);
        if (rc != 0) {
            return _msg_builder(cmd, RESP_ERROR,
                                "{\"status\":\"error\",\"msg\":\"dark status unavailable\"}");
        }

        return pd_dark_status_response(cmd, &status);
    }

    if (strcasecmp(action, "reset_lowest_dark") == 0) {
        parse_rc = coo_json_extract_bool(cmd->payload, "persistent", &persist);
        if (parse_rc == COO_JSON_EXTRACT_ERR) {
            return _msg_builder(cmd, RESP_ERROR,
                                "{\"status\":\"error\",\"msg\":\"invalid persistent\"}");
        }

        rc = photodiode_reset_lowest_dark(channel, persist);
        if (rc != 0) {
            return _msg_builder(cmd, RESP_ERROR,
                                "{\"status\":\"error\",\"msg\":\"reset failed\"}");
        }
        return _msg_builder(cmd, RESP_OK, "{\"status\":\"success\"}");
    }

    return _msg_builder(cmd, RESP_ERROR, "{\"status\":\"error\",\"msg\":\"unknown action\"}");
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
        return _msg_builder(cmd, RESP_ERROR,
                            "{\"status\":\"error\",\"msg\":\"photodiodes unavailable on this board\"}");
    }

    rc = pd_parse_channel_from_key(cmd, &channel);
    if (rc != 0) {
        return _msg_builder(cmd, RESP_ERROR,
                            "{\"status\":\"error\",\"msg\":\"pdsettings key must be pdsettings/yj or pdsettings/hk\"}");
    }

    app_settings_get_photodiode(&settings);
    rc = pd_settings_channel_json(payload, sizeof(payload), channel,
                                  &settings.channel[channel]);
    if (rc != 0) {
        return _msg_builder(cmd, RESP_ERROR,
                            "{\"status\":\"error\",\"msg\":\"pdsettings response too large\"}");
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
        return _msg_builder(cmd, RESP_ERROR,
                            "{\"status\":\"error\",\"msg\":\"photodiodes unavailable on this board\"}");
    }

    rc = pd_parse_channel_from_key(cmd, &channel);
    if (rc != 0) {
        return _msg_builder(cmd, RESP_ERROR,
                            "{\"status\":\"error\",\"msg\":\"pdsettings key must be pdsettings/yj or pdsettings/hk\"}");
    }

    app_settings_get_photodiode(&settings);
    channel_settings = settings.channel[channel];

    parse_rc = coo_json_extract_bool(cmd->payload, "persistent", &persist);
    if (parse_rc == COO_JSON_EXTRACT_ERR) {
        return _msg_builder(cmd, RESP_ERROR,
                            "{\"status\":\"error\",\"msg\":\"invalid persistent\"}");
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
        return _msg_builder(cmd, RESP_ERROR,
                            "{\"status\":\"error\",\"msg\":\"invalid pdsettings value\"}");
    }

    if (!changed) {
        return _msg_builder(cmd, RESP_ERROR,
                            "{\"status\":\"error\",\"msg\":\"no pdsettings fields supplied\"}");
    }

    app_settings_update_photodiode_channel((uint8_t)channel,
                                           &channel_settings,
                                           persist);
    return _msg_builder(cmd, RESP_OK, "{\"status\":\"success\"}");
}


struct OutMsg status_get(const struct Command *cmd) {
    struct network_ipv4_info net = {0};
    char payload[MAX_PAYLOAD_LEN]={0};

    (void)network_get_ipv4_info(&net);
    snprintf(payload, MAX_PAYLOAD_LEN,
             "{\"fwversion\":\"%s\",\"bootcount\":%u,\"uptime\":%lld,"
             "\"board_type\":\"%s\",\"board_valid\":%s,\"mems_switches\":%u,"
             "\"network_ready\":%s,\"ip\":\"%s\",\"laser_power\":%s}",
             APP_VERSION_STRING,
             app_settings_get_boot_count(),
             (long long)k_uptime_get(),
             devices_board_type_name(),
             devices_board_type() != HISPEC_BOARD_UNKNOWN ? "true" : "false",
             router.num_switches,
             net.link_ready ? "true" : "false",
             net.ip,
             power_enabled() ? "true" : "false");
    return _msg_builder(cmd, RESP_OK, payload);
}

struct OutMsg temp_get(const struct Command *cmd)
{
    struct tempsense_status ts = {0};
    char payload[MAX_PAYLOAD_LEN] = {0};

    tempsense_get_status(&ts);
    if (ts.valid) {
        snprintf(payload, sizeof(payload),
                 "{\"ambient_c\":%.3f,\"ambient_age_ms\":%u,\"laserbankavg_c\":null}",
                 (double)ts.ambient_c,
                 ts.age_ms);
    } else {
        snprintf(payload, sizeof(payload),
                 "{\"ambient_c\":null,\"ambient_age_ms\":null,\"laserbankavg_c\":null,"
                 "\"status\":\"error\",\"msg\":\"ambient temperature unavailable\",\"last_error\":%d}",
                 ts.last_error);
    }

    return _msg_builder(cmd, ts.valid ? RESP_OK : RESP_ERROR, payload);
}
