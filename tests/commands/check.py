"""Command regressions using production C bodies and the real Zephyr JSON parser.

Run from the workspace: ./.venv/bin/python hispec-tib/tests/commands/check.py.
Domain operations, hardware, scheduling, and persistence are stubbed in handler checks.
The catalog check stubs handlers to prove invalid keys never reach them.
"""
from pathlib import Path
import re
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
DISPATCH = 'lib/coo_commons/command_dispatch.c'
COMMAND = 'app/src/command.c'
ATTEN = 'app/src/attenuator_command.c'
LASER = 'app/src/laser_command.c'
MEMS = 'app/src/mems_command.c'


def block(file, marker):
    text = (ROOT / file).read_text()
    start = text.rindex(marker)  # Definition, after any forward declaration.
    end = text.index('\n}', start) + 2
    return text[start:end] + (';' if text[end:end + 1] == ';' else '') + '\n'


def functions(file, *markers):
    return ''.join(block(file, marker) for marker in markers)


header = (ROOT / 'include/coo_commons/command_dispatch.h').read_text()
common = r'''
#include <assert.h>
#include <ctype.h>
#include <errno.h>
#include <float.h>
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <arpa/inet.h>
#include <coo_commons/json_utils.h>
#define snprintk snprintf
#define LOG_INF(...) ((void)0)
#define COO_CMD_PAYLOAD_MAX 1024
#define MAX_PAYLOAD_LEN COO_CMD_PAYLOAD_MAX
#define MQTT_QOS_1_AT_LEAST_ONCE 1
'''
common += header[header.index('#define COO_CMD_TOPIC_MAX'):header.index('#if defined(CONFIG_CONSOLE')]
common += header[header.index('enum coo_cmd_msg_type {'):header.index('struct coo_cmd_work {')]
common += header[header.index('struct coo_cmd_spec;'):header.index('/**\n * @brief Runtime wiring')]
common += functions(DISPATCH,
    'bool coo_cmd_key_matches_prefix(', 'const char *coo_cmd_key_suffix_after(',
    'int coo_cmd_key_suffix_segment_copy(', 'int coo_cmd_key_suffix_pair_copy(',
    'bool coo_cmd_payload_empty(', 'int coo_cmd_make_response(', 'int coo_cmd_reply(',
    'int coo_cmd_ok(', 'int coo_cmd_error(', 'int coo_cmd_error_rc(',
    'int coo_cmd_invalid_response(', 'int coo_cmd_unknown_response(',
    'int coo_cmd_unsupported_response(', 'int coo_cmd_busy_response(',
    'static int runtime_unknown_argument_response(', 'static int runtime_validation_reply(',
    'static int runtime_validate_payload_keys(')
common += r'''
static struct coo_cmd_request cmd;
static struct coo_cmd_response out;
static unsigned checks;
static void request(const char *key,const char *payload) {
    memset(&cmd,0,sizeof(cmd)); memset(&out,0,sizeof(out));
    assert(strlen(key)<sizeof(cmd.key) && strlen(payload)<sizeof(cmd.payload));
    strcpy(cmd.key,key); strcpy(cmd.payload,payload); cmd.payload_len=strlen(payload);
    cmd.msg_type=COO_CMD_EFFECT;
}
static void error_response(int rc,const char *message) {
    /* Successful construction of an ERROR response returns zero. */
    assert(rc==0 && out.msg_type==COO_CMD_RESP_ERROR);
    assert(out.payload_len==strlen(out.payload));
    assert(strstr(out.payload,"\"error\":") && strstr(out.payload,message));
    ++checks;
}
'''

source = common
source += '#include "laser_properties.h"\n'
source += block('app/src/app_settings.h', 'struct app_laser_channel_settings {')
source += functions(LASER, 'static bool parse_laserbank_mode_request(',
                    'static int laser_parse_settings_update(')
source += functions(COMMAND, 'static enum coo_cmd_msg_type classify_route_loss(',
                    'static enum coo_cmd_msg_type classify_laser_request(')
source += block(MEMS, 'static const char *const route_loss_laser_names[] = {')
source += functions(MEMS, 'static int route_loss_parse_db_string(',
    'static int route_loss_extract_field_transmission(', 'static int route_loss_extract_value(')
source += r'''
static int mems_split_channel_index(const char *name,uint8_t *index) {
    if(!strcmp(name,"yj")) {*index=0;return 0;}
    if(!strcmp(name,"hk")) {*index=1;return 0;}
    return -EINVAL;
}
'''
source += functions(MEMS, 'static int split_channel_index_from_key(', 'static int split_parse_channel(')
source += r'''
struct dac_channel_cfg {int channel_id;};
struct k_mutex {int unused;};
#define APP_ATTENUATOR_PHYSICAL_COUNT 2
'''
for line in (ROOT / 'app/src/attenuator.h').read_text().splitlines():
    if line.startswith(('#define ATTENUATOR_', '#define FVOA_DEFAULT_')):
        source += line + '\n'
