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
#include <string.h>
#include <zephyr/data/json.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/clock.h>
#include <zephyr/sys/util.h>
#include <hispec_build_version.h>
#include <time.h>
#include <zephyr/net/net_ip.h>

#include "devices.h"
#include "lasers.h"
#include "app_identity.h"
#include "app_output.h"
#include "app_settings.h"
#include "attenuator.h"
#include "attenuator_command.h"
#include "laser_command.h"
#include "laserbank_tempcontrol.h"
#include "mems_command.h"
#include "mems_switching.h"
#include "photodiode_command.h"
#include "photodiode.h"
#include "laser_calibration.h"
#include "attenuator_calibration.h"
#include <coo_commons/ota.h>
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

static uint16_t mqtt_msg_id = 1;

/* MQTT and serial ingress use k_msgq so callbacks never execute hardware work.
 * Depth is intentionally small: clients should retry instead of letting stale
 * hardware commands pile up.
 */
K_MSGQ_DEFINE(inbound_queue,
              sizeof(struct coo_cmd_request),
              2,      /* depth */
              4);     /* 4‐byte align */

/* Responses, warnings, and telemetry leave the executor through this bounded
 * queue. The main loop owns MQTT publish retries and serial printing.
 */
K_MSGQ_DEFINE(outbound_queue,
              sizeof(struct coo_cmd_response),
              8,
              4);

/* Replies cannot be displaced by a stream burst. */
K_MSGQ_DEFINE(reply_queue, sizeof(struct coo_cmd_response), 4, 4);

extern struct mems_switch mems_switches[MEMS_ROUTER_MAX_SWITCHES];
extern struct mems_router router;
// extern struct attenuator attenuators[NUM_ATTENUATORS];

static bool command_tib_supported(const struct coo_cmd_spec *spec, void *user_data);
static enum coo_cmd_msg_type classify_route_loss(const struct coo_cmd_request *cmd,
                                                 const struct coo_cmd_spec *spec,
                                                 void *user_data);
static enum coo_cmd_msg_type classify_laser_request(const struct coo_cmd_request *cmd,
                                                   const struct coo_cmd_spec *spec,
                                                   void *user_data);
static int help_options_get(const struct coo_cmd_request *cmd,
                       struct coo_cmd_response *out);
static int serial_mems_switch_shorthand(const char *key, const char *payload,
                                        char *out, size_t out_len,
                                        void *user_data);
static void command_prepare_reboot(bool erase_non_ip_settings, void *user_data);
/** Format configured network policy and runtime status; response publication belongs to main. */
static int ip_get(const struct coo_cmd_request *cmd, struct coo_cmd_response *out);
/** Validate/apply network policy and optional NVS persistence; DHCP completion is asynchronous. */
static int ip_set(const struct coo_cmd_request *cmd, struct coo_cmd_response *out);
/** Copy broker configuration into the reply; no broker connection is attempted here. */
static int mqtt_get(const struct coo_cmd_request *cmd, struct coo_cmd_response *out);
/** Validate/resolve and save broker intent; main owns reconnect/revert. May block on DNS/NVS. */
static int mqtt_set(const struct coo_cmd_request *cmd, struct coo_cmd_response *out);
/** Format current UTC and SNTP status; no synchronization request is sent. */
static int time_get(const struct coo_cmd_request *cmd, struct coo_cmd_response *out);
/** Set UTC through the shared RTC guard; may perform RTC I/O, never persists a last-time NVS value. */
static int time_set(const struct coo_cmd_request *cmd, struct coo_cmd_response *out);
/** Format bounded owner snapshots without hardware I/O or persistence. */
static int status_get(const struct coo_cmd_request *cmd, struct coo_cmd_response *out);
/** Read available temperatures without powering the bank; idle TECs alone define bank temperature. */
static int temps_get(const struct coo_cmd_request *cmd, struct coo_cmd_response *out);

#define CMD_HELP(_usage, _args, _values, _notes, _flags) \
    .help = &(const struct coo_cmd_help_entry){ \
        .usage = (_usage), .args = (_args), .values = (_values), \
        .notes = (_notes), .flags = (_flags) }

#define CMD_SPEC(_key, _get, _set, _class, _guard, _keys, _usage, _args, _values, _notes, _flags, ...) \
    { .key = (_key), .query_handler = (_get), .effect_handler = (_set), \
      .class_policy = (_class), .mqtt_query_allowed_during_serial_guard = (_guard), \
      .allowed_payload_keys = (_keys), \
      CMD_HELP(_usage, _args, _values, _notes, _flags), __VA_ARGS__ }

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

#define CMD_SPEC_TIB_PREFIX(_key, _get, _set, _class, _guard, _keys, _usage, _args, _values, _notes, _flags, ...) \
    { .key = (_key), .query_handler = (_get), .effect_handler = (_set), \
      .class_policy = (_class), .supported = command_tib_supported, \
      .mqtt_query_allowed_during_serial_guard = (_guard), \
      .key_prefix_match = true, .allowed_payload_keys = (_keys), \
      CMD_HELP(_usage, _args, _values, _notes, _flags), __VA_ARGS__ }

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
 * and help for a command key. OTA denies queries/effects unless a row explicitly
 * permits them; builtins carry their policy in COO dispatch. Help-only rows
 * document parameterized forms dispatched by a shorter prefix row.
 */
