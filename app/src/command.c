/**
 * @file command.c
 * @brief HISPEC command table, request classification, and app command handlers.
 *
 * The common command runtime owns MQTT/serial topic handling, executor loops,
 * warning publication, and outbound drain behavior. This file supplies the
 * static command spec table, app serial shorthand callback, help metadata, and
 * command handlers that cut across domains.
 */

#include "command.h"
#include <errno.h>
#include <math.h>
#include <strings.h>
#include <string.h>
#include <zephyr/sys/clock.h>
#include <zephyr/sys/util.h>
#include <hispec_build_version.h>
#include <time.h>
#include <zephyr/net/net_ip.h>

#include "devices.h"
#include "lasers.h"
#include "app_identity.h"
#include "app_settings.h"
#include "attenuator.h"
#include "attenuator_command.h"
#include "laser_command.h"
#include "laserbank_tempcontrol.h"
#include "mems_command.h"
#include "mems_switching.h"
#include "photodiode_command.h"
#include "throughput_command.h"
#include "throughput_monitor.h"
#if defined(CONFIG_SNTP)
#include "sntp_sync.h"
#endif
#include "housekeeping.h"
#include <coo_commons/json_utils.h>
#include <coo_commons/mqtt_client.h>
#include <coo_commons/network.h>

LOG_MODULE_REGISTER(command, LOG_LEVEL_DBG);

#define SERIAL_WRAP_COLUMN COO_CMD_SERIAL_WRAP_COLUMN
#define COMMAND_REBOOT_DELAY_MS 3000U

static uint16_t mqtt_msg_id = 1;

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

static bool command_tib_supported(const struct coo_cmd_spec *spec, void *user_data);
static enum coo_cmd_msg_type classify_route_loss(const struct coo_cmd_request *cmd,
                                                 const struct coo_cmd_spec *spec,
                                                 void *user_data);
static enum coo_cmd_msg_type classify_laser_level(const struct coo_cmd_request *cmd,
                                                  const struct coo_cmd_spec *spec,
                                                  void *user_data);
static enum coo_cmd_msg_type classify_laser_tune(const struct coo_cmd_request *cmd,
                                                 const struct coo_cmd_spec *spec,
                                                 void *user_data);
static enum coo_cmd_msg_type classify_laser_settings(const struct coo_cmd_request *cmd,
                                                     const struct coo_cmd_spec *spec,
                                                     void *user_data);
static enum coo_cmd_msg_type classify_pd(const struct coo_cmd_request *cmd,
                                         const struct coo_cmd_spec *spec,
                                         void *user_data);
static int catalog_get(const struct coo_cmd_request *cmd,
                       struct coo_cmd_response *out);
static int serial_mems_switch_shorthand(const char *key, const char *payload,
                                        char *out, size_t out_len,
                                        void *user_data);
static void command_prepare_reboot(bool erase_non_ip_settings, void *user_data);

#define CMD_HELP(_usage, _args, _values, _notes, _flags) \
    .help = &(const struct coo_cmd_help_entry){ \
        .usage = (_usage), .args = (_args), .values = (_values), \
        .notes = (_notes), .flags = (_flags) }

#define CMD_SPEC(_key, _get, _set, _class, _guard, _keys, _usage, _args, _values, _notes, _flags) \
    { .key = (_key), .query_handler = (_get), .effect_handler = (_set), \
      .class_policy = (_class), .mqtt_query_allowed_during_serial_guard = (_guard), \
      .allowed_payload_keys = (_keys), \
      CMD_HELP(_usage, _args, _values, _notes, _flags) }

#define CMD_SPEC_PREFIX(_key, _get, _set, _class, _guard, _keys, _usage, _args, _values, _notes, _flags) \
    { .key = (_key), .query_handler = (_get), .effect_handler = (_set), \
      .class_policy = (_class), .mqtt_query_allowed_during_serial_guard = (_guard), \
      .key_prefix_match = true, .allowed_payload_keys = (_keys), \
      CMD_HELP(_usage, _args, _values, _notes, _flags) }

#define CMD_SPEC_CUSTOM(_key, _get, _set, _classify, _guard, _keys, _usage, _args, _values, _notes, _flags) \
    { .key = (_key), .query_handler = (_get), .effect_handler = (_set), \
      .class_policy = COO_CMD_CLASS_CUSTOM, .custom_classify = (_classify), \
      .mqtt_query_allowed_during_serial_guard = (_guard), \
      .allowed_payload_keys = (_keys), \
      CMD_HELP(_usage, _args, _values, _notes, _flags) }

#define CMD_SPEC_TIB(_key, _get, _set, _class, _guard, _keys, _usage, _args, _values, _notes, _flags) \
    { .key = (_key), .query_handler = (_get), .effect_handler = (_set), \
      .class_policy = (_class), .supported = command_tib_supported, \
      .mqtt_query_allowed_during_serial_guard = (_guard), \
      .allowed_payload_keys = (_keys), \
      CMD_HELP(_usage, _args, _values, _notes, _flags) }

#define CMD_SPEC_TIB_PREFIX(_key, _get, _set, _class, _guard, _keys, _usage, _args, _values, _notes, _flags) \
    { .key = (_key), .query_handler = (_get), .effect_handler = (_set), \
      .class_policy = (_class), .supported = command_tib_supported, \
      .mqtt_query_allowed_during_serial_guard = (_guard), \
      .key_prefix_match = true, .allowed_payload_keys = (_keys), \
      CMD_HELP(_usage, _args, _values, _notes, _flags) }

#define CMD_SPEC_TIB_CUSTOM(_key, _get, _set, _classify, _guard, _keys, _usage, _args, _values, _notes, _flags) \
    { .key = (_key), .query_handler = (_get), .effect_handler = (_set), \
      .class_policy = COO_CMD_CLASS_CUSTOM, .custom_classify = (_classify), \
      .supported = command_tib_supported, \
      .mqtt_query_allowed_during_serial_guard = (_guard), \
      .allowed_payload_keys = (_keys), \
      CMD_HELP(_usage, _args, _values, _notes, _flags) }

#define CMD_HELP_ONLY(_key, _supported, _usage, _args, _values, _notes, _flags) \
    { .key = (_key), .supported = (_supported), \
      CMD_HELP(_usage, _args, _values, _notes, _flags) }

/*
 * One static row owns dispatch, classification, serial-guard query allowance,
 * and help for a command key. Help-only rows document parameterized command
 * forms whose real dispatch is handled by a shorter prefix row.
 */
