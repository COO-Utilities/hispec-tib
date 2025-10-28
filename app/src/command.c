//
// Created by Jeb Bailey on 5/30/25.
//

#include "command.h"
// #include "devices.h"
#include <ctype.h>
#include <stdio.h>
#include <strings.h>

#include "devices.h"
#include "attenuator.h"
#include "maiman.h"
#include "mems_switching.h"
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


struct json_type_msg {
    char msg_type[8];
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
    { "memsroute",  memsroute_get,    memsroute_set    },
    { "mems",       mems_get,    mems_set    },
    { "laser",      laser_setting_get,laser_setting_set},
    { "power",      power_get,        power_set        },
    { "atten",      atten_setting_get,  atten_setting_set  },
    { "status",     status_get,       NULL  },
    { "sleep",      NULL,  sleep_set  }, // GET only
};


const struct DispatchEntry *find_dispatch(const char *key) {
    //TODO
    for (size_t i = 0; i < ARRAY_SIZE(dispatch_table); ++i) {
        if (strcmp(dispatch_table[i].key, key) == 0) {
            return &dispatch_table[i];
        }
    }
    return NULL;
}


struct OutMsg dispatch_command(const struct Command *cmd) {
    LOG_INF("Dispatching: %s", cmd->key);
    struct OutMsg r;

    const struct DispatchEntry *entry = find_dispatch(cmd->key);
    if (!entry) {
        r = unknown_response(cmd);
    } else {
        DispatchFunc func = (cmd->msg_type == SET) ? entry->set_handler : entry->get_handler;
        r = func==NULL ? unsupported_response(cmd) : func(cmd);
    }
    return r;
}


int parse_key_pair(const char *key,
                   char *out_name, size_t max_name,
                   char *out_setting, size_t max_setting)
{
    // Find the first slash
    const char *slash = strchr(key, '/');
    if (!slash) {
        return -1;
    }

    size_t name_len = slash - key;
    if (name_len == 0 || name_len >= max_name) {
        // Name empty or too long for buffer (including null)
        return -2;
    }

    // Copy name
    memcpy(out_name, key, name_len);
    out_name[name_len] = '\0';

    // Copy setting, up to max_setting-1 characters, null terminated
    const char *setting_start = slash + 1;
    size_t setting_len = strcspn(setting_start, "/"); // Up to next '/', or full string
    if (setting_len == 0 || setting_len >= max_setting) {
        // Setting empty or too long for buffer
        return -3;
    }
    memcpy(out_setting, setting_start, setting_len);
    out_setting[setting_len] = '\0';

    return 0;
}



bool parse_msg_type_from_payload(const char *payload, enum MsgType *msg_type_out)
{

    struct json_type_msg msg = {0};
    const struct json_obj_descr descr[] = {
        JSON_OBJ_DESCR_PRIM(struct json_type_msg, msg_type, JSON_TOK_STRING)
    };

    int rc = json_obj_parse((char *) payload, strlen(payload), descr, ARRAY_SIZE(descr), &msg);
    if (rc < 0) {
        return false;
    }

    // Case-insensitive check for supported types
    if (strncasecmp(msg.msg_type, "get", 4) == 0) {
        *msg_type_out = GET;
        return true;
    }
    if (strncasecmp(msg.msg_type, "set", 4) == 0) {
        *msg_type_out = SET;
        return true;
    }
    return false;
}

struct OutMsg _msg_builder(const struct Command *cmd, enum MsgType msgtyp, const char *msg) {
    struct OutMsg r = { 0 };
    r.msg_type = msgtyp;
    r.qos = MQTT_QOS_1_AT_LEAST_ONCE;

    //        snprintf(r.payload, MAX_PAYLOAD_LEN, "{\"error\":\"Invalid route\"}");


    // Set default response topic, but override if cmd provides a valid one
    strncpy(r.topic, "cmd/hsfib-tib/resp", sizeof(r.topic) - 1);
    if (cmd && strlen(cmd->response_topic) > 0 && strlen(cmd->response_topic) < sizeof(r.topic)) {
        strncpy(r.topic, cmd->response_topic, sizeof(r.topic) - 1);
    }

    // Echo correlation_data if present
    if (cmd && cmd->corr_len > 0 && cmd->corr_len < sizeof(r.correlation_data)) {
        memcpy(r.correlation_data, cmd->correlation_data, cmd->corr_len);
        r.corr_len = cmd->corr_len;
    }