static const struct coo_cmd_spec command_specs[] = {
    CMD_SPEC("ip", ip_get, ip_set, COO_CMD_CLASS_DEFAULT, true,
             "try_dhcp_first,prefer_dhcpdns,prefer_dhcpntp,ip,subnet,gateway,dns,ntp,persist",
             "ip [try_dhcp_first=<bool> prefer_dhcpdns=<bool> prefer_dhcpntp=<bool> ip=<IPv4> subnet=<IPv4> gateway=<IPv4> dns=<IPv4> ntp=<IPv4> persist=<bool>]",
             "query with no payload; effect when any listed field is supplied",
             "bool: true|false|on|off|yes|no",
             "reconfigures IPv4 immediately; persist=true stores app-owned IP settings",
             COO_CMD_HELP_QUERY | COO_CMD_HELP_EFFECT | COO_CMD_HELP_SERIAL_GUARD_QUERY,
                 .ota_query_allowed = true),
    { .key = "mqtt", .query_handler = mqtt_get, .effect_handler = mqtt_set,
      .class_policy = COO_CMD_CLASS_DEFAULT,
      .serial_positional = { .field = { "broker", "persist" }, .required_count = 1U },
      .allowed_payload_keys = "broker,persist",
      .ota_query_allowed = true,
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
      .ota_query_allowed = true,
      .mqtt_query_allowed_during_serial_guard = true,
      CMD_HELP("time [unix_ms=<utc-ms>]",
               "unix_ms required for effect",
               "unsigned millisecond Unix epoch",
               "sets running UTC and RTC calendar; does not persist stale UTC in NVS",
               COO_CMD_HELP_QUERY | COO_CMD_HELP_EFFECT | COO_CMD_HELP_SERIAL_GUARD_QUERY) },
    CMD_SPEC("temps", temps_get, NULL, COO_CMD_CLASS_DEFAULT, true, "",
             "temps", "none", NULL, "cached ambient and fresh powered-bank TEC temperatures; idle probes define bank temperature",
             COO_CMD_HELP_QUERY | COO_CMD_HELP_SERIAL_GUARD_QUERY,
                 .ota_query_allowed = true),
    CMD_SPEC("status", status_get, NULL, COO_CMD_CLASS_ALWAYS_QUERY, true,
             "view,channel",
             "status [view=<emission|mems|system> channel=<yj|hk>]",
             "channel required only for emission",
             "TIB: emission, mems, system; AS/CAL: system",
             "cached status; default includes all activity and emitted output power",
             COO_CMD_HELP_QUERY | COO_CMD_HELP_SERIAL_GUARD_QUERY,
                 .ota_query_allowed = true, .reboot_query_allowed = true),
    CMD_SPEC("help/options", help_options_get, NULL, COO_CMD_CLASS_ALWAYS_QUERY, true, "",
             "help/options",
             "none",
             NULL,
             "query-only static laser names and selected board route names",
             COO_CMD_HELP_QUERY | COO_CMD_HELP_SERIAL_GUARD_QUERY,
                 .ota_query_allowed = true),
    CMD_SPEC_CUSTOM("mems/route/loss", memsroute_get, memsroute_set,
                    classify_route_loss, true,
                    "route,1028y,1270j,1430yj,1430hk,1510h,2330k,generic,split,persist",
                    "mems/route/loss route=<route> [<laser|generic>=<loss> persist=<bool>]",
                    "route required; set exactly one source loss or a split tuple",
                    "source fields: 1028y,1270j,1430yj,1430hk,1510h,2330k,generic (TIB CAL/return)",
                    "loss: fraction lost 0 <= loss < 1, or a quoted nonnegative dB value",
                    COO_CMD_HELP_QUERY | COO_CMD_HELP_EFFECT | COO_CMD_HELP_SERIAL_GUARD_QUERY),
    CMD_SPEC("mems/route", memsroute_get, memsroute_set,
             COO_CMD_CLASS_DEFAULT, true, "input,output,force",
             "mems/route input=<input> output=<output> [force=<bool>]",
             "input and output required; force=true re-pulses route steps",
             "route names are board profile input/output route keys",
             "applies a named static MEMS route",
             COO_CMD_HELP_QUERY | COO_CMD_HELP_EFFECT | COO_CMD_HELP_SERIAL_GUARD_QUERY),
    { .key = "mems", .query_handler = mems_get, .effect_handler = mems_set,
      .class_policy = COO_CMD_CLASS_DEFAULT,
      .serial_shorthand = serial_mems_switch_shorthand,
      .key_prefix_match = true,
      .allowed_payload_keys = "state,duty_cycle,off_in_s,cycle_ms,force,toggle_rate_hz",
      .mqtt_query_allowed_during_serial_guard = true },
    CMD_HELP_ONLY("mems/<switchname>", NULL,
                  "mems/<switchname> [state=<A|B> force=<bool> duty_cycle=<0..1> cycle_ms=<ms> off_in_s=<s>]",
                  "state required for effect; force is static only; duty_cycle, cycle_ms, and off_in_s optional",
                  "switchname is one active board MEMS switch name",
                  "serial shorthand accepts: mems/<switchname> A [duty_cycle] [off_in_s]",
                  COO_CMD_HELP_QUERY | COO_CMD_HELP_EFFECT | COO_CMD_HELP_SERIAL_GUARD_QUERY),
    CMD_SPEC_PREFIX("mems/split", splitting_get, splitting_set,
             COO_CMD_CLASS_DEFAULT, true,
             "channel,ratio1,ratio2,ratio3,cycle_ms,off_in_s,toggle_rate_hz",
             "mems/split [channel=<yj|hk> ratio1=<0..1> ratio2=<0..1> cycle_ms=<ms> off_in_s=<s>]",
             "channel, ratio1, and ratio2 are required for effect; cycle_ms and off_in_s optional",
             "channel: yj,hk",
             "mems/split/yj and mems/split/hk query current splitter state",
             COO_CMD_HELP_QUERY | COO_CMD_HELP_EFFECT | COO_CMD_HELP_SERIAL_GUARD_QUERY),
    CMD_SPEC_TIB("measure_throughput", NULL, measure_throughput_set,
                 COO_CMD_CLASS_DEFAULT, false,
                 "laser,channel,fiber,input,output,autolevel,autolevel_response_hz,initial_level,max_flux_nw,off_in_s,format,stop",
                 "measure_throughput laser=<laser|none> [channel=<yj|hk> input=<name> output=<name> fiber=<M|S> autolevel=<bool> autolevel_response_hz=<Hz> initial_level=<0..1> max_flux_nw=<value> off_in_s=<s> format=<json|binary>]",
                 "stop=<yj|hk|all>; known laser requires output; none requires channel and autolevel=false, with optional input/output pair",
                 "format: json,binary",
                 "TIB-only; autolevel requires positive off_in_s; zero is unlimited passive measurement",
                 COO_CMD_HELP_EFFECT),
    CMD_SPEC_TIB_PREFIX("laser/calibrate/records", laser_calibration_records_get, NULL,
                 COO_CMD_CLASS_ALWAYS_QUERY, true, "",
                 "laser/calibrate/records[/<chunk>]", "binary metadata or raw records", NULL,
                 "MQTT-only latest shared calibration dataset", COO_CMD_HELP_QUERY | COO_CMD_HELP_SERIAL_GUARD_QUERY),
    CMD_SPEC_TIB("laser/calibrate", laser_calibration_get, laser_calibration_set,
                 COO_CMD_CLASS_DEFAULT, true,
                 "laser,output,fiber,dwell_ms,settle_ms,apply,max_fit_residual_pct,pd_target_mv,determine_static_loss,loopback_loss_db,update_noise,other_output,persist,stop,continue,restore_defaults",
                 "laser/calibrate [laser=<name> output=<route> ... | stop=true | continue=true | restore_defaults=true laser=<name> output=<route>]",
                 "loss fitting requires external loopback_loss_db; defaults apply=true, dwell_ms=1000, settle_ms=2000, fiber=S, max_fit_residual_pct=5, pd_target_mv=1500, persist=false",
                 "output: yj_ao,yj_fei,hk_ao,hk_fei", "reference-anchored current sweep; query for progress/results",
                 COO_CMD_HELP_QUERY | COO_CMD_HELP_EFFECT | COO_CMD_HELP_SERIAL_GUARD_QUERY),
    CMD_SPEC_TIB_CUSTOM("laser", laser_get, laser_set, classify_laser_request,
                        true, "name,value,value_ma,off_in_s,stop",
                        "laser name=<laser> [value=<0..1>|value_ma=<mA> off_in_s=<s> stop=<bool>]",
                        "name required; off_in_s must be positive; omitted uses laser/settings autooff_s",
                        "laser: 1028y,1270j,1430yj,1430hk,1510h,2330k",
                        "TIB-only laser output status/set command",
                        COO_CMD_HELP_QUERY | COO_CMD_HELP_EFFECT | COO_CMD_HELP_SERIAL_GUARD_QUERY),
    CMD_SPEC_TIB_CUSTOM("laser/tune", laser_tune_get, laser_tune_set,
                        classify_laser_request, true, "name,tune_nm",
                        "laser/tune name=<laser> [tune_nm=<nm>]",
                        "name required; tune_nm makes it an effect",
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
                        classify_laser_request, true, "name,settings,persist",
                        "laser/settings name=<laser> [settings={...} persist=<bool>]",
                        "name required; settings object required for effect",
                        "laser: 1028y,1270j,1430yj,1430hk,1510h,2330k",
                        "TIB-only app-owned laser policy/settings wrapper",
                        COO_CMD_HELP_QUERY | COO_CMD_HELP_EFFECT | COO_CMD_HELP_SERIAL_GUARD_QUERY),
    CMD_SPEC_TIB_PREFIX("laser/bankpower", laserbank_power, laserbank_power,
                 COO_CMD_CLASS_SUFFIX_OR_PAYLOAD_EFFECT, true, "mode",
                 "laser/bankpower [mode=<auto|override_on|override_off>]",
                 "mode required for effect; suffix form laser/bankpower/<mode> also works",
                 "mode: auto,override_on,override_off",
                 "TIB-only laser-bank supply override",
                 COO_CMD_HELP_QUERY | COO_CMD_HELP_EFFECT | COO_CMD_HELP_SERIAL_GUARD_QUERY,
                 .ota_query_allowed = true),
    CMD_SPEC_TIB("laser/clearfaults", NULL, laserbank_clearfaults,
                 COO_CMD_CLASS_ALWAYS_EFFECT, false, "",
                 "laser/clearfaults",
                 "none", NULL,
                 "TIB-only checks LD overcurrent latches and power-cycles the bank only when one is set",
                 COO_CMD_HELP_EFFECT),
    CMD_SPEC_TIB_PREFIX("laser/bankheater", laserbank_heater, laserbank_heater,
                 COO_CMD_CLASS_SUFFIX_OR_PAYLOAD_EFFECT, true, "mode",
                 "laser/bankheater [mode=<auto|override_on|override_off>]",
                 "mode required for effect; suffix form laser/bankheater/<mode> also works",
                 "mode: auto,override_on,override_off",
                 "TIB-only laser-bank heater relay mode",
                 COO_CMD_HELP_QUERY | COO_CMD_HELP_EFFECT | COO_CMD_HELP_SERIAL_GUARD_QUERY),
    CMD_SPEC_TIB_PREFIX("atten/calibrate/records", atten_calibration_records_get, NULL,
             COO_CMD_CLASS_ALWAYS_QUERY, true, "",
             "atten/calibrate/records/<dac1|dac2>[/<chunk>]",
             "query binary calibration metadata or a fixed raw-record chunk",
             "physical: dac1,dac2",
             "MQTT-only binary dataset exposes retained records and bridge roles",
             COO_CMD_HELP_QUERY | COO_CMD_HELP_SERIAL_GUARD_QUERY),
	    CMD_SPEC_TIB("atten/calibrate", atten_calibration_get, atten_calibration_set,
	             COO_CMD_CLASS_DEFAULT, true,
	             "stop,laser,output,fiber,dwell_ms,persist",
	             "atten/calibrate [laser=<name> output=<route> fiber=<M|S> dwell_ms=<ms> persist=<bool>|stop=<bool>]",
	             "laser and output required for start; stop=true cancels; no payload queries calibration state",
	             "name: 1028y,1270j,1430yj,1430hk,1510h,2330k",
	             "automatic TIB attenuator calibration retains binary acquisition records",
	             COO_CMD_HELP_QUERY | COO_CMD_HELP_EFFECT | COO_CMD_HELP_SERIAL_GUARD_QUERY),
    { .key = "atten", .query_handler = atten_setting_get,
      .effect_handler = atten_setting_set,
      .class_policy = COO_CMD_CLASS_DEFAULT,
      .key_prefix_match = true,
	      .allowed_payload_keys = "value,value_db,value1,value2,value1_db,value2_db,value1_mv,value2_mv,dac1,dac2,open1_mv,open2_mv,persist",
      .mqtt_query_allowed_during_serial_guard = true },
    CMD_HELP_ONLY("atten/<name>", NULL,
                  "atten/<name> [value=<linear>|value_db=<dB>|value1=<linear>|value1_db=<dB>|value1_mv=<mV>|value2=<linear>|value2_db=<dB>|value2_mv=<mV>]",
                  "no payload queries; total value fields cannot mix with value1/value2 fields",
                  "name: 1028y,1270j,1430yj,1430hk,1510h,2330k,lfc",
                  "sets or queries total or per-physical attenuator transmission",
                  COO_CMD_HELP_QUERY | COO_CMD_HELP_EFFECT | COO_CMD_HELP_SERIAL_GUARD_QUERY),
    CMD_HELP_ONLY("atten/<name>/settings", NULL,
                  "atten/<name>/settings [open1_mv=<mV> open2_mv=<mV> persist=<bool>] or full JSON with dac1/dac2 objects",
                  "all fields optional; omitted values retained; persist=true saves complete current settings",
                  "dac1/dac2: fvoa_50pct_mv,slope_inv_fvoa_mv,max_atten_db,max_calibrated_db,gain,rms_db,correction_coeff,max_fit_residual_db",
                  "model changes may move DACs to retain total dB; opens/policy alone do not; persist defaults false; rejected during calibration",
                  COO_CMD_HELP_QUERY | COO_CMD_HELP_EFFECT | COO_CMD_HELP_SERIAL_GUARD_QUERY),
    { .key = "pd/dark", .query_handler = pd_dark_get, .effect_handler = pd_dark_set,
      .class_policy = COO_CMD_CLASS_DEFAULT,
      .supported = command_tib_supported,
      .key_prefix_match = true,
      .allowed_payload_keys = "duration_ms,dark_mv,rms_mv,reset_lowest,persist",
      .mqtt_query_allowed_during_serial_guard = true },
    CMD_HELP_ONLY("pd/dark/<channel>", command_tib_supported,
                  "pd/dark/<channel> [duration_ms=<ms>|dark_mv=<mV> rms_mv=<mV> reset_lowest=<bool> persist=<bool>]",
                  "channel required in key; duration_ms waits for 10s PD-on/laser-off then captures; reset_lowest requires measured dark; persist defaults false",
                  "channel: yj,hk",
                  "TIB-only photodiode dark measurement and forced dark update",
                  COO_CMD_HELP_QUERY | COO_CMD_HELP_EFFECT | COO_CMD_HELP_SERIAL_GUARD_QUERY),
    { .key = "pd", .query_handler = pd_get, .effect_handler = NULL,
      .class_policy = COO_CMD_CLASS_DEFAULT,
      .supported = command_tib_supported,
      .key_prefix_match = true,
      .allowed_payload_keys = "",
      .mqtt_query_allowed_during_serial_guard = true,
      CMD_HELP("pd[/<channel>]",
               "query only; no payload",
               "channel: yj,hk",
               "TIB-only photodiode latest-sample and monitoring-window status",
               COO_CMD_HELP_QUERY | COO_CMD_HELP_SERIAL_GUARD_QUERY) },
    { .key = "pd/settings", .query_handler = pd_settings_get,
      .effect_handler = pd_settings_set,
      .class_policy = COO_CMD_CLASS_DEFAULT,
      .supported = command_tib_supported,
      .key_prefix_match = true,
      .allowed_payload_keys = "noisewarn_mv,responsivity_a_per_w,transimpedance_v_per_a,power,autooff_s,persist",
      .mqtt_query_allowed_during_serial_guard = true },
    CMD_HELP_ONLY("pd/settings/<channel>", command_tib_supported,
                  "pd/settings/<channel> [noisewarn_mv=<mV> responsivity_a_per_w=<A/W> transimpedance_v_per_a=<V/A> power=<auto|override_on|override_off> autooff_s=<s> persist=<bool>]",
                  "channel required in key; listed fields optional for effect",
                  "channel: yj,hk",
                  "TIB-only app-owned photodiode response settings and relay power intent",
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

static enum coo_cmd_msg_type classify_route_loss(const struct coo_cmd_request *cmd,
                                                 const struct coo_cmd_spec *spec,
                                                 void *user_data)
{
    ARG_UNUSED(spec);
    ARG_UNUSED(user_data);

    return cmd != NULL &&
           coo_json_validate_top_level_keys(cmd->payload, "route", NULL, 0U) != 0 ?
           COO_CMD_EFFECT : COO_CMD_QUERY;
}

/* Only the selector is a query. Any supplied setting must reach validation,
 * including malformed values and orphan off_in_s/persist arguments.
 */
static enum coo_cmd_msg_type classify_laser_request(const struct coo_cmd_request *cmd,
                                                   const struct coo_cmd_spec *spec,
                                                   void *user_data)
{
    ARG_UNUSED(spec);
    ARG_UNUSED(user_data);

    return cmd != NULL &&
           coo_json_validate_top_level_keys(cmd->payload, "name", NULL, 0U) != 0 ?
           COO_CMD_EFFECT : COO_CMD_QUERY;
}

// TODO serial_read_three_tokens and serial_single_value_payload are coo_command scope, not app command.c scope, refactor
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
        /* Ordinary reboot preserves PD intent. An intentional settings erase
         * shuts them down before deleting their saved state. */
        if (erase_non_ip_settings) {
            (void)housekeeping_power_set(HOUSEKEEPING_POWER_YJ_PHOTODIODE, false);
            (void)housekeeping_power_set(HOUSEKEEPING_POWER_HK_PHOTODIODE, false);
        }
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

int command_runtime_init(struct k_work_q *blocking_work_q)
{
    const struct coo_cmd_runtime_config cfg = {
        .device_id = app_mqtt_device_id(),
        .inbound_queue = &inbound_queue,
        .outbound_queue = &outbound_queue,
        .reply_queue = &reply_queue,
        .mqtt_msg_id = &mqtt_msg_id,
        .command_specs = command_specs,
        .command_spec_count = ARRAY_SIZE(command_specs),
        .reboot_prepare = command_prepare_reboot,
        .reboot_work_q = blocking_work_q,
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

int app_output_emit(const struct coo_cmd_runtime_emit_args *args)
{
    return coo_cmd_runtime_emit(&command_runtime, args);
}




/* COMMAND HANDLERS */
//TODO evaluate moving mqtt, ip, and time commands to coo_command as builtins

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
                       "{\"src\":\"%s\",\"try_dhcp_first\":%s,"
                       "\"prefer_dhcpdns\":%s,\"prefer_dhcpntp\":%s,"
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

static int ip_get(const struct coo_cmd_request *cmd, struct coo_cmd_response *out)
{
    char payload[COO_CMD_PAYLOAD_MAX];

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

/* Validate supplied addresses before network fallback can mask a typo.
 * Optional server/gateway fields retain their empty-string clear operation.
 */
static int command_extract_ipv4(const char *json, const char *key,
                                char *out, size_t out_len, bool allow_empty)
{
    struct in_addr address;
    int rc = coo_json_extract_string(json, key, out, out_len);

    if (rc == COO_JSON_EXTRACT_OK && !(allow_empty && out[0] == '\0') &&
        net_addr_pton(AF_INET, out, &address) != 0) {
        return COO_JSON_EXTRACT_ERR;
    }
    return rc;
}

static int ip_set(const struct coo_cmd_request *cmd, struct coo_cmd_response *out)
{
    struct app_ip_settings ip_cfg;
    char response[COO_CMD_PAYLOAD_MAX];
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

    parse_rc = coo_json_extract_bool(cmd->payload, "try_dhcp_first", &ip_cfg.try_dhcp_first);
    if (!dhcp_supported) {
        if (parse_rc != COO_JSON_EXTRACT_MISSING) {
            unsupported_dhcp = true;
        }
    } else {
        if (parse_rc == COO_JSON_EXTRACT_OK) {
            changed = true;
            network_changed = true;
        } else if (parse_rc == COO_JSON_EXTRACT_ERR) {
            return coo_cmd_error(out, cmd, "invalid try_dhcp_first");
        }
    }

    parse_rc = coo_json_extract_bool(cmd->payload, "prefer_dhcpdns", &ip_cfg.prefer_dhcp_dns);
    if (!dns_supported) {
        if (parse_rc != COO_JSON_EXTRACT_MISSING) {
            unsupported_dns = true;
        }
    } else {
        if (parse_rc == COO_JSON_EXTRACT_OK) {
            changed = true;
            network_changed = true;
        } else if (parse_rc == COO_JSON_EXTRACT_ERR) {
            return coo_cmd_error(out, cmd, "invalid prefer_dhcpdns");
        }
    }

    parse_rc = coo_json_extract_bool(cmd->payload, "prefer_dhcpntp", &ip_cfg.prefer_dhcp_ntp);
    if (!ntp_supported) {
        if (parse_rc != COO_JSON_EXTRACT_MISSING) {
            unsupported_ntp = true;
        }
    } else {
        if (parse_rc == COO_JSON_EXTRACT_OK) {
            changed = true;
            ntp_changed = true;
        } else if (parse_rc == COO_JSON_EXTRACT_ERR) {
            return coo_cmd_error(out, cmd, "invalid prefer_dhcpntp");
        }
    }

    parse_rc = command_extract_ipv4(cmd->payload, "ip", buf, sizeof(buf), false);
    if (parse_rc == COO_JSON_EXTRACT_OK) {
        strncpy(ip_cfg.ip, buf, sizeof(ip_cfg.ip) - 1);
        ip_cfg.ip[sizeof(ip_cfg.ip) - 1] = '\0';
        changed = true;
        network_changed = true;
    } else if (parse_rc == COO_JSON_EXTRACT_ERR) {
        return coo_cmd_error(out, cmd, "invalid ip");
    }

    parse_rc = command_extract_ipv4(cmd->payload, "subnet", buf, sizeof(buf), false);
    if (parse_rc == COO_JSON_EXTRACT_OK) {
        strncpy(ip_cfg.subnet, buf, sizeof(ip_cfg.subnet) - 1);
        ip_cfg.subnet[sizeof(ip_cfg.subnet) - 1] = '\0';
        changed = true;
        network_changed = true;
    } else if (parse_rc == COO_JSON_EXTRACT_ERR) {
        return coo_cmd_error(out, cmd, "invalid subnet");
    }

    parse_rc = command_extract_ipv4(cmd->payload, "gateway", buf, sizeof(buf), true);
    if (parse_rc == COO_JSON_EXTRACT_OK) {
        strncpy(ip_cfg.gateway, buf, sizeof(ip_cfg.gateway) - 1);
        ip_cfg.gateway[sizeof(ip_cfg.gateway) - 1] = '\0';
        changed = true;
        network_changed = true;
    } else if (parse_rc == COO_JSON_EXTRACT_ERR) {
        return coo_cmd_error(out, cmd, "invalid gateway");
    }

    parse_rc = command_extract_ipv4(cmd->payload, "dns", buf, sizeof(buf), true);
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

    parse_rc = command_extract_ipv4(cmd->payload, "ntp", buf, sizeof(buf), true);
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
        int save_rc = app_settings_update_ip(&ip_cfg, persist);
#if defined(CONFIG_SNTP)
        if (ntp_changed) {
            sntp_sync_schedule_now();
        }
#endif
        if (save_rc != 0) {
            return coo_cmd_error_rc(out, cmd, "IP settings applied; persistence failed", save_rc);
        }
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

static int mqtt_get(const struct coo_cmd_request *cmd, struct coo_cmd_response *out)
{
    struct app_mqtt_settings mqtt_cfg = {0};
    struct coo_mqtt_broker_config broker_cfg = {0};
    char endpoint[160] = {0};
    char payload[COO_CMD_PAYLOAD_MAX] = {0};
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

static int mqtt_set(const struct coo_cmd_request *cmd, struct coo_cmd_response *out)
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

    rc = app_settings_update_mqtt(&mqtt_cfg, persist);
    if (rc != 0) {
        return coo_cmd_error_rc(out, cmd, "MQTT settings applied; persistence failed", rc);
    }
    return coo_cmd_ok(out, cmd);
}

static int time_get(const struct coo_cmd_request *cmd, struct coo_cmd_response *out)
{
    struct timespec ts = {0};
    uint64_t utc_ms;
    char payload[COO_CMD_PAYLOAD_MAX];

    if (sys_clock_gettime(SYS_CLOCK_REALTIME, &ts) != 0) {
        return coo_cmd_error(out, cmd, "clock read failed");
    }
    utc_ms = ((uint64_t)ts.tv_sec * 1000ULL) + ((uint64_t)ts.tv_nsec / 1000000ULL);

    snprintk(payload, sizeof(payload),
             "{\"utc\":%llu,\"uptime_s\":%lld}",
             (unsigned long long)utc_ms, (long long)k_uptime_get()/1000);

    return coo_cmd_reply(out, cmd, COO_CMD_RESP_OK, payload);
}

static int time_set(const struct coo_cmd_request *cmd, struct coo_cmd_response *out)
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

    if (sntp_sync_set_time(&ts) != 0) {
        return coo_cmd_error(out, cmd, "clock set failed");
    }

    return coo_cmd_ok(out, cmd);
}

static bool help_options_name_seen(const char *const names[], uint8_t count,
                              const char *name)
{
    for (uint8_t i = 0U; i < count; ++i) {
        if (names[i] != NULL && name != NULL && strcmp(names[i], name) == 0) {
            return true;
        }
    }
    return false;
}

static int help_options_append_static_string(char *payload, size_t payload_len,
                                        size_t *off, const char *value)
{
    /* Catalog names are compile-time route/laser identifiers, not user text. */
    return coo_json_append(payload, payload_len, off, "\"%s\"",
                           value != NULL ? value : "");
}

static int help_options_append_lasers(char *payload, size_t payload_len, size_t *off)
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
            if (help_options_append_static_string(
                    payload, payload_len, off,
                    hispec_laser_name((enum hispec_laser_id)i)) != 0) {
                return -ENOSPC;
            }
        }
    }

    return coo_json_append(payload, payload_len, off, "]");
}

static int help_options_append_route_name_array(char *payload, size_t payload_len,
                                           size_t *off, const char *field,
                                           bool inputs)
{
    const char *names[MEMS_ROUTER_MAX_ROUTES] = {0};
    uint8_t count = 0U;

    for (uint8_t i = 0U; router.routes != NULL && i < router.num_routes; ++i) {
        const char *name = inputs ? router.routes[i].key.input_name :
                                    router.routes[i].key.output_name;

        if (!help_options_name_seen(names, count, name)) {
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
        if (help_options_append_static_string(payload, payload_len, off, names[i]) != 0) {
            return -ENOSPC;
        }
    }
    return coo_json_append(payload, payload_len, off, "]");
}

static int help_options_append_routes(char *payload, size_t payload_len, size_t *off)
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
            help_options_append_static_string(payload, payload_len, off,
                                         router.routes[i].key.input_name) != 0 ||
            coo_json_append(payload, payload_len, off, ",") != 0 ||
            help_options_append_static_string(payload, payload_len, off,
                                         router.routes[i].key.output_name) != 0 ||
            coo_json_append(payload, payload_len, off, "]") != 0) {
            return -ENOSPC;
        }
    }
    return coo_json_append(payload, payload_len, off, "]");
}