for file, names in {
    'app/src/attenuator.h': ['attenuator_model_coeffs', 'attenuator_dac_cfg', 'attenuator'],
    'app/src/app_settings.h': ['app_attenuator_physical_settings', 'app_attenuator_channel_settings'],
}.items():
    source += functions(file, *('struct ' + name + ' {' for name in names))
source += functions(ATTEN, 'enum attenuator_setting {',
    'enum attenuator_physical_value_mode {', 'struct attenuator_physical_value {')
source += r'''
static struct attenuator attenuators[1];
static int writes,notes,saves,reads;
static bool fail_io;
static int attenuator_index_from_command(const struct coo_cmd_request *c,
    enum attenuator_setting *s,uint8_t *i) {
    *i=0; *s=strstr(c->key,"/coeff")?ATTENUATOR_SETTING_COEFF:ATTENUATOR_SETTING_COMPACT;
    return 0;
}
static bool attenuator_set_db(struct attenuator *a,double v,bool calibrated)
{(void)a;(void)v;(void)calibrated;++writes;return !fail_io;}
static bool attenuator_set_linear(struct attenuator *a,double v,bool calibrated)
{return attenuator_set_db(a,v,calibrated);}
static bool attenuator_set_physical_db(struct attenuator *a,uint8_t i,double v)
{(void)i;return attenuator_set_db(a,v,false);}
static bool attenuator_set_physical_voltage(struct attenuator *a,uint8_t i,float v)
{return attenuator_set_physical_db(a,i,v);}
static void throughput_monitor_note_attenuator_changed(uint8_t i) {(void)i;++notes;}
static int attenuator_status_reply(const struct coo_cmd_request *c,struct coo_cmd_response *o,uint8_t i)
{(void)i;++reads;return coo_cmd_reply(o,c,COO_CMD_RESP_OK,"{\"linear\":0.5}");}
static void attenuator_snapshot(struct attenuator *a,struct attenuator *s) {*s=*a;}
static int attenuator_apply_coefficients_preserve_db(struct attenuator *a,const struct attenuator_model_coeffs *c)
{(void)a;(void)c;++writes;return fail_io?-EIO:0;}
static void app_settings_update_attenuator_channel(uint8_t i,const struct app_attenuator_channel_settings *s,bool p)
{(void)i;(void)s;(void)p;++saves;}
/* Real model arithmetic and coefficient validity are covered by tests/throughput. */
static bool attenuator_model_coefficients_valid(const struct attenuator_model_coeffs *c)
{(void)c;return true;}
'''
source += functions(ATTEN, 'static int parse_attenuator_coeff_object(',
    'static int attenuator_extract_optional_double(', 'static int attenuator_extract_physical_value(',
    'static bool attenuator_set_physical_value(', 'static int attenuator_set_compact_value(',
    'int atten_setting_set(')
