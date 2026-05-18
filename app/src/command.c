/**
 * @file command.c
 * @brief HISPEC command table, request classification, and app command handlers.
 *
 * The common command runtime owns MQTT/serial topic handling, executor loops,
 * warning publication, and outbound drain behavior. This file supplies the
 * static command table, HISPEC request classification rules, serial shorthand
 * callback, scheduled actions, and command handlers that cut across domains.
 */

#include "command.h"
// #include "devices.h"
#include <errno.h>
#include <math.h>
#include <strings.h>
#include <zephyr/sys/reboot.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/sys/clock.h>
#include <zephyr/sys/util.h>
#include <app_version.h>
#include <time.h>
#include <zephyr/net/net_ip.h>

#include "devices.h"
#include "laserbank_tempcontrol.h"
#include "lasers.h"
#include "app_identity.h"
#include "app_settings.h"
#include "app_scheduled_actions.h"
#include "attenuator.h"
#include "attenuator_command.h"
#include "laser_command.h"
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

#define SERIAL_WRAP_COLUMN 80U

static uint16_t mqtt_msg_id = 1;
static atomic_t serial_network_ignore_active;
static char last_command_name[MAX_KEY_LEN];
static char last_command_source[8] = "unknown";
static int64_t last_command_time_ms;


/* MQTT and serial ingress use k_msgq so callbacks never execute hardware work.
 * Depth is intentionally small: clients should retry instead of letting stale
 * hardware commands pile up.
 */
K_MSGQ_DEFINE(inbound_queue,
              sizeof(struct coo_cmd_request),
              MAX_PENDING_COMMANDS,      /* depth */
              4);     /* 4‐byte align */

/* Responses, warnings, and telemetry leave the executor through this bounded
 * queue. The main loop owns MQTT publish retries and serial printing.
 */
K_MSGQ_DEFINE(outbound_queue,
              sizeof(struct coo_cmd_response),
              8,
              4);

extern struct mems_switch mems_switches[MEMS_ROUTER_MAX_SWITCHES];
extern struct mems_router router;
// extern struct attenuator attenuators[NUM_ATTENUATORS];

static const struct coo_cmd_dispatch_entry dispatch_table[] = {
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
    { "laser/status", laser_get, NULL },
    { "laser/settings", laser_settings_get, laser_settings_set },
    { "laser/tune", laser_tune_get, laser_tune_set },
    { "laser",      laser_get, laser_set },
    { "atten",      atten_setting_get,  atten_setting_set  },
    { "pdsettings", pd_settings_get, pd_settings_set },
    { "pd",         pd_get,          pd_set          },
    { "temp",       temp_get,         NULL             },
    { "status",     status_get,       NULL  },
};

static struct coo_cmd_runtime command_runtime;

static const struct coo_cmd_dispatch_entry *find_dispatch(const char *key)
{
    return coo_cmd_find_dispatch(dispatch_table, ARRAY_SIZE(dispatch_table), key);
}

static bool command_pd_dark_status_query(const struct coo_cmd_request *cmd)
{
    char action[20] = {0};

    return cmd != NULL &&
           coo_json_extract_string(cmd->payload, "action",
                                   action, sizeof(action)) == COO_JSON_EXTRACT_OK &&
           strcasecmp(action, "dark_status") == 0;
}

static bool command_should_record_lastcommand(const struct coo_cmd_request *cmd)
{
    const struct coo_cmd_dispatch_entry *entry;

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

    if (cmd->msg_type != COO_CMD_EFFECT || entry->set_handler == NULL) {
        return false;
    }

    if (strcmp(entry->key, "pd") == 0 && command_pd_dark_status_query(cmd)) {
        return false;
    }

    return true;
}

static void record_lastcommand(const struct coo_cmd_request *cmd)
{
    strncpy(last_command_name, cmd->key, sizeof(last_command_name) - 1);
    last_command_name[sizeof(last_command_name) - 1] = '\0';
    snprintk(last_command_source, sizeof(last_command_source), "%s",
             cmd->source == COO_CMD_SOURCE_SERIAL ? "serial" : "mqtt");
    last_command_time_ms = k_uptime_get();
}


static struct coo_cmd_response dispatch_command(const struct coo_cmd_request *cmd) {
    LOG_INF("Dispatching: %s", cmd->key);
    struct coo_cmd_response r;

    if (command_should_record_lastcommand(cmd)) {
        record_lastcommand(cmd);
    }

    r = coo_cmd_dispatch(cmd, dispatch_table, ARRAY_SIZE(dispatch_table),
                         coo_cmd_unknown_response, coo_cmd_unsupported_response);
    return r;
}