static int help_options_get(const struct coo_cmd_request *cmd,
                       struct coo_cmd_response *out)
{
    char payload[COO_CMD_PAYLOAD_MAX] = {0};
    size_t off = 0U;

    if (coo_json_append(payload, sizeof(payload), &off,
                        "{\"board\":\"%s\",",
                        devices_board_type_name()) != 0 ||
        help_options_append_lasers(payload, sizeof(payload), &off) != 0 ||
        coo_json_append(payload, sizeof(payload), &off, ",") != 0 ||
        help_options_append_route_name_array(payload, sizeof(payload), &off,
                                        "route_inputs", true) != 0 ||
        coo_json_append(payload, sizeof(payload), &off, ",") != 0 ||
        help_options_append_route_name_array(payload, sizeof(payload), &off,
                                        "route_outputs", false) != 0 ||
        coo_json_append(payload, sizeof(payload), &off, ",") != 0 ||
        help_options_append_routes(payload, sizeof(payload), &off) != 0 ||
        coo_json_append(payload, sizeof(payload), &off, "}") != 0) {
        return coo_cmd_error(out, cmd, "help/options response too large");
    }

    return coo_cmd_reply(out, cmd, COO_CMD_RESP_OK, payload);
}

/* Status is executor-owned formatting of owner snapshots. No hardware refresh,
 * persistence, deadline maintenance or publication belongs in this path.
 * Keep floats bounded even for damaged calibration; finite values use %.6g.
 */
