//
// Created by Jeb Bailey on 5/30/25.
//

#include "command.h"
// #include "devices.h"
#include <ctype.h>
#include <errno.h>
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
#include "app_settings.h"
#include "app_scheduled_actions.h"
#include "app_warning.h"
#include "attenuator.h"
#include "maiman.h"
#include "mems_switching.h"
#include "sntp_sync.h"
#include "tempsense.h"
#include <coo_commons/json_utils.h>
#include <coo_commons/network.h>

LOG_MODULE_REGISTER(command, LOG_LEVEL_DBG);

#define MQTT_DEVICE_ID "hsfib-tib"
#define MQTT_CMD_PREFIX "cmd/" MQTT_DEVICE_ID "/req/"
#define MQTT_RESP_PREFIX "cmd/" MQTT_DEVICE_ID "/resp/"
#define SERIAL_LINE_MAX 220
#define SERIAL_WRAP_COLUMN 80U

static uint16_t mqtt_msg_id = 1;
static atomic_t serial_network_ignore_active;


/* one command at a time */
K_MSGQ_DEFINE(inbound_queue,
              sizeof(struct Command),
              MAX_PENDING_COMMANDS,      /* depth */
              4);     /* 4‐byte align */

/* up to 8 pending publishes */
K_MSGQ_DEFINE(outbound_queue,
              sizeof(struct OutMsg),
              8,
              4);

extern const struct gpio_dt_spec power_gpio;
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