static const struct coo_cmd_spec command_specs[] = {
    CMD_SPEC("ip", ip_get, ip_set, COO_CMD_CLASS_DEFAULT, true,
             "trydhcpfirst,preferdhcpdns,preferdhcpntp,ip,subnet,gateway,dns,ntp,persist",
             "ip [trydhcpfirst=<bool> preferdhcpdns=<bool> preferdhcpntp=<bool> ip=<IPv4> subnet=<IPv4> gateway=<IPv4> dns=<IPv4> ntp=<IPv4> persist=<bool>]",
             "query with no payload; effect when any listed field is supplied",
             "bool: true|false|on|off|yes|no",
             "reconfigures IPv4 immediately; persist=true stores app-owned IP settings",
             COO_CMD_HELP_QUERY | COO_CMD_HELP_EFFECT | COO_CMD_HELP_SERIAL_GUARD_QUERY),
    { .key = "mqtt", .query_handler = mqtt_get, .effect_handler = mqtt_set,
      .class_policy = COO_CMD_CLASS_DEFAULT,
      .serial_positional = { .field = { "broker", "persist" }, .required_count = 1U },
      .allowed_payload_keys = "broker,persist",
      .mqtt_query_allowed_during_serial_guard = true,
      CMD_HELP("mqtt [broker=<host-or-ip:port> persist=<bool>]",
               "broker is required for effect; persist is optional",
               "broker examples: 192.168.1.5:1883, hispec.caltech.edu:1883",
               "runtime broker changes cause the main loop to reconnect",
               COO_CMD_HELP_QUERY | COO_CMD_HELP_EFFECT | COO_CMD_HELP_SERIAL_GUARD_QUERY) },
    { .key = "time", .query_handler = time_get, .effect_handler = time_set,
      .class_policy = COO_CMD_CLASS_DEFAULT,
      .serial_positional = { .field = { "unix_ms" }, .required_count = 1U,
                             .numeric_mask = BIT(0) },
      .allowed_payload_keys = "unix_ms",
      .mqtt_query_allowed_during_serial_guard = true,
      CMD_HELP("time [unix_ms=<utc-ms>]",
               "unix_ms required for effect",
               "unsigned millisecond Unix epoch",
               "sets Zephyr realtime clock and records last known UTC",
               COO_CMD_HELP_QUERY | COO_CMD_HELP_EFFECT | COO_CMD_HELP_SERIAL_GUARD_QUERY) },
    CMD_SPEC("temp", temp_get, NULL, COO_CMD_CLASS_DEFAULT, true, "",
             "temp", "none", NULL, "cached housekeeping temperature status",
             COO_CMD_HELP_QUERY | COO_CMD_HELP_SERIAL_GUARD_QUERY),
    CMD_SPEC("status", status_get, NULL, COO_CMD_CLASS_ALWAYS_QUERY, true,
             "ip,lasers,attens",
             "status [ip=<bool> lasers=<bool> attens=<bool>]",
             "all fields optional",
             "bool: true|false|on|off|yes|no",
             "query-only firmware and subsystem status",
             COO_CMD_HELP_QUERY | COO_CMD_HELP_SERIAL_GUARD_QUERY),
    CMD_SPEC("catalog", catalog_get, NULL, COO_CMD_CLASS_ALWAYS_QUERY, true, "",
             "catalog",
             "none",
             NULL,
             "query-only static laser names and selected board route names",
             COO_CMD_HELP_QUERY | COO_CMD_HELP_SERIAL_GUARD_QUERY),
    CMD_SPEC_CUSTOM("memsroute/route_loss", memsroute_get, memsroute_set,
                    classify_route_loss, true,
                    "route,1028y,1270j,1430yj,1430hk,1510h,2330k,split,persist",
                    "memsroute/route_loss route=<route> [<laser>=<transmission> ... persist=<bool>]",
                    "route required for effect; laser fields optional by query/effect mode",
                    "laser fields: 1028y,1270j,1430yj,1430hk,1510h,2330k,split",
                    "stores user route-loss estimates used by optical calculations",
                    COO_CMD_HELP_QUERY | COO_CMD_HELP_EFFECT | COO_CMD_HELP_SERIAL_GUARD_QUERY),
    CMD_SPEC("memsroute", memsroute_get, memsroute_set,
             COO_CMD_CLASS_DEFAULT, true, "input,output,force",
             "memsroute input=<input> output=<output> [force=<bool>]",
             "input and output required; force=true re-pulses route steps",
             "route names are board profile input/output route keys",
             "applies a named static MEMS route",
             COO_CMD_HELP_QUERY | COO_CMD_HELP_EFFECT | COO_CMD_HELP_SERIAL_GUARD_QUERY),
    { .key = "mems", .query_handler = mems_get, .effect_handler = mems_set,
      .class_policy = COO_CMD_CLASS_DEFAULT,
      .serial_shorthand = serial_mems_switch_shorthand,
      .key_prefix_match = true,
      .allowed_payload_keys = "state,duty_cycle,off_in_s,cycle_ms,force,toggle_rate_hz,value",
      .mqtt_query_allowed_during_serial_guard = true },
    CMD_HELP_ONLY("mems/<switchname>", NULL,
                  "mems/<switchname> [state=<A|B> force=<bool> duty_cycle=<0..1> cycle_ms=<ms> off_in_s=<s>]",
                  "state required for effect; force is static only; duty_cycle, cycle_ms, and off_in_s optional",
                  "switchname is one active board MEMS switch name",
                  "serial shorthand accepts: mems/<switchname> A [duty_cycle] [off_in_s]",
                  COO_CMD_HELP_QUERY | COO_CMD_HELP_EFFECT | COO_CMD_HELP_SERIAL_GUARD_QUERY),
    CMD_SPEC_PREFIX("split", splitting_get, splitting_set,
             COO_CMD_CLASS_DEFAULT, true,
             "channel,ratio1,ratio2,ratio3,cycle_ms,off_in_s,toggle_rate_hz",
             "split [channel=<yj|hk> ratio1=<0..1> ratio2=<0..1> cycle_ms=<ms> off_in_s=<s>]",
             "channel, ratio1, and ratio2 are required for effect; cycle_ms and off_in_s optional",
             "channel: yj,hk",
             "split/yj and split/hk query current splitter state",
             COO_CMD_HELP_QUERY | COO_CMD_HELP_EFFECT | COO_CMD_HELP_SERIAL_GUARD_QUERY),
    CMD_SPEC_TIB("measure_throughput", NULL, measure_throughput_set,
                 COO_CMD_CLASS_DEFAULT, false,
                 "laser,fiber,input,output,autolevel,max_flux_ph_s,off_in_s,format,stop",
                 "measure_throughput laser=<laser|none> [fiber=<M|S> input=<name> output=<name> autolevel=<bool> max_flux_ph_s=<value> off_in_s=<s> format=<json|binary>]",
                 "stop with stop=<yj|hk|all>; laser=none requires input, output, and autolevel=false",
                 "format: json,binary",
                 "TIB-only throughput monitor command",
                 COO_CMD_HELP_EFFECT),
    CMD_SPEC_TIB_CUSTOM("laser", laser_get, laser_set, classify_laser_level,
                        true, "name,level,autooff_s",
                        "laser name=<laser> [level=<percent> autooff_s=<s>]",
                        "name required; level makes it an effect",
                        "laser: 1028y,1270j,1430yj,1430hk,1510h,2330k",
                        "TIB-only laser output status/set command",
                        COO_CMD_HELP_QUERY | COO_CMD_HELP_EFFECT | COO_CMD_HELP_SERIAL_GUARD_QUERY),
    CMD_SPEC_TIB_CUSTOM("laser/tune", laser_tune_get, laser_tune_set,
                        classify_laser_tune, true, "name,tune_nm,delta_nm",
                        "laser/tune name=<laser> [tune_nm=<nm>|delta_nm=<nm>]",
                        "name required; tune_nm or delta_nm makes it an effect",
                        "laser: 1028y,1270j,1430yj,1430hk,1510h,2330k",
                        "TIB-only stored tune request",
                        COO_CMD_HELP_QUERY | COO_CMD_HELP_EFFECT | COO_CMD_HELP_SERIAL_GUARD_QUERY),
    CMD_SPEC_TIB("laser/status", laser_status_get, NULL,
                 COO_CMD_CLASS_ALWAYS_QUERY, true, "name",
                 "laser/status name=<laser>",
                 "name required",
                 "laser: 1028y,1270j,1430yj,1430hk,1510h,2330k",
                 "TIB-only detailed engineering status; may perform slow Modbus reads",
                 COO_CMD_HELP_QUERY | COO_CMD_HELP_SERIAL_GUARD_QUERY),
    CMD_SPEC_TIB_CUSTOM("laser/settings", laser_settings_get, laser_settings_set,
                        classify_laser_settings, true, "name,settings,persist",
                        "laser/settings name=<laser> [settings={...} persist=<bool>]",
                        "name required; settings object required for effect",
                        "laser: 1028y,1270j,1430yj,1430hk,1510h,2330k",
                        "TIB-only app-owned laser policy/settings wrapper",
                        COO_CMD_HELP_QUERY | COO_CMD_HELP_EFFECT | COO_CMD_HELP_SERIAL_GUARD_QUERY),
    CMD_SPEC_TIB_PREFIX("laserbank/power", laserbank_power, laserbank_power,
                 COO_CMD_CLASS_SUFFIX_OR_PAYLOAD_EFFECT, true, "mode",
                 "laserbank/power [mode=<auto|override_on|override_off>]",
                 "mode required for effect; suffix form laserbank/power/<mode> also works",
                 "mode: auto,override_on,override_off",
                 "TIB-only laser-bank supply override",
                 COO_CMD_HELP_QUERY | COO_CMD_HELP_EFFECT | COO_CMD_HELP_SERIAL_GUARD_QUERY),
    CMD_SPEC_TIB("laserbank/clearfaults", NULL, laserbank_clearfaults,
                 COO_CMD_CLASS_ALWAYS_EFFECT, false, "",
                 "laserbank/clearfaults",
                 "none", NULL,
                 "TIB-only power-cycles the laser bank to clear latched faults",
                 COO_CMD_HELP_EFFECT),
    CMD_SPEC_TIB_PREFIX("laserbank/heater", laserbank_heater, laserbank_heater,
                 COO_CMD_CLASS_SUFFIX_OR_PAYLOAD_EFFECT, true, "mode",
                 "laserbank/heater [mode=<auto|override_on|override_off>]",
                 "mode required for effect; suffix form laserbank/heater/<mode> also works",
                 "mode: auto,override_on,override_off",
                 "TIB-only laser-bank heater relay mode",
                 COO_CMD_HELP_QUERY | COO_CMD_HELP_EFFECT | COO_CMD_HELP_SERIAL_GUARD_QUERY),
    CMD_SPEC_PREFIX("atten/calibrate/data", atten_calibration_data_get, NULL,
             COO_CMD_CLASS_ALWAYS_QUERY, true, "",
             "atten/calibrate/data/<dac1|dac2>[/<start>]",
             "query retained fit data page; start defaults to 0 and advances by 5",
             "physical: dac1,dac2",
             "returns calibration fit input and residual diagnostics without relying on transient telemetry",
             COO_CMD_HELP_QUERY | COO_CMD_HELP_SERIAL_GUARD_QUERY),
    CMD_SPEC("atten/calibrate", atten_calibration_get, atten_calibration_set,
             COO_CMD_CLASS_DEFAULT, true,
             "action,mode,stop,continue,other_mv,dac1,dac2,attenuator,laser,output,fiber,dwell_ms,persist",
             "atten/calibrate action=<auto|manual|continue|fit|stop> [attenuator=<name> fiber=<dac1|dac2> other_mv=<mV> dwell_ms=<ms> persist=<bool>]",
             "action required for effect; no payload queries calibration state",
             "name: 1028y,1270j,1430yj,1430hk,1510h,2330k,lfc",
             "calibration actions are mode-specific and may run across commands",
             COO_CMD_HELP_QUERY | COO_CMD_HELP_EFFECT | COO_CMD_HELP_SERIAL_GUARD_QUERY),
    { .key = "atten", .query_handler = atten_setting_get,
      .effect_handler = atten_setting_set,
      .class_policy = COO_CMD_CLASS_DEFAULT,
      .key_prefix_match = true,
      .allowed_payload_keys = "value,dac1,dac2,persist",
      .mqtt_query_allowed_during_serial_guard = true },
    CMD_HELP_ONLY("atten/<name>/value", NULL,
                  "atten/<name>/value [value=<linear>]",
                  "value required for effect",
                  "name: 1028y,1270j,1430yj,1430hk,1510h,2330k,lfc",
                  "sets or queries total logical transmission",
                  COO_CMD_HELP_QUERY | COO_CMD_HELP_EFFECT | COO_CMD_HELP_SERIAL_GUARD_QUERY),
    CMD_HELP_ONLY("atten/<name>/valuedb", NULL,
                  "atten/<name>/valuedb [value=<dB>]",
                  "value required for effect",
                  "name: 1028y,1270j,1430yj,1430hk,1510h,2330k,lfc",
                  "serial shorthand wraps a single numeric value field",
                  COO_CMD_HELP_QUERY | COO_CMD_HELP_EFFECT | COO_CMD_HELP_SERIAL_GUARD_QUERY),
    CMD_HELP_ONLY("atten/<name>/coeff", NULL,
                  "atten/<name>/coeff [dac1=[slope,offset] dac2=[slope,offset] persist=<bool>]",
                  "dac1 and dac2 arrays required for effect",
                  "name: 1028y,1270j,1430yj,1430hk,1510h,2330k,lfc",
                  "send JSON for coeff updates; key=value shorthand cannot express arrays",
                  COO_CMD_HELP_QUERY | COO_CMD_HELP_EFFECT | COO_CMD_HELP_SERIAL_GUARD_QUERY),
    { .key = "pd", .query_handler = pd_get, .effect_handler = pd_set,
      .class_policy = COO_CMD_CLASS_CUSTOM,
      .custom_classify = classify_pd,
      .supported = command_tib_supported,
      .allowed_payload_keys = "channel,action,duration_ms,persist",
      .mqtt_query_allowed_during_serial_guard = true,
      CMD_HELP("pd [channel=<yj|hk> action=<measure_dark|dark_status|reset_lowest_dark> duration_ms=<ms> persist=<bool>]",
               "channel and action required for effects; no payload queries live values",
               "channel: yj,hk; action: measure_dark,dark_status,reset_lowest_dark",
               "TIB-only photodiode status and dark-calibration actions",
               COO_CMD_HELP_QUERY | COO_CMD_HELP_EFFECT | COO_CMD_HELP_SERIAL_GUARD_QUERY) },
    { .key = "pdsettings", .query_handler = pd_settings_get,
      .effect_handler = pd_settings_set,
      .class_policy = COO_CMD_CLASS_DEFAULT,
      .supported = command_tib_supported,
      .key_prefix_match = true,
      .allowed_payload_keys = "dark_mv,noise_rms_mV,responsivity_a_per_w,transimpedance_v_per_a,power,autooff_s,persist",
      .mqtt_query_allowed_during_serial_guard = true },
    CMD_HELP_ONLY("pdsettings/<channel>", command_tib_supported,
                  "pdsettings/<channel> [dark_mv=<mV> noise_rms_mV=<mV> responsivity_a_per_w=<A/W> transimpedance_v_per_a=<V/A> power=<auto|override_on|override_off> autooff_s=<s> persist=<bool>]",
                  "channel required in key; listed fields optional for effect",
                  "channel: yj,hk",
                  "TIB-only app-owned photodiode calibration/settings and relay power intent",
                  COO_CMD_HELP_QUERY | COO_CMD_HELP_EFFECT | COO_CMD_HELP_SERIAL_GUARD_QUERY),
};