BUILD_ASSERT(sizeof(HISPEC_BUILD_VERSION) <= 65, "status firmware identity exceeds its payload budget");

#define STATUS_ADD(...) do { int rc_ = coo_json_append(b, COO_CMD_PAYLOAD_MAX, o, __VA_ARGS__); if (rc_ != 0) return rc_; } while (0)
#define STATUS_CALL(expr) do { int rc_ = (expr); if (rc_ != 0) return rc_; } while (0)
#define STATUS_NUM(value) STATUS_CALL(status_number(b, o, (value)))
#define STATUS_STR(value) STATUS_CALL(status_string(b, o, (value)))

static int status_number(char *b, size_t *o, double value)
{
    if (isfinite(value)) STATUS_ADD("%.6g", value);
    else STATUS_ADD("null");
    return 0;
}

/* Unlike the JSON-safe literal helper, identity/configuration need escaping. */
static int status_string(char *b, size_t *o, const char *value)
{
    if (value == NULL) { STATUS_ADD("null"); return 0; }
    STATUS_ADD("\"");
    for (const unsigned char *p = (const unsigned char *)value; *p; ++p) {
        if (*p == '"' || *p == '\\') STATUS_ADD("\\%c", *p);
        else if (*p < 0x20) STATUS_ADD("\\u%04x", *p);
        else STATUS_ADD("%c", *p);
    }
    STATUS_ADD("\"");
    return 0;
}