source += r'''
#define NET_IPV4_ADDR_LEN 16
#define CONFIG_NET_DHCPV4 1
#define CONFIG_DNS_RESOLVER 1
#define CONFIG_SNTP 1
static int net_addr_pton(int family,const char *text,void *address)
{return inet_pton(family,text,address)==1?0:-EINVAL;}
'''
source += block('app/src/app_settings.h', 'struct app_ip_settings {')
source += r'''
struct network_config {int unused;};
static void app_settings_get_ip(struct app_ip_settings *s) {memset(s,0,sizeof(*s));}
static void network_config_from_app_ip(const struct app_ip_settings *s,struct network_config *n)
{(void)s;(void)n;}
static int network_reconfigure(const struct network_config *n) {(void)n;++writes;return fail_io?-EIO:0;}
static void app_settings_update_ip(const struct app_ip_settings *s,bool p) {(void)s;(void)p;++saves;}
static void sntp_sync_schedule_now(void) {++notes;}
'''
source += functions(COMMAND, 'static int command_extract_ipv4(', 'int ip_set(')
source += functions('app/src/lasers.h', 'enum hispec_laser_id {', 'struct hispec_laser_driver_profile {')
source += functions('app/src/lasers.c', 'static bool float_is_valid(', 'static int validate_laser_settings(')
source += r'''
static int hispec_laser_id_from_name(const char *name,enum hispec_laser_id *id)
{if(strcmp(name,"1028y"))return -EINVAL;*id=HISPEC_LASER_1028_Y;return 0;}
static int hispec_laser_get_channel_settings(enum hispec_laser_id id,struct app_laser_channel_settings *s)
{(void)id;*s=(struct app_laser_channel_settings){.properties=LASER_1028,.expected_serial=1,.current_set_calibration_pct=100};return 0;}
static int hispec_laser_validate_channel_settings(enum hispec_laser_id id,const struct app_laser_channel_settings *s)
{(void)id;return validate_laser_settings(&(struct hispec_laser_driver_profile){.properties=&LASER_1028},s);}
static void throughput_monitor_note_laser_changed(enum hispec_laser_id id,bool stop) {(void)id;assert(stop);++notes;}
static int hispec_laser_update_channel_settings(enum hispec_laser_id id,const struct app_laser_channel_settings *s,bool p)
{(void)id;(void)s;(void)p;++writes;return fail_io?-EIO:0;}
'''
source += functions(LASER, 'static int laser_cmd_error_rc(', 'static int command_laser_id_from_payload(', 'int laser_settings_set(')
source += r'''
#define MEMS_SOURCEDEST_MAX_LEN 24
#define APP_ROUTE_LOSS_ROUTE_MAX_LEN 64
#define PHOTODIODE_CHANNEL_COUNT 2
#define PHOTODIODE_DARK_MIN_MV -100
#define PHOTODIODE_DARK_MAX_MV 100
#define LOG_ERR(...) ((void)0)
enum photodiode_channel {PHOTODIODE_CHANNEL_YJ,PHOTODIODE_CHANNEL_HK};
enum housekeeping_power_output {YJ,HK};
static const char *photodiode_channel_names[]={"yj","hk"};
struct mems_route {struct {const char *input_name,*output_name;} key;};
static int router,stops;
static struct mems_route route;
static const struct mems_route *mems_router_get_route(int *r,const char *in,const char *out)
{(void)r;route=(struct mems_route){{in,out}};return &route;}
static int mems_router_apply_route(int *r,const struct mems_route *p,bool force,const char **failed,char *state)
{(void)r;(void)p;(void)force;(void)failed;(void)state;++writes;return 0;}
static int app_settings_get_route_loss(const char *r,const char *l,double *tx) {(void)r;(void)l;*tx=1;return 0;}
'''
source += functions('app/src/throughput_monitor.h', 'struct throughput_monitor_request {', 'struct throughput_monitor_status {')
source += r'''
static int throughput_monitor_stop(uint8_t channel,struct throughput_monitor_status *s) {(void)channel;(void)s;++stops;return 0;}
static int throughput_monitor_prepare_start(const struct throughput_monitor_request *r) {(void)r;++notes;return 0;}
static int throughput_monitor_start(const struct throughput_monitor_request *r,struct throughput_monitor_status *s)
{(void)r;(void)s;++writes;return 0;}
'''
throughput = (ROOT / 'app/src/throughput_command.c').read_text()
source += throughput[throughput.index('enum throughput_format {'):]
source += functions('app/src/attenuator_calibration.h', 'struct attenuator_calibration_fit_metrics {',
    'struct attenuator_calibration_status {', 'struct attenuator_calibration_auto_request {')
source += functions(ATTEN, 'struct atten_calibration_routes {',
    'static const struct atten_calibration_routes atten_calibration_routes[',
    'static const struct coo_json_string_choice attenuator_cal_fiber_choices[] = {',
    'static int atten_calibration_pd_error(')
source += r'''
struct app_photodiode_settings {struct {struct {double mean_mv;} dark;} channel[2];};
struct photodiode_status {struct {bool dark_pending;double mv;} channel[2];};
static void app_settings_get_photodiode(struct app_photodiode_settings *s) {memset(s,0,sizeof(*s));}
static void photodiode_get_status(struct photodiode_status *s) {memset(s,0,sizeof(*s));}
static int housekeeping_power_get(enum housekeeping_power_output p,bool *on) {(void)p;*on=true;return 0;}
static int attenuator_index_from_laser_id(enum hispec_laser_id id,uint8_t *i) {(void)id;*i=0;return 0;}
static bool devices_attenuator_channel_available(uint8_t i) {(void)i;return true;}
static int attenuator_calibration_stop(struct attenuator_calibration_status *s) {s->state="stopped";++stops;return 0;}
static int attenuator_calibration_start_auto(const struct attenuator_calibration_auto_request *r,struct attenuator_calibration_status *s)
{(void)r;s->state="running";++writes;return 0;}
static int attenuator_calibration_format_status(char *p,size_t n,const struct attenuator_calibration_status *s)
{return snprintf(p,n,"{\"state\":\"%s\"}",s->state)<(int)n?0:-ENOSPC;}
static int attenuator_calibration_write_record_chunk(void *p,size_t n,uint8_t physical,uint8_t chunk,size_t *written)
{(void)p;(void)n;(void)physical;(void)chunk;(void)written;return -ERANGE;}
static int attenuator_calibration_write_data_metadata(void *p,size_t n,uint8_t physical,size_t *written)
{(void)p;(void)n;(void)physical;(void)written;return -ENOENT;}
'''
source += functions(ATTEN, 'static int atten_calibration_status_reply(', 'int atten_calibration_set(',
    'static int parse_calibration_records_suffix(', 'int atten_calibration_records_get(')