#undef CMD_HELP
#undef CMD_SPEC
#undef CMD_SPEC_PREFIX
#undef CMD_SPEC_CUSTOM
#undef CMD_SPEC_TIB
#undef CMD_SPEC_TIB_PREFIX
#undef CMD_SPEC_TIB_CUSTOM
#undef CMD_HELP_ONLY

static struct coo_cmd_runtime command_runtime;

static bool command_tib_supported(const struct coo_cmd_spec *spec, void *user_data)
{
    ARG_UNUSED(spec);
    ARG_UNUSED(user_data);

    return devices_board_type() == HISPEC_BOARD_TIB;
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

static enum coo_cmd_msg_type classify_route_loss(const struct coo_cmd_request *cmd,
                                                 const struct coo_cmd_spec *spec,
                                                 void *user_data)
{
    ARG_UNUSED(spec);
    ARG_UNUSED(user_data);

    return route_loss_payload_has_value(cmd != NULL ? cmd->payload : NULL) ?
           COO_CMD_EFFECT : COO_CMD_QUERY;
}

static enum coo_cmd_msg_type classify_laser_level(const struct coo_cmd_request *cmd,
                                                  const struct coo_cmd_spec *spec,
                                                  void *user_data)
{
    double fval;

    ARG_UNUSED(spec);
    ARG_UNUSED(user_data);

    return cmd != NULL &&
           coo_json_extract_double(cmd->payload, "level", &fval) != COO_JSON_EXTRACT_MISSING ?
           COO_CMD_EFFECT : COO_CMD_QUERY;
}

static enum coo_cmd_msg_type classify_laser_tune(const struct coo_cmd_request *cmd,
                                                 const struct coo_cmd_spec *spec,
                                                 void *user_data)
{
    double fval;

    ARG_UNUSED(spec);
    ARG_UNUSED(user_data);

    return cmd != NULL &&
           (coo_json_extract_double(cmd->payload, "tune_nm", &fval) != COO_JSON_EXTRACT_MISSING ||
            coo_json_extract_double(cmd->payload, "delta_nm", &fval) != COO_JSON_EXTRACT_MISSING) ?
           COO_CMD_EFFECT : COO_CMD_QUERY;
}

static enum coo_cmd_msg_type classify_laser_settings(const struct coo_cmd_request *cmd,
                                                     const struct coo_cmd_spec *spec,
                                                     void *user_data)
{
    char settings_json[MAX_PAYLOAD_LEN];

    ARG_UNUSED(spec);
    ARG_UNUSED(user_data);

    return cmd != NULL &&
           coo_json_extract_object(cmd->payload, "settings",
                                   settings_json, sizeof(settings_json)) != COO_JSON_EXTRACT_MISSING ?
           COO_CMD_EFFECT : COO_CMD_QUERY;
}

static enum coo_cmd_msg_type classify_pd(const struct coo_cmd_request *cmd,
                                         const struct coo_cmd_spec *spec,
                                         void *user_data)
{
    char action[20] = {0};

    ARG_UNUSED(spec);
    ARG_UNUSED(user_data);

    if (cmd == NULL || coo_cmd_payload_empty(cmd)) {
        return COO_CMD_QUERY;
    }
    if (coo_json_extract_string(cmd->payload, "action",
                                action, sizeof(action)) == COO_JSON_EXTRACT_OK &&
        strcasecmp(action, "dark_status") == 0) {
        return COO_CMD_QUERY;
    }

    return COO_CMD_EFFECT;
}

static int serial_read_three_tokens(const char *payload,
                                    char *t0, size_t t0_len,
                                    char *t1, size_t t1_len,
                                    char *t2, size_t t2_len)
{
    const char *cursor = payload;

    if (!coo_cmd_serial_next_token(&cursor, t0, t0_len)) {
        return -EINVAL;
    }
    (void)coo_cmd_serial_next_token(&cursor, t1, t1_len);
    (void)coo_cmd_serial_next_token(&cursor, t2, t2_len);
    return coo_cmd_serial_has_extra(cursor) ? -EINVAL : 0;
}

static int serial_single_value_payload(const char *token, char *out, size_t out_len)
{
    size_t off = 0;
    int written;

    written = snprintk(out, out_len, "{\"value\":");
    if (written < 0 || written >= (int)out_len) {
        return -ENOSPC;
    }
    off = (size_t)written;
    if (coo_cmd_serial_append_json_value(out, out_len, &off, token) != 0) {
        return -EINVAL;
    }
    written = snprintk(out + off, out_len - off, "}");
    return (written < 0 || written >= (int)(out_len - off)) ? -ENOSPC : 0;
}

/* Convert a few common human serial shorthands into the same JSON payloads MQTT
 * uses. These are per-command translations, not another dispatcher.
 */
static int serial_mems_switch_shorthand(const char *key, const char *payload,
                                        char *out, size_t out_len,
                                        void *user_data)
{
    char t0[96] = {0};
    char t1[96] = {0};
    char t2[96] = {0};
    size_t off = 0;
    int written;

    ARG_UNUSED(user_data);

    if (serial_read_three_tokens(payload, t0, sizeof(t0), t1, sizeof(t1),
                                 t2, sizeof(t2)) != 0) {
        return -EINVAL;
    }
    if (strchr(key, '/') == NULL) {
        if (t1[0] != '\0' || t2[0] != '\0') {
            return -EINVAL;
        }
        return serial_single_value_payload(t0, out, out_len);
    }

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
        coo_cmd_serial_append_json_field(out, out_len, &off, "off_in_s", t2, true) != 0) {
        return -EINVAL;
    }
    written = snprintk(out + off, out_len - off, "}");
    return (written < 0 || written >= (int)(out_len - off)) ? -ENOSPC : 0;
}

