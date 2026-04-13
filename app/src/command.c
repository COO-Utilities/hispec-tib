//
// Created by Jeb Bailey on 5/30/25.
//

#include "command.h"
// #include "devices.h"
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <strings.h>
#include <zephyr/sys/reboot.h>
#include <zephyr/sys/util.h>
#include <app_version.h>
#include <time.h>

#include "devices.h"
#include "app_settings.h"
#include "attenuator.h"
#include "maiman.h"
#include "mems_switching.h"
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

struct json_value_string {
    char value[MEMS_SWITCH_NAME_LEN];
};

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
    return coo_json_parse_key_pair(key, out_name, max_name, out_setting, max_setting);
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
                        "{\"help\":\"help,ip,time,status,reboot,serialguard,memsroute,mems,laser,power,atten\"}");
}

struct OutMsg ip_get(const struct Command *cmd)
{
    struct app_ip_settings ip_cfg;
    struct network_ipv4_info net = {0};
    char payload[MAX_PAYLOAD_LEN];

    app_settings_get_ip(&ip_cfg);
    (void)network_get_ipv4_info(&net);

    snprintk(payload, sizeof(payload),
             "{\"source\":\"%s\",\"trydhcpfirst\":%s,"
             "\"manual\":{\"ip\":\"%s\",\"subnet\":\"%s\",\"gateway\":\"%s\"},"
             "\"active\":{\"ready\":%s,\"ip\":\"%s\"}}",
             network_ipv4_source_str(net.source),
             ip_cfg.try_dhcp_first ? "true" : "false",
             ip_cfg.ip, ip_cfg.subnet, ip_cfg.gateway,
             net.link_ready ? "true" : "false",
             net.ip);

    return _msg_builder(cmd, RESP_OK, payload);
}

struct OutMsg ip_set(const struct Command *cmd)
{
    struct app_ip_settings ip_cfg;
    char response[MAX_PAYLOAD_LEN];
    bool persist = false;
    bool changed = false;
    bool unsupported_dhcp = false;
    bool unsupported_dns = false;
    bool unsupported_ntp = false;
    char buf[NET_IPV4_ADDR_LEN];

    app_settings_get_ip(&ip_cfg);

    if (coo_json_has_key(cmd->payload, "trydhcpfirst")) {
        if (!network_feature_dhcp_enabled()) {
            unsupported_dhcp = true;
        } else if (coo_json_extract_bool(cmd->payload, "trydhcpfirst", &ip_cfg.try_dhcp_first)) {
            changed = true;
        }
    }
    if (coo_json_has_key(cmd->payload, "preferdhcpdns")) {
        if (!network_feature_dns_enabled()) {
            unsupported_dns = true;
        } else if (coo_json_extract_bool(cmd->payload, "preferdhcpdns", &ip_cfg.prefer_dhcp_dns)) {
            changed = true;
        }
    }
    if (coo_json_has_key(cmd->payload, "preferdhcpntp")) {
        if (!network_feature_ntp_enabled()) {
            unsupported_ntp = true;
        } else if (coo_json_extract_bool(cmd->payload, "preferdhcpntp", &ip_cfg.prefer_dhcp_ntp)) {
            changed = true;
        }
    }
    if (coo_json_extract_string(cmd->payload, "ip", buf, sizeof(buf))) {
        strncpy(ip_cfg.ip, buf, sizeof(ip_cfg.ip) - 1);
        ip_cfg.ip[sizeof(ip_cfg.ip) - 1] = '\0';
        changed = true;
    }
    if (coo_json_extract_string(cmd->payload, "subnet", buf, sizeof(buf))) {
        strncpy(ip_cfg.subnet, buf, sizeof(ip_cfg.subnet) - 1);
        ip_cfg.subnet[sizeof(ip_cfg.subnet) - 1] = '\0';
        changed = true;
    }
    if (coo_json_extract_string(cmd->payload, "gateway", buf, sizeof(buf))) {
        strncpy(ip_cfg.gateway, buf, sizeof(ip_cfg.gateway) - 1);
        ip_cfg.gateway[sizeof(ip_cfg.gateway) - 1] = '\0';
        changed = true;
    }
    if (coo_json_has_key(cmd->payload, "dns")) {
        if (!network_feature_dns_enabled()) {
            unsupported_dns = true;
        } else if (coo_json_extract_string(cmd->payload, "dns", buf, sizeof(buf))) {
            strncpy(ip_cfg.dns, buf, sizeof(ip_cfg.dns) - 1);
            ip_cfg.dns[sizeof(ip_cfg.dns) - 1] = '\0';
            changed = true;
        }
    }
    if (coo_json_has_key(cmd->payload, "ntp")) {
        if (!network_feature_ntp_enabled()) {
            unsupported_ntp = true;
        } else if (coo_json_extract_string(cmd->payload, "ntp", buf, sizeof(buf))) {
            strncpy(ip_cfg.ntp, buf, sizeof(ip_cfg.ntp) - 1);
            ip_cfg.ntp[sizeof(ip_cfg.ntp) - 1] = '\0';
            changed = true;
        }
    }
    (void)coo_json_extract_bool(cmd->payload, "persistent", &persist);

    if (!changed && !(unsupported_dhcp || unsupported_dns || unsupported_ntp)) {
        return _msg_builder(cmd, RESP_ERROR, "{\"status\":\"error\",\"msg\":\"no recognized ip fields\"}");
    }