source += r'''
int main(void) {
    uint64_t u64; uint32_t u32; uint16_t u16; double d,values[2]; size_t count;
    const char *bad_unsigned[]={"-1","-0","1.5","1e2","18446744073709551616","true","\"1\"","null"};
    for(size_t i=0;i<ARRAY_SIZE(bad_unsigned);++i) {
        char json[96];snprintf(json,sizeof(json),"{\"n\":%s}",bad_unsigned[i]);u64=7;
        assert(coo_json_extract_u64(json,"n",&u64)==COO_JSON_EXTRACT_ERR && u64==7);++checks;
    }
    char max64[]="{\"n\":18446744073709551615}", max32[]="{\"n\":4294967295}";
    char over32[]="{\"n\":4294967296}", max16[]="{\"n\":65535}", over16[]="{\"n\":65536}";
    assert(coo_json_extract_u64(max64,"n",&u64)==0 && u64==UINT64_MAX);
    assert(coo_json_extract_u32(max32,"n",&u32)==0 && u32==UINT32_MAX);
    u32=7; assert(coo_json_extract_u32(over32,"n",&u32)==COO_JSON_EXTRACT_ERR && u32==7);
    assert(coo_json_extract_optional_u16(max16,"n",&u16,NULL)==0 && u16==UINT16_MAX);
    u16=7; assert(coo_json_extract_optional_u16(over16,"n",&u16,NULL)!=0 && u16==7);
    const char *bad_float[]={"NaN","Infinity","-Infinity","1e999"};
    for(size_t i=0;i<ARRAY_SIZE(bad_float);++i) {
        char json[96];snprintf(json,sizeof(json),"{\"n\":%s}",bad_float[i]);d=7;
        assert(coo_json_extract_double(json,"n",&d)==COO_JSON_EXTRACT_ERR && d==7);
        snprintf(json,sizeof(json),"{\"n\":[1,%s]}",bad_float[i]);values[0]=7;count=9;
        assert(coo_json_extract_double_array(json,"n",values,2,&count)==COO_JSON_EXTRACT_ERR);
        assert(values[0]==7);++checks;
    }
    char finite[]="{\"n\":[1.25,-2e3]}";
    assert(coo_json_extract_double_array(finite,"n",values,2,&count)==0 && count==2 && values[1]==-2000);

    const char *bad_atten[]={"{}","{\"value\":-1}","{\"value\":0}","{\"value\":2}",
        "{\"value_db\":-1}","{\"value\":0.5,\"value_db\":3}","{\"value\":0.5,\"value1\":0.5}",
        "{\"value1\":0.5,\"value2\":-0.1}","{\"value1_mv\":2000,\"value2_db\":-1}",
        "{\"value1_mv\":2000,\"value2_mv\":1e100}",
        "{\"value1\":0.5,\"value1_db\":3}","{\"value2\":\"bad\"}","{\"value\":NaN}",
        "{\"value\":0.5,\"persist\":true}","{\"dac1\":{}}"};
    for(size_t i=0;i<ARRAY_SIZE(bad_atten);++i) {
        request("atten/1028y",bad_atten[i]);error_response(atten_setting_set(&cmd,&out),"error");
        assert(writes==0 && saves==0 && notes==0 && reads==0);
    }
    request("atten/1028y","{\"value\":0.5}");fail_io=true;
    error_response(atten_setting_set(&cmd,&out),"apply failed");assert(writes==1 && notes==0 && reads==0);
    fail_io=false;assert(atten_setting_set(&cmd,&out)==0 && out.msg_type==COO_CMD_RESP_OK);
    assert(writes==2 && notes==1 && reads==1 && strstr(out.payload,"linear"));
    writes=notes=reads=0;
    request("atten/1028y/coeff","{\"value\":0.5}");
    error_response(atten_setting_set(&cmd,&out),"value invalid");assert(writes==0 && saves==0);
    char model[]="{\"dac1\":{\"fvoa_50pct_mv\":2500,\"slope_inv_fvoa_mv\":0.002,\"max_atten_db\":55,\"max_calibrated_db\":50,\"gain\":1.533}}";
    struct attenuator_model_coeffs coeff={0};
    assert(parse_attenuator_coeff_object(model,"dac1",&coeff)==0 && coeff.rms_db==ATTENUATOR_DEFAULT_RMS_DB);
    char bad_model[]="{\"dac1\":{\"gain\":1,\"typo\":1}}";
    assert(parse_attenuator_coeff_object(bad_model,"dac1",&coeff)!=0);

    struct app_laser_channel_settings settings={0};bool changed;char invalid[64];
    const char *bad_settings[]={"{\"model_number\":\"x\"}","{\"dne_current_ma\":100}",
        "{\"total_emitting_s\":0}","{\"current_set_calibration_%\":100}","{\"tec_pid\":{\"q\":1}}",
        "{\"tec_pid\":{\"p\":-1}}","{\"tec_pid\":{\"p\":65536}}","{\"autooff_s\":-1}",
        "{\"expected_serial\":0}","{\"fractional_noise\":NaN}","{\"operating_temp_range_c\":[1,Infinity]}"};
    for(size_t i=0;i<ARRAY_SIZE(bad_settings);++i) {
        char json[160];strcpy(json,bad_settings[i]);changed=false;strcpy(invalid,"settings");
        assert(laser_parse_settings_update(json,&settings,&changed,invalid)!=0);
        request("laser/settings","{}");
        snprintf(cmd.payload,sizeof(cmd.payload),"{\"name\":\"1028y\",\"settings\":%s}",json);
        cmd.payload_len=strlen(cmd.payload);
        error_response(laser_settings_set(&cmd,&out),"invalid or read-only");
        assert(writes==0 && notes==0);
    }
    char good_settings[]="{\"autooff_s\":0,\"current_set_calibration_pct\":100,\"tec_pid\":{\"p\":65535}}";
    changed=false;assert(laser_parse_settings_update(good_settings,&settings,&changed,invalid)==0);
    assert(changed && settings.properties.tec_pid.kp==65535);
    request("laser/settings","{\"name\":\"1028y\",\"settings\":{\"max_current_ma\":1}}");
    error_response(laser_settings_set(&cmd,&out),"out of range");assert(writes==0 && notes==0);
    request("laser/settings","{\"name\":\"1028y\",\"persist\":true}");
    error_response(laser_settings_set(&cmd,&out),"settings object");assert(writes==0 && notes==0);
    request("laser/settings","{\"name\":\"1028y\",\"settings\":{\"autooff_s\":30}}");
    assert(laser_settings_set(&cmd,&out)==0 && out.msg_type==COO_CMD_RESP_OK && writes==1 && notes==1);
    writes=notes=0;
    request("laser","{\"name\":\"1028y\"}");assert(classify_laser_request(&cmd,NULL,NULL)==COO_CMD_QUERY);
    request("laser","{\"name\":\"1028y\",\"autooff_s\":1}");assert(classify_laser_request(&cmd,NULL,NULL)==COO_CMD_EFFECT);
    request("laser/settings","{\"name\":\"1028y\",\"persist\":true}");assert(classify_laser_request(&cmd,NULL,NULL)==COO_CMD_EFFECT);
    request("mems/route/loss","{\"route\":\"a\",\"persist\":true}");assert(classify_route_loss(&cmd,NULL,NULL)==COO_CMD_EFFECT);

    const struct coo_json_string_choice modes[]={{"auto",0},{"override_on",1},{"override_off",2}};int mode;
    const char *bad_mode_keys[]={"laser/bankpower/nope","laser/bankpower/","laser/bankpower/auto"};
    for(size_t i=0;i<ARRAY_SIZE(bad_mode_keys);++i) {
        request(bad_mode_keys[i],"{\"mode\":\"override_on\"}");
        assert(!parse_laserbank_mode_request(&cmd,"laser/bankpower",modes,3,&mode));++checks;
    }
    request("laser/bankpower/auto","{\"mode\":\"auto\"}");assert(parse_laserbank_mode_request(&cmd,"laser/bankpower",modes,3,&mode));
    request("laser/bankpower/auto","{\"mode\":false}");assert(!parse_laserbank_mode_request(&cmd,"laser/bankpower",modes,3,&mode));
    uint8_t channel;
    request("mems/split/nope","{\"channel\":\"yj\"}");assert(split_parse_channel(&cmd,&channel)!=0);
    request("mems/split/yj","{\"channel\":\"hk\"}");assert(split_parse_channel(&cmd,&channel)!=0);
    request("mems/split/yj","{\"channel\":\"yj\"}");assert(split_parse_channel(&cmd,&channel)==0 && channel==0);
    char laser[16];double transmission;
    request("mems/route/loss","{\"1028y\":0.1,\"1270j\":0.2}");assert(route_loss_extract_value(&cmd,laser,sizeof(laser),&transmission)==-EALREADY);
    request("mems/route/loss","{\"1028y\":0.1,\"1270j\":true}");assert(route_loss_extract_value(&cmd,laser,sizeof(laser),&transmission)!=0);
    request("mems/route/loss","{\"1028y\":0.1}");assert(route_loss_extract_value(&cmd,laser,sizeof(laser),&transmission)==0 && transmission==0.9);

    const char *bad_ip[]={"{\"ip\":\"999.1.1.1\"}","{\"ip\":\"\"}","{\"subnet\":\"bad\"}",
        "{\"gateway\":\"a.b.c.d\"}","{\"dns\":\"host\"}","{\"ntp\":\"1.2.3\"}",
        "{\"ip\":\"10.1.2.3\",\"persist\":\"bad\"}","{\"ip\":\"10.1.2.3\",\"dns\":false}"};
    for(size_t i=0;i<ARRAY_SIZE(bad_ip);++i) {
        request("ip",bad_ip[i]);error_response(ip_set(&cmd,&out),"invalid");
        assert(writes==0 && saves==0 && notes==0);
    }
    request("ip","{\"ip\":\"10.1.2.3\",\"gateway\":\"\",\"dns\":\"\",\"ntp\":\"\"}");
    assert(ip_set(&cmd,&out)==0 && out.msg_type==COO_CMD_RESP_OK && writes==1 && saves==1 && notes==1);
    writes=saves=notes=0;
    request("measure_throughput","{\"stop\":\"all\",\"laser\":false,\"off_in_s\":-1,\"format\":42}");
    assert(measure_throughput_set(&cmd,&out)==0 && out.msg_type==COO_CMD_RESP_OK && stops==1 && writes==0 && notes==0);
    request("atten/calibrate","{\"stop\":true,\"laser\":false,\"dwell_ms\":-1,\"persist\":42}");
    assert(atten_calibration_set(&cmd,&out)==0 && out.msg_type==COO_CMD_RESP_OK && stops==2 && writes==0);
    request("measure_throughput","{\"stop\":true}");error_response(measure_throughput_set(&cmd,&out),"invalid stop");
    request("atten/calibrate","{\"stop\":23}");error_response(atten_calibration_set(&cmd,&out),"invalid stop");
    request("measure_throughput","{\"laser\":\"1028y\",\"output\":\"yj_ao\",\"off_in_s\":-1}");
    error_response(measure_throughput_set(&cmd,&out),"invalid off_in_s");
    request("atten/calibrate","{\"laser\":\"1028y\",\"output\":\"yj_ao\",\"dwell_ms\":-1}");
    error_response(atten_calibration_set(&cmd,&out),"invalid dwell_ms");
    assert(stops==2 && writes==0 && notes==0 && saves==0);
    request("atten/calibrate/records/dac1/99","{}");
    error_response(atten_calibration_records_get(&cmd,&out),"chunk index out of range");
    request("atten/calibrate/records/dac1","{}");
    error_response(atten_calibration_records_get(&cmd,&out),"calibration records unavailable");
    printf("command parser/handler checks passed (%u rejection cases plus valid boundaries)\n",checks);
}
'''