static void command_prepare_reboot(bool erase_non_ip_settings, void *user_data)
{

    ARG_UNUSED(user_data);

    LOG_WRN("Preparing hardware for reboot");
    if (devices_board_type() == HISPEC_BOARD_TIB) {
        struct throughput_monitor_status monitor_status;
        int rc;
        (void)throughput_monitor_stop(PHOTODIODE_CHANNEL_COUNT, &monitor_status);
        (void)housekeeping_power_set(HOUSEKEEPING_POWER_YJ_PHOTODIODE, false);
        (void)housekeeping_power_set(HOUSEKEEPING_POWER_HK_PHOTODIODE, false);


        rc = hispec_laser_stop_all_outputs(true);
        if (rc != 0) {
            LOG_WRN("Laser output shutdown before reboot failed (%d)", rc);
        }

        (void)laserbank_tempcontrol_set_heater_mode(LASERBANK_HEATER_MODE_OVERRIDE_OFF, false);
        (void)housekeeping_power_set(HOUSEKEEPING_POWER_BANK_HEATER, false);
    }

    if (erase_non_ip_settings) {
        int rc = app_settings_erase_non_ip_settings();

        if (rc != 0) {
            LOG_ERR("Erase non-IP settings before reboot failed (%d)", rc);
        }
    }
}