static int status_flux(char *b, size_t *o, double value, bool split, bool external)
{
    if (isfinite(value) && (split || external)) {
        STATUS_ADD("\"%.6g (%snot including split)\"", value, external ? "laser(s) only; " : "");
    } else STATUS_NUM(value);
    return 0;
}

struct status_atten {
    double mv[2], open_mv[2], loss_db[2], linear;
    bool valid;
};

struct status_optics {
    struct app_status_settings settings;
    struct mems_switch_snapshot mems[MEMS_ROUTER_MAX_SWITCHES];
    struct hispec_laser_flux_estimate laser[HISPEC_LASER_COUNT];
    struct status_atten atten[NUM_ATTENUATORS];
    bool settings_valid, mems_valid;
};

/* Cached per-DAC losses already belong to attenuator.c; no model fit/readback. */
static void status_atten_snapshot(uint8_t index, struct status_atten *out)
{
    struct attenuator cached;
    attenuator_snapshot(&attenuators[index], &cached);
    const struct attenuator_dac_cfg *dac[] = {&cached.dac_cfg1, &cached.dac_cfg2};
    out->valid = true;
    for (size_t i = 0; i < 2; ++i) {
        out->mv[i] = dac[i]->valid ? (double)dac[i]->voltage : (double)NAN;
        out->loss_db[i] = dac[i]->valid ? dac[i]->attenuation_db : (double)NAN;
        out->open_mv[i] = attenuator_open_voltage(dac[i]);
        out->valid &= dac[i]->valid && isfinite(out->loss_db[i]);
    }
    out->linear = out->valid ? pow(10.0, -(out->loss_db[0] + out->loss_db[1]) / 10.0) : (double)NAN;
}

/* Called once per response; these copies live only for this request. */
static void status_optics_snapshot(struct status_optics *s, bool emission)
{
    memset(s, 0, sizeof(*s));
    s->settings_valid = app_settings_try_get_status(&s->settings);
    s->mems_valid = mems_router_snapshot(&router, s->mems);
    if (!emission) return;
    if (devices_board_type() == HISPEC_BOARD_TIB) {
        for (size_t i = 0; i < HISPEC_LASER_COUNT; ++i) {
            uint8_t index;
            (void)laser_estimate_flux(i, &s->laser[i]);
            if (attenuator_index_from_laser_id(i, &index) == 0)
                status_atten_snapshot(index, &s->atten[i]);
        }
    } else if (devices_board_type() == HISPEC_BOARD_CAL_YJ || devices_board_type() == HISPEC_BOARD_CAL_HK) {
        status_atten_snapshot(HISPEC_ATTENUATOR_LFC_INDEX, &s->atten[0]);
    }
}

static const struct mems_switch_snapshot *status_switch(const struct status_optics *s, const char *name)
{
    if (!s->mems_valid) return NULL;
    for (size_t i = 0; i < router.num_switches; ++i)
        if (strcmp(router.switches[i]->name, name) == 0) return &s->mems[i];
    return NULL;
}

/* A single selected branch, independent of instantaneous toggle phase. */
static int status_branch(const struct mems_switch_snapshot *sw, char state)
{
    if (sw == NULL || !sw->known) return -1;
    double duty = state == 'A' ? sw->a_duty : 1.0 - sw->a_duty;
    return duty == 0.0 ? 0 : (sw->toggling && duty < 1.0 ? 2 : 1);
}

static int status_route(const struct status_optics *s, const char *input, const char *output)
{
    if (!s->mems_valid) return -1;
    const struct mems_route *route = mems_router_get_route(&router, input, output);
    return route != NULL ? mems_route_snapshot_state(&router, s->mems, route) : 0;
}

static double status_laser_flux(const struct status_optics *s, size_t i)
{
    const struct hispec_laser_flux_estimate *laser = &s->laser[i];
    if (strcmp(laser->output_state, "stopped") == 0 || strcmp(laser->output_state, "unexpected stop") == 0)
        return 0.0;
    if (strcmp(laser->output_state, "running") != 0 || laser->control_fault || laser->communication_fault)
        return (double)NAN;
    return laser->flux_nw * s->atten[i].linear;
}

/* 1430's forward switch belongs only to that source, not the combined lasers. */
static int status_laser_route(const struct status_optics *s, size_t i, const char *output, bool retro)
{
    const char *ch = i < 3 ? "yj" : "hk";
    bool is1430 = i == HISPEC_LASER_1430_YJ || i == HISPEC_LASER_1430_HK;
    char input[24];
    if (retro) {
        if (!is1430) return 0;
        snprintk(input, sizeof(input), "%s_forward_retro", ch);
        return status_branch(status_switch(s, input), 'B');
    }
    snprintk(input, sizeof(input), "%s_%s", ch, is1430 ? "1430" : "laser");
    return status_route(s, input, output);
}

static int status_atten_fields(char *b, size_t *o, const struct status_atten *a)
{
    STATUS_ADD("{\"mv1\":"); STATUS_NUM(a->mv[0]);
    STATUS_ADD(",\"mv2\":"); STATUS_NUM(a->mv[1]);
    STATUS_ADD(",\"open1_mv\":"); STATUS_NUM(a->open_mv[0]);
    STATUS_ADD(",\"open2_mv\":"); STATUS_NUM(a->open_mv[1]);
    STATUS_ADD(",\"loss1_db\":"); STATUS_NUM(a->loss_db[0]);
    STATUS_ADD(",\"loss2_db\":"); STATUS_NUM(a->loss_db[1]);
    STATUS_ADD(",\"loss_db\":"); STATUS_NUM(a->loss_db[0] + a->loss_db[1]);
    STATUS_ADD(",\"linear\":"); STATUS_NUM(a->linear);
    return 0; /* Caller appends optional flux and closes the object. */
}

static int status_tib_outputs(char *b, size_t *o, const struct status_optics *s)
{
    STATUS_ADD(",\"outputs\":{");
    for (size_t ch = 0; ch < 2; ++ch) {
        const char *channel = ch == 0 ? "yj" : "hk";
        for (size_t port = 0; port < 3; ++port) {
            bool retro = port == 2, unknown = false, split = false, local = false;
            char output[16], cal[16];
            int paths[3];
            double flux = 0.0;
            snprintk(output, sizeof(output), "%s_%s", channel, port == 0 ? "ao" : port == 1 ? "fei" : "retro");
            snprintk(cal, sizeof(cal), "%s_cal", channel);
            int external = retro ? 0 : status_route(s, cal, output);
            unknown = external < 0;
            split = external == 2;
            for (size_t j = 0; j < 3; ++j) {
                size_t i = ch * 3 + j;
                paths[j] = status_laser_route(s, i, output, retro);
                unknown |= paths[j] < 0;
                split |= paths[j] == 2;
                if (paths[j] <= 0) continue;
                local = true;
                double source = status_laser_flux(s, i);
                /* Zero emission stays zero even without a route calibration. */
                if (!retro && source != 0.0)
                    source *= s->settings_valid ? s->settings.launch_tx[i][port] : (double)NAN;
                flux += source;
            }
            if (unknown || (external > 0 && !local)) flux = (double)NAN;
            STATUS_ADD("%s\"%s\":{\"from\":", ch || port ? "," : "", output);
            if (unknown) STATUS_ADD("null");
            else {
                bool first = true;
                STATUS_ADD("[");
                for (size_t j = 0; j < 3; ++j) if (paths[j] > 0) {
                    STATUS_ADD("%s\"%s\"", first ? "" : ",", hispec_laser_name(ch * 3 + j));
                    first = false;
                }
                if (external > 0) STATUS_ADD("%s\"%s\"", first ? "" : ",", cal);
                STATUS_ADD("]");
            }
            STATUS_ADD(",\"flux_nw\":");
            STATUS_CALL(status_flux(b, o, flux, split && flux != 0.0, external > 0));
            if (retro) STATUS_ADD(",\"light\":\"%s\"", !isfinite(flux) ? "unknown" :
                                  flux == 0.0 ? "off" : split ? "split" : "on");
            STATUS_ADD("}");
        }
    }
    STATUS_ADD("}");
    return 0;
}

static int status_mems(char *b, size_t *o, const struct status_optics *s)
{
    static const char *const tib_labels[4][2] = {
        {"forward", "retro"}, {"laser", "cal"}, {"ao", "fei"}, {"sm", "mm"},
    };
    static const char *const as_labels[3][2] = {
        {"cal/split1", "split2/split3"}, {"cal", "split1"}, {"split2", "split3"},
    };
    static const char *const cal_labels[7][2] = {
        {"lfc", "etalon"}, {"cathgas", "bb"}, {"bb to cal2", "bb to is"},
        {"lfc/etalon", "bb/cathgas"}, {"nm", "selected source"}, {"bb", "nm"}, {"spec", "tib"},
    };
    STATUS_ADD(",\"mems\":");
    if (!s->mems_valid) { STATUS_ADD("null"); return 0; }
    STATUS_ADD("{");
    for (size_t i = 0; i < router.num_switches; ++i) {
        const struct mems_switch_snapshot *sw = &s->mems[i];
        STATUS_ADD("%s\"%s\":", i ? "," : "", router.switches[i]->name);
        if (!sw->known) STATUS_ADD("null");
        else if (sw->toggling) {
            STATUS_ADD("{\"a_duty_pct\":"); STATUS_NUM(sw->a_duty * 100.0);
            STATUS_ADD(",\"cycle_hz\":"); STATUS_NUM(sw->cycle_ms ? 1000.0 / sw->cycle_ms : (double)NAN);
            STATUS_ADD("}");
        } else {
            size_t side = sw->state == 'A' ? 0 : 1;
            const char *label = devices_board_type() == HISPEC_BOARD_TIB ? tib_labels[i < 6 ? i % 3 : 3][side] :
                devices_board_type() == HISPEC_BOARD_AS ? as_labels[i % 3][side] : cal_labels[i][side];
            STATUS_ADD("\"%c (%s)\"", sw->state, label);
        }
    }
    STATUS_ADD("}");
    return 0;
}

