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

#include "devices.h"
#include "laserbank_control.h"
#include "lasers.h"
#include "app_identity.h"
#include "app_settings.h"
#include "app_scheduled_actions.h"
#include "app_warning.h"
#include "attenuator.h"
#include "attenuator_command.h"
#include "maiman.h"
#include "mems_command.h"
#include "mems_switching.h"
#include "photodiode_command.h"
#include "throughput_command.h"
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
static char command_warning_topic[MAX_TOPIC_LEN];

static void command_serial_line_handler(char *line, void *user_data);


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

static struct coo_cmd_runtime command_runtime = {
    .inbound_queue = &inbound_queue,
    .outbound_queue = &outbound_queue,
    .execute_handler = dispatch_command,
    .dispatch_table = dispatch_table,
    .dispatch_count = ARRAY_SIZE(dispatch_table),
    .unknown_handler = unknown_response,
    .unsupported_handler = unsupported_response,
    .mqtt_msg_id = &mqtt_msg_id,
    .warning_topic = command_warning_topic,
    .serial_wrap_column = SERIAL_WRAP_COLUMN,
    .serial_line_handler = command_serial_line_handler,
};

const struct DispatchEntry *find_dispatch(const char *key)
{
    return coo_cmd_find_dispatch(dispatch_table, ARRAY_SIZE(dispatch_table), key);
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

    r = coo_cmd_dispatch(cmd, dispatch_table, ARRAY_SIZE(dispatch_table),
                         unknown_response, unsupported_response);
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

static bool copy_topic(const struct mqtt_utf8 *topic, char *out, size_t out_len)
{
    return coo_cmd_copy_mqtt_utf8(topic, out, out_len);
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
    return coo_cmd_payload_empty(cmd);
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

    if (!coo_cmd_serial_next_token(&cursor, t0, sizeof(t0))) {
        return -EINVAL;
    }
    (void)coo_cmd_serial_next_token(&cursor, t1, sizeof(t1));
    (void)coo_cmd_serial_next_token(&cursor, t2, sizeof(t2));
    if (coo_cmd_serial_has_extra(cursor)) {
        return -EINVAL;
    }

    if (strncmp(key, "mems/", 5) == 0) {
        written = snprintk(out, out_len, "{\"state\":");
        if (written < 0 || written >= (int)out_len) {
            return -ENOSPC;
        }
        off = (size_t)written;
        if (coo_cmd_serial_append_json_value(out, out_len, &off, t0) != 0) {
            return -EINVAL;
        }
        if (t1[0] != '\0' &&
            coo_cmd_serial_append_json_field(out, out_len, &off, "duty_cycle", t1, true) != 0) {
            return -EINVAL;
        }
        if (t2[0] != '\0' &&
            coo_cmd_serial_append_json_field(out, out_len, &off, "stopafter_s", t2, true) != 0) {
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
        if (coo_cmd_serial_append_json_value(out, out_len, &off, seconds) != 0) {
            return -EINVAL;
        }
        if (t1[0] != '\0' &&
            coo_cmd_serial_append_json_field(out, out_len, &off, "persistent", t1, true) != 0) {
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
        if (coo_cmd_serial_append_json_value(out, out_len, &off, t0) != 0) {
            return -EINVAL;
        }
        if (t1[0] != '\0' &&
            coo_cmd_serial_append_json_field(out, out_len, &off, "persistent", t1, true) != 0) {
            return -EINVAL;
        }
        if (t2[0] != '\0') {
            return -EINVAL;
        }
        written = snprintk(out + off, out_len - off, "}");
        return (written < 0 || written >= (int)(out_len - off)) ? -ENOSPC : 0;
    }

    if (strcmp(key, "time") == 0) {
        if (!coo_cmd_serial_token_is_number(t0)) {
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
    if (coo_cmd_serial_append_json_value(out, out_len, &off, t0) != 0) {
        return -EINVAL;
    }
    written = snprintk(out + off, out_len - off, "}");
    return (written < 0 || written >= (int)(out_len - off)) ? -ENOSPC : 0;
}

static int command_serial_payload_from_shorthand(const char *key,
                                                 const char *payload,
                                                 char *out,
                                                 size_t out_len,
                                                 void *user_data)
{
    ARG_UNUSED(user_data);

    return serial_payload_from_shorthand(key, payload, out, out_len);
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
    return coo_cmd_normalize_serial_payload(key, payload,
                                            command_serial_payload_from_shorthand,
                                            NULL, out, out_len);
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

    if (!derive_default_response_topic(cmd.key, cmd.response_topic, sizeof(cmd.response_topic))) {
        struct OutMsg r = invalid_command_response(&cmd);
        (void)k_msgq_put(&outbound_queue, &r, K_NO_WAIT);
        return;
    }

    if (pub->prop.response_topic.utf8 != NULL &&
        pub->prop.response_topic.size > 0U &&
        pub->prop.response_topic.size < sizeof(cmd.response_topic)) {
        memcpy(cmd.response_topic, pub->prop.response_topic.utf8, pub->prop.response_topic.size);
        cmd.response_topic[pub->prop.response_topic.size] = '\0';
    }

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

static void command_serial_line_handler(char *line, void *user_data)
{
    ARG_UNUSED(user_data);

    command_parse_serial_line(line);
}

void command_executor_thread(void *p1, void *p2, void *p3)
{
    ARG_UNUSED(p1);
    ARG_UNUSED(p2);
    ARG_UNUSED(p3);

    coo_cmd_runtime_executor_thread(&command_runtime, NULL, NULL);
}

void command_serial_thread(void *p1, void *p2, void *p3)
{
    ARG_UNUSED(p1);
    ARG_UNUSED(p2);
    ARG_UNUSED(p3);

    coo_cmd_runtime_serial_thread(&command_runtime, NULL, NULL);
}

void command_drain_outbound_queue(struct mqtt_client *client, bool mqtt_available)
{
    coo_cmd_runtime_drain_outbound(&command_runtime, client, mqtt_available);
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
        return coo_cmd_error(cmd, "laser bank unavailable on this board");
    }

    if (cmd != NULL &&
        (cmd->msg_type == MSG_SET ||
         command_suffix_after(cmd, "laserbank/power")[0] != '\0')) {
        if (!parse_laserbank_power_request(cmd, &mode)) {
            return coo_cmd_error(cmd, "override must be auto, override_on, or override_off");
        }
        rc = hispec_laser_bank_power_mode_set(mode);
        if (rc != 0) {
            return coo_cmd_error_rc(cmd, "laser bank power mode failed", rc);
        }
    }

    mode = hispec_laser_bank_power_mode_get();
    snprintk(payload, sizeof(payload),
             "{\"mode\":\"%s\",\"powered\":%s}",
             hispec_laser_bank_power_mode_name(mode),
             hispec_laser_bank_power_is_enabled() ? "true" : "false");
    return coo_cmd_reply(cmd, RESP_OK, payload);
}

struct OutMsg laserbank_clearfaults(const struct Command *cmd)
{
    bool fault = false;
    uint32_t off_ms = 0U;
    char payload[MAX_PAYLOAD_LEN] = {0};
    int rc;

    if (devices_board_type() != HISPEC_BOARD_TIB) {
        return coo_cmd_error(cmd, "laser bank unavailable on this board");
    }

    if (!power_enabled()) {
        return coo_cmd_reply(cmd, RESP_OK, "{\"off_ms\":0}");
    }
    rc = hispec_laser_bank_any_overcurrent_fault(&fault);
    if (rc != 0) {
        return coo_cmd_error_rc(cmd, "overcurrent status unavailable", rc);
    }
    if (!fault) {
        return coo_cmd_reply(cmd, RESP_OK, "{\"off_ms\":0}");
    }
    if (hispec_laser_bank_clear_faults(LASERBANK_FAULT_CLEAR_OFF_MS) != 0) {
        return coo_cmd_error(cmd, "laser bank power cycle failed");
    }
    off_ms = LASERBANK_FAULT_CLEAR_OFF_MS;

    if (!power_enabled()) {
        return coo_cmd_error(cmd, "laser bank power cycle could not turn on");
    }

    snprintf(payload, sizeof(payload),
             "{\"off_ms\":%u}", off_ms);
    return coo_cmd_reply(cmd, RESP_OK, payload);
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
        return coo_cmd_error(cmd, "laser bank unavailable on this board");
    }

    if (cmd != NULL &&
        (cmd->msg_type == MSG_SET ||
         command_suffix_after(cmd, "laserbank/heater")[0] != '\0')) {
        if (!parse_heater_request(cmd, &mode)) {
            return coo_cmd_reply(cmd, RESP_ERROR,
                                "{\"error\":\"Use laserbank/heater auto|override_on|override_off\"}");
        }
        int rc = laserbank_control_set_heater_mode(mode, true);
        if (rc != 0) {
            return coo_cmd_error_rc(cmd, "laser bank heater relay unavailable", rc);
        }
    }

    laserbank_control_status_payload(payload, sizeof(payload));
    return coo_cmd_reply(cmd, RESP_OK, payload);
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

    rc = app_mqtt_format_data_topic("warning", command_warning_topic,
                                    sizeof(command_warning_topic));
    if (rc != 0) {
        return rc;
    }

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
    return coo_cmd_invalid_response(cmd);
}

struct OutMsg unknown_response(const struct Command *cmd) {
    return coo_cmd_unknown_response(cmd);
}

struct OutMsg unsupported_response(const struct Command *cmd) {
    return coo_cmd_unsupported_response(cmd);
}

struct OutMsg busy_response(const struct Command *cmd) {
    return coo_cmd_busy_response(cmd);
}

struct OutMsg serial_active_response(const struct Command *cmd) {
    return coo_cmd_serial_active_response(cmd);
}

struct OutMsg help_get(const struct Command *cmd)
{
    return coo_cmd_reply(cmd, RESP_OK,
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

    return coo_cmd_reply(cmd, RESP_OK, payload);
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
            return coo_cmd_error(cmd, "invalid trydhcpfirst");
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
            return coo_cmd_error(cmd, "invalid preferdhcpdns");
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
            return coo_cmd_error(cmd, "invalid preferdhcpntp");
        }
    }

    parse_rc = coo_json_extract_string(cmd->payload, "ip", buf, sizeof(buf));
    if (parse_rc == COO_JSON_EXTRACT_OK) {
        strncpy(ip_cfg.ip, buf, sizeof(ip_cfg.ip) - 1);
        ip_cfg.ip[sizeof(ip_cfg.ip) - 1] = '\0';
        changed = true;
        network_changed = true;
    } else if (parse_rc == COO_JSON_EXTRACT_ERR) {
        return coo_cmd_error(cmd, "invalid ip");
    }

    parse_rc = coo_json_extract_string(cmd->payload, "subnet", buf, sizeof(buf));
    if (parse_rc == COO_JSON_EXTRACT_OK) {
        strncpy(ip_cfg.subnet, buf, sizeof(ip_cfg.subnet) - 1);
        ip_cfg.subnet[sizeof(ip_cfg.subnet) - 1] = '\0';
        changed = true;
        network_changed = true;
    } else if (parse_rc == COO_JSON_EXTRACT_ERR) {
        return coo_cmd_error(cmd, "invalid subnet");
    }

    parse_rc = coo_json_extract_string(cmd->payload, "gateway", buf, sizeof(buf));
    if (parse_rc == COO_JSON_EXTRACT_OK) {
        strncpy(ip_cfg.gateway, buf, sizeof(ip_cfg.gateway) - 1);
        ip_cfg.gateway[sizeof(ip_cfg.gateway) - 1] = '\0';
        changed = true;
        network_changed = true;
    } else if (parse_rc == COO_JSON_EXTRACT_ERR) {
        return coo_cmd_error(cmd, "invalid gateway");
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
            return coo_cmd_error(cmd, "invalid dns");
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
            return coo_cmd_error(cmd, "invalid ntp");
        }
    }

    parse_rc = coo_json_extract_bool(cmd->payload, "persistent", &persist);
    if (parse_rc == COO_JSON_EXTRACT_ERR) {
        return coo_cmd_error(cmd, "invalid persistent");
    }

    if (!changed && !(unsupported_dhcp || unsupported_dns || unsupported_ntp)) {
        return coo_cmd_error(cmd, "no recognized ip fields");
    }

    if (network_changed) {
        struct network_config net_cfg;
        int rc;

        network_config_from_app_ip(&ip_cfg, &net_cfg);
        rc = network_reconfigure(&net_cfg);
        if (rc != 0) {
            return coo_cmd_error_rc(cmd, "network reconfigure failed", rc);
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
        return coo_cmd_reply(cmd, RESP_OK, response);
    }

    return coo_cmd_ok(cmd);
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
    return coo_cmd_reply(cmd, RESP_OK, payload);
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
        return coo_cmd_error(cmd, "missing broker");
    }
    if (parse_rc == COO_JSON_EXTRACT_ERR) {
        return coo_cmd_error(cmd, "invalid broker");
    }
    if (!coo_mqtt_parse_broker_endpoint(endpoint, &broker_cfg)) {
        return coo_cmd_error(cmd, "broker must be host-or-ip:port");
    }
    rc = coo_mqtt_resolve_broker_config(&broker_cfg, resolved_ip, sizeof(resolved_ip));
    if (rc == -ENOTSUP) {
        return coo_cmd_error(cmd, "broker hostname requires DNS");
    }
    if (rc != 0) {
        return coo_cmd_error(cmd, "broker host did not resolve");
    }
    strncpy(mqtt_cfg.broker_host, broker_cfg.host, sizeof(mqtt_cfg.broker_host) - 1U);
    mqtt_cfg.broker_host[sizeof(mqtt_cfg.broker_host) - 1U] = '\0';
    mqtt_cfg.broker_port = broker_cfg.port;

    parse_rc = coo_json_extract_bool(cmd->payload, "persistent", &persist);
    if (parse_rc == COO_JSON_EXTRACT_ERR) {
        return coo_cmd_error(cmd, "invalid persistent");
    }

    app_settings_update_mqtt(&mqtt_cfg, persist);
    return coo_cmd_ok(cmd);
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

    return coo_cmd_reply(cmd, RESP_OK, payload);
}

struct OutMsg time_set(const struct Command *cmd)
{
    uint64_t utc_ms = 0;
    struct timespec ts = {0};
    int parse_rc;

    parse_rc = coo_json_extract_u64(cmd->payload, "linuxtime_ms", &utc_ms);
    if (parse_rc == COO_JSON_EXTRACT_MISSING) {
        return coo_cmd_error(cmd, "missing linuxtime_ms");
    }
    if (parse_rc == COO_JSON_EXTRACT_ERR) {
        return coo_cmd_error(cmd, "invalid linuxtime_ms");
    }

    ts.tv_sec = utc_ms / 1000ULL;
    ts.tv_nsec = (utc_ms % 1000ULL) * 1000000ULL;

    if (clock_settime(CLOCK_REALTIME, &ts) != 0) {
        return coo_cmd_error(cmd, "clock_settime failed");
    }

    return coo_cmd_ok(cmd);
}

struct OutMsg reboot_set(const struct Command *cmd)
{
    int rc;

    rc = app_scheduled_action_schedule(APP_SCHEDULED_ACTION_REBOOT, K_MSEC(250));
    if (rc < 0) {
        return coo_cmd_error(cmd, "failed to schedule reboot");
    }

    return coo_cmd_ok(cmd);
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
    return coo_cmd_reply(cmd, RESP_OK, payload);
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
        return coo_cmd_error(cmd, "invalid seconds");
    }
    if (parse_rc_seconds == COO_JSON_EXTRACT_MISSING &&
        parse_rc_value == COO_JSON_EXTRACT_MISSING) {
        return coo_cmd_error(cmd, "missing seconds");
    }

    parse_rc_persist = coo_json_extract_bool(cmd->payload, "persistent", &persist);
    if (parse_rc_persist == COO_JSON_EXTRACT_ERR) {
        return coo_cmd_error(cmd, "invalid persistent");
    }
    app_settings_set_serial_holdoff_s(holdoff_s, persist);
    if (cmd->source == CMD_SRC_SERIAL) {
        command_serial_note_activity();
    }
    return coo_cmd_ok(cmd);
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
    return coo_cmd_error(cmd, "laser bank unavailable on this board");
}

static struct OutMsg laser_error_response(const struct Command *cmd,
                                          const char *msg,
                                          int rc)
{
    return coo_cmd_error_rc(cmd, msg, rc);
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
        return coo_cmd_error(cmd, "missing or invalid laser name");
    }

    rc = hispec_laser_get_status(id, &status);
    if (rc != 0 && !status.bank_powered) {
        return laser_error_response(cmd, "laser status failed", rc);
    }
    if (laser_append_compact_status(payload, sizeof(payload), &status) != 0) {
        return coo_cmd_error(cmd, "laser response too large");
    }
    return rc == 0 ? coo_cmd_reply(cmd, RESP_OK, payload) :
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
        return coo_cmd_error(cmd, "missing or invalid laser name");
    }
    parse_rc = coo_json_extract_float(cmd->payload, "level", &level);
    if (parse_rc != COO_JSON_EXTRACT_OK || level < 0.0f || level > 100.0f) {
        return coo_cmd_error(cmd, "level must be 0..100");
    }
    rc = hispec_laser_get_channel_settings(id, &settings);
    if (rc != 0) {
        return laser_error_response(cmd, "laser settings unavailable", rc);
    }
    autooff_s = settings.autooff_s;
    parse_rc = coo_json_extract_u32(cmd->payload, "autooff_s", &autooff_s);
    if (parse_rc == COO_JSON_EXTRACT_ERR) {
        return coo_cmd_error(cmd, "invalid autooff_s");
    }

    throughput_monitor_note_laser_changed(id);
    rc = hispec_laser_set_output_percent_autooff(id, level, autooff_s);
    if (rc != 0) {
        return laser_error_response(cmd, "laser level failed", rc);
    }
    return coo_cmd_ok(cmd);
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
        return coo_cmd_error(cmd, "missing or invalid laser name");
    }
    snprintk(payload, sizeof(payload),
             "{\"name\":\"%s\",\"tune_nm\":%.4f}",
             hispec_laser_name(id),
             (double)hispec_laser_get_tune_delta_nm(id));
    return coo_cmd_reply(cmd, RESP_OK, payload);
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
        return coo_cmd_error(cmd, "missing or invalid laser name");
    }
    parse_rc = coo_json_extract_float(cmd->payload, "tune_nm", &delta_nm);
    if (parse_rc == COO_JSON_EXTRACT_MISSING) {
        parse_rc = coo_json_extract_float(cmd->payload, "delta_nm", &delta_nm);
    }
    if (parse_rc != COO_JSON_EXTRACT_OK) {
        return coo_cmd_error(cmd, "missing tune_nm");
    }
    throughput_monitor_note_laser_changed(id);
    rc = hispec_laser_set_tune_delta_nm(id, delta_nm, true);
    if (rc != 0) {
        return laser_error_response(cmd, "laser tune failed", rc);
    }
    return coo_cmd_ok(cmd);
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
        return coo_cmd_error(cmd, "missing or invalid laser name");
    }
    rc = hispec_laser_get_channel_settings(id, &settings);
    if (rc != 0) {
        return laser_error_response(cmd, "laser settings unavailable", rc);
    }
    if (laser_settings_payload(payload, sizeof(payload), id, &settings) != 0) {
        return coo_cmd_error(cmd, "laser settings response too large");
    }
    return coo_cmd_reply(cmd, RESP_OK, payload);
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
        return coo_cmd_error(cmd, "missing or invalid laser name");
    }
    rc = hispec_laser_get_channel_settings(id, &settings);
    if (rc != 0) {
        return laser_error_response(cmd, "laser settings unavailable", rc);
    }

    rc = coo_json_extract_object(cmd->payload, "settings", settings_json, sizeof(settings_json));
    if (rc == COO_JSON_EXTRACT_ERR) {
        return coo_cmd_error(cmd, "invalid settings object");
    }
    json = rc == COO_JSON_EXTRACT_OK ? settings_json : cmd->payload;

    rc = laser_parse_settings_update(json, &settings, &driver_changed, &changed);
    if (rc != 0) {
        return laser_error_response(cmd, "invalid laser settings", rc);
    }
    if (!changed) {
        return coo_cmd_error(cmd, "no laser settings fields supplied");
    }

    throughput_monitor_note_laser_changed(id);
    rc = hispec_laser_update_channel_settings(id, &settings, driver_changed, true);
    if (rc != 0) {
        return laser_error_response(cmd, "laser settings update failed", rc);
    }
    return coo_cmd_ok(cmd);
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
        return coo_cmd_error(cmd, "missing or invalid laser name");
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
        return coo_cmd_error(cmd, "laser engineering status response too large");
    }
    return rc == 0 ? coo_cmd_reply(cmd, RESP_OK, payload) :
           laser_error_response(cmd, "laser engineering status failed", rc);
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
        return coo_cmd_error(cmd, "invalid ip");
    }
    parse_rc = coo_json_extract_bool(cmd->payload, "lasers", &include_lasers);
    if (parse_rc == COO_JSON_EXTRACT_ERR) {
        return coo_cmd_error(cmd, "invalid lasers");
    }
    parse_rc = coo_json_extract_bool(cmd->payload, "attens", &include_attens);
    if (parse_rc == COO_JSON_EXTRACT_ERR) {
        return coo_cmd_error(cmd, "invalid attens");
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
        return coo_cmd_error(cmd, "status response too large");
    }

    if (include_ip) {
        struct OutMsg ip = ip_get(cmd);

        if (ip.msg_type != RESP_OK ||
            coo_json_append(payload, sizeof(payload), &off,
                            ",\"ip\":%s", ip.payload) != 0) {
            return coo_cmd_error(cmd, "status response too large");
        }
    }

    if (include_lasers) {
        if (coo_json_append(payload, sizeof(payload), &off, ",\"lasers\":{") != 0) {
            return coo_cmd_error(cmd, "status response too large");
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
                return coo_cmd_error(cmd, "status response too large");
            }
        }
        if (coo_json_append(payload, sizeof(payload), &off, "}") != 0) {
            return coo_cmd_error(cmd, "status response too large");
        }
    }

    if (include_attens) {
        bool first = true;

        if (coo_json_append(payload, sizeof(payload), &off, ",\"attens\":{") != 0) {
            return coo_cmd_error(cmd, "status response too large");
        }
        for (uint8_t i = 0U; i < HISPEC_LASER_COUNT; ++i) {
            uint8_t atten_index;
            struct attenuator_status atten = {0};
            bool valid;

            if (attenuator_index_from_laser_id((enum hispec_laser_id)i, &atten_index) != 0 ||
                !devices_attenuator_channel_available(atten_index)) {
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
                return coo_cmd_error(cmd, "status response too large");
            }
            first = false;
        }
        if (coo_json_append(payload, sizeof(payload), &off, "}") != 0) {
            return coo_cmd_error(cmd, "status response too large");
        }
    }

    if (coo_json_append(payload, sizeof(payload), &off,
                        ",\"lastcommand\":{\"name\":\"%s\",\"source\":\"%s\","
                        "\"time\":%lld}}",
                        last_command_name,
                        last_command_source,
                        (long long)last_command_time_ms) != 0) {
        return coo_cmd_error(cmd, "status response too large");
    }

    return coo_cmd_reply(cmd, RESP_OK, payload);
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
        return coo_cmd_error(cmd, "temp response too large");
    }

    if (coo_json_append(payload, sizeof(payload), &off,
                        ",\"laserbank_c\":") != 0 ||
        coo_json_append_float_or_null(payload, sizeof(payload), &off,
                                      bank_count > 0U ? bank_sum / (double)bank_count : (double)NAN,
                                      3) != 0 ||
        coo_json_append(payload, sizeof(payload), &off,
                        ",\"laser\":{") != 0) {
        return coo_cmd_error(cmd, "temp response too large");
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
            return coo_cmd_error(cmd, "temp response too large");
        }
    }
    if (coo_json_append(payload, sizeof(payload), &off, "}}") != 0) {
        return coo_cmd_error(cmd, "temp response too large");
    }

    return coo_cmd_reply(cmd, RESP_OK, payload);
}