def run(name, text, extra_flags=()):
    with tempfile.TemporaryDirectory(prefix='hispec-command-') as tmp:
        cfile, exe = Path(tmp) / (name + '.c'), Path(tmp) / name
        cfile.write_text(text)
        # Host architecture selection only; firmware does not enable POSIX.
        subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror',
            '-Wno-unused-function', '-Wno-unused-const-variable',
            *extra_flags,
            '-DCONFIG_ARCH_POSIX=1', '-DCONFIG_JSON_LIBRARY_FP_SUPPORT=1', '-Dsnprintk=snprintf',
            '-I' + str(ROOT.parent / 'zephyr/include'), '-I' + str(ROOT / 'include'),
            '-I' + str(ROOT / 'app/src'), str(cfile),
            str(ROOT.parent / 'zephyr/lib/utils/json.c'), str(ROOT / 'lib/coo_commons/json_utils.c'),
            '-lm', '-o', str(exe)], check=True)
        subprocess.run([str(exe)], check=True)


run('handlers', source)

# Unsupported optional IP features retain their partial-response behavior.
ip_disabled = source[:source.index('int main(void) {')]
for feature in ('CONFIG_NET_DHCPV4', 'CONFIG_DNS_RESOLVER', 'CONFIG_SNTP'):
    ip_disabled = ip_disabled.replace('#define ' + feature + ' 1', '')