static int status_external_outputs(char *b, size_t *o, const struct status_optics *s)
{
    const bool as = devices_board_type() == HISPEC_BOARD_AS;
    const char *const cal_outputs[] = {"spec", "tib", "is"};
    const char *const as_outputs[] = {"yj_cal", "yj_split", "hk_cal", "hk_split"};
    STATUS_ADD(",\"outputs\":{");
    for (size_t port = 0; port < (as ? 4 : 3); ++port) {
        const char *output = as ? as_outputs[port] : cal_outputs[port];
        STATUS_ADD("%s\"%s\":{\"from\":", port ? "," : "", output);
        bool unknown = !s->mems_valid;
        if (as) {
            const char *ch = port < 2 ? "yj" : "hk";
            char input[16], first[16], middle[16];
            snprintk(input, sizeof(input), "%s_calin", ch);
            snprintk(first, sizeof(first), "%s_as1", ch);
            snprintk(middle, sizeof(middle), "%s_as2", ch);
            int a = status_branch(status_switch(s, first), 'A');
            int branch = status_branch(status_switch(s, first), 'B');
            int mid = status_branch(status_switch(s, middle), port % 2 ? 'B' : 'A');
            unknown |= a < 0 || mid < 0;
            bool connected = (port % 2 && branch > 0) || (a > 0 && mid > 0);
            if (unknown) STATUS_ADD("null");
            else STATUS_ADD(connected ? "[\"%s\"]" : "[]", input);
        } else {
            for (size_t i = 0; i < router.num_routes && !unknown; ++i)
                if (strcmp(router.routes[i].key.output_name, output) == 0)
                    unknown |= mems_route_snapshot_state(&router, s->mems, &router.routes[i]) < 0;
            if (unknown) STATUS_ADD("null");
            else {
                bool first = true;
                STATUS_ADD("[");
                for (size_t i = 0; i < router.num_routes; ++i) {
                    const struct mems_route *r = &router.routes[i];
                    if (strcmp(r->key.output_name, output) != 0 ||
                        mems_route_snapshot_state(&router, s->mems, r) <= 0) continue;
                    STATUS_ADD("%s\"%s\"", first ? "" : ",", r->key.input_name);
                    first = false;
                }
                STATUS_ADD("]");
            }
        }
        STATUS_ADD("}");
    }
    STATUS_ADD("}");
    if (as) {
        STATUS_ADD(",\"split\":{");
        for (size_t ch = 0; ch < 2; ++ch) {
            double ratio[3];
            bool known = s->settings_valid && s->mems_valid &&
                mems_split_ratio_snapshot(&router, s->mems, ch, s->settings.split_configured[ch],
                                          s->settings.split_tx[ch], ratio);
            STATUS_ADD("%s\"%s\":{\"ratio_out\":", ch ? "," : "", ch ? "hk" : "yj");
            if (known) {
                STATUS_ADD("["); STATUS_NUM(ratio[0]); STATUS_ADD(",");
                STATUS_NUM(ratio[1]); STATUS_ADD(","); STATUS_NUM(ratio[2]); STATUS_ADD("]");
            } else STATUS_ADD("null");
            uint32_t remaining = 0;
            for (size_t i = 0; i < router.num_switches; ++i)
                if (s->mems_valid && strncmp(router.switches[i]->name, ch ? "hk_" : "yj_", 3) == 0)
                    remaining = MAX(remaining, s->mems[i].off_in_s);
            if (remaining) STATUS_ADD(",\"off_in_s\":%u", remaining);
            STATUS_ADD("}");
        }
        STATUS_ADD("}");
    } else {
        STATUS_ADD(",\"atten\":{\"lfc\":");
        STATUS_CALL(status_atten_fields(b, o, &s->atten[0]));
        STATUS_ADD("}}");
    }
    return status_mems(b, o, s);
}

static int status_emission(char *b, size_t *o, const struct status_optics *s, size_t channel)
{
    const char *ch = channel ? "hk" : "yj";
    STATUS_ADD(",\"channel\":\"%s\",\"emission\":{", ch);
    for (size_t j = 0; j < 3; ++j) {
        size_t i = channel * 3 + j;
        const struct hispec_laser_flux_estimate *l = &s->laser[i];
        STATUS_ADD("%s\"%s\":{\"output_state\":", j ? "," : "", hispec_laser_name(i));
        STATUS_STR(l->output_state);
        STATUS_ADD(",\"i_mA\":"); STATUS_NUM(l->current_ma);
        STATUS_ADD(",\"power_mw\":"); STATUS_NUM(l->power_mw);
        STATUS_ADD(",\"tuned_nm\":"); STATUS_NUM(l->wavelength_nm);
        if (l->off_in_s) STATUS_ADD(",\"off_in_s\":%u", l->off_in_s);
        STATUS_ADD(",\"atten\":");
        STATUS_CALL(status_atten_fields(b, o, &s->atten[i]));
        double source = status_laser_flux(s, i);
        STATUS_ADD(",\"flux_nw\":"); STATUS_NUM(source);
        STATUS_ADD("},\"routes\":");
        if (!s->mems_valid) STATUS_ADD("null");
        else {
            bool first = true;
            STATUS_ADD("{");
            for (size_t port = 0; port < 3; ++port) {
                bool retro = port == 2;
                char output[16];
                snprintk(output, sizeof(output), "%s_%s", ch, port == 0 ? "ao" : port == 1 ? "fei" : "retro");
                int path = status_laser_route(s, i, output, retro);
                if (path == 0) continue;
                STATUS_ADD("%s\"%s\":{", first ? "" : ",", output);
                first = false;
                double tx = s->settings_valid && !retro ? s->settings.launch_tx[i][port] : (double)NAN;
                if (!retro) {
                    STATUS_ADD("\"loss_db\":"); STATUS_NUM(tx > 0.0 ? -10.0 * log10(tx) : (double)NAN);
                    STATUS_ADD(",");
                }
                double flux = path < 0 ? (double)NAN : retro || source == 0.0 ? source : source * tx;
                STATUS_ADD("\"flux_nw\":"); STATUS_CALL(status_flux(b, o, flux, path == 2 && flux != 0.0, false));
                if (retro) STATUS_ADD(",\"light\":\"%s\"", !isfinite(flux) ? "unknown" :
                                      flux == 0.0 ? "off" : path == 2 ? "split" : "on");
                STATUS_ADD("}");
            }
            STATUS_ADD("}");
        }
        STATUS_ADD("}");
    }
    STATUS_ADD("}");
    return 0;
}

enum status_error {
    STATUS_RESET_ERROR = BIT(0), STATUS_RELAY_ERROR = BIT(1), STATUS_BANK_ERROR = BIT(2),
    STATUS_PD_YJ_ERROR = BIT(3), STATUS_PD_HK_ERROR = BIT(4), STATUS_BOARD_ERROR = BIT(5),
    STATUS_NTP_ERROR = BIT(6),
};

/* Shared active work on every profile; append to the caller's activity list. */
static int status_activity_end(char *b, size_t *o, const struct status_optics *s,
                               const struct coo_ota_status *ota, bool ota_valid, bool first)
{
    for (size_t i = 0; s->mems_valid && i < router.num_switches; ++i) if (s->mems[i].toggling) {
        STATUS_ADD("%s\"mems: toggling\"", first ? "" : ","); first = false;
        break;
    }
#if defined(CONFIG_COO_CMD_SERIAL_GUARD)
    if (atomic_get(&command_runtime.serial_guard_active)) {
        STATUS_ADD("%s\"serial guard active\"", first ? "" : ","); first = false;
    }
#endif
    if (atomic_get(&command_runtime.reboot_pending)) {
        STATUS_ADD("%s\"reboot requested\"", first ? "" : ","); first = false;
    }
    if (!ota_valid || ota->active)
        STATUS_ADD("%s\"ota: %s\"", first ? "" : ",", ota_valid ? "active" : "status busy");
    STATUS_ADD("]");
    return 0;
}