    if (changed) {
        app_settings_update_ip(&ip_cfg, persist);
    }

    if (unsupported_dhcp || unsupported_dns || unsupported_ntp) {
        snprintk(response, sizeof(response),
                 "{\"status\":\"partial\",\"dhcp\":\"%s\",\"dns\":\"%s\",\"ntp\":\"%s\",\"apply\":\"reboot_required\"}",
                 unsupported_dhcp ? "unsupported" : "ok",
                 unsupported_dns ? "unsupported" : "ok",
                 unsupported_ntp ? "unsupported" : "ok");
        return _msg_builder(cmd, RESP_OK, response);
    }

    return _msg_builder(cmd, RESP_OK, "{\"status\":\"success\",\"apply\":\"reboot_required\"}");
}

struct OutMsg time_get(const struct Command *cmd)
{
    struct timespec ts = {0};
    uint64_t utc_ms;
    char payload[MAX_PAYLOAD_LEN];

    clock_gettime(CLOCK_REALTIME, &ts);
    utc_ms = ((uint64_t)ts.tv_sec * 1000ULL) + ((uint64_t)ts.tv_nsec / 1000000ULL);

    snprintk(payload, sizeof(payload),
             "{\"utc\":%llu,\"ticks\":%u,\"uptime\":%lld}",
             (unsigned long long)utc_ms, k_cycle_get_32(), (long long)k_uptime_get());

    return _msg_builder(cmd, RESP_OK, payload);
}

struct OutMsg time_set(const struct Command *cmd)
{
    uint64_t utc_ms = 0;
    struct timespec ts = {0};

    if (!coo_json_extract_u64(cmd->payload, "linuxtime_ms", &utc_ms)) {
        return _msg_builder(cmd, RESP_ERROR, "{\"status\":\"error\",\"msg\":\"missing linuxtime_ms\"}");
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

    if (!coo_json_extract_u32(cmd->payload, "seconds", &holdoff_s) &&
        !coo_json_extract_u32(cmd->payload, "value", &holdoff_s)) {
        return _msg_builder(cmd, RESP_ERROR, "{\"status\":\"error\",\"msg\":\"missing seconds\"}");
    }

    (void)coo_json_extract_bool(cmd->payload, "persistent", &persist);
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

    // Parse { "value": [input_string, output_string] }
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

        rc = mems_switch_set_state(sw, step->state);

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



struct OutMsg mems_get(const struct Command *cmd) {


    if (strcmp(cmd->key, "mems") == 0) {
        char payload[MAX_PAYLOAD_LEN] = {0};
        size_t off = 0;

        off += snprintk(payload + off, sizeof(payload) - off, "{");
        for (uint8_t i = 0; i < router.num_switches; ++i) {
            char state = 'U';

            if (i > 0U) {
                off += snprintk(payload + off, sizeof(payload) - off, ",");
            }

            (void)mems_switch_get_state(router.switches[i], &state);
            off += snprintk(payload + off, sizeof(payload) - off,
                            "\"%s\":\"%c\"",
                            router.switches[i]->name, state);
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

    char state;
    mems_switch_get_state(sw, &state);

    char payload[MAX_PAYLOAD_LEN]={0};
    snprintf(payload, MAX_PAYLOAD_LEN, "{\"value\":\"%c\"}", state);
    return _msg_builder(cmd, RESP_OK, payload);
}


struct OutMsg mems_set(const struct Command *cmd) {

    char name[16], mems_switch[16];
    if  (parse_key_pair(cmd->key, name, 15, mems_switch, 15)!=0) {
        return _msg_builder(cmd, RESP_ERROR,"{\"error\":\"Failed to parse mems switch name\"}");
    }

    // Parse { "value": "" }
    struct json_value_string in_data = {0};
    struct json_obj_descr d[] = {
        JSON_OBJ_DESCR_PRIM(struct json_value_string, value, JSON_TOK_STRING),
    };
    if (json_obj_parse((char *) cmd->payload, cmd->payload_len, d, ARRAY_SIZE(d), &in_data) < 0) {
        return _msg_builder(cmd, RESP_ERROR, "{\"error\":\"Failed to parse switch state\"}");
    }

    struct mems_switch *sw = mems_router_find_switch(&router, mems_switch);

    if (sw==NULL) {
        return _msg_builder(cmd, RESP_ERROR,"{\"error\":\"Invalid switch name\"}");
    }

    if (mems_switch_set_state(sw, in_data.value[0])!=0) {
        return _msg_builder(cmd, RESP_ERROR,"{\"error\":\"Invalid switch state\"}");
    }

    return _msg_builder(cmd, RESP_OK, "{\"status\":\"OK\"}");
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

    if (!coo_json_extract_bool(cmd->payload, "value", &value)) {
        return _msg_builder(cmd, RESP_ERROR,"{\"error\":\"Missing setting value\"}");
    }

    if (value) enable_power();
    else disable_power();
    return _msg_builder(cmd, RESP_OK,"{\"status\":\"OK\"}");
}

struct OutMsg sleep_set(const struct Command *cmd) {

    bool value;

    if (!coo_json_extract_bool(cmd->payload, "value", &value)) {
        return _msg_builder(cmd, RESP_ERROR,"{\"error\":\"Missing setting value\"}");
    }

    //TODO
    ARG_UNUSED(value);

    return _msg_builder(cmd, RESP_OK,"{\"status\":\"OK\"}");
}