const struct DispatchEntry dispatch_table[] = {
    { "help",      help_get,         NULL             },
    { "ip",        ip_get,           ip_set           },
    { "mqtt",      mqtt_get,         mqtt_set         },
    { "time",      time_get,         time_set         },
    { "reboot",    NULL,             reboot_set       },
    { "serialguard", serial_guard_get, serial_guard_set },
    { "memsroute",  memsroute_get,    memsroute_set    },
    { "mems",       mems_get,         mems_set         },
    { "laser",      laser_setting_get,laser_setting_set},
    { "power",      power_get,        power_set        },
    { "atten",      atten_setting_get,  atten_setting_set  },
    { "temp",       temp_get,         NULL             },
    { "status",     status_get,       NULL  },
    { "sleep",      NULL,  sleep_set  }, // GET only
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

struct OutMsg _msg_builder(const struct Command *cmd, enum MsgType msgtyp, const char *msg) {
    struct OutMsg r = { 0 };
    r.msg_type = msgtyp;
    r.target = (cmd && cmd->source == CMD_SRC_SERIAL) ? OUT_TARGET_SERIAL : OUT_TARGET_MQTT;
    r.qos = MQTT_QOS_1_AT_LEAST_ONCE;

    //        snprintf(r.payload, MAX_PAYLOAD_LEN, "{\"error\":\"Invalid route\"}");


    // Set default response topic, but override if cmd provides a valid one
    snprintk(r.topic, sizeof(r.topic), "cmd/hsfib-tib/resp");
    if (cmd && strlen(cmd->response_topic) > 0 && strlen(cmd->response_topic) < sizeof(r.topic)) {
        strncpy(r.topic, cmd->response_topic, sizeof(r.topic) - 1);
    }

    // Echo correlation_data if present
    if (cmd && cmd->corr_len > 0 && cmd->corr_len < sizeof(r.correlation_data)) {
        memcpy(r.correlation_data, cmd->correlation_data, cmd->corr_len);
        r.corr_len = cmd->corr_len;
    }

    snprintk(r.payload, sizeof(r.payload), "%s", msg);
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

static bool derive_default_response_topic(const char *key, char *topic_out, size_t topic_out_len)
{
    const int n = snprintk(topic_out, topic_out_len, "%s%s", MQTT_RESP_PREFIX, key);

    return n > 0 && n < (int)topic_out_len;
}

static void enqueue_serial_error(const char *msg)
{
    struct OutMsg out = {0};

    out.target = OUT_TARGET_SERIAL;
    out.msg_type = RESP_ERROR;
    snprintk(out.topic, sizeof(out.topic), MQTT_RESP_PREFIX "serial");
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
            append_serial_json_field(out, out_len, &off, "port", t1, true) != 0) {
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

    prefix_len = strlen(MQTT_CMD_PREFIX);
    if (strncmp(req_topic, MQTT_CMD_PREFIX, prefix_len) != 0) {
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

    if (!command_network_mqtt_allowed()) {
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
    if (!gpio_is_ready_dt(&power_gpio)) {
        return false;
    }
    int val = gpio_pin_get_dt(&power_gpio);
    return val==1;
}

bool enable_power() {
    if (!gpio_is_ready_dt(&power_gpio)) {
        LOG_ERR("POWER_GPIO not ready");
        return false;
    }
    if (power_enabled())
        return false;
    int err = gpio_pin_set_dt(&power_gpio, 1);
    if (err) {
        LOG_ERR("Failed to set POWER_GPIO high\n");
    }
    return true;
}

bool disable_power() {
    if (!gpio_is_ready_dt(&power_gpio)) {
        LOG_ERR("POWER_GPIO not ready");
        return false;
    }
    if (!power_enabled())
        return false;
    //TODO set POWER_GPIO low
    int err = gpio_pin_set_dt(&power_gpio, 0);
    if (err) {
        LOG_ERR("Failed to set POWER_GPIO low\n");
    }
    return true;

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
                        "{\"help\":\"help,ip,mqtt,time,temp,status,reboot,serialguard,memsroute,mems,laser,power,atten\"}");
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
//TODO update mqtt get/set to drop port: key and convert host to form <host-or-ip>:<port -JIB
{
    struct app_mqtt_settings mqtt_cfg = {0};
    char payload[MAX_PAYLOAD_LEN] = {0};
#if defined(CONFIG_DNS_RESOLVER)
    const bool dns_supported = true;
#else
    const bool dns_supported = false;
#endif

    app_settings_get_mqtt(&mqtt_cfg);
    snprintk(payload, sizeof(payload),
             "{\"broker\":\"%s\",\"port\":%u,\"dns_supported\":%s}",
             mqtt_cfg.broker_host,
             mqtt_cfg.broker_port,
             dns_supported ? "true" : "false");
    return _msg_builder(cmd, RESP_OK, payload);
}

struct OutMsg mqtt_set(const struct Command *cmd)
//TODO update mqtt get/set to drop port: key and convert host to form <host-or-ip>:<port -JIB
{
    struct app_mqtt_settings mqtt_cfg = {0};
    char host[sizeof(mqtt_cfg.broker_host)] = {0};
    uint32_t port = 0U;
    bool persist = false;
    bool changed = false;
    int parse_rc;
#if defined(CONFIG_DNS_RESOLVER)
    const bool dns_supported = true;
#else
    const bool dns_supported = false;
#endif

    app_settings_get_mqtt(&mqtt_cfg);

    parse_rc = coo_json_extract_string(cmd->payload, "broker", host, sizeof(host));
    if (parse_rc == COO_JSON_EXTRACT_MISSING) {
        parse_rc = coo_json_extract_string(cmd->payload, "host", host, sizeof(host));
    }
    if (parse_rc == COO_JSON_EXTRACT_ERR) {
        return _msg_builder(cmd, RESP_ERROR, "{\"status\":\"error\",\"msg\":\"invalid broker\"}");
    }
    if (parse_rc == COO_JSON_EXTRACT_OK) {
        if (!mqtt_host_is_numeric_ipv4(host) && !dns_supported) {
            return _msg_builder(cmd, RESP_ERROR,
                                "{\"status\":\"error\",\"msg\":\"broker hostname requires DNS\"}");
        }
        strncpy(mqtt_cfg.broker_host, host, sizeof(mqtt_cfg.broker_host) - 1U);
        mqtt_cfg.broker_host[sizeof(mqtt_cfg.broker_host) - 1U] = '\0';
        changed = true;
    }

    parse_rc = coo_json_extract_u32(cmd->payload, "port", &port);
    if (parse_rc == COO_JSON_EXTRACT_ERR) {
        return _msg_builder(cmd, RESP_ERROR, "{\"status\":\"error\",\"msg\":\"invalid port\"}");
    }
    if (parse_rc == COO_JSON_EXTRACT_OK) {
        if (port == 0U || port > UINT16_MAX) {
            return _msg_builder(cmd, RESP_ERROR, "{\"status\":\"error\",\"msg\":\"port out of range\"}");
        }
        mqtt_cfg.broker_port = (uint16_t)port;
        changed = true;
    }

    parse_rc = coo_json_extract_bool(cmd->payload, "persistent", &persist);
    if (parse_rc == COO_JSON_EXTRACT_ERR) {
        return _msg_builder(cmd, RESP_ERROR, "{\"status\":\"error\",\"msg\":\"invalid persistent\"}");
    }

    if (!changed) {
        return _msg_builder(cmd, RESP_ERROR, "{\"status\":\"error\",\"msg\":\"no recognized mqtt fields\"}");
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


struct OutMsg memsroute_get(const struct Command *cmd)
{
    struct mems_route_key keys[MEMS_ROUTER_MAX_ROUTES];
    uint8_t n_routes = mems_router_active_routes(&router, keys, MEMS_ROUTER_MAX_ROUTES);

    char buf[MAX_PAYLOAD_LEN];
    size_t offset = 0;
    int written;

    // Begin JSON object
    offset += snprintf(buf, sizeof(buf), "{\"active_routes\":{");

    for (uint8_t i = 0; i < n_routes; ++i) {
        // Add comma if not the first entry
        if (i > 0) {
            written = snprintf(buf + offset, sizeof(buf) - offset, ",");
            if (written < 0 || written >= (int)(sizeof(buf) - offset)) {
                return _msg_builder(cmd, RESP_ERROR, "{\"error\":\"overflow building JSON\"}");
            }
            offset += written;
        }
        // Add "input":"output" pair
        written = snprintf(
            buf + offset, sizeof(buf) - offset,
            "\"%s\":\"%s\"",
            keys[i].input_name, keys[i].output_name
        );
        if (written < 0 || written >= (int)(sizeof(buf) - offset)) {
            return _msg_builder(cmd, RESP_ERROR, "{\"error\":\"overflow building JSON\"}");
        }
        offset += written;
    }

    // Close object and top-level
    written = snprintf(buf + offset, sizeof(buf) - offset, "}}");
    if (written < 0 || written >= (int)(sizeof(buf) - offset)) {
        return _msg_builder(cmd, RESP_ERROR, "{\"error\":\"overflow building JSON\"}");
    }

    return _msg_builder(cmd, RESP_OK, buf);
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

    // Extract laser### and <setting> from key
    char atten_name[16], setting[16];
    if (parse_key_pair(cmd->key, atten_name, 15, setting, 15)!=0) {
        return _msg_builder(cmd, RESP_ERROR,"{\"error\":\"Failed to parse atten/setting\"}");
    }

    laser_t laser_id = get_laser_channel(atten_name+5);

    if (laser_id==LASER_UNKNOWN) {
        return _msg_builder(cmd, RESP_ERROR,"{\"error\":\"Invalid attenuator\"}");
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

    // Extract laser### and <setting>
    char atten_name[16], setting[16];
    if (parse_key_pair(cmd->key, atten_name, 15, setting, 15)!=0) {
        return _msg_builder(cmd, RESP_ERROR,"{\"error\":\"Failed to parse laser/setting\"}");
    }

    laser_t laser_id = get_laser_channel(atten_name+5);

    if (laser_id==LASER_UNKNOWN) {
        return _msg_builder(cmd, RESP_ERROR,"{\"error\":\"Invalid attenuator\"}");
    }

    // Parse value
    struct coeffs {
        float db2volt[3];
        size_t db2volt_len;
        float volt2db[3];
        size_t volt2db_len;
    };

    // Parse value
    struct json_value_float {
        float value;
    };

    if (strcasecmp(setting, "coeff") == 0) {

        struct coeffs parsed_coeffs = {0};

        const struct json_obj_descr coeff_descr[] = {
            JSON_OBJ_DESCR_ARRAY(struct coeffs, db2volt, 3, db2volt_len, JSON_TOK_FLOAT),
            JSON_OBJ_DESCR_ARRAY(struct coeffs, volt2db, 3, volt2db_len, JSON_TOK_FLOAT),
        };

        if (json_obj_parse((char *) cmd->payload, cmd->payload_len, coeff_descr,
                                 ARRAY_SIZE(coeff_descr), &parsed_coeffs) < 0 ||
                                 parsed_coeffs.db2volt_len != 3 || parsed_coeffs.volt2db_len != 3) {
            return _msg_builder(cmd, RESP_ERROR,"{\"error\":\"Improper arguments\"}");
        }

        double db;
        attenuator_get(&attenuators[laser_id], &db, false);

        for (int i=0; i<3; i++) {
            attenuators[laser_id].coeff_db_to_volt[i]=parsed_coeffs.db2volt[i];
            attenuators[laser_id].coeff_volt_to_db[i]=parsed_coeffs.volt2db[i];
        }

        attenuator_set(&attenuators[laser_id], db, false);

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

    return _msg_builder(cmd, RESP_OK, "{\"status\":\"OK\"}");
}


struct OutMsg status_get(const struct Command *cmd) {
    struct network_ipv4_info net = {0};
    char payload[MAX_PAYLOAD_LEN]={0};

    (void)network_get_ipv4_info(&net);
    snprintf(payload, MAX_PAYLOAD_LEN,
             "{\"fwversion\":\"%s\",\"bootcount\":%u,\"uptime\":%lld,"
             "\"network_ready\":%s,\"ip\":\"%s\",\"laser_power\":%s}",
             APP_VERSION_STRING,
             app_settings_get_boot_count(),
             (long long)k_uptime_get(),
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


struct OutMsg power_get(const struct Command *cmd) {
    char payload[MAX_PAYLOAD_LEN]={0};
    snprintf(payload, MAX_PAYLOAD_LEN, "{\"laser_power\":%s}", power_enabled() ? "true" : "false");
    return _msg_builder(cmd, RESP_OK, payload);
}



struct OutMsg power_set(const struct Command *cmd) {

    bool value;
    int parse_rc;

    parse_rc = coo_json_extract_bool(cmd->payload, "value", &value);
    if (parse_rc == COO_JSON_EXTRACT_MISSING) {
        return _msg_builder(cmd, RESP_ERROR,"{\"error\":\"Missing setting value\"}");
    }
    if (parse_rc == COO_JSON_EXTRACT_ERR) {
        return _msg_builder(cmd, RESP_ERROR,"{\"error\":\"Invalid setting value\"}");
    }

    if (value) enable_power();
    else disable_power();
    return _msg_builder(cmd, RESP_OK,"{\"status\":\"OK\"}");
}

struct OutMsg sleep_set(const struct Command *cmd) {

    bool value;
    int parse_rc;

    parse_rc = coo_json_extract_bool(cmd->payload, "value", &value);
    if (parse_rc == COO_JSON_EXTRACT_MISSING) {
        return _msg_builder(cmd, RESP_ERROR,"{\"error\":\"Missing setting value\"}");
    }
    if (parse_rc == COO_JSON_EXTRACT_ERR) {
        return _msg_builder(cmd, RESP_ERROR,"{\"error\":\"Invalid setting value\"}");
    }

    //TODO
    ARG_UNUSED(value);

    return _msg_builder(cmd, RESP_OK,"{\"status\":\"OK\"}");
}