int command_runtime_init(void)
{
    const struct coo_cmd_runtime_config cfg = {
        .device_id = app_mqtt_device_id(),
        .inbound_queue = &inbound_queue,
        .outbound_queue = &outbound_queue,
        .mqtt_msg_id = &mqtt_msg_id,
        .serial_wrap_column = SERIAL_WRAP_COLUMN,
        .command_specs = command_specs,
        .command_spec_count = ARRAY_SIZE(command_specs),
        .lastcommand_nvs = app_settings_nvs_fs(),
        .lastcommand_nvs_id = APP_SETTINGS_NVS_ID_LAST_COMMAND,
#if defined(CONFIG_COO_CMD_REBOOT)
        .reboot_delay_ms = COMMAND_REBOOT_DELAY_MS,
        .reboot_prepare = command_prepare_reboot,
#endif
    };
    int rc;

    rc = coo_cmd_runtime_configure(&command_runtime, &cfg);
    if (rc != 0) {
        return rc;
    }

    return 0;
}

struct coo_cmd_runtime *command_runtime_get(void)
{
    return &command_runtime;
}





/* COMMAND HANDLERS */


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
                       "{\"src\":\"%s\",\"trydhcpfirst\":%s,"
                       "\"preferdhcpdns\":%s,\"preferdhcpntp\":%s,"
                       "\"manual\":{\"ip\":\"%s\",\"subnet\":\"%s\",\"gateway\":\"%s\",\"dns\":\"%s\",\"ntp\":\"%s\"},"
                       "\"active\":{\"ready\":%s,\"ip\":\"%s\"},"
                       "\"ntp\":{\"src\":\"%s\",\"server\":\"%s\"}}",
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