static bool mqtt_get_allowed_during_serial_guard(const char *key)
{
    const struct coo_cmd_dispatch_entry *entry = find_dispatch(key);

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

static bool command_payload_empty(const struct coo_cmd_request *cmd)
{
    return coo_cmd_payload_empty(cmd);
}

static bool route_loss_payload_has_value(const char *payload)
{
    static const char *const route_loss_value_keys[] = {
        "1028y", "1270j", "1430yj", "1430hk", "1510h", "2330k", "split",
    };
    char text[32];
    double value;

    if (payload == NULL) {
        return false;
    }

    for (uint8_t i = 0U; i < ARRAY_SIZE(route_loss_value_keys); ++i) {
        const char *key = route_loss_value_keys[i];

        if (coo_json_extract_double(payload, key, &value) == COO_JSON_EXTRACT_OK ||
            coo_json_extract_string(payload, key, text, sizeof(text)) == COO_JSON_EXTRACT_OK) {
            return true;
        }
    }

    return strstr(payload, "\"split\"") != NULL;
}

static enum coo_cmd_msg_type command_infer_msg_type(const struct coo_cmd_request *cmd,
                                                    void *user_data)
{
    float fval;

    ARG_UNUSED(user_data);

    if (cmd == NULL) {
        return COO_CMD_QUERY;
    }

    if (command_payload_empty(cmd)) {
        if (strcmp(cmd->key, "reboot") == 0 ||
            strcmp(cmd->key, "laserbank/clearfaults") == 0 ||
            strncmp(cmd->key, "laserbank/power/", strlen("laserbank/power/")) == 0 ||
            strncmp(cmd->key, "laserbank/heater/", strlen("laserbank/heater/")) == 0) {
            return COO_CMD_EFFECT;
        }
        return COO_CMD_QUERY;
    }

    if (strcmp(cmd->key, "status") == 0 ||
        strcmp(cmd->key, "laser/status") == 0 ||
        strcmp(cmd->key, "laser/engstatus") == 0) {
        return COO_CMD_QUERY;
    }

    if (strcmp(cmd->key, "memsroute/route_loss") == 0) {
        return route_loss_payload_has_value(cmd->payload) ? COO_CMD_EFFECT : COO_CMD_QUERY;
    }

    if (strcmp(cmd->key, "laser") == 0) {
        return coo_json_extract_float(cmd->payload, "level", &fval) != COO_JSON_EXTRACT_MISSING ?
               COO_CMD_EFFECT : COO_CMD_QUERY;
    }

    if (strcmp(cmd->key, "laser/tune") == 0) {
        return coo_json_extract_float(cmd->payload, "tune_nm", &fval) != COO_JSON_EXTRACT_MISSING ||
               coo_json_extract_float(cmd->payload, "delta_nm", &fval) != COO_JSON_EXTRACT_MISSING ?
               COO_CMD_EFFECT : COO_CMD_QUERY;
    }

    if (strcmp(cmd->key, "laser/settings") == 0) {
        char settings_json[MAX_PAYLOAD_LEN];

        return coo_json_extract_object(cmd->payload, "settings",
                                       settings_json, sizeof(settings_json)) != COO_JSON_EXTRACT_MISSING ?
               COO_CMD_EFFECT : COO_CMD_QUERY;
    }

    return COO_CMD_EFFECT;
}

/* Convert a few common human serial shorthands into the same JSON payloads MQTT
 * uses. This is deliberately a small translation table, not another dispatcher.
 * Examples: "power on", "serialguard off", "mems/foo A 0.5 30".
 */
static int serial_payload_from_shorthand(const char *key, const char *payload,
                                         char *out, size_t out_len,
                                         void *user_data)
{
    const char *cursor = payload;
    char t0[96] = {0};
    char t1[96] = {0};
    char t2[96] = {0};
    size_t off = 0;
    int written;

    ARG_UNUSED(user_data);

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

void command_handle_mqtt_publish(const struct mqtt_publish_param *pub)
{
    coo_cmd_runtime_handle_mqtt_publish(&command_runtime, pub);
}

static void command_serial_note_activity(void *user_data)
{
    const uint32_t holdoff_s = app_settings_get_serial_holdoff_s();
    int rc;

    ARG_UNUSED(user_data);

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

static bool command_network_mqtt_allowed(void)
{
    return atomic_get(&serial_network_ignore_active) == 0;
}

static bool command_mqtt_accept(const struct coo_cmd_request *cmd, void *user_data)
{
    ARG_UNUSED(user_data);

    return command_network_mqtt_allowed() ||
           (cmd != NULL && cmd->msg_type == COO_CMD_QUERY &&
            mqtt_get_allowed_during_serial_guard(cmd->key));
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
    const struct coo_cmd_runtime_config cfg = {
        .device_id = app_mqtt_device_id(),
        .inbound_queue = &inbound_queue,
        .outbound_queue = &outbound_queue,
        .execute_handler = dispatch_command,
        .dispatch_table = dispatch_table,
        .dispatch_count = ARRAY_SIZE(dispatch_table),
        .unknown_handler = coo_cmd_unknown_response,
        .unsupported_handler = coo_cmd_unsupported_response,
        .mqtt_msg_id = &mqtt_msg_id,
        .serial_wrap_column = SERIAL_WRAP_COLUMN,
        .classify = command_infer_msg_type,
        .mqtt_accept = command_mqtt_accept,
        .serial_activity = command_serial_note_activity,
        .serial_shorthand = serial_payload_from_shorthand,
    };
    int rc;

    rc = coo_cmd_runtime_configure(&command_runtime, &cfg);
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

struct coo_cmd_runtime *command_runtime_get(void)
{
    return &command_runtime;
}





/* COMMAND HANDLERS */


struct coo_cmd_response help_get(const struct coo_cmd_request *cmd)
{
    return coo_cmd_reply(cmd, COO_CMD_RESP_OK,
                        "{\"help\":\"help,ip,mqtt,time,temp,status,reboot,serialguard,"
                        "memsroute,mems,split,measure_throughput,laser,laserbank,"
                        "atten,pd,pdsettings\"}");
}

static int ip_status_payload(char *payload, size_t payload_len)
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
    int written;

    app_settings_get_ip(&ip_cfg);
    (void)network_get_ipv4_info(&net);
#if defined(CONFIG_SNTP)
    sntp_sync_get_status(&sntp);
    ntp_source = sntp_sync_source_str(sntp.source);
    ntp_server = sntp.server;
#endif

    written = snprintk(payload, payload_len,
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

    return (written >= 0 && (size_t)written < payload_len) ? 0 : -ENOSPC;
}

struct coo_cmd_response ip_get(const struct coo_cmd_request *cmd)
{
    char payload[MAX_PAYLOAD_LEN];

    if (ip_status_payload(payload, sizeof(payload)) != 0) {
        return coo_cmd_error(cmd, "ip response too large");
    }

    return coo_cmd_reply(cmd, COO_CMD_RESP_OK, payload);
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

struct coo_cmd_response ip_set(const struct coo_cmd_request *cmd)
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
        return coo_cmd_reply(cmd, COO_CMD_RESP_OK, response);
    }

    return coo_cmd_ok(cmd);
}

struct coo_cmd_response mqtt_get(const struct coo_cmd_request *cmd)
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
    return coo_cmd_reply(cmd, COO_CMD_RESP_OK, payload);
}

struct coo_cmd_response mqtt_set(const struct coo_cmd_request *cmd)
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

struct coo_cmd_response time_get(const struct coo_cmd_request *cmd)
{
    struct timespec ts = {0};
    uint64_t utc_ms;
    char payload[MAX_PAYLOAD_LEN];

    if (sys_clock_gettime(SYS_CLOCK_REALTIME, &ts) != 0) {
        return coo_cmd_error(cmd, "clock read failed");
    }
    utc_ms = ((uint64_t)ts.tv_sec * 1000ULL) + ((uint64_t)ts.tv_nsec / 1000000ULL);

    snprintk(payload, sizeof(payload),
             "{\"utc\":%llu,\"uptime\":%lld}",
             (unsigned long long)utc_ms, (long long)k_uptime_get());

    return coo_cmd_reply(cmd, COO_CMD_RESP_OK, payload);
}

struct coo_cmd_response time_set(const struct coo_cmd_request *cmd)
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

    if (sys_clock_settime(SYS_CLOCK_REALTIME, &ts) != 0) {
        return coo_cmd_error(cmd, "clock set failed");
    }

    return coo_cmd_ok(cmd);
}

struct coo_cmd_response reboot_set(const struct coo_cmd_request *cmd)
{
    int rc;

    rc = app_scheduled_action_schedule(APP_SCHEDULED_ACTION_REBOOT, K_MSEC(250));
    if (rc < 0) {
        return coo_cmd_error(cmd, "failed to schedule reboot");
    }

    return coo_cmd_ok(cmd);
}

struct coo_cmd_response serial_guard_get(const struct coo_cmd_request *cmd)
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
    return coo_cmd_reply(cmd, COO_CMD_RESP_OK, payload);
}

struct coo_cmd_response serial_guard_set(const struct coo_cmd_request *cmd)
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
    if (cmd->source == COO_CMD_SOURCE_SERIAL) {
        command_serial_note_activity(NULL);
    }
    return coo_cmd_ok(cmd);
}


struct coo_cmd_response status_get(const struct coo_cmd_request *cmd)
{
    struct tempsense_status ts = {0};
    struct laserbank_tempcontrol_status bank = {0};
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
    laserbank_tempcontrol_get_status(&bank);
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
        char ip_payload[MAX_PAYLOAD_LEN];

        if (ip_status_payload(ip_payload, sizeof(ip_payload)) != 0 ||
            coo_json_append(payload, sizeof(payload), &off,
                            ",\"ip\":%s", ip_payload) != 0) {
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

    return coo_cmd_reply(cmd, COO_CMD_RESP_OK, payload);
}

struct coo_cmd_response temp_get(const struct coo_cmd_request *cmd)
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

    return coo_cmd_reply(cmd, COO_CMD_RESP_OK, payload);
}