ip_disabled += r'''
int main(void) {
    request("ip","{\"ip\":\"10.1.2.3\",\"dns\":false,\"ntp\":\"host\",\"try_dhcp_first\":42}");
    assert(ip_set(&cmd,&out)==0 && out.msg_type==COO_CMD_RESP_OK);
    assert(strstr(out.payload,"unsupported") && writes==1 && saves==1 && notes==0);
    puts("unsupported IP feature partial-response check passed");
}
'''
# The existing ip_set keeps ntp_changed when SNTP is compiled out.
run('ip_disabled', ip_disabled, ('-Wno-unused-but-set-variable',))

catalog = common + r'''
#define CONFIG_COO_CMD_REBOOT 1
#define CONFIG_COO_CMD_SERIAL_GUARD 1
#define LOG_WRN(...) ((void)0)
#define K_MSEC(ms) (ms)
#define K_NO_WAIT 0
struct coo_cmd_runtime {
    const struct coo_cmd_spec *command_specs;size_t command_spec_count;void *user_data;
    int reboot_pending,reboot_work;uint32_t reboot_delay_ms;bool reboot_erase_non_ip_settings;
    uint32_t serial_guard_seconds;uint16_t serial_wrap_column;char device_id[32];
    struct coo_cmd_request ingress_cmd;struct coo_cmd_response outbound_scratch;void *inbound_queue;
};
static int calls,scheduled,queued;
static int atomic_get(const int *p) {return *p;}
static bool atomic_cas(int *p,int old,int next) {if(*p!=old)return false;*p=next;return true;}
static void atomic_clear(int *p) {*p=0;}
static int k_work_schedule(int *w,uint32_t ms) {(void)w;(void)ms;++scheduled;return 0;}
static void runtime_record_lastcommand(struct coo_cmd_runtime *r,const struct coo_cmd_request *c) {(void)r;(void)c;}
static bool runtime_serial_guard_active(const struct coo_cmd_runtime *r) {(void)r;return false;}
static void runtime_clear_serial_guard(struct coo_cmd_runtime *r) {(void)r;++scheduled;}
static void runtime_note_serial_guard_activity(struct coo_cmd_runtime *r) {(void)r;}
static int runtime_serial_guard_get(struct coo_cmd_runtime *r,const struct coo_cmd_request *c,struct coo_cmd_response *o)
{(void)r;return coo_cmd_ok(o,c);}
static int runtime_help_response(struct coo_cmd_runtime *r,const struct coo_cmd_request *c,struct coo_cmd_response *o)
{(void)r;return coo_cmd_ok(o,c);}
static bool command_tib_supported(const struct coo_cmd_spec *s,void *u) {(void)s;(void)u;return true;}
static int record_handler(const struct coo_cmd_request *c,struct coo_cmd_response *o)
{++calls;return coo_cmd_ok(o,c);}
'''
catalog += functions(DISPATCH, 'static bool coo_cmd_spec_key_matches(',
    'const struct coo_cmd_spec *\ncoo_cmd_runtime_find_spec(', 'bool coo_cmd_runtime_spec_supported(',
    'static bool runtime_key_is_help(', 'static bool runtime_key_is_serial_guard(',
    'static bool runtime_key_is_reboot(', 'static int runtime_serial_guard_set(',
    'static int runtime_parse_reboot_options(', 'static bool runtime_reboot_pending(',
    'static int runtime_reboot_set(', 'static bool runtime_handle_builtin_request(',
    'static int runtime_execute_default(', 'static enum coo_cmd_msg_type runtime_classify(')