int ip_get(const struct coo_cmd_request *cmd, struct coo_cmd_response *out)
{
    char payload[MAX_PAYLOAD_LEN];

    if (ip_status_payload(payload, sizeof(payload)) != 0) {
        return coo_cmd_error(out, cmd, "ip response too large");
    }

    return coo_cmd_reply(out, cmd, COO_CMD_RESP_OK, payload);
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

int ip_set(const struct coo_cmd_request *cmd, struct coo_cmd_response *out)
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
            return coo_cmd_error(out, cmd, "invalid trydhcpfirst");
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
            return coo_cmd_error(out, cmd, "invalid preferdhcpdns");
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
            return coo_cmd_error(out, cmd, "invalid preferdhcpntp");
        }
    }

    parse_rc = coo_json_extract_string(cmd->payload, "ip", buf, sizeof(buf));
    if (parse_rc == COO_JSON_EXTRACT_OK) {
        strncpy(ip_cfg.ip, buf, sizeof(ip_cfg.ip) - 1);
        ip_cfg.ip[sizeof(ip_cfg.ip) - 1] = '\0';
        changed = true;
        network_changed = true;
    } else if (parse_rc == COO_JSON_EXTRACT_ERR) {
        return coo_cmd_error(out, cmd, "invalid ip");
    }

    parse_rc = coo_json_extract_string(cmd->payload, "subnet", buf, sizeof(buf));
    if (parse_rc == COO_JSON_EXTRACT_OK) {
        strncpy(ip_cfg.subnet, buf, sizeof(ip_cfg.subnet) - 1);
        ip_cfg.subnet[sizeof(ip_cfg.subnet) - 1] = '\0';
        changed = true;
        network_changed = true;
    } else if (parse_rc == COO_JSON_EXTRACT_ERR) {
        return coo_cmd_error(out, cmd, "invalid subnet");
    }

    parse_rc = coo_json_extract_string(cmd->payload, "gateway", buf, sizeof(buf));
    if (parse_rc == COO_JSON_EXTRACT_OK) {
        strncpy(ip_cfg.gateway, buf, sizeof(ip_cfg.gateway) - 1);
        ip_cfg.gateway[sizeof(ip_cfg.gateway) - 1] = '\0';
        changed = true;
        network_changed = true;
    } else if (parse_rc == COO_JSON_EXTRACT_ERR) {
        return coo_cmd_error(out, cmd, "invalid gateway");
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
            return coo_cmd_error(out, cmd, "invalid dns");
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
            return coo_cmd_error(out, cmd, "invalid ntp");
        }
    }

    if (coo_json_extract_optional_bool(cmd->payload, "persist",
                                       &persist, NULL) != 0) {
        return coo_cmd_error(out, cmd, "invalid persist");
    }

    if (!changed && !(unsupported_dhcp || unsupported_dns || unsupported_ntp)) {
        return coo_cmd_error(out, cmd, "no recognized ip fields");
    }

    if (network_changed) {
        struct network_config net_cfg;
        int rc;

        network_config_from_app_ip(&ip_cfg, &net_cfg);
        rc = network_reconfigure(&net_cfg);
        if (rc != 0) {
            return coo_cmd_error_rc(out, cmd, "network reconfigure failed", rc);
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
        return coo_cmd_reply(out, cmd, COO_CMD_RESP_OK, response);
    }

    return coo_cmd_ok(out, cmd);
}

int mqtt_get(const struct coo_cmd_request *cmd, struct coo_cmd_response *out)
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
    return coo_cmd_reply(out, cmd, COO_CMD_RESP_OK, payload);
}

int mqtt_set(const struct coo_cmd_request *cmd, struct coo_cmd_response *out)
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
        return coo_cmd_error(out, cmd, "missing broker");
    }
    if (parse_rc == COO_JSON_EXTRACT_ERR) {
        return coo_cmd_error(out, cmd, "invalid broker");
    }
    if (!coo_mqtt_parse_broker_endpoint(endpoint, &broker_cfg)) {
        return coo_cmd_error(out, cmd, "broker must be host-or-ip:port");
    }
    rc = coo_mqtt_resolve_broker_config(&broker_cfg, resolved_ip, sizeof(resolved_ip));
    if (rc == -ENOTSUP) {
        return coo_cmd_error(out, cmd, "broker hostname requires DNS");
    }
    if (rc != 0) {
        return coo_cmd_error(out, cmd, "broker host did not resolve");
    }
    strncpy(mqtt_cfg.broker_host, broker_cfg.host, sizeof(mqtt_cfg.broker_host) - 1U);
    mqtt_cfg.broker_host[sizeof(mqtt_cfg.broker_host) - 1U] = '\0';
    mqtt_cfg.broker_port = broker_cfg.port;

    if (coo_json_extract_optional_bool(cmd->payload, "persist",
                                       &persist, NULL) != 0) {
        return coo_cmd_error(out, cmd, "invalid persist");
    }

    app_settings_update_mqtt(&mqtt_cfg, persist);
    return coo_cmd_ok(out, cmd);
}

int time_get(const struct coo_cmd_request *cmd, struct coo_cmd_response *out)
{
    struct timespec ts = {0};
    uint64_t utc_ms;
    char payload[MAX_PAYLOAD_LEN];

    if (sys_clock_gettime(SYS_CLOCK_REALTIME, &ts) != 0) {
        return coo_cmd_error(out, cmd, "clock read failed");
    }
    utc_ms = ((uint64_t)ts.tv_sec * 1000ULL) + ((uint64_t)ts.tv_nsec / 1000000ULL);

    snprintk(payload, sizeof(payload),
             "{\"utc\":%llu,\"uptime_s\":%lld}",
             (unsigned long long)utc_ms, (long long)k_uptime_get()/1000);

    return coo_cmd_reply(out, cmd, COO_CMD_RESP_OK, payload);
}

int time_set(const struct coo_cmd_request *cmd, struct coo_cmd_response *out)
{
    uint64_t utc_ms = 0;
    struct timespec ts = {0};
    int parse_rc;

    parse_rc = coo_json_extract_u64(cmd->payload, "unix_ms", &utc_ms);
    if (parse_rc == COO_JSON_EXTRACT_MISSING) {
        return coo_cmd_error(out, cmd, "missing unix_ms");
    }
    if (parse_rc == COO_JSON_EXTRACT_ERR) {
        return coo_cmd_error(out, cmd, "invalid unix_ms");
    }

    ts.tv_sec = utc_ms / 1000ULL;
    ts.tv_nsec = (utc_ms % 1000ULL) * 1000000ULL;

    if (sys_clock_settime(SYS_CLOCK_REALTIME, &ts) != 0) {
        return coo_cmd_error(out, cmd, "clock set failed");
    }
    app_settings_note_time_utc_ms(utc_ms);

    return coo_cmd_ok(out, cmd);
}

static bool catalog_name_seen(const char *const names[], uint8_t count,
                              const char *name)
{
    for (uint8_t i = 0U; i < count; ++i) {
        if (names[i] != NULL && name != NULL && strcmp(names[i], name) == 0) {
            return true;
        }
    }
    return false;
}

static int catalog_append_static_string(char *payload, size_t payload_len,
                                        size_t *off, const char *value)
{
    /* Catalog names are compile-time route/laser identifiers, not user text. */
    return coo_json_append(payload, payload_len, off, "\"%s\"",
                           value != NULL ? value : "");
}

static int catalog_append_lasers(char *payload, size_t payload_len, size_t *off)
{
    if (coo_json_append(payload, payload_len, off, "\"lasers\":[") != 0) {
        return -ENOSPC;
    }

    if (devices_board_type() == HISPEC_BOARD_TIB) {
        for (uint8_t i = 0U; i < HISPEC_LASER_COUNT; ++i) {
            if (i > 0U &&
                coo_json_append(payload, payload_len, off, ",") != 0) {
                return -ENOSPC;
            }
            if (catalog_append_static_string(
                    payload, payload_len, off,
                    hispec_laser_name((enum hispec_laser_id)i)) != 0) {
                return -ENOSPC;
            }
        }
    }

    return coo_json_append(payload, payload_len, off, "]");
}