static int status_tib_main(char *b, size_t *o, const struct status_optics *s,
                           const struct coo_ota_status *ota, bool ota_valid, unsigned *errors)
{
    struct laserbank_tempcontrol_status bank;
    laserbank_tempcontrol_get_status(&bank);
    bool heater = false;
    int relay_error = housekeeping_power_get_confirmed(HOUSEKEEPING_POWER_BANK_HEATER, &heater);
    if (relay_error) *errors |= STATUS_RELAY_ERROR;
    if (bank.last_error != 0 && bank.last_error != -EBUSY) *errors |= STATUS_BANK_ERROR;
    STATUS_ADD(",\"bank\":{\"power\":\"%s (%s)\",\"heater\":\"%s (%s)\",\"idle_tec_avg_c\":",
               bank.bank_powered ? "on" : "off", hispec_laser_bank_power_mode_name(hispec_laser_bank_power_mode_get()),
               relay_error ? "unknown" : heater ? "on" : "off", laserbank_heater_mode_name(bank.heater_mode));
    STATUS_NUM(bank.idle_tec_temp_avg_c);
    STATUS_ADD("},\"lasers\":{");
    for (size_t i = 0; i < HISPEC_LASER_COUNT; ++i) {
        const struct hispec_laser_flux_estimate *l = &s->laser[i];
        STATUS_ADD("%s\"%s\":\"%s%s%s%s\"", i ? "," : "", hispec_laser_name(i), l->output_state,
                   l->control_fault ? "; operation failed" : "",
                   l->communication_fault ? "; no response" : "",
                   s->atten[i].valid ? "" : "; atten unknown");
    }
    STATUS_ADD("}");
    STATUS_CALL(status_tib_outputs(b, o, s));

    struct photodiode_sample_snapshot pd[PHOTODIODE_CHANNEL_COUNT];
    struct throughput_monitor_status monitor[PHOTODIODE_CHANNEL_COUNT];
    struct laser_calibration_status laser_cal;
    struct attenuator_calibration_status atten_cal;
    photodiode_snapshot(pd);
    bool monitor_valid = throughput_monitor_snapshot(pd, monitor);
    laser_calibration_get_status(&laser_cal);
    attenuator_calibration_get_status(&atten_cal);
    const char *laser_activity = laser_cal.shutdown_pending ? "stop failed" :
        laser_cal.state == LASER_CAL_RUNNING ? "running" :
        laser_cal.state == LASER_CAL_FITTING ? "fitting" :
        laser_cal.state == LASER_CAL_AWAITING_OUTPUT ? "awaiting_output" : NULL;
    int atten_laser = -1;
    if (atten_cal.shutdown_pending || (atten_cal.state && strcmp(atten_cal.state, "running") == 0)) {
        for (size_t i = 0; i < HISPEC_LASER_COUNT; ++i) {
            uint8_t index;
            if (attenuator_index_from_laser_id(i, &index) == 0 && index == atten_cal.attenuator_index) atten_laser = i;
        }
    }
    STATUS_ADD(",\"pd\":{");
    for (size_t ch = 0; ch < 2; ++ch) {
        const char *channel = ch ? "hk" : "yj";
        bool power = false;
        int rc = housekeeping_power_get_confirmed(ch, &power);
        if (rc) *errors |= STATUS_RELAY_ERROR;
        double coeff = monitor_valid ? monitor[ch].wavelength_coeff : (double)NAN;
        int cal_laser = laser_activity != NULL ? (int)laser_cal.laser : atten_laser;
        if (cal_laser >= 0 && (cal_laser < 3 ? 0U : 1U) == ch)
            coeff = photodiode_wavelength_coefficient(ch, s->laser[cal_laser].nominal_wavelength_nm);
        if (!power || rc) coeff = (double)NAN;
        bool overrange = power && rc == 0 && pd[ch].mv >= PHOTODIODE_ADC_USABLE_MV;
        if (overrange) *errors |= ch ? STATUS_PD_HK_ERROR : STATUS_PD_YJ_ERROR;
        char name[24], from[16];
        snprintk(name, sizeof(name), "%s_sm_mm", channel);
        const struct mems_switch_snapshot *sw = status_switch(s, name);
        snprintk(from, sizeof(from), "%s_%s", channel, sw != NULL && sw->state == 'B' ? "mm" : "sm");
        STATUS_ADD("%s\"%s\":{\"power\":\"%s\",\"from\":", ch ? "," : "", channel, rc ? "unknown" : power ? "on" : "off");
        STATUS_STR(sw != NULL && sw->known ? from : NULL);
        STATUS_ADD(",\"net_mv\":"); STATUS_NUM(power && rc == 0 ? pd[ch].net_mv : (double)NAN);
        STATUS_ADD(",\"power_nw\":"); STATUS_NUM(!overrange ? pd[ch].power_uw * 1000.0 * coeff : (double)NAN);
        STATUS_ADD(",\"wavelength_coeff\":"); STATUS_NUM(coeff);
        STATUS_ADD("}");
    }
    STATUS_ADD("},\"activity\":");
    /* Keep other activity visible when the monitor's I/O owner holds its lock. */
    bool first = monitor_valid;
    STATUS_ADD("[");
    if (!monitor_valid) STATUS_ADD("\"measure_throughput: status busy\"");
    for (size_t ch = 0; monitor_valid && ch < 2; ++ch) if (monitor[ch].active || monitor[ch].stopping) {
        STATUS_ADD("%s\"measure_throughput %s: %s", first ? "" : ",", ch ? "hk" : "yj",
                   monitor[ch].stopping ? "stopping" : monitor[ch].preparing ? "preparing" :
                   monitor[ch].autolevel ? "autolevel" : "passive");
        if (monitor[ch].laser_name) STATUS_ADD(" (%s)", monitor[ch].laser_name);
        STATUS_ADD("\""); first = false;
    }
    if (laser_activity) {
        STATUS_ADD("%s\"laser/calibrate %s: %s\"", first ? "" : ",", hispec_laser_name(laser_cal.laser), laser_activity);
        first = false;
    }
    if (atten_laser >= 0) {
        STATUS_ADD("%s\"atten/calibrate %s: %s\"", first ? "" : ",", hispec_laser_name(atten_laser),
                   atten_cal.shutdown_pending ? "stop failed" : "running");
        first = false;
    }
    for (size_t ch = 0; ch < 2; ++ch) if (pd[ch].dark_pending) {
        STATUS_ADD("%s\"pd/dark %s: running\"", first ? "" : ",", ch ? "hk" : "yj"); first = false;
    }
    return status_activity_end(b, o, s, ota, ota_valid, first);
}

static int status_system(char *b, size_t *o, const struct status_optics *s,
                         const struct coo_ota_status *ota, bool ota_valid, unsigned *errors)
{
    struct network_ipv4_info net;
    (void)network_get_ipv4_info(&net);
    STATUS_ADD(",\"ip\":{\"src\":"); STATUS_STR(network_ipv4_source_str(net.source));
    STATUS_ADD(",\"active\":{\"ip\":"); STATUS_STR(net.has_ipv4 ? net.ip : NULL);
    STATUS_ADD(",\"subnet\":"); STATUS_STR(net.has_ipv4 ? net.netmask : NULL);
    STATUS_ADD(",\"gateway\":"); STATUS_STR(net.has_ipv4 ? net.gateway : NULL);
    STATUS_ADD("}},\"mqtt\":");
    if (s->settings_valid) {
        char endpoint[160];
        snprintk(endpoint, sizeof(endpoint), "%s:%u", s->settings.mqtt.broker_host, s->settings.mqtt.broker_port);
        STATUS_ADD("{\"broker\":"); STATUS_STR(endpoint); STATUS_ADD("}");
    } else STATUS_ADD("null");
#if defined(CONFIG_SNTP)
    struct sntp_sync_status ntp;
    sntp_sync_get_status(&ntp);
    STATUS_ADD(",\"ntp\":{\"src\":"); STATUS_STR(sntp_sync_source_str(ntp.source));
    STATUS_ADD(",\"server\":"); STATUS_STR(ntp.server[0] ? ntp.server : NULL);
    STATUS_ADD(",\"last_sync_utc_ms\":");
    if (ntp.last_sync_utc_ms) STATUS_ADD("%llu", (unsigned long long)ntp.last_sync_utc_ms);
    else STATUS_ADD("null");
    STATUS_ADD("}");
    if (ntp.enabled && ntp.last_error) *errors |= STATUS_NTP_ERROR;
#endif
    /* Executor is the sole writer; do not copy the payload-sized request. */
    const struct coo_cmd_lastcommand *last = &command_runtime.lastcommand;
    STATUS_ADD(",\"lastcmd\":");
    if (last->valid) {
        STATUS_ADD("{\"name\":"); STATUS_STR(last->request.key);
        STATUS_ADD(",\"src\":\"%s\",\"t_ms\":%lld}", coo_cmd_source_name(last->request.source), (long long)last->time_ms);
    } else STATUS_ADD("null");
    STATUS_ADD(",\"queues\":{\"inbound\":%u,\"replies\":%u,\"best_effort\":%u}",
               k_msgq_num_used_get(&inbound_queue), k_msgq_num_used_get(&reply_queue), k_msgq_num_used_get(&outbound_queue));
    const atomic_t *counters[] = {&command_runtime.dropped_best_effort, &command_runtime.dropped_replies,
                                 &command_runtime.publish_failures};
    const char *const names[] = {"dropped_best_effort", "dropped_replies", "publish_failures"};
    for (size_t i = 0; i < ARRAY_SIZE(counters); ++i) {
        atomic_val_t n = atomic_get(counters[i]);
        if (n) STATUS_ADD(",\"%s\":%lu", names[i], (unsigned long)n);
    }
    STATUS_ADD(",\"ota\":");
    if (!ota_valid) STATUS_ADD("null");
    else {
        STATUS_ADD("{\"image\":\"%s\",\"image_hash\":\"", ota->confirmed ? "permanent" : "trial");
        for (size_t i = 0; i < sizeof(ota->image_hash); ++i) STATUS_ADD("%02x", ota->image_hash[i]);
        STATUS_ADD("\"");
        if (ota->enabled) STATUS_ADD(",\"enabled\":true");
        if (ota->pending) STATUS_ADD(",\"pending\":true");
        if (ota->remaining_s) STATUS_ADD(",\"remaining_s\":%u", ota->remaining_s);
        if (ota->trial_remaining_s) STATUS_ADD(",\"trial_remaining_s\":%u", ota->trial_remaining_s);
        STATUS_ADD("}");
    }
#if defined(CONFIG_COO_CMD_SERIAL_GUARD)
    int64_t guard = k_ticks_to_ms_ceil64(k_work_delayable_remaining_get(&command_runtime.serial_guard_work));
    if (atomic_get(&command_runtime.serial_guard_active) && guard > 0)
        STATUS_ADD(",\"serial_guard_s\":%lld", (long long)((guard + 999) / 1000));
#endif
    int64_t reboot = k_ticks_to_ms_ceil64(k_work_delayable_remaining_get(&command_runtime.reboot_work));
    if (atomic_get(&command_runtime.reboot_pending) && reboot > 0)
        STATUS_ADD(",\"reboot_in_s\":%lld", (long long)((reboot + 999) / 1000));
    return 0;
}