catalog += functions(COMMAND, 'static enum coo_cmd_msg_type classify_route_loss(',
    'static enum coo_cmd_msg_type classify_laser_request(')
catalog += functions(DISPATCH, 'static const char *skip_serial_space(',
    'bool coo_cmd_serial_next_token(', 'bool coo_cmd_serial_has_extra(',
    'bool coo_cmd_serial_token_is_number(', 'static bool serial_token_has_control(',
    'static bool serial_token_is_json_number(', 'static int serial_append_json_number(',
    'static const char *serial_token_bool_json(', 'int coo_cmd_serial_append_json_value(',
    'int coo_cmd_serial_append_json_field(', 'static int serial_payload_from_key_values(',
    'static int serial_payload_from_value(', 'static int serial_payload_from_positional(',
    'static int serial_payload_from_serial_guard(', 'int coo_cmd_normalize_serial_payload(',
    'static int runtime_normalize_serial_payload(', 'static bool payload_has_text(')
catalog += functions(COMMAND, 'static int serial_read_three_tokens(',
    'static int serial_single_value_payload(', 'static int serial_mems_switch_shorthand(')
catalog += r'''
static int coo_cmd_format_response_topic(const char *d,const char *k,char *o,size_t n)
{(void)d;return snprintf(o,n,"resp/%s",k)<(int)n?0:-ENOSPC;}
static void runtime_enqueue_serial_error(struct coo_cmd_runtime *r,const char *message)
{(void)r;coo_cmd_error(&out,NULL,message);}
static void runtime_print_serial_help(struct coo_cmd_runtime *r,uint16_t width) {(void)r;(void)width;}
static void runtime_enqueue_response(struct coo_cmd_runtime *r,struct coo_cmd_response *o) {(void)r;out=*o;}
static int k_msgq_put(void *q,const struct coo_cmd_request *c,int timeout)
{(void)q;(void)timeout;cmd=*c;++queued;return 0;}
'''
catalog += block(DISPATCH, 'void coo_cmd_runtime_handle_serial_line(')
table_source = (ROOT / COMMAND).read_text()
table = table_source[table_source.index('#define CMD_HELP('):table_source.index('#undef CMD_HELP')]
# Compile the actual catalog unchanged, binding only its domain handlers to a
# recorder. The keys, dispatch policies, schema lists, and serial forms stay real.
handlers = set(re.findall(r'CMD_SPEC\w*\("[^"\n]+",\s*(\w+),\s*(\w+)', table))
handler_names = {name for pair in handlers for name in pair}
handler_names.update(re.findall(r'\.(?:query|effect)_handler\s*=\s*(\w+)', table))
handler_names.discard('NULL')
catalog += ''.join('#define ' + name + ' record_handler\n' for name in sorted(handler_names))
catalog += table
catalog += r'''
int main(void) {
    struct coo_cmd_runtime runtime={.command_specs=command_specs,.command_spec_count=ARRAY_SIZE(command_specs)};
    unsigned endpoints=0;
    for(size_t i=0;i<ARRAY_SIZE(command_specs);++i) {
        const struct coo_cmd_spec *spec=&command_specs[i];
        if(!spec->query_handler && !spec->effect_handler) continue;
        request(spec->key,"{\"typo\":1}");cmd.msg_type=runtime_classify(&runtime,&cmd);
        error_response(runtime_execute_default(&runtime,&cmd,&out),"unknown argument");
        assert(strstr(out.payload,"typo") && calls==0);++endpoints;
        request(spec->key,"{\"bad\\\"key\":1}");
        error_response(runtime_execute_default(&runtime,&cmd,&out),"invalid payload");
        assert(calls==0);
    }
    const char *builtin[]={"help","serialguard","reboot"};
    for(size_t i=0;i<ARRAY_SIZE(builtin);++i) {
        request(builtin[i],"{\"typo\":1}");
        assert(runtime_handle_builtin_request(&runtime,&cmd,&out));error_response(0,"unknown argument");
        assert(scheduled==0 && runtime.reboot_pending==0 && runtime.serial_guard_seconds==0);
    }
    const char *guard_bad[]={"{\"seconds\":-1}","{\"seconds\":1.5}","{\"seconds\":4294967296}",
        "{\"seconds\":1,\"persist\":false}","{\"persist\":true}"};
    for(size_t i=0;i<ARRAY_SIZE(guard_bad);++i) {
        request("serialguard",guard_bad[i]);assert(runtime_handle_builtin_request(&runtime,&cmd,&out));
        error_response(0,"error");assert(scheduled==0 && runtime.serial_guard_seconds==0);
    }
    const char *reboot_bad[]={"{\"erase_non_ip_settings\":true,\"value\":\"erase_non_ip_settings\"}",
        "{\"erase_non_ip_settings\":23}","{\"value\":\"typo\"}"};
    for(size_t i=0;i<ARRAY_SIZE(reboot_bad);++i) {
        request("reboot",reboot_bad[i]);assert(runtime_handle_builtin_request(&runtime,&cmd,&out));
        error_response(0,"invalid reboot options");assert(scheduled==0 && !runtime.reboot_pending);
    }
    const char *empty[]={"","{}"," \t{ \n } \r\n"};
    for(size_t i=0;i<ARRAY_SIZE(empty);++i) {
        request("temps",empty[i]);assert(coo_cmd_payload_empty(&cmd));
        assert(runtime_classify(&runtime,&cmd)==COO_CMD_QUERY);
    }
    request("laser/bankpower/","{}");assert(runtime_classify(&runtime,&cmd)==COO_CMD_EFFECT);
    request("laser/bankheater/","{}");assert(runtime_classify(&runtime,&cmd)==COO_CMD_EFFECT);
    request("laser/typo","{}");error_response(runtime_execute_default(&runtime,&cmd,&out),"Unknown request");
    request("mems/yj_laser_cal","{\"value\":\"B\"}");
    error_response(runtime_execute_default(&runtime,&cmd,&out),"unknown argument");

    char long_token[300],normalized[1024];memset(long_token,'1',sizeof(long_token)-1);long_token[sizeof(long_token)-1]=0;
    const char *cursor=long_token;char tiny[8]="kept";
    assert(!coo_cmd_serial_next_token(&cursor,tiny,sizeof(tiny)) && cursor==long_token && !strcmp(tiny,"kept"));
    assert(coo_cmd_serial_has_extra(cursor));
    assert(coo_cmd_normalize_serial_payload("time",long_token,NULL,NULL,normalized,sizeof(normalized))!=0);
    long_token[4]='=';
    assert(coo_cmd_normalize_serial_payload("time",long_token,NULL,NULL,normalized,sizeof(normalized))!=0);
    memset(long_token,'k',COO_CMD_KEY_MAX);long_token[COO_CMD_KEY_MAX]=0;
    coo_cmd_runtime_handle_serial_line(&runtime,long_token);error_response(0,"command key too long");assert(queued==0);
    char serial[]="mems/yj_laser_cal B 0.5 30";
    coo_cmd_runtime_handle_serial_line(&runtime,serial);assert(queued==1);
    assert(strstr(cmd.payload,"\"state\":\"A\"") && strstr(cmd.payload,"\"off_in_s\":30"));
    request("reboot","{\"erase_non_ip_settings\":false}");
    assert(runtime_handle_builtin_request(&runtime,&cmd,&out));
    assert(out.msg_type==COO_CMD_RESP_OK && runtime.reboot_pending && scheduled==1);
    printf("catalog/serial/builtin checks passed (%u command rows, %u rejection cases)\n",endpoints,checks);
}
'''
run('catalog', catalog)