static int catalog_append_route_name_array(char *payload, size_t payload_len,
                                           size_t *off, const char *field,
                                           bool inputs)
{
    const char *names[MEMS_ROUTER_MAX_ROUTES] = {0};
    uint8_t count = 0U;

    for (uint8_t i = 0U; router.routes != NULL && i < router.num_routes; ++i) {
        const char *name = inputs ? router.routes[i].key.input_name :
                                    router.routes[i].key.output_name;

        if (!catalog_name_seen(names, count, name)) {
            names[count++] = name;
        }
    }

    if (coo_json_append(payload, payload_len, off, "\"%s\":[", field) != 0) {
        return -ENOSPC;
    }
    for (uint8_t i = 0U; i < count; ++i) {
        if (i > 0U &&
            coo_json_append(payload, payload_len, off, ",") != 0) {
            return -ENOSPC;
        }
        if (catalog_append_static_string(payload, payload_len, off, names[i]) != 0) {
            return -ENOSPC;
        }
    }
    return coo_json_append(payload, payload_len, off, "]");
}

static int catalog_append_routes(char *payload, size_t payload_len, size_t *off)
{
    if (coo_json_append(payload, payload_len, off, "\"routes\":[") != 0) {
        return -ENOSPC;
    }
    for (uint8_t i = 0U; router.routes != NULL && i < router.num_routes; ++i) {
        if (i > 0U &&
            coo_json_append(payload, payload_len, off, ",") != 0) {
            return -ENOSPC;
        }
        if (coo_json_append(payload, payload_len, off, "[") != 0 ||
            catalog_append_static_string(payload, payload_len, off,
                                         router.routes[i].key.input_name) != 0 ||
            coo_json_append(payload, payload_len, off, ",") != 0 ||
            catalog_append_static_string(payload, payload_len, off,
                                         router.routes[i].key.output_name) != 0 ||
            coo_json_append(payload, payload_len, off, "]") != 0) {
            return -ENOSPC;
        }
    }
    return coo_json_append(payload, payload_len, off, "]");
}

static int catalog_get(const struct coo_cmd_request *cmd,
                       struct coo_cmd_response *out)
{
    char payload[MAX_PAYLOAD_LEN] = {0};
    size_t off = 0U;

    if (coo_json_append(payload, sizeof(payload), &off,
                        "{\"board\":\"%s\",",
                        devices_board_type_name()) != 0 ||
        catalog_append_lasers(payload, sizeof(payload), &off) != 0 ||
        coo_json_append(payload, sizeof(payload), &off, ",") != 0 ||
        catalog_append_route_name_array(payload, sizeof(payload), &off,
                                        "route_inputs", true) != 0 ||
        coo_json_append(payload, sizeof(payload), &off, ",") != 0 ||
        catalog_append_route_name_array(payload, sizeof(payload), &off,
                                        "route_outputs", false) != 0 ||
        coo_json_append(payload, sizeof(payload), &off, ",") != 0 ||
        catalog_append_routes(payload, sizeof(payload), &off) != 0 ||
        coo_json_append(payload, sizeof(payload), &off, "}") != 0) {
        return coo_cmd_error(out, cmd, "catalog response too large");
    }

    return coo_cmd_reply(out, cmd, COO_CMD_RESP_OK, payload);
}

static int command_append_seconds_or_null(char *payload, size_t payload_len,
                                          size_t *off, bool active, double seconds)
{
    if (!active || seconds < 0.0 || seconds != seconds) {
        return coo_json_append(payload, payload_len, off, "null");
    }
    return coo_json_append(payload, payload_len, off, "%lld", (long long)seconds);
}

static int command_append_i64_or_null(char *payload, size_t payload_len,
                                      size_t *off, bool active, int64_t seconds)
{
    if (!active) {
        return coo_json_append(payload, payload_len, off, "null");
    }
    return coo_json_append(payload, payload_len, off, "%lld", (long long)seconds);
}

int status_get(const struct coo_cmd_request *cmd, struct coo_cmd_response *out)
{
    struct housekeeping_temperature_status ts = {0};
    bool include_ip = false;
    bool include_lasers = false;
    bool include_attens = false;
    struct coo_cmd_lastcommand lastcommand;
    bool has_lastcommand;
    char payload[MAX_PAYLOAD_LEN] = {0};
    size_t off = 0U;
    uint64_t pd_on_s;

    if (coo_json_extract_optional_bool(cmd->payload, "ip",
                                       &include_ip, NULL) != 0) {
        return coo_cmd_error(out, cmd, "invalid ip");
    }
    if (coo_json_extract_optional_bool(cmd->payload, "lasers",
                                       &include_lasers, NULL) != 0) {
        return coo_cmd_error(out, cmd, "invalid lasers");
    }
    if (coo_json_extract_optional_bool(cmd->payload, "attens",
                                       &include_attens, NULL) != 0) {
        return coo_cmd_error(out, cmd, "invalid attens");
    }

    housekeeping_get_temperature_status(&ts);
    has_lastcommand = coo_cmd_runtime_get_lastcommand(&command_runtime, &lastcommand);
    pd_on_s = (uint64_t)MAX(housekeeping_power_on_time_s(HOUSEKEEPING_POWER_YJ_PHOTODIODE),
                            housekeeping_power_on_time_s(HOUSEKEEPING_POWER_HK_PHOTODIODE));
    if (coo_json_append(payload, sizeof(payload), &off,
                        "{\"fw\":\"%s\",\"boots\":%u,"
                        "\"board\":\"%s\",\"board_ok\":%s,"
                        "\"mems_switches\":%u,\"relay_err\":%d,"
                        "\"amb_c\":",
                        HISPEC_BUILD_VERSION,
                        app_settings_get_boot_count(),
                        devices_board_type_name(),
                        devices_board_type() != HISPEC_BOARD_UNKNOWN ? "true" : "false",
                        router.num_switches,
                        devices_relay_gpio_last_error()) != 0 ||
        coo_json_append_float_or_null(payload, sizeof(payload), &off,
                                      ts.valid ? ts.ambient_c : (double)NAN, 3) != 0 ||
        coo_json_append(payload, sizeof(payload), &off,
                        ",\"pd_on_s\":%llu,"
                        "\"laserbank_on_s\":%u",
                        (unsigned long long)pd_on_s,
                        hispec_laser_bank_power_on_duration_s()) != 0) {
        return coo_cmd_error(out, cmd, "status response too large");
    }

    if (include_ip) {
        char ip_payload[MAX_PAYLOAD_LEN];

        if (ip_status_payload(ip_payload, sizeof(ip_payload)) != 0 ||
            coo_json_append(payload, sizeof(payload), &off,
                            ",\"ip\":%s", ip_payload) != 0) {
            return coo_cmd_error(out, cmd, "status response too large");
        }
    }

    if (include_lasers) {
        if (coo_json_append(payload, sizeof(payload), &off, ",\"lasers\":{") != 0) {
            return coo_cmd_error(out, cmd, "status response too large");
        }
        for (uint8_t i = 0U; i < HISPEC_LASER_COUNT; ++i) {
            struct hispec_laser_status laser = {0};
            int rc = hispec_laser_get_status((enum hispec_laser_id)i, &laser);

            if (coo_json_append(payload, sizeof(payload), &off,
                                "%s\"%s\":{\"power_mw\":",
                                i == 0U ? "" : ",",
                                hispec_laser_name((enum hispec_laser_id)i)) != 0 ||
                coo_json_append_float_or_null(payload, sizeof(payload), &off,
                                              rc == 0 ? laser.estimated_power_mw : (double)NAN, 3) != 0 ||
                coo_json_append(payload, sizeof(payload), &off,
                                ",\"ready\":%s,\"tec_on_s\":",
                                rc == 0 && laser.ready_to_operate ? "true" : "false") != 0 ||
                command_append_seconds_or_null(payload, sizeof(payload), &off,
                                               rc == 0 && laser.tec_runtime_active,
                                               laser.tec_on_time_s) != 0 ||
                coo_json_append(payload, sizeof(payload), &off,
                                ",\"off_in_s\":") != 0 ||
                command_append_i64_or_null(payload, sizeof(payload), &off,
                                           rc == 0 && laser.autooff_active,
                                           laser.off_in_s) != 0 ||
                coo_json_append(payload, sizeof(payload), &off, "}") != 0) {
                return coo_cmd_error(out, cmd, "status response too large");
            }
        }
        if (coo_json_append(payload, sizeof(payload), &off, "}") != 0) {
            return coo_cmd_error(out, cmd, "status response too large");
        }
    }

    if (include_attens) {
        bool first = true;

        if (coo_json_append(payload, sizeof(payload), &off, ",\"attens\":{") != 0) {
            return coo_cmd_error(out, cmd, "status response too large");
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
                return coo_cmd_error(out, cmd, "status response too large");
            }
            first = false;
        }
        if (coo_json_append(payload, sizeof(payload), &off, "}") != 0) {
            return coo_cmd_error(out, cmd, "status response too large");
        }
    }

    if (has_lastcommand &&
        coo_json_append(payload, sizeof(payload), &off,
                        ",\"lastcmd\":{\"name\":\"%s\",\"src\":\"%s\","
                        "\"t_ms\":%lld}}",
                        lastcommand.request.key,
                        coo_cmd_source_name(lastcommand.request.source),
                        (long long)lastcommand.time_ms) != 0) {
        return coo_cmd_error(out, cmd, "status response too large");
    }
    if (!has_lastcommand &&
        coo_json_append(payload, sizeof(payload), &off,
                        ",\"lastcmd\":{\"name\":\"\",\"src\":\"unknown\","
                        "\"t_ms\":0}}") != 0) {
        return coo_cmd_error(out, cmd, "status response too large");
    }

    return coo_cmd_reply(out, cmd, COO_CMD_RESP_OK, payload);
}

int temp_get(const struct coo_cmd_request *cmd, struct coo_cmd_response *out)
{
    struct housekeeping_temperature_status ts = {0};
    struct hispec_laser_channel_temperature channel_temp[HISPEC_LASER_COUNT] = {0};
    char payload[MAX_PAYLOAD_LEN] = {0};
    size_t off = 0U;
    double bank_sum = 0.0;
    uint8_t bank_count = 0U;
    int laser_rc = 0;

    housekeeping_get_temperature_status(&ts);
    if (devices_board_type() == HISPEC_BOARD_TIB) {
        laser_rc = hispec_laser_bank_read_temperatures(channel_temp);
        if (laser_rc == -EBUSY) {
            return coo_cmd_busy_response(out, cmd);
        }
        if (laser_rc == 0) {
            for (uint8_t i = 0U; i < HISPEC_LASER_COUNT; ++i) {
                if (channel_temp[i].valid) {
                    bank_sum += (double)channel_temp[i].tec_temperature_c;
                    bank_count++;
                }
            }
        }
    }

    if (coo_json_append(payload, sizeof(payload), &off,
                        "{\"ambient_c\":") != 0 ||
        coo_json_append_float_or_null(payload, sizeof(payload), &off,
                                      ts.valid ? ts.ambient_c : (double)NAN, 3) != 0) {
        return coo_cmd_error(out, cmd, "temp response too large");
    }

    if (coo_json_append(payload, sizeof(payload), &off,
                        ",\"laserbank_c\":") != 0 ||
        coo_json_append_float_or_null(payload, sizeof(payload), &off,
                                      bank_count > 0U ? bank_sum / (double)bank_count : (double)NAN,
                                      3) != 0 ||
        coo_json_append(payload, sizeof(payload), &off,
                        ",\"laser\":{") != 0) {
        return coo_cmd_error(out, cmd, "temp response too large");
    }
    for (uint8_t i = 0U; i < HISPEC_LASER_COUNT; ++i) {
        if (coo_json_append(payload, sizeof(payload), &off,
                            "%s\"%s\":",
                            i == 0U ? "" : ",",
                            hispec_laser_name((enum hispec_laser_id)i)) != 0 ||
            coo_json_append_float_or_null(payload, sizeof(payload), &off,
                                          laser_rc == 0 && channel_temp[i].valid ?
                                          channel_temp[i].tec_temperature_c : (double)NAN,
                                          3) != 0) {
            return coo_cmd_error(out, cmd, "temp response too large");
        }
    }
    if (coo_json_append(payload, sizeof(payload), &off, "}}") != 0) {
        return coo_cmd_error(out, cmd, "temp response too large");
    }

    return coo_cmd_reply(out, cmd, COO_CMD_RESP_OK, payload);
}