static int status_payload(char *b, size_t *o, int view, int channel)
{
    struct status_optics optics;
    struct coo_ota_status ota;
    unsigned errors = 0;
    bool ota_valid = coo_ota_try_get_status(&ota);
    status_optics_snapshot(&optics, view == 0 || view == 1);
    struct timespec utc;
    bool utc_valid = sys_clock_gettime(SYS_CLOCK_REALTIME, &utc) == 0;
    char reset[192];
    bool reset_valid = devices_boot_reset_cause(reset, sizeof(reset));
    if (!reset_valid) errors |= STATUS_RESET_ERROR;
    STATUS_ADD("{\"device\":"); STATUS_STR(app_mqtt_device_id());
    if (view) STATUS_ADD(",\"view\":\"%s\"", view == 1 ? "emission" : view == 2 ? "mems" : "system");
    if (!view) {
        STATUS_ADD(",\"fw\":"); STATUS_STR(HISPEC_BUILD_VERSION);
        STATUS_ADD(",\"boots\":");
        if (optics.settings_valid) STATUS_ADD("%u", optics.settings.boot_count);
        else STATUS_ADD("null");
    }
    STATUS_ADD(",\"utc\":");
    if (utc_valid) STATUS_ADD("%llu", (unsigned long long)((uint64_t)utc.tv_sec * 1000U + utc.tv_nsec / 1000000U));
    else STATUS_ADD("null");
    STATUS_ADD(",\"uptime_s\":%lld,\"reset_cause\":", (long long)(k_uptime_get() / 1000));
    STATUS_STR(reset_valid ? reset : NULL);
    if (view == 1) STATUS_CALL(status_emission(b, o, &optics, channel));
    else if (view == 2) STATUS_CALL(status_mems(b, o, &optics));
    else if (view == 3) STATUS_CALL(status_system(b, o, &optics, &ota, ota_valid, &errors));
    else {
        struct housekeeping_temperature_status temp;
        housekeeping_get_temperature_status(&temp);
        if (housekeeping_relay_error()) errors |= STATUS_RELAY_ERROR;
        STATUS_ADD(",\"ambient_c\":");
        if (temp.valid) STATUS_NUM(temp.ambient_c);
        else if (temp.last_error == -ENODEV) STATUS_STR("sensor not available (ENODEV)");
        else if (temp.last_error == -ETIMEDOUT) STATUS_STR("sensor did not respond (ETIMEDOUT)");
        else if (temp.last_error == -EIO) STATUS_STR("sensor read failed (EIO)");
        else if (temp.last_error) STATUS_STR("sensor read failed");
        else STATUS_ADD("null");
        if (devices_board_type() == HISPEC_BOARD_TIB)
            STATUS_CALL(status_tib_main(b, o, &optics, &ota, ota_valid, &errors));
        else {
            if (devices_board_type() != HISPEC_BOARD_UNKNOWN) STATUS_CALL(status_external_outputs(b, o, &optics));
            else errors |= STATUS_BOARD_ERROR;
            STATUS_ADD(",\"activity\":[");
            STATUS_CALL(status_activity_end(b, o, &optics, &ota, ota_valid, true));
        }
    }
    bool first = true;
    int relay_error = errors & STATUS_RELAY_ERROR ? housekeeping_relay_error() : 0;
    const char *const error_text[] = {
        "reset cause read failed", relay_error == -ETIMEDOUT ? "relay controller did not respond" :
        relay_error == -ENODEV ? "relay controller unavailable" : "relay initialization failed",
        "bank temperature control failed",
        "pd yj: ADC overrange", "pd hk: ADC overrange", "board identity unavailable", "NTP synchronization failed",
    };
    for (size_t i = 0; i < ARRAY_SIZE(error_text); ++i) if (errors & BIT(i)) {
        STATUS_ADD("%s", first ? ",\"errors\":[" : ","); STATUS_STR(error_text[i]); first = false;
    }
    if (view == 1) for (size_t i = channel * 3; i < (channel + 1) * 3; ++i) {
        const struct hispec_laser_flux_estimate *l = &optics.laser[i];
        if (!l->control_fault && !l->communication_fault && optics.atten[i].valid) continue;
        STATUS_ADD("%s\"%s:%s%s%s\"", first ? ",\"errors\":[" : ",", hispec_laser_name(i),
                   l->control_fault ? " operation failed" : "",
                   l->communication_fault ? " no response" : "", optics.atten[i].valid ? "" : " atten unknown");
        first = false;
    }
    if (!first) STATUS_ADD("]");
    STATUS_ADD("}");
    return 0;
}

#undef STATUS_STR
#undef STATUS_NUM
#undef STATUS_CALL
#undef STATUS_ADD

static int status_get(const struct coo_cmd_request *cmd, struct coo_cmd_response *out)
{
    static const struct coo_json_string_choice views[] = {{"emission", 1}, {"mems", 2}, {"system", 3}};
    static const struct coo_json_string_choice channels[] = {{"yj", 0}, {"hk", 1}};
    int view = 0, channel = 0;
    int v = coo_json_extract_string_choice(cmd->payload, "view", views, ARRAY_SIZE(views), &view);
    int c = coo_json_extract_string_choice(cmd->payload, "channel", channels, ARRAY_SIZE(channels), &channel);
    if (v == COO_JSON_EXTRACT_ERR) return coo_cmd_error(out, cmd, "invalid status view");
    if (view != 1 && c != COO_JSON_EXTRACT_MISSING)
        return coo_cmd_error(out, cmd, "channel is only used with emission");
    if (view == 1 && c != COO_JSON_EXTRACT_OK)
        return coo_cmd_error(out, cmd, "emission requires channel yj or hk");
    if (devices_board_type() != HISPEC_BOARD_TIB && (view == 1 || view == 2))
        return coo_cmd_error(out, cmd, view == 2 ? "mems is included in status" : "emission requires TIB");
    size_t offset = 0;
    if (status_payload(out->payload, &offset, view, channel) != 0)
        return coo_cmd_error(out, cmd, "status response too large");
    return coo_cmd_reply(out, cmd, COO_CMD_RESP_OK, out->payload);
}

static int temps_get(const struct coo_cmd_request *cmd, struct coo_cmd_response *out)
{
    struct housekeeping_temperature_status ts = {0};
    struct hispec_laser_channel_temperature channel_temp[HISPEC_LASER_COUNT] = {0};
    char payload[COO_CMD_PAYLOAD_MAX] = {0};
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
                if (channel_temp[i].valid && !channel_temp[i].tec_enabled) {
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
        return coo_cmd_error(out, cmd, "temps response too large");
    }

    if (coo_json_append(payload, sizeof(payload), &off,
                        ",\"laserbank_c\":") != 0 ||
        coo_json_append_float_or_null(payload, sizeof(payload), &off,
                                      bank_count > 0U ? bank_sum / (double)bank_count : (double)NAN,
                                      3) != 0 ||
        coo_json_append(payload, sizeof(payload), &off,
                        ",\"lasers\":{") != 0) {
        return coo_cmd_error(out, cmd, "temps response too large");
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
            return coo_cmd_error(out, cmd, "temps response too large");
        }
    }
    if (coo_json_append(payload, sizeof(payload), &off, "}}") != 0) {
        return coo_cmd_error(out, cmd, "temps response too large");
    }

    return coo_cmd_reply(out, cmd, COO_CMD_RESP_OK, payload);
}
