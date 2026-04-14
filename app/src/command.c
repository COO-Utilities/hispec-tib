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
#include <zephyr/sys/reboot.h>
#include <zephyr/sys/util.h>
#include <app_version.h>
#include <time.h>
#include <zephyr/net/net_ip.h>

#include "devices.h"
#include "app_settings.h"
#include "attenuator.h"
#include "maiman.h"
#include "mems_switching.h"
#include "sntp_sync.h"
#include <coo_commons/json_utils.h>
#include <coo_commons/network.h>
LOG_MODULE_REGISTER(command, LOG_LEVEL_DBG);


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

static void reboot_work_handler(struct k_work *work)
{
    ARG_UNUSED(work);
    sys_reboot(SYS_REBOOT_COLD);
}

static struct k_work_delayable reboot_work;
static bool reboot_work_initialized;





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

struct OutMsg help_get(const struct Command *cmd)
{
    return _msg_builder(cmd, RESP_OK,
                        "{\"help\":\"help,ip,mqtt,time,status,reboot,serialguard,memsroute,mems,laser,power,atten\"}");
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
    if (!reboot_work_initialized) {
        k_work_init_delayable(&reboot_work, reboot_work_handler);
        reboot_work_initialized = true;
    }

    k_work_schedule(&reboot_work, K_MSEC(250));
    return _msg_builder(cmd, RESP_OK, "{\"status\":\"success\"}");
}

struct OutMsg serial_guard_get(const struct Command *cmd)
{
    char payload[MAX_PAYLOAD_LEN];
    snprintk(payload, sizeof(payload), "{\"serialguard_s\":%u}", app_settings_get_serial_holdoff_s());
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

        rc = mems_switch_set_state(sw, step->state, 0,0);

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
             "{\"state\":\"%s\",\"duty_cycle\":%.3f,\"toggle_rate_hz\":%.3f,\"stopafter_s\":%u}",
             state_buf,
             (double)status.duty_cycle,
             (double)status.toggle_rate_hz,
             status.stopafter_s);

    return _msg_builder(cmd, RESP_OK, payload);
}


struct OutMsg mems_get(const struct Command *cmd) {


    if (strcmp(cmd->key, "mems") == 0) {
        char payload[MAX_PAYLOAD_LEN] = {0};
        size_t off = 0;
        struct mems_switch_status status = {0};
        char state_buf[4] = {0};

        off += snprintk(payload + off, sizeof(payload) - off, "{");
        for (uint8_t i = 0; i < router.num_switches; ++i) {
            //TODO this for loop likely exceeds maximum buffer
            if (i > 0U) {
                off += snprintk(payload + off, sizeof(payload) - off, ",");
            }

            mems_switch_get_status(router.switches[i], &status);
            mems_format_state(&status, state_buf, sizeof(state_buf));
            off += snprintk(payload + off, sizeof(payload) - off,
                            "\"%s\":{\"state\":\"%s\",\"duty_cycle\":%.3f,\"toggle_rate_hz\":%.3f,\"stopafter_s\":%u}",
                            router.switches[i]->name,
                            state_buf,
                            (double)status.duty_cycle,
                            (double)status.toggle_rate_hz,
                            status.stopafter_s);
        }
        snprintk(payload + off, sizeof(payload) - off, "}");
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
    uint32_t stopafter_s_u32 = 0U;
    bool has_duty_cycle = false;
    bool has_stopafter_s = false;
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
        rc = mems_switch_set_state(sw, requested_state[0], duty_cycle, stopafter_s_u32);
    } else {
        rc = mems_switch_set_state(sw, requested_state[0], 0, 0);
    }

    if (rc == -ERANGE) {
        return _msg_builder(cmd, RESP_ERROR, "{\"error\":\"MEMS setting out of range\"}");
    }
    if (rc != 0) {
        return _msg_builder(cmd, RESP_ERROR, "{\"error\":\"Invalid MEMS setting\"}");
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