    r.payload_len = strlen(msg);
    strncpy(r.payload, msg, sizeof(r.payload) - 1);
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
    if (strncasecmp(laser_name, "1430yj", 7) == 0) {
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
    int val = gpio_pin_get_dt(&power_gpio);
    return val==1;
}

bool enable_power() {
    if (power_enabled())
        return false;
    int err = gpio_pin_set_dt(&power_gpio, 1);
    if (err) {
        LOG_ERR("Failed to set POWER_GPIO high\n");
    }
    return true;
}

bool disable_power() {
    if (!power_enabled())
        return false;
    //TODO set POWER_GPIO low
    int err = gpio_pin_set_dt(&power_gpio, 0);
    if (err) {
        LOG_ERR("Failed to set POWER_GPIO low\n");
    }
    return true;

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
    if (strcasecmp(setting, "coeff")) {
        snprintf(payload, MAX_PAYLOAD_LEN,
                 "{\"db2volt\":[%.4f,%.4f,%.4f],\"volt2db\":[%.4f,%.4f,%.4f]}",
                 attenuators[laser_id].coeff_db_to_volt[0],
                 attenuators[laser_id].coeff_db_to_volt[1],
                 attenuators[laser_id].coeff_db_to_volt[2],
                 attenuators[laser_id].coeff_volt_to_db[0],
                 attenuators[laser_id].coeff_volt_to_db[1],
                 attenuators[laser_id].coeff_volt_to_db[2]);
    } else if (strcasecmp(setting, "value") || strcasecmp(setting, "valuedb")) {
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

    if (strcasecmp(setting, "coeff")) {

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

    } else if (strcasecmp(setting, "value")) {

        struct json_value_float in_data = {0};
        struct json_obj_descr d[] = {
            JSON_OBJ_DESCR_PRIM(struct json_value_float, value, JSON_TOK_NUMBER)
        };
        if (json_obj_parse((char *) cmd->payload, cmd->payload_len, d, 1, &in_data) < 0) {
            return _msg_builder(cmd, RESP_ERROR,"{\"error\":\"Missing setting value\"}");
        }
        attenuator_set(&attenuators[laser_id], in_data.value, true);

    } else if (strcasecmp(setting, "valuedb")) {
        struct json_value_float in_data = {0};
        struct json_obj_descr d[] = {
            JSON_OBJ_DESCR_PRIM(struct json_value_float, value, JSON_TOK_NUMBER)
        };
        if (json_obj_parse((char *) cmd->payload, cmd->payload_len, d, 1, &in_data) < 0) {
            return _msg_builder(cmd, RESP_ERROR,"{\"error\":\"Missing setting value\"}");
        }
        attenuator_set(&attenuators[laser_id], in_data.value, false);

    } else {
        return _msg_builder(cmd, RESP_ERROR,"{\"error\":\"Invalid setting\"}");
    }

    return _msg_builder(cmd, RESP_OK, "{\"status\":\"OK\"}");
}


struct OutMsg status_get(const struct Command *cmd) {
    char payload[MAX_PAYLOAD_LEN]={0};
    snprintf(payload, MAX_PAYLOAD_LEN, "{\"power\":%s}", power_enabled() ? "true" : "false");
    return _msg_builder(cmd, RESP_OK, payload);
}


struct OutMsg power_get(const struct Command *cmd) {
    char payload[MAX_PAYLOAD_LEN]={0};
    snprintf(payload, MAX_PAYLOAD_LEN, "{\"power\":%s}", power_enabled() ? "true" : "false");
    return _msg_builder(cmd, RESP_OK, payload);
}



struct OutMsg power_set(const struct Command *cmd) {

    // Parse values
    struct json_value_bool args = {.value=false};
    struct json_obj_descr d[] = {
        JSON_OBJ_DESCR_PRIM(struct json_value_bool, value, JSON_TOK_TRUE)
    };
    if (json_obj_parse((char *) cmd->payload, cmd->payload_len, d,1, &args) < 0) {
        return _msg_builder(cmd, RESP_ERROR,"{\"error\":\"Missing setting value\"}");
    }

    if (args.value) enable_power();
    else disable_power();
    return _msg_builder(cmd, RESP_ERROR,"{\"status\":\"OK\"}");
}

struct OutMsg sleep_set(const struct Command *cmd) {

    // Parse values
    struct json_value_bool args = {.value=false};
    struct json_obj_descr d[] = {
        JSON_OBJ_DESCR_PRIM(struct json_value_bool, value, JSON_TOK_TRUE)
    };
    if (json_obj_parse((char *) cmd->payload, cmd->payload_len, d,1, &args) < 0) {
        return _msg_builder(cmd, RESP_ERROR,"{\"error\":\"Missing setting value\"}");
    }

    //TODO do anything necessary to grasefully shupdown the lasers.
    if (args.value) disable_power();
    return _msg_builder(cmd, RESP_ERROR,"{\"status\":\"OK\"}");
}