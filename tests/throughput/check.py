"""Host checks for production C arithmetic/control with ADC and actuator I/O stubbed.

Run with the workspace venv: python tests/throughput/check.py.
Function bodies and data layouts are read from firmware so these checks exercise
its implementation; this is not a second Python model of the control loop.
"""
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]


def block(file, marker):
    text = (ROOT / 'app/src' / file).read_text()
    start = text.index(marker)
    opening = text.index('{', start)
    depth = 1
    end = opening + 1
    while depth:
        depth += (text[end] == '{') - (text[end] == '}')
        end += 1
    prefix = block(file, 'enum throughput_phase {') + ';\n' if marker == 'struct throughput_state {' else ''
    return prefix + text[start:end] + (';' if marker.startswith('struct ') else '') + '\n'


quantize_source = r'''
#define DIVIDER_CURRENT 10.0
#define CLAMP(x,lo,hi) ((x)<(lo)?(lo):((x)>(hi)?(hi):(x)))
''' + block('lasers.c', 'double hispec_laser_quantize_current_ma(')
monitor_text = (ROOT/'app/src/throughput_monitor.c').read_text()
priority_source = monitor_text[monitor_text.index('#define TP_ATTEN_FIRST'):monitor_text.index('#define TP_FLAG_OVERRANGE')]
initial_level_define = next(line for line in (ROOT/'app/src/throughput_monitor.h').read_text().splitlines()
                            if line.startswith('#define THROUGHPUT_DEFAULT_INITIAL_LEVEL')) + '\n'


# Real host mutexes and an I/O barrier prove getters do not wait for transactions.
# Recursive state locks match Zephyr's mutex semantics. An alarm bounds deadlocks.
mutex_harness = r'''
#include <pthread.h>
#include <stdatomic.h>
#include <unistd.h>
#include <time.h>
struct k_mutex {pthread_mutex_t mutex;};
static void init_mutex(struct k_mutex *m) {
    pthread_mutexattr_t a; assert(!pthread_mutexattr_init(&a));
    assert(!pthread_mutexattr_settype(&a,PTHREAD_MUTEX_RECURSIVE));
    assert(!pthread_mutex_init(&m->mutex,&a)); pthread_mutexattr_destroy(&a);
}
static int k_mutex_lock(struct k_mutex *m,int timeout) {
    (void)timeout; return pthread_mutex_lock(&m->mutex);
}
static void k_mutex_unlock(struct k_mutex *m) {assert(!pthread_mutex_unlock(&m->mutex));}
static atomic_bool block_io,entered_io,release_io;
static void io_barrier(void) {
    if(!atomic_load(&block_io)) return;
    atomic_store(&entered_io,true);
    while(!atomic_load(&release_io)) nanosleep(&(struct timespec){.tv_nsec=1000000},NULL);
}
static void wait_for_io(void) {
    while(!atomic_load(&entered_io)) nanosleep(&(struct timespec){.tv_nsec=1000000},NULL);
}
'''


source = r'''
#include <assert.h>
#include <errno.h>
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <stdarg.h>
#define MAX(a,b) ((a)>(b)?(a):(b))
#define MIN(a,b) ((a)<(b)?(a):(b))
#define ARRAY_SIZE(a) (sizeof(a)/sizeof((a)[0]))
#define PD_WINDOW_MAX_SAMPLES 40
#define PHOTODIODE_CHANNEL_COUNT 2
#define PHOTODIODE_SAMPLE_INTERVAL_MS 50U
#define PHOTODIODE_ADC_USABLE_MV 2000.0
#define PHOTODIODE_ADC_LSB_MV .0625
#define TP_LOW_FRACTION .2
#define TP_HIGH_FRACTION .8
#define TP_MIN_ATTEN_TX 1e-9
#define TP_FLAG_OVERRANGE 1
#define TP_FLAG_AUTOLEVEL 2
#define K_FOREVER 0
#define PD_HARDWARE_LOG_RATELIMIT_MS 10000
#define PD_NOISE_WARNING_COOLDOWN_MS 10000
#define LOG_WRN(...) ((void)0)
#define snprintk snprintf
#define COO_CMD_RUNTIME_EMIT_DATA 0
#define COO_CMD_RUNTIME_EMIT_WARNING 1
#define COO_CMD_RUNTIME_EMIT_BEST_EFFORT 0
static int64_t clock_ms=105;
static int pd_runtime_lock;
static int64_t k_uptime_get(void) {return clock_ms;}
static void k_mutex_lock(int *p,int t) {(void)p;(void)t;}
static void k_mutex_unlock(int *p) {(void)p;}
enum photodiode_channel {PHOTODIODE_CHANNEL_YJ,PHOTODIODE_CHANNEL_HK};
enum hispec_laser_id {HISPEC_LASER_1028_Y};
struct app_pd_dark_result {uint32_t duration_ms; uint16_t failed_samples; double mean_mv,rms_mv;};
struct app_pd_channel_settings {struct app_pd_dark_result dark; double responsivity_a_per_w,transimpedance_v_per_a,noise_warn_rms_mv;};
'''
for file,names in {
    'photodiode.h':['photodiode_window_result','photodiode_channel_status'],
    'photodiode.c':['pd_window_runtime','photodiode_dark_action','photodiode_runtime_channel'],
    'throughput_monitor.c':['throughput_source_reference','throughput_state'],
}.items():
    for name in names: source += block(file,'struct '+name+' {')
source += quantize_source + priority_source
source += r'''
static struct photodiode_runtime_channel pd_runtime[2];
static struct attenuator {double attenuation_db;} attenuators[6];
struct attenuator_transmission_estimate { double attenuation_db; };
static bool attenuator_estimate_transmission(struct attenuator *a, struct attenuator_transmission_estimate *out)
{out->attenuation_db=a->attenuation_db;return true;}
static double written_tx,written_pct;
static int fail_write; static bool clamp_atten,clamp_laser;
static int atten_writes,laser_writes;
static double applied_current;
typedef struct {double threshold_current_ma,nominal_current_ma;} laserprops_t;
struct app_laser_channel_settings {laserprops_t properties;double min_autolevel_current_ma;};
static struct app_laser_channel_settings control_settings={{8,60},8.1};
static int hispec_laser_get_channel_settings(enum hispec_laser_id l,struct app_laser_channel_settings *s)
{(void)l;*s=control_settings;return 0;}
struct hispec_laser_flux_estimate {double current_ma;};
static int laser_estimate_flux(enum hispec_laser_id l,struct hispec_laser_flux_estimate *s)
{(void)l;s->current_ma=applied_current;return 0;}
static bool attenuator_set_linear(struct attenuator *a,double tx,bool calibrated_only) {
    assert(calibrated_only);
    atten_writes++;written_tx=tx; if(fail_write) return false;
    if(!clamp_atten) a->attenuation_db=-10*log10(tx);
    return true;
}
static int hispec_laser_set_output_percent_autooff(enum hispec_laser_id l,double p,unsigned t,bool tune)
{(void)l;assert(!tune && t==0);laser_writes++;written_pct=p;
 if(!fail_write && !clamp_laser) applied_current=hispec_laser_quantize_current_ma(8+52*p/100,0,60);
 return fail_write?-EIO:0;}
static void pd_window_result_clear(struct photodiode_window_result *w) {memset(w,0,sizeof(*w)); w->mean_mv=w->mean_net_mv=w->mean_net_err_mv=NAN;}
'''
source += block('photodiode.c','double photodiode_power_uw_from_mv(')
for marker in ['static double pd_read_noise_mv(', 'static double pd_dark_mean_error_mv(',
               'static void pd_window_recompute(', 'static void pd_window_add_sample(']:
    source += block('photodiode.c',marker)
source += r'''
static double photodiode_wavelength_coefficient(double nm) {(void)nm;return 1;}
static void pd_windows_ensure_locked(struct photodiode_runtime_channel *r) {if(!r->fixed_window.target_samples)r->fixed_window.target_samples=10;if(!r->configurable_window.target_samples)r->configurable_window.target_samples=10;}
static bool pd_sample_is_step(struct photodiode_runtime_channel *r,double mv) {(void)r;(void)mv;return false;}
static void pd_window_snapshot_last(struct pd_window_runtime *w) {w->last=w->current;}
static bool pd_stage_completed_dark_locked(struct photodiode_runtime_channel *r,struct app_pd_dark_result *d,bool *p,bool *l,bool *f) {(void)r;(void)d;(void)p;(void)l;(void)f;return false;}
static void pd_commit_dark_result(enum photodiode_channel c,struct app_pd_dark_result *d,bool p,bool l,bool f) {(void)c;(void)d;(void)p;(void)l;(void)f;}
static void pd_emit_dark_failed_warning(enum photodiode_channel c) {(void)c;}
static void pd_emit_adc_error_warning(enum photodiode_channel c,int rc) {(void)c;(void)rc;}
static const char *photodiode_channel_names[]={"yj","hk"};
struct coo_cmd_response {char payload[2048];size_t payload_len;};
struct coo_cmd_runtime_emit_args {int type,delivery;const char *suffix,*code,*msg,*context;struct coo_cmd_response *out;};
static struct coo_cmd_response throughput_sample_msg;
static void *command_runtime_get(void) {return NULL;}
static int coo_cmd_runtime_emit(void *r,const struct coo_cmd_runtime_emit_args *a) {(void)r;(void)a;return 0;}
static int coo_json_append(char *out,size_t n,size_t *off,const char *fmt,...) {
    va_list a;va_start(a,fmt);int k=vsnprintf(out+*off,n-*off,fmt,a);va_end(a);
    if(k<0 || (size_t)k>=n-*off) return -ENOSPC;
    *off+=(size_t)k;return 0;
}
static void sys_put_le64(uint64_t x,uint8_t *p) {for(int i=0;i<8;i++)p[i]=(uint8_t)(x>>(8*i));}
static void sys_put_le16(uint16_t x,uint8_t *p) {p[0]=x;p[1]=x>>8;}
static void channel_fiber_name(char *b,size_t n,enum photodiode_channel c,char f) {(void)c;(void)f;snprintf(b,n,"yj_m");}
static int pd_power_output(int c) {return c;}
static uint64_t housekeeping_power_on_time_s(int c) {(void)c;return 1;}
static uint64_t hispec_laser_current_on_time_s(int c) {(void)c;return 2;}
static const char *hispec_laser_name(int c) {(void)c;return "1028y";}
'''
source += block('photodiode.c','static void pd_window_reset_current(')
source += block('photodiode.c','static void pd_set_configurable_window_locked(')
source += block('photodiode.c','static void pd_update_channel(')
for marker in ['static int autolevel_adjust(', 'static void put_bytes(', 'static void put_u64(',
               'static void put_i16(', 'static void put_f64(', 'static void publish_sample(']:
    source += block('throughput_monitor.c',marker)
source += r'''
static void near(double a,double b) {assert(fabs(a-b)<=1e-10*MAX(fabs(b),1e-20));}
int main(void) {
    struct app_pd_channel_settings s={.responsivity_a_per_w=.93,.transimpedance_v_per_a=2e10,
        .dark={.mean_mv=10,.rms_mv=.5,.duration_ms=500}};
    near(pd_read_noise_mv(&s.dark),.5);
    near(pd_dark_mean_error_mv(&s.dark),.5/sqrt(10));
    s.dark.failed_samples=2;near(pd_dark_mean_error_mv(&s.dark),.5/sqrt(8));
    s.dark.failed_samples=10;assert(isnan(pd_dark_mean_error_mv(&s.dark)));
    s.dark.duration_ms=0;near(pd_dark_mean_error_mv(&s.dark),.5);
    near(pd_read_noise_mv(&s.dark),.0625/sqrt(12));
    near(photodiode_power_uw_from_mv(-100,&s),0);
    assert(isnan(photodiode_power_uw_from_mv(NAN,&s)));
    struct throughput_source_reference ref={.delivered_power_nw=2,.atten_tx=.001,.atten_db=30,.wavelength_nm=1028,.laser_output_power_uw=1000,.laser_current_ma=60};
    pd_update_channel(0,0,1600,&s,100,123000);
    near(pd_runtime[0].net_mv,90);assert(pd_runtime[0].t_ms==123002);
    clock_ms=155;pd_update_channel(0,-EIO,0,&s,150,123050);
    assert(pd_runtime[0].sample_ms==100);
    assert(pd_runtime[0].fixed_window.current.failed_samples==1);
    clock_ms=205;pd_update_channel(0,0,32000,&s,200,123100);
    assert(pd_runtime[0].sample_ms==200);
    assert(isnan(pd_runtime[0].net_err_mv));
    for(int i=0;i<10;i++) pd_update_channel(0,-EIO,0,&s,250+i*50,123150+i*50);
    assert(!pd_runtime[0].fixed_window.current.valid);

    /* An acquisition begun before reset never fills the new window, at either
     * ADC rate and at arbitrary phase relative to the 50 ms sampling timer. */
    for(int conversion_ms=4;conversion_ms<=16;conversion_ms+=12) {
        for(int phase=1;phase<50;phase++) {
            clock_ms=1000+phase;
            pd_set_configurable_window_locked(&pd_runtime[0],2);
            clock_ms=1050+conversion_ms;
            pd_update_channel(0,0,1600,&s,1000,123000);
            assert(pd_runtime[0].configurable_window.current.sample_length==0);
            pd_update_channel(0,0,1600,&s,1050,123050);
            assert(pd_runtime[0].configurable_window.current.sample_length==1);
            clock_ms=1100+conversion_ms;
            pd_update_channel(0,-EIO,0,&s,1100,123100);
            assert(pd_runtime[0].configurable_window.current.sample_length==2);
            assert(pd_runtime[0].configurable_window.current.failed_samples==1);
        }
    }
    struct throughput_state state={.has_laser=true,.autolevel=true,.pd_route_tx=.5,.laser_route_tx=.2,.source=ref};
    struct photodiode_channel_status pd={.mv=0,.net_mv=0};
    assert(autolevel_adjust(&state,&pd)==1);near(written_tx,.003); /* No startup gate. */
    pd.mv=1000;pd.net_mv=1000;assert(autolevel_adjust(&state,&pd)==0);
    atten_writes=laser_writes=0;
    pd.mv=2000;pd.net_mv=0;assert(autolevel_adjust(&state,&pd)==1); /* Raw bright wins. */
    if(TP_AUTOLEVEL_DIM_PRIORITY==TP_LASER_FIRST) {
        assert(laser_writes==1 && atten_writes==0);near(applied_current,25.3);
    } else {assert(laser_writes==0 && atten_writes==1);near(written_tx,.001/3);}
    fail_write=1;assert(autolevel_adjust(&state,&pd)==-EIO);fail_write=0;
    clamp_atten=true;attenuators[0].attenuation_db=state.source.atten_db;
    assert(autolevel_adjust(&state,&pd)==1);near(applied_current,25.3); /* Pair limit yields to laser. */
    clamp_atten=false;pd.mv=pd.net_mv=0;state.max_flux_ph_s=1;assert(autolevel_adjust(&state,&pd)==0);

    state.max_flux_ph_s=0;state.source=ref;state.source.laser_current_ma=8.1;
    pd.mv=pd.net_mv=1900;laser_writes=0;
    assert(autolevel_adjust(&state,&pd)==1 && laser_writes==0);near(written_tx,.001/3);
    state.source=ref;clamp_laser=true;applied_current=60;
    assert(autolevel_adjust(&state,&pd)==1);near(written_tx,.001/3);clamp_laser=false;
    state.source.atten_tx=1;state.source.atten_db=0;attenuators[0].attenuation_db=0;
    state.source.laser_current_ma=25.3;pd.mv=pd.net_mv=100;
    assert(autolevel_adjust(&state,&pd)==1);near(applied_current,59.9);
    state.source.laser_current_ma=60;
    assert(autolevel_adjust(&state,&pd)==0); /* Both actuators at their bright limit. */
    state.source.laser_current_ma=8.1;state.source.laser_output_power_uw=1;
    double flux=1e-6*1028e-9/(6.62607015e-34*299792458.0);
    state.max_flux_ph_s=flux*1.5; /* A 0.15 mA excess must not round above the cap. */
    assert(autolevel_adjust(&state,&pd)==0);
    state.max_flux_ph_s=flux*2.5;
    assert(autolevel_adjust(&state,&pd)==1);near(applied_current,8.2);

    pd=(struct photodiode_channel_status){.t_ms=123,.sample_ms=100,.raw=12,.mv=10,.net_mv=9,.net_err_mv=.01,
        .power_uw=.0002,.power_err_uw=1e-15};
    state.source=(struct throughput_source_reference){.delivered_power_nw=2,.delivered_power_err_nw=.1,
        .laser_output_power_uw=1000,.laser_output_power_err_uw=30,.atten_tx=.2,.wavelength_nm=1028};
    state.previous_source=state.source;state.source.delivered_power_nw=6;state.input_changed_ms=125;
    double reported_tp;
    state.binary=true;publish_sample(&state,&pd);
    memcpy(&reported_tp,throughput_sample_msg.payload+16,sizeof(reported_tp));near(reported_tp,.2);
    pd.sample_ms=150;publish_sample(&state,&pd);
    memcpy(&reported_tp,throughput_sample_msg.payload+16,sizeof(reported_tp));near(reported_tp,.4/6);
    /* A manually extinguished source still publishes PD data; its ratio is unknown. */
    state.source.delivered_power_nw=0;state.autolevel=false;
    publish_sample(&state,&pd);memcpy(&reported_tp,throughput_sample_msg.payload+16,sizeof(reported_tp));assert(isnan(reported_tp));
    memcpy(&reported_tp,throughput_sample_msg.payload+40,sizeof(reported_tp));near(reported_tp,.4);
    state.binary=false;publish_sample(&state,&pd);
    assert(strstr(throughput_sample_msg.payload,"\"tp\":null") && strstr(throughput_sample_msg.payload,"\"autolevel\":false"));
    state.autolevel=true;
    state.source=state.previous_source;state.input_changed_ms=0;
    for(int i=0;i<4;i++) {
        if(i==1) pd.mv=2000;
        if(i==2) {pd.mv=10;pd.net_mv=-1;pd.power_uw=photodiode_power_uw_from_mv(pd.net_mv,&s);}
        if(i==3) pd.power_uw=0;
        state.binary=false;publish_sample(&state,&pd);puts(throughput_sample_msg.payload);
        state.binary=true;publish_sample(&state,&pd);assert(throughput_sample_msg.payload_len==179);
        for(size_t j=0;j<throughput_sample_msg.payload_len;j++) printf("%02x",(uint8_t)throughput_sample_msg.payload[j]);
        puts("");
    }
    /* Passive light has a numerical PD power/error on either return fiber,
     * while both serializations explicitly leave the denominator unknown. */
    state.has_laser=state.autolevel=false;
    state.laser_route_tx=NAN;
    state.source=(struct throughput_source_reference){NAN,NAN,NAN,NAN,NAN,NAN,NAN,NAN};
    state.previous_source=state.source;
    pd.power_uw=.0002;pd.power_err_uw=.00001;pd.net_mv=9;pd.net_err_mv=.1;
    for(int fiber=0;fiber<2;fiber++) {
        state.pd_route_tx=fiber?.60:.98;
        state.binary=false;publish_sample(&state,&pd);puts(throughput_sample_msg.payload);
        state.binary=true;publish_sample(&state,&pd);
        for(size_t j=0;j<throughput_sample_msg.payload_len;j++) printf("%02x",(uint8_t)throughput_sample_msg.payload[j]);
        puts("");
    }

}
'''
with tempfile.TemporaryDirectory() as tmp:
    cfile,exe=Path(tmp)/'sample.c',Path(tmp)/'sample'
    cfile.write_text(source)
    for priority in (0,1):
        subprocess.run(['cc','-std=c11','-Wall','-Wextra','-Werror',f'-DTP_AUTOLEVEL_DIM_PRIORITY={priority}',str(cfile),'-lm','-o',str(exe)],check=True)
        sample_wire_lines=subprocess.check_output([str(exe)],text=True).splitlines()
    bad=subprocess.run(['cc','-std=c11','-DTP_AUTOLEVEL_DIM_PRIORITY=2',str(cfile),'-lm','-o',str(exe)],capture_output=True,text=True)
    assert bad.returncode and 'TP_AUTOLEVEL_DIM_PRIORITY must be' in bad.stderr
print('Fresh acquisition, ADC failures, dark uncertainty, and autolevel C checks passed')

# Laser estimator, validation and NVS record round trip; no hardware is involved.
source = r'''
#include <assert.h>
#include <errno.h>
#include <math.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include "laser_properties.h"
#define APP_LASER_CHANNEL_COUNT 6
#define PLANCK_J_S 6.62607015e-34
#define LIGHT_M_PER_S 299792458.0
#define K_FOREVER 0
#define K_NO_WAIT 0
#define MAX(a,b) ((a)>(b)?(a):(b))
static int laser_state_lock;
static bool laser_runtime_initialized=true;
static int k_mutex_lock(int *p,int t) { (void)p; (void)t; return 0; }
static void k_mutex_unlock(int *p) { (void)p; }
'''
for line in (ROOT/'app/src/lasers.h').read_text().splitlines():
    if line.startswith('#define HISPEC_LASER_DEFAULT_'):
        source += line + '\n'
source += quantize_source
source += block('lasers.h', 'enum hispec_laser_id {') + ';\n'
for file, names in {
    'lasers.h': ['hispec_laser_driver_profile', 'hispec_laser_flux_estimate'],
    'app_settings.h': ['app_laser_channel_settings', 'app_laser_settings'],
    'app_settings.c': ['app_nvs_laser_policy'],
}.items():
    for name in names:
        source += block(file, 'struct ' + name + ' {')
for name in ['default_laser_props', 'default_laser_expected_serial']:
    text = (ROOT/'app/src/app_settings.c').read_text()
    start = text.rfind('static const ', 0, text.index(name))
    source += text[start:text.index('};', start)+2]+'\n'
source += r'''
static struct app_laser_channel_settings laser_settings[HISPEC_LASER_COUNT];
static struct { bool valid; double current_ma, tec_temperature_c; } laser_output_estimate[HISPEC_LASER_COUNT];
struct app_settings_snapshot { struct app_laser_settings laser; };
static void laser_defaults(struct app_settings_snapshot *s) {
'''
source += block('app_settings.c', 'for (uint8_t i = 0U; i < APP_LASER_CHANNEL_COUNT; ++i)') + '}\n'
for marker in ['static bool float_is_valid(', 'static bool float_is_positive(',
               'static int validate_laser_settings(', 'static bool laser_driver_settings_differ(',
               'double hispec_laser_estimate_power_mw(', 'double hispec_laser_estimate_wavelength_nm(',
               'int laser_estimate_flux(']:
    source += block('lasers.c', marker)
for marker in ['static void laser_policy_from_settings(', 'static void app_nvs_apply_laser_policy(']:
    source += block('app_settings.c', marker)
source += r'''
#define APP_NVS_ID_LASER_POLICY_CH0 100
#define APP_NVS_ID_LASER_TOTAL_CH0 200
static void log_warning(const char *fmt,...) {(void)fmt;}
#define LOG_WRN(...) log_warning(__VA_ARGS__)
static bool app_nvs_ready=true;
static int app_nvs,policy_size;
static struct app_nvs_laser_policy nvs_policy;
static int nvs_read(int *fs,uint16_t id,void *data,size_t len) {
 (void)fs;
 if(id>=100 && id<106){memcpy(data,&nvs_policy,len);return policy_size;}
 assert(id>=200 && id<206);double total=1234;assert(len==sizeof(total));
 memcpy(data,&total,len);return len;
}
static int hispec_laser_validate_channel_settings(enum hispec_laser_id id,struct app_laser_channel_settings *s){
 return validate_laser_settings(&(struct hispec_laser_driver_profile){.properties=default_laser_props[id]},s);
}
#define snprintk snprintf
static const char *hispec_laser_name(enum hispec_laser_id id){(void)id;return "1028y";}
'''
for marker in ['static uint16_t laser_policy_nvs_id(', 'static uint16_t laser_total_nvs_id(',
               'static bool double_in_range(', 'static bool app_nvs_read_exact(', 'static void app_nvs_load_laser(']:
    source += block('app_settings.c',marker)
source += block('laser_command.c','static int laser_settings_payload(')
source += r'''
int main(void) {
    struct app_settings_snapshot defaults;
    laser_defaults(&defaults);
    double expected[] = {0.13505925,0.0267592,0.0267592,0.0267592,0.0267592,0.00913911};
    for (int i=0;i<HISPEC_LASER_COUNT;++i) {
        struct app_laser_channel_settings *s=&defaults.laser.channel[i], restored=*s;
        struct app_nvs_laser_policy stored;
        struct hispec_laser_driver_profile profile={.properties=default_laser_props[i]};
        assert(s->fractional_noise == 0.001);
        assert(fabs(s->constant_noise_mw-expected[i])<1e-12);
        assert(validate_laser_settings(&profile,s)==0);
        assert(s->min_autolevel_current_ma>s->properties.threshold_current_ma);
        s->fractional_noise=0.02+i*0.01;
        s->constant_noise_mw=0.01+i*0.01;
        assert(!laser_driver_settings_differ(s,&restored));
        laser_policy_from_settings(&stored,s);
        app_nvs_apply_laser_policy(&restored,&stored);
        assert(restored.fractional_noise==s->fractional_noise);
        assert(restored.constant_noise_mw==s->constant_noise_mw);
        assert(restored.min_autolevel_current_ma==s->min_autolevel_current_ma);
        char payload[1024];
        assert(!laser_settings_payload(payload,sizeof(payload),i,s));
        assert(strstr(payload,"\"min_autolevel_current_ma\":"));
        assert(laser_settings_payload(payload,32,i,s)==-ENOSPC);
        laser_settings[i]=*s;
        laser_output_estimate[i].valid=true;
        laser_output_estimate[i].current_ma=s->properties.max_current_ma;
        laser_output_estimate[i].tec_temperature_c=s->properties.operating_temp_c;
        struct hispec_laser_flux_estimate estimate;
        assert(laser_estimate_flux(i,&estimate)==0);
        double power=(s->properties.max_current_ma-s->properties.threshold_current_ma)*s->properties.efficiency_mw_per_ma;
        assert(fabs(estimate.power_mw-power)<1e-12);
        double nominal_flux=estimate.flux_ph_s;
        laser_output_estimate[i].valid=false; /* Operational faults do not gate arithmetic. */
        assert(laser_estimate_flux(i,&estimate)==0 && estimate.flux_ph_s==nominal_flux);
        assert(fabs(estimate.power_err_mw-hypot(power*s->fractional_noise,s->constant_noise_mw))<1e-12);
        laser_settings[i].constant_noise_mw=0;
        assert(laser_estimate_flux(i,&estimate)==0);
        assert(fabs(estimate.power_err_mw-power*s->fractional_noise)<1e-12);
        laser_settings[i].fractional_noise=0;
        laser_settings[i].constant_noise_mw=s->constant_noise_mw;
        assert(laser_estimate_flux(i,&estimate)==0);
        assert(estimate.power_err_mw==s->constant_noise_mw && estimate.flux_ph_s==nominal_flux);
        restored.fractional_noise=-0.01;
        assert(validate_laser_settings(&profile,&restored)==-ERANGE);
        restored=*s;
        restored.properties.threshold_current_ma+=1.04;
        assert(validate_laser_settings(&profile,&restored)==0);
        assert(restored.min_autolevel_current_ma>=restored.properties.threshold_current_ma+.1);
        assert(fabs(restored.min_autolevel_current_ma*10-round(restored.min_autolevel_current_ma*10))<1e-12);
        restored.min_autolevel_current_ma=restored.properties.nominal_current_ma+1;
        assert(validate_laser_settings(&profile,&restored)==-ERANGE);
        restored=*s;restored.min_autolevel_current_ma=NAN;
        assert(validate_laser_settings(&profile,&restored)==-ERANGE);
        restored=*s;
        restored.fractional_noise=NAN;
        assert(validate_laser_settings(&profile,&restored)==-ERANGE);
        restored.fractional_noise=0;
        restored.constant_noise_mw=INFINITY;
        assert(validate_laser_settings(&profile,&restored)==-ERANGE);
    }
    /* Old-size policy rejection preserves defaults and loads the separate total. */
    struct app_settings_snapshot loaded;
    laser_policy_from_settings(&nvs_policy,&defaults.laser.channel[0]);
    nvs_policy.min_autolevel_current_ma=20;
    policy_size=sizeof(nvs_policy)-sizeof(double);
    laser_defaults(&loaded);app_nvs_load_laser(&loaded);
    for(int i=0;i<HISPEC_LASER_COUNT;i++){
      assert(loaded.laser.channel[i].min_autolevel_current_ma==defaults.laser.channel[i].min_autolevel_current_ma);
      assert(loaded.laser.channel[i].total_emitting_s==1234);
    }
    policy_size=sizeof(nvs_policy);app_nvs_load_laser(&loaded);
    assert(loaded.laser.channel[0].min_autolevel_current_ma==20 && loaded.laser.channel[0].total_emitting_s==1234);
    puts("laser uncertainty C regressions passed");
}
'''
with tempfile.TemporaryDirectory() as tmp:
    cfile, exe = Path(tmp)/'laser.c', Path(tmp)/'laser'
    cfile.write_text(source)
    subprocess.run(['cc','-std=c11','-Wall','-Wextra','-Werror','-I',str(ROOT/'app/src'),str(cfile),'-lm','-o',str(exe)],check=True)
    subprocess.run([str(exe)],check=True)

# Host command fields and telemetry remain usable in both documented formats.
import sys
import json
import dataclasses
sys.path.insert(0, str(ROOT/'tools'))
import hispec_fibpcb as host
client = object.__new__(host.HispecFibPcb)
client._request_ok = lambda command, payload: (command, payload)
for name in host.LASER_NAMES:
    command, payload = client.laser_settings(name, fractional_noise=0.03, constant_noise_mw=0.1, persist=True)
    assert command == 'laser/settings' and payload['settings'] == {'fractional_noise':0.03, 'constant_noise_mw':0.1}
    assert payload['persist']
for key in ('fractional_noise','constant_noise_mw','min_autolevel_current_ma'):
    for value in (-1, float('nan'), float('inf')):
        try:
            client.laser_settings(host.LASER_NAMES[0], **{key:value})
        except host.HispecFibError:
            pass
        else:
            raise AssertionError(f'accepted invalid {key}: {value}')
settings = {field.name:0 for field in dataclasses.fields(host.LaserSettings)}
settings.update(model='test',expected_serial=123,tec_pid={'p':0,'i':0,'d':0},
                operating_temp_range_c=[17,38],fractional_noise=0.03,constant_noise_mw=0.435675,
                min_autolevel_current_ma=14.6)
client._request_json = lambda command, payload: {'name':payload['name'], 'settings':settings}
result = client.laser_settings(host.LASER_NAMES[0])
assert result.fractional_noise == 0.03 and result.constant_noise_mw == 0.435675
assert result.min_autolevel_current_ma == 14.6
assert client.laser_settings('1028y',min_autolevel_current_ma=15)[1]['settings']=={'min_autolevel_current_ma':15}
for i in range(0,len(sample_wire_lines),2):
    jsample=host.decode_throughput_payload(sample_wire_lines[i])
    binary=bytes.fromhex(sample_wire_lines[i+1]); sample=host.decode_throughput_payload(binary)
    for name in host._THROUGHPUT_FLOAT_FIELDS:
        a,b=getattr(sample,name),getattr(jsample,name)
        assert (a!=a and b!=b) or abs(a-b)<=1e-10*max(abs(a),1e-20), name
    assert sample.flags==jsample.flags and sample.autolevel==jsample.autolevel
    if i>=8:
        assert not sample.autolevel and jsample.laser=='none'
        assert sample.tp!=sample.tp and sample.tp_err!=sample.tp_err
        assert sample.delivered_power_nw!=sample.delivered_power_nw and sample.wavelength_nm!=sample.wavelength_nm
        assert abs(sample.pd_power_nw-.2/sample.pd_route_tx)<1e-10
        assert abs(sample.pd_power_err_nw-.01/sample.pd_route_tx)<1e-10
    else:
        assert sample.autolevel
    assert sample.t_ms==123 and sample.pd_raw==12
    if i==0:
        assert sample.pd_power_err_nw==2e-12 and abs(sample.tp-.2)<1e-15
    if i==2:
        assert sample.flags==('overrange',) and sample.tp_err!=sample.tp_err
    if i==4: assert sample.tp==0 and sample.tp_err>0
    if i==6: assert sample.tp==0 and sample.tp_err>0
# Use the finite first acquisition for subsequent rendering/collector checks.
binary=bytes.fromhex(sample_wire_lines[1]); sample=host.decode_throughput_payload(binary)
print('Python laser settings and JSON/binary telemetry checks passed')

# Current firmware response shapes: retain nullable timers and laser diagnostics.
drift_client = object.__new__(host.HispecFibPcb)
responses = {
    'status': dict(fw='test', boots=1, board='tib', board_ok=True, mems_switches=8,
                   relay_err=0, amb_c=None, pd_on_s=0, laserbank_on_s=0,
                   lastcmd=dict(name='', src='unknown', t_ms=0), lasers={}),
    'laser': dict(name='1028y', powered=True, ready=False, blocked_reason=None,
                  tec_on_s=None, emit_on_s=None, emit_total_s=None, temp_c=None,
                  i_mA=None, value=None, power_mw=None, nominal_nm=1028.0,
                  tuned_nm=None, tune_nm=0.0, tec_ma=None, diode_v=None,
                  tec_v=None, off_in_s=None, oc_fault=False),
    'laser/status': dict(name='1028y', read_rc=0, powered=True, dev_id=1,
                         serial=123, expected_serial=123, serial_ok=True,
                         raw_state=0, raw_lock=0, raw_tec=0, blocking_lock=0,
                         blocked_reason=None, op_started=False, ready=False,
                         curr_set_internal=True, enable_internal=True, ext_ntc_denied=True,
                         interlock_denied=True, interlock=False, ext_ntc_interlock=False,
                         ld_overcurrent=False, ld_overheat=False, tec_started=False,
                         tec_set_internal=True, tec_enable_internal=True, tec_error=False,
                         tec_selfheat=False, i_mA=None, curr_meas_ma=None, curr_min_ma=None,
                         curr_max_ma=None, drv_max_ma=None, ocp_ma=None, curr_cal_pct=None,
                         diode_v=None, tec_temp_set_c=None, tec_temp_c=None, pcb_temp_c=None,
                         tec_ma=None, tec_curr_lim_a=None, tec_v=None, pid=[1,2,3], ntc_t_coeff=None),
    'laser/bankheater': dict(mode='auto', auto_state='waiting_for_temps', heater_on=False,
                             bank_power=True, ambient_c=None, idle_tec_temps=0,
                             idle_tec_avg_c=None, last_error=0, poll_age_s=None),
    'time': dict(utc=12345, uptime_s=12),
}
drift_client._request = lambda key, payload=None: responses[key]
for remaining in (None, 0, 31):
    ready = remaining is not None
    responses['status']['lasers']['1028y'] = dict(
        power_mw=None, ready=ready, tec_on_s=remaining, off_in_s=remaining)
    summary = drift_client.status(lasers=True).lasers[0].value
    assert summary.ready is ready and summary.off_in_s == remaining and summary.tec_on_s == remaining
for ready, reason, blocking in ((True, None, 0), (False, 'driver_identity_mismatch', 0x12)):
    responses['laser'].update(ready=ready, blocked_reason=reason)
    responses['laser/status'].update(ready=ready, blocked_reason=reason, blocking_lock=blocking)
    compact, engineering = drift_client.laser('1028y'), drift_client.laser_status('1028y')
    assert compact.ready is ready and compact.blocked_reason == reason
    assert engineering.ready is ready and engineering.blocked_reason == reason
    assert engineering.blocking_lock == blocking and engineering.pid == (1,2,3)
for ambient, average in ((None, None), (21.25, 18.75)):
    responses['laser/bankheater'].update(ambient_c=ambient, idle_tec_avg_c=average)
    heater = drift_client.laser_bankheater()
    assert heater.ambient_c == ambient and heater.idle_tec_avg_c == average
from typing import get_type_hints
heater_types = get_type_hints(host.LaserBankHeater)
assert heater_types['ambient_c'] == heater_types['idle_tec_avg_c'] == float | None
assert drift_client.time().uptime_s == 12
for payload in ('{}', '{"uptime_s":12}'):
    try:
        host.decode_warning(payload)
    except KeyError:
        pass
    else:
        raise AssertionError('warning without uptime_ms was accepted')
assert host.decode_warning('{"uptime_ms":0}').uptime_ms == 0
print('Python nullable status, laser diagnostics, heater types, and warning contract checks passed')

# Model-uncertainty and accepted-fit installation with a fixed zero-sensitivity
# model. Exercise replacement/rollback here; the real model is checked below.
source = r'''
#include <assert.h>
#include <errno.h>
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#define APP_ATTENUATOR_PHYSICAL_COUNT 2
#define APP_ATTENUATOR_CHANNEL_COUNT 6
#define MAX_PAYLOAD_LEN 2048
#define COO_JSON_EXTRACT_OK 0
#define COO_JSON_EXTRACT_ERR -1
#define COO_JSON_EXTRACT_MISSING 1
#define ARRAY_SIZE(a) (sizeof(a)/sizeof((a)[0]))
#define ATTENUATOR_DB_EPSILON 1e-6
struct dac_channel_cfg {int channel_id;};
'''
source += mutex_harness
for line in (ROOT/'app/src/attenuator.h').read_text().splitlines():
    if line.startswith(('#define ATTENUATOR_', '#define FVOA_DEFAULT_')):
        source += line+'\n'
for file, names in {
    'attenuator.h': ['attenuator_model_coeffs','atten_model_eval','attenuator_dac_cfg','attenuator_status',
                    'attenuator_transmission_estimate','attenuator'],
    'app_settings.h': ['app_attenuator_physical_settings','app_attenuator_channel_settings'],
    'attenuator_calibration.h': ['attenuator_calibration_fit_metrics'],
}.items():
    for name in names:
        source += block(file,'struct '+name+' {')
source += r'''
static struct attenuator attenuators[6];
static struct {int attenuator_index; bool persistent; struct attenuator_calibration_fit_metrics fit[2];} cal;
static struct app_attenuator_channel_settings saved;
static int saves;
static bool fail_write;
static double sample_tx=0.001;
#define K_FOREVER 0
static struct k_mutex attenuator_io_lock,attenuator_state_lock;
static void app_settings_update_attenuator_channel(int i,const struct app_attenuator_channel_settings *s,bool persist)
{ (void)i; assert(persist); saved=*s; ++saves; }
static bool attenuator_read_physical(struct attenuator_dac_cfg *d, const struct attenuator_model_coeffs *c)
{ (void)c; d->valid=true; d->attenuation_db=d->voltage; return true; }
static bool attenuator_set_db_staged(struct attenuator *a,double db,bool calibrated_only) {
    assert(!calibrated_only); (void)db; io_barrier();
    if(fail_write) {a->dac_cfg1.voltage=42;a->dac_cfg2.valid=false;return false;}
    return true;
}
static double attenuator_model_floor_linear(const struct attenuator_model_coeffs *c)
{ return pow(10,-c->max_atten_db/10); }
static double attenuator_model_voltage_to_db(const struct attenuator_model_coeffs *c,float mv)
{ (void)c; return mv; }
static bool atten_model_eval(const struct attenuator_model_coeffs *c,float mv,struct atten_model_eval *out)
{ (void)c; *out=(struct atten_model_eval){.db=mv,.tx=pow(10,-mv/10)}; return true; }
/* JSON extraction stubs select absent/explicit RMS; the parser's replacement
 * semantics and validation, not the shared JSON library, are under test here. */
static int rms_status=COO_JSON_EXTRACT_MISSING;
static double parsed_rms;
static int coo_json_extract_object(const char *j,const char *key,char *out,size_t n)
{ (void)j; (void)key; (void)n; out[0]=0; return COO_JSON_EXTRACT_OK; }
static int coo_json_validate_top_level_keys(const char *j,const char *keys,char *bad,size_t n)
{ (void)j; (void)keys; (void)bad; (void)n; return 0; }
static int coo_json_extract_double(const char *j,const char *key,double *out) {
    (void)j;
    if (!strcmp(key,"rms_db")) {if (rms_status==0) *out=parsed_rms; return rms_status;}
    if (!strcmp(key,"fvoa_50pct_mv")) *out=2500;
    if (!strcmp(key,"slope_inv_fvoa_mv")) *out=0.002;
    if (!strcmp(key,"max_atten_db")) *out=55;
    if (!strcmp(key,"max_calibrated_db")) *out=50;
    if (!strcmp(key,"gain")) *out=1.533;
    return COO_JSON_EXTRACT_OK;
}
static int coo_json_extract_double_array(const char *j,const char *key,double *out,size_t n,size_t *len)
{ (void)j;(void)key;(void)out;(void)n;(void)len;return COO_JSON_EXTRACT_MISSING; }
'''
for marker in ['bool atten_model_db_sigma(', 'void attenuator_snapshot(', 'static void attenuator_commit(', 'static bool attenuator_model_coeff_valid(', 'bool attenuator_model_coefficients_valid(',
               'bool attenuator_estimate_transmission(', 'int attenuator_apply_coefficients_preserve_db(']:
    source += block('attenuator.c',marker)
source += block('app_settings.c','static bool attenuator_channel_valid(')
source += block('attenuator_calibration.c','static int apply_fit_to_settings_locked(')
source += block('attenuator_command.c','static int parse_attenuator_coeff_object(')
source += r'''
static void *replace_coefficients(void *arg) {
    struct attenuator_model_coeffs *c=arg;
    assert(attenuator_apply_coefficients_preserve_db(&attenuators[0],c)==0);
    return NULL;
}
int main(void) {
    init_mutex(&attenuator_io_lock);init_mutex(&attenuator_state_lock);
    struct attenuator *a=&attenuators[0];
    a->coeff1=(struct attenuator_model_coeffs){.fvoa_50pct_mv=2500,.slope_inv_fvoa_mv=0.002,
        .max_atten_db=55,.max_calibrated_db=50,.gain=1.533,.rms_db=ATTENUATOR_DEFAULT_RMS_DB};
    a->coeff2=a->coeff1;
    struct attenuator_transmission_estimate out;
    for(int i=0;i<3;++i) {
        sample_tx=pow(10,-i*3);
        a->dac_cfg1.valid=a->dac_cfg2.valid=true;
        a->dac_cfg1.voltage=a->dac_cfg2.voltage=i*15;
        assert(attenuator_estimate_transmission(a,&out));
        assert(out.linear==sample_tx && out.attenuation_db==i*30 && out.voltage1==i*15);
        assert(fabs(out.linear_err/out.linear-log(10)/10*hypot(2,2))<1e-12);
    }
    cal.persistent=true;
    for(int i=0;i<2;++i) cal.fit[i]=(struct attenuator_calibration_fit_metrics){
        .accepted=true,.fvoa_50pct_mv=2600+i,.slope_inv_fvoa_mv=0.003,.max_atten_db=50,.max_calibrated_db=40,
        .rms_db=0.75+i,.correction_coeff={0,0,0,0,0.25f,-0.5f}};
    assert(apply_fit_to_settings_locked()==0 && saves==1);
    assert(saved.physical[0].rms_db==0.75 && saved.physical[1].rms_db==1.75);
    assert(saved.physical[0].max_calibrated_db==40 && a->coeff2.max_calibrated_db==40);
    assert(a->coeff1.rms_db==0.75 && a->coeff2.rms_db==1.75);
    assert(saved.physical[0].correction_coeff[4]==0.25f && saved.physical[1].correction_coeff[5]==-0.5f);
    assert(a->coeff1.correction_coeff[5]==-0.5f && a->coeff2.correction_coeff[4]==0.25f);
    assert(attenuator_channel_valid(&saved));
    assert(attenuator_estimate_transmission(a,&out));
    assert(fabs(out.linear_err/out.linear-log(10)/10*hypot(0.75,1.75))<1e-12);
    cal.fit[0].accepted=false; cal.fit[0].rms_db=99;
    assert(apply_fit_to_settings_locked()==-EINVAL && saves==1 && a->coeff1.rms_db==0.75);
    cal.fit[0].accepted=true; fail_write=true;
    assert(apply_fit_to_settings_locked()==-EIO && saves==1 && a->coeff1.rms_db==0.75);
    fail_write=false;
    assert(parse_attenuator_coeff_object("{}","dac1",&a->coeff1)==0);
    assert(a->coeff1.rms_db==2); /* Do not inherit the previous 0.75 dB fit. */
    rms_status=COO_JSON_EXTRACT_OK; parsed_rms=0;
    assert(parse_attenuator_coeff_object("{}","dac1",&a->coeff1)==0 && a->coeff1.rms_db==0);
    parsed_rms=-1;
    assert(parse_attenuator_coeff_object("{}","dac1",&a->coeff1)==-EINVAL);
    parsed_rms=NAN;
    assert(parse_attenuator_coeff_object("{}","dac1",&a->coeff1)==-EINVAL);
    saved.physical[0].rms_db=INFINITY;
    assert(!attenuator_channel_valid(&saved));
    /* Block the writer after it staged new coefficients. Estimator must see
     * the old complete pair and return before I/O is released. */
    a->coeff1.rms_db=.5;
    struct attenuator_model_coeffs next[2]={a->coeff1,a->coeff2};
    next[0].rms_db=1.25;next[1].rms_db=2.5;
    a->dac_cfg1.valid=a->dac_cfg2.valid=true;
    double old_rms=a->coeff1.rms_db;
    pthread_t writer;alarm(5);atomic_store(&block_io,true);
    assert(!pthread_create(&writer,NULL,replace_coefficients,next));wait_for_io();
    assert(attenuator_estimate_transmission(a,&out));
    assert(fabs(out.linear_err/out.linear-log(10)/10*hypot(old_rms,a->coeff2.rms_db))<1e-12);
    atomic_store(&release_io,true);assert(!pthread_join(writer,NULL));alarm(0);
    assert(attenuator_estimate_transmission(a,&out));
    assert(fabs(out.linear_err/out.linear-log(10)/10*hypot(1.25,2.5))<1e-12);
    puts("attenuator uncertainty and concurrent publication C regressions passed");
}
'''
with tempfile.TemporaryDirectory() as tmp:
    cfile, exe = Path(tmp)/'atten.c', Path(tmp)/'atten'
    cfile.write_text(source)
    subprocess.run(['cc','-pthread','-D_POSIX_C_SOURCE=200809L','-std=c11','-Wall','-Wextra','-Werror',str(cfile),'-lm','-o',str(exe)],check=True)
    subprocess.run([str(exe)],check=True)

coeff = {'fvoa_50pct_mv':2500,'slope_inv_fvoa_mv':0.002,'max_atten_db':55,'max_calibrated_db':50,
         'gain':1.533,'correction_coeff':[0]*6,'rms_db':0.75}
parsed = host._decode_atten_physical_coeff(coeff,'dac1')
assert parsed.rms_db == 0.75
assert host._atten_physical_coeff_payload('dac1',parsed)['rms_db'] == 0.75
assert host._atten_coeff_tuple('dac1',parsed) == host._atten_coeff_tuple('dac1',{**coeff,'rms_db':2})
for value in (0,2,0.75):
    assert host._atten_physical_coeff_payload('dac1',{**coeff,'rms_db':value})['rms_db'] == value
for value in (-1,float('nan'),float('inf')):
    try:
        host._atten_physical_coeff_payload('dac1',{**coeff,'rms_db':value})
    except host.HispecFibError:
        pass
    else:
        raise AssertionError('accepted invalid attenuator RMS')
without_rms = {key:value for key,value in coeff.items() if key != 'rms_db'}
assert 'rms_db' not in host._atten_physical_coeff_payload('dac1',without_rms)
assert 'rms_db' not in host._atten_physical_coeff_payload('dac1',(2500,0.002,55,1.533,50))
assert parsed.max_calibrated_db == 50
try:
    host._atten_physical_coeff_payload('dac1', {k:v for k,v in coeff.items() if k != 'max_calibrated_db'})
except host.HispecFibError: pass
else: raise AssertionError('accepted missing calibrated limit')
print('Python attenuator RMS checks passed')

# Real FVOA evaluator/analytic derivatives and estimator versus the Python
# finite difference. Include the scope example, distinct gains, and plateaus.
import io
import numpy as np

noise_coeff = host.AttenuatorCoeff(
    host.AttenuatorPhysicalCoeff(3144.95, .00303104, 48.36, 35.0, 1.533,
                                tuple(float(np.float32(x)) for x in (.12,-.03,.01,0,0,0)), .75),
    host.AttenuatorPhysicalCoeff(3456.12, .00247498, 61.95, 61.95, 1.8, (0.,)*6, 1.25),
)
means, scope_rms = (3824., 3942.), (7.2, 9.3)
noise = host.attenuator_noise(noise_coeff, mean_fvoa_mv=means, noise_rms_mv=scope_rms)
assert list(noise.component) == ['dac1', 'dac2', 'pair']
np.testing.assert_allclose(noise.tx[2], noise.tx[0]*noise.tx[1], rtol=1e-14)
for i,c in enumerate((noise_coeff.dac1,noise_coeff.dac2)):
    # Reproduce the original notebook calculation with the actual scope inputs.
    a_minus,a0,a_plus = host._atten_db_from_coeff(
        host._atten_coeff_tuple('scope',c), (means[i]+np.array([-.1,0,.1]))/c.gain)
    np.testing.assert_allclose(noise.electrical_rms_db[i], abs(a_plus-a_minus)/.2*scope_rms[i])
    np.testing.assert_allclose(noise.tx[i], 10**(-a0/10))
np.testing.assert_allclose(noise.model_sigma_db, [.75,1.25,np.hypot(.75,1.25)])
np.testing.assert_allclose(noise.electrical_rms_db[2], np.hypot(*noise.electrical_rms_db[:2]))
np.testing.assert_allclose(noise.total_sigma_db, np.hypot(noise.model_sigma_db,noise.electrical_rms_db))
for field in ('model_sigma','electrical_rms','total_sigma'):
    np.testing.assert_allclose(noise[field+'_tx'], noise.tx*np.log(10)/10*noise[field+'_db'])
np.testing.assert_allclose(noise.electrical_rms_pct, 100*noise.electrical_rms_tx/noise.tx)
zero = host.attenuator_noise(noise_coeff,mean_fvoa_mv=means,noise_rms_mv=(0,0))
np.testing.assert_array_equal(zero.electrical_rms_db, 0)
np.testing.assert_array_equal(zero.total_sigma_tx, zero.model_sigma_tx)
double = host.attenuator_noise(noise_coeff,mean_fvoa_mv=means,noise_rms_mv=np.array(scope_rms)*2)
np.testing.assert_allclose(double.electrical_rms_tx, 2*noise.electrical_rms_tx)
np.testing.assert_array_equal(double.model_sigma_tx, noise.model_sigma_tx)
different_model = dataclasses.replace(noise_coeff,dac1=dataclasses.replace(noise_coeff.dac1,rms_db=1.5))
changed = host.attenuator_noise(different_model,mean_fvoa_mv=means,noise_rms_mv=scope_rms)
np.testing.assert_array_equal(changed.electrical_rms_tx,noise.electrical_rms_tx)
assert changed.model_sigma_tx[0] == 2*noise.model_sigma_tx[0]
assert changed.total_sigma_tx[2] > noise.total_sigma_tx[2]
different_gain = dataclasses.replace(noise_coeff,dac1=dataclasses.replace(noise_coeff.dac1,gain=2.0))
changed = host.attenuator_noise(different_gain,mean_fvoa_mv=means,noise_rms_mv=scope_rms)
np.testing.assert_allclose(changed.electrical_rms_tx,noise.electrical_rms_tx,rtol=1e-9)
for mean,rms in [((1,),scope_rms),(means,7),((1,2,3),scope_rms),((np.nan,1),scope_rms),
                 (means,(np.inf,0)),((-1,1),scope_rms),(means,(1,-1)),(('bad',1),scope_rms)]:
    try: host.attenuator_noise(noise_coeff,mean_fvoa_mv=mean,noise_rms_mv=rms)
    except host.HispecFibError: pass
    else: raise AssertionError('accepted invalid scope inputs')
for bad in (None, host.AttenuatorCoeff(None,noise_coeff.dac2), *(
        dataclasses.replace(noise_coeff,dac1=dataclasses.replace(noise_coeff.dac1,**fields))
        for fields in ({'gain':0},{'slope_inv_fvoa_mv':-1},{'rms_db':-1},{'rms_db':np.nan},
                       {'correction_coeff':(0,)},{'correction_coeff':(np.nan,)*6},
                       {'max_atten_db':10000},{'fvoa_50pct_mv':-10000}))):
    try: host.attenuator_noise(bad,mean_fvoa_mv=means,noise_rms_mv=scope_rms)
    except host.HispecFibError: pass
    else: raise AssertionError('accepted unusable coefficients')

model_source = r'''
#include <assert.h>
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#define CLAMP(x,lo,hi) fmin(fmax((x),(lo)),(hi))
#define ZSL_ERF erf
#define ZSL_EXP exp
#define ZSL_LOG10 log10
typedef double zsl_real_t;
struct dac_channel_cfg {int channel_id;};
'''
for file,prefixes in [('attenuator.h',('#define ATTENUATOR_', '#define FVOA_DEFAULT_')),
                      ('attenuator.c',('#define MODEL_', '#define ATTENUATOR_DB_PER_NEPER'))]:
    for line in (ROOT/'app/src'/file).read_text().splitlines():
        if line.startswith(prefixes): model_source += line+'\n'
model_source += '#ifndef M_PI\n#define M_PI 3.14159265358979323846\n#endif\n'
for name in ('attenuator_model_coeffs','atten_model_eval','attenuator_dac_cfg',
             'attenuator_transmission_estimate','attenuator'):
    model_source += block('attenuator.h','struct '+name+' {')
model_source += 'static void attenuator_snapshot(const struct attenuator *a,struct attenuator *b) {*b=*a;}\n'
for marker in ('static double attenuator_model_delta_to_raw_linear(',
               'static double attenuator_model_voltage_to_delta(',
               'static double attenuator_model_floor_linear(',
               'static bool attenuator_model_correction_active(',
               'static double attenuator_model_chebyshev_db(',
               'static double attenuator_model_correction_db(',
               'bool atten_model_eval(', 'bool atten_model_db_sigma('):
    model_source += block('attenuator.c',marker)
model_main = r'''
int main(void) {
 struct attenuator a={
  .coeff1={.fvoa_50pct_mv=3144.95,.slope_inv_fvoa_mv=.00303104,.max_atten_db=48.36,.max_calibrated_db=35,
           .gain=1.533,.rms_db=.75,.correction_coeff={.12,-.03,.01,0,0,0}},
  .coeff2={.fvoa_50pct_mv=3456.12,.slope_inv_fvoa_mv=.00247498,.max_atten_db=61.95,
           .gain=1.8,.rms_db=1.25},
  .dac_cfg1={.valid=true},.dac_cfg2={.valid=true}};
 double mean1[]={0,3144.95,3824,4100,5000},mean2[]={0,3456.12,3942,4300,5500};
 for(int i=0;i<5;i++) {
  a.dac_cfg1.voltage=mean1[i]/a.coeff1.gain;a.dac_cfg2.voltage=mean2[i]/a.coeff2.gain;
  struct atten_model_eval e1,e2;struct attenuator_transmission_estimate out;
  assert(atten_model_eval(&a.coeff1,a.dac_cfg1.voltage,&e1));
  assert(atten_model_eval(&a.coeff2,a.dac_cfg2.voltage,&e2));
  assert(attenuator_estimate_transmission(&a,&out));
  if(i==0 || i==4) assert(e1.d_db_d_voltage_mv==0 && e2.d_db_d_voltage_mv==0);
  printf("%.17g %.17g %.17g %.17g %.17g %.17g %.17g %.17g %.17g %.17g %.17g\n",
   a.dac_cfg1.voltage*a.coeff1.gain,a.dac_cfg2.voltage*a.coeff2.gain,
   e1.db,e1.tx,e1.d_db_d_voltage_mv,e2.db,e2.tx,e2.d_db_d_voltage_mv,
   out.attenuation_db,out.linear,out.linear_err);
 }
 struct attenuator_transmission_estimate out;
 assert(!attenuator_estimate_transmission(NULL,&out));
 assert(!attenuator_estimate_transmission(&a,NULL));
 a.dac_cfg1.valid=false;assert(!attenuator_estimate_transmission(&a,&out));a.dac_cfg1.valid=true;
 a.coeff1.gain=0;assert(!attenuator_estimate_transmission(&a,&out));a.coeff1.gain=1.533;
 a.coeff1.rms_db=NAN;assert(!attenuator_estimate_transmission(&a,&out));
}
'''
firmware_noise = float(next(line.split()[-1] for line in (ROOT/'app/src/attenuator.h').read_text().splitlines()
                            if line.startswith('#define ATTENUATOR_FVOA_NOISE_RMS_MV ')))
with tempfile.TemporaryDirectory() as tmp:
    cfile,exe = Path(tmp)/'noise.c',Path(tmp)/'noise'
    for rms in (firmware_noise,0.0):
        cfile.write_text(model_source+'\n#undef ATTENUATOR_FVOA_NOISE_RMS_MV\n'
                         +f'#define ATTENUATOR_FVOA_NOISE_RMS_MV {rms}\n'
                         +block('attenuator.c','bool attenuator_estimate_transmission(')+model_main)
        subprocess.run(['cc','-std=c11','-Wall','-Wextra','-Werror',str(cfile),'-lm','-o',str(exe)],check=True)
        rows=np.loadtxt(io.StringIO(subprocess.check_output([str(exe)],text=True)))
        for v1,v2,db1,tx1,slope1,db2,tx2,slope2,db,tx,err in rows:
            result=host.attenuator_noise(noise_coeff,mean_fvoa_mv=(v1,v2),noise_rms_mv=(rms,rms))
            np.testing.assert_allclose(result.db,[db1,db2,db],rtol=1e-9,atol=1e-12)
            np.testing.assert_allclose(result.tx,[tx1,tx2,tx],rtol=1e-9)
            np.testing.assert_allclose(result.electrical_rms_db[:2],
                np.abs([slope1/noise_coeff.dac1.gain,slope2/noise_coeff.dac2.gain])*rms,rtol=2e-6,atol=1e-10)
            np.testing.assert_allclose(result.total_sigma_tx[2],err,rtol=2e-6)
print('FVOA scope inputs, separate uncertainties, analytic C/Python parity, and zero-noise checks passed')

# Forward/inverse agreement includes both sides of the calibrated endpoint.
inverse_source = model_source + "\n#define ZSL_FMA fma\n#define ZSL_LOG log\n#define ZSL_ABS fabs\n#define ATTENUATOR_DB_EPSILON 1e-6\n#define ATTENUATOR_MODEL_INVERSE_STEPS 24U\n"
probability = (ROOT.parent/'modules/lib/zscilib/src/probability.c').read_text()
start = probability.index('zsl_real_t zsl_prob_erf_inv(')
inverse_source += probability[start:probability.index('\n}', start)+2]+'\n'
for marker in ('static double attenuator_model_raw_linear(', 'static double attenuator_model_open_linear(',
               'double attenuator_model_voltage_to_db(', 'bool attenuator_model_db_to_voltage(',
               'bool attenuator_estimate_transmission('):
    inverse_source += block('attenuator.c', marker)
curve_validation_source = inverse_source
inverse_source += r'''
int main(void){
 struct attenuator_model_coeffs c={.fvoa_50pct_mv=3000,.slope_inv_fvoa_mv=.0025,
  .max_atten_db=80,.max_calibrated_db=55,.gain=1.533,.correction_coeff={4}};
 double targets[]={1,20,54.99,55,55.01,60,70};
 for(unsigned i=0;i<sizeof(targets)/sizeof(targets[0]);i++) {
  float mv;assert(attenuator_model_db_to_voltage(&c,targets[i],&mv));
  assert(fabs(attenuator_model_voltage_to_db(&c,mv)-targets[i])<1e-4);
 }
 c.correction_coeff[0]=0;
 for(unsigned i=0;i<sizeof(targets)/sizeof(targets[0]);i++) {
  float mv;assert(attenuator_model_db_to_voltage(&c,targets[i],&mv));
  assert(fabs(attenuator_model_voltage_to_db(&c,mv)-targets[i])<1e-3);
 }
}
'''
with tempfile.TemporaryDirectory() as tmp:
    cfile,exe=Path(tmp)/'inverse.c',Path(tmp)/'inverse'
    cfile.write_text(inverse_source)
    subprocess.run(['cc','-std=c11','-Wall','-Wextra','-Werror',str(cfile),'-lm','-o',str(exe)],check=True)
    subprocess.run([str(exe)],check=True)
print('Calibrated endpoint and rough-tail forward/inverse round trips passed')

# Recorded DAC1's six-term candidate is valid at retained points but turns near
# the open end. The five-term refit must survive the production curve checker.
curve_validation_source += '#include <string.h>\n#define ATTEN_CAL_CORRECTION_MONOTONIC_EPS_DB 1.0e-4\n'
fit_clock_source = r"""
#define ATTEN_CAL_FIT_CPU_BUDGET_MS 10
static int64_t fit_clock;
static int fit_sleeps;
static bool fit_pauses_enabled=true;
static int64_t k_uptime_get(void){return fit_pauses_enabled?fit_clock++:0;}
static void k_msleep(int ms){assert(ms==1);++fit_sleeps;fit_clock+=ms;}
""" + block('attenuator_calibration.c', 'static void fit_pause_if_due(')
curve_validation_source += fit_clock_source

for marker in ('struct atten_cal_record {', 'struct atten_cal_fit_point {',
               'static bool fit_curve_valid('):
    curve_validation_source += block('attenuator_calibration.c', marker)
fit_replay_source = curve_validation_source
curve_validation_source += r'''
int main(void) {
 struct attenuator_model_coeffs c={.fvoa_50pct_mv=3299.99362586,.slope_inv_fvoa_mv=.00289068400715,
  .max_atten_db=73.3546880086,.max_calibrated_db=55,.gain=1.533,
  .correction_coeff={-38.8084831,-24.2570744,-62.5005569,-6.50202751,-14.9005966,6.59192467}};
 struct atten_cal_record records[2]={{.sweep_mv=2150},{.sweep_mv=2950}};
 struct atten_cal_fit_point points[2]={{.record_index=0},{.record_index=1}};
 struct atten_model_eval eval;float failed_mv;
 assert(atten_model_eval(&c,2150,&eval) && eval.d_db_d_voltage_mv>0);
 assert(!fit_curve_valid(&c,points,records,2,&failed_mv,&eval));
 assert(failed_mv>1700 && failed_mv<2150);
 float corrected[6]={-44.3638229,-43.984787,-71.6152267,-21.0198765,-19.2872257,0};
 memcpy(c.correction_coeff,corrected,sizeof(corrected));
 assert(fit_curve_valid(&c,points,records,2,&failed_mv,&eval));
}
'''
with tempfile.TemporaryDirectory() as tmp:
    cfile,exe=Path(tmp)/'curve_validation.c',Path(tmp)/'curve_validation'
    cfile.write_text(curve_validation_source)
    subprocess.run(['cc','-std=c11','-Wall','-Wextra','-Werror',str(cfile),'-lm','-o',str(exe)],check=True)
    subprocess.run([str(exe)],check=True)
print('Recorded between-sample correction failure and reduced-order curve validation passed')

# Replay the recorded 199-point acquisition through the production fitter. Clock
# reads simulate elapsed work; sleep is counted, not a host scheduling claim.
# Both runs must have identical coefficients, metrics, acceptance and warnings.
fit_replay_source += '\n#include <errno.h>\n#define MAX(a,b) fmax(a,b)\n#define MIN(a,b) fmin(a,b)\n'
cal_text = (ROOT/'app/src/attenuator_calibration.c').read_text()
fit_replay_source += cal_text[cal_text.index('#define ATTEN_CAL_DEFAULT_DWELL_MS'):cal_text.index('enum atten_cal_state {')]
for line in (ROOT/'app/src/attenuator_calibration.h').read_text().splitlines():
    if line.startswith('#define ATTENUATOR_CAL_'): fit_replay_source += line+'\n'
for marker in ('enum atten_cal_record_event {', 'enum atten_cal_record_classification {',
               'struct atten_cal_bridge {'):
    fit_replay_source += block('attenuator_calibration.c', marker) + (';\n' if marker.startswith('enum') else '')
fit_replay_source += block('attenuator_calibration.h', 'struct attenuator_calibration_fit_metrics {')
fit_replay_source += r'''
static struct attenuator attenuators[1];
static struct {
 uint8_t attenuator_index,reference_record_index[2],record_count[2],bridge_count[2];
 bool reference_record_index_valid[2];
 struct atten_cal_record records[2][ATTENUATOR_CAL_RECORD_COUNT];
 struct atten_cal_bridge bridges[2][ATTENUATOR_CAL_RECORD_COUNT];
} cal;
static struct atten_cal_fit_point cal_fit_points[ATTENUATOR_CAL_RECORD_COUNT];
#define LOG_WRN(...) ((void)0)
#define snprintk snprintf
#define COO_CMD_RUNTIME_EMIT_WARNING 1
#define COO_CMD_RUNTIME_EMIT_BEST_EFFORT 0
struct coo_cmd_runtime_emit_args {int type,delivery;const char *code,*msg,*context;};
static char warnings[4][160];static int warning_count;
static void *command_runtime_get(void){return NULL;}
static void coo_cmd_runtime_emit(void *p,const struct coo_cmd_runtime_emit_args *a){
 (void)p;assert(warning_count<4);snprintf(warnings[warning_count++],160,"%s",a->context);
}
static const char *physical_name(unsigned p){return p?"dac2":"dac1";}
'''
fit_replay_source += block('attenuator.c', 'bool atten_model_correction_basis(')
for marker in ('static bool record_is_fit_candidate(', 'static int build_segment_scales_locked(',
               'static int build_fit_points_locked(', 'static int estimate_max_atten_db(',
               'static int fit_point_weighted_eval(', 'static double fit_cost(',
               'static void fit_initial_guess(', 'static int fit_optimize_db(',
               'static int solve_correction_normal_equation(', 'static int fit_correction_coeff_locked(',
               'static int fit_one_physical_locked('):
    fit_replay_source += block('attenuator_calibration.c', marker)
fixture = np.load(ROOT/'tests/throughput/calibration_replay.npz', allow_pickle=False)
meta = json.loads(str(fixture['metadata']))
fit_replay_source += 'static void load_records(void){\n'
for physical,name in enumerate(('dac1','dac2')):
    rows = fixture['records'][fixture['records']['physical']==name]
    m = meta[name]
    fit_replay_source += (f'cal.record_count[{physical}]={len(rows)};'
                          f'cal.reference_record_index[{physical}]={m["reference_record"]};'
                          f'cal.reference_record_index_valid[{physical}]=true;'
                          f'cal.bridge_count[{physical}]={m["bridge_count"]};\n')
    for i,bridge in enumerate(m['bridges']):
        fit_replay_source += f'cal.bridges[{physical}][{i}]=(struct atten_cal_bridge){{{bridge["before_record"]},{bridge["after_record"]}}};\n'
    for row in rows:
        values = {key:format(float(row[key]), '.17g') for key in
                  ('sweep_mv','other_mv','laser_pct','signal_mv','signal_err_mv','max_mv','segment')}
        values.update(event='ATTEN_CAL_EVENT_'+row['event'].upper(),
                      classification='ATTEN_CAL_CLASSIFICATION_'+row['classification'].upper())
        fit_replay_source += f'cal.records[{physical}][{row["record"]}]=(struct atten_cal_record){{'+','.join(f'.{key}={value}' for key,value in values.items())+'};\n'
fit_replay_source += r'''
 attenuators[0].coeff1.gain=attenuators[0].coeff2.gain=1.533;
}
int main(void){
 load_records();
 struct attenuator_calibration_fit_metrics baseline[2],paused[2];
 int baseline_rc[2];char baseline_warnings[4][160];
 fit_pauses_enabled=false;
 for(int i=0;i<2;i++)baseline_rc[i]=fit_one_physical_locked(i,&baseline[i]);
 assert(fit_sleeps==0);memcpy(baseline_warnings,warnings,sizeof(warnings));
 int baseline_warning_count=warning_count;warning_count=0;memset(warnings,0,sizeof(warnings));
 fit_pauses_enabled=true;
 for(int i=0;i<2;i++){
  int before=fit_sleeps;
  assert(fit_one_physical_locked(i,&paused[i])==baseline_rc[i]);
  assert(!memcmp(&baseline[i],&paused[i],sizeof(paused[i])) && fit_sleeps>before);
  assert(paused[i].valid && paused[i].accepted);
  printf("Replay dac%d: points=%u, rms_db=%.9g, identical with pauses (%d sleeps)\n",
         i+1,paused[i].points,paused[i].rms_db,fit_sleeps-before);
 }
 assert(warning_count==baseline_warning_count && !memcmp(warnings,baseline_warnings,sizeof(warnings)));
 return 0;
}
'''
with tempfile.TemporaryDirectory() as tmp:
    cfile,exe=Path(tmp)/'fit_replay.c',Path(tmp)/'fit_replay'
    cfile.write_text(fit_replay_source)
    subprocess.run(['cc','-std=c11','-Wall','-Wextra','-Werror','-Wno-unused-function',str(cfile),'-lm','-o',str(exe)],check=True)
    subprocess.run([str(exe)],check=True)
print('Recorded calibration pause/no-pause coefficients, metrics, acceptance and warnings match exactly')

# Effective route calibration: defaults, overrides, NVS restore, and public loss precision.
import re
source = r'''
#include <assert.h>
#include <errno.h>
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#define ARRAY_SIZE(a) (sizeof(a)/sizeof((a)[0]))
#define K_FOREVER 0
#define LOG_WRN(...) ((void)0)
static void k_mutex_lock(int *p,int t) {(void)p;(void)t;}
static void k_mutex_unlock(int *p) {(void)p;}
static void str_set(char *out,size_t n,const char *s) {snprintf(out,n,"%s",s);}
static int coo_json_append(char *out,size_t n,size_t *off,const char *fmt,...) {
    va_list args; va_start(args,fmt); int k=vsnprintf(out+*off,n-*off,fmt,args); va_end(args);
    if(k<0 || (size_t)k>=n-*off) return -ENOSPC;
    *off+=(size_t)k; return 0;
}
'''
header = (ROOT/'app/src/app_settings.h').read_text()
source += '\n'.join(re.findall(r'^#define APP_ROUTE_LOSS_.*$', header, re.M)) + '\n'
source += block('app_settings.h', 'struct app_route_loss_record {')
source += block('app_settings.h', 'struct app_route_loss_settings {')
source += r'''
struct app_settings_snapshot {struct app_route_loss_settings route_loss;};
static struct {struct app_settings_snapshot snapshot; int lock;} g_settings;
static struct app_route_loss_record disk[APP_ROUTE_LOSS_RECORD_COUNT];
static void app_nvs_persist_route_loss_index(uint8_t i,const struct app_route_loss_record *r) {disk[i]=*r;}
static unsigned route_loss_nvs_id(uint8_t i) {return i;}
static bool app_nvs_read_exact(unsigned i,void *p,size_t n,const char *name) {
    (void)name; if(!disk[i].configured) return false; memcpy(p,&disk[i],n); return true;
}
'''
settings_text = (ROOT/'app/src/devices.c').read_text()
start = settings_text.rfind('static const struct {',0,settings_text.index('} default_route_losses[]'))
source += settings_text[start:settings_text.index('\n};',start)+3] + '\n'
source += block('devices.c','double devices_route_loss_default(')
for marker in ['static bool route_loss_record_valid(', 'static void app_nvs_load_route_loss(',
               'static int route_loss_record_index_locked(', 'int app_settings_get_route_loss(',
               'int app_settings_set_route_loss(']:
    source += block('app_settings.c',marker)
source += block('mems_command.c','static int route_loss_append_loss(')
source += r'''
int main(void) {
    const char *lasers[]={"1028y","1270j","1430yj","1430hk","1510h","2330k"};
    const char *inputs[]={"yj_laser","yj_laser","yj_1430","hk_1430","hk_laser","hk_laser"};
    double tx; char route[24],json[64]; size_t off;
    for(unsigned i=0;i<6;i++) {
        const char *channel=i<3?"yj":"hk";
        for(unsigned j=0;j<2;j++) {
            snprintf(route,sizeof(route),"%s_to_%s_%s",inputs[i],channel,j?"fei":"ao");
            assert(app_settings_get_route_loss(route,lasers[i],&tx)==0);
            bool found=false;
            for(unsigned d=0;d<ARRAY_SIZE(default_route_losses);d++) {
                if(!strcmp(route,default_route_losses[d].route) && !strcmp(lasers[i],default_route_losses[d].laser)) {
                    assert(tx==default_route_losses[d].transmission);found=true;break;
                }
            }
            assert(found);
            off=0; assert(route_loss_append_loss(json,sizeof(json),&off,tx)==0);
            double loss=strtod(json,NULL);
            assert(loss<1 && fabs((1-loss)/tx-1)<2e-6);
            printf("%s %s %s\n",route,lasers[i],json);
            snprintf(route,sizeof(route),"%s_%s_to_%s_pd",channel,j?"sm":"mm",channel);
            assert(app_settings_get_route_loss(route,lasers[i],&tx)==0 && tx==(j?.60:.98));
        }
    }
    /* Unknown illumination still has a return path; per-laser overrides are
     * only selected when the source is known. No new persisted key is needed. */
    assert(app_settings_get_route_loss("yj_sm_to_yj_pd",NULL,&tx)==0 && tx==.60);
    assert(app_settings_get_route_loss("hk_mm_to_hk_pd",NULL,&tx)==0 && tx==.98);
    /* Defaults occupy no override slots, including after a simulated reboot. */
    for(unsigned i=0;i<APP_ROUTE_LOSS_RECORD_COUNT;i++) assert(!g_settings.snapshot.route_loss.record[i].configured);
    assert(app_settings_get_route_loss("1028y_to_M","1028y",&tx)==0 && tx==1);
    assert(app_settings_get_route_loss("yj_laser_to_yj_ao","1430hk",&tx)==0 && tx==1);
    assert(app_settings_get_route_loss("yj_calin_to_yj_split","split1",&tx)==0 && tx==1);
    assert(app_settings_set_route_loss("yj_1430_to_yj_ao","1430yj",1e-11,true)==0);
    assert(app_settings_set_route_loss("yj_1430_to_yj_ao","1430yj",1,false)==0);
    assert(app_settings_get_route_loss("yj_1430_to_yj_ao","1430yj",&tx)==0 && tx==1);
    memset(&g_settings.snapshot,0,sizeof(g_settings.snapshot));
    app_nvs_load_route_loss(&g_settings.snapshot);
    assert(app_settings_get_route_loss("yj_1430_to_yj_ao","1430yj",&tx)==0 && tx==1e-11);
    assert(app_settings_get_route_loss("yj_1430_to_yj_fei","1430yj",&tx)==0 && tx==6.81472e-11);
    assert(app_settings_set_route_loss("yj_sm_to_yj_pd","1028y",.5,true)==0);
    assert(app_settings_get_route_loss("yj_sm_to_yj_pd","1028y",&tx)==0 && tx==.5);
    assert(app_settings_get_route_loss("yj_sm_to_yj_pd",NULL,&tx)==0 && tx==.60);

}
'''
with tempfile.TemporaryDirectory() as tmp:
    cfile, exe = Path(tmp)/'routes.c', Path(tmp)/'routes'
    cfile.write_text(source)
    subprocess.run(['cc','-std=c11','-Wall','-Wextra','-Werror',str(cfile),'-lm','-o',str(exe)],check=True)
    replies = subprocess.check_output([str(exe)],text=True).splitlines()
for reply in replies:
    route, laser, loss = reply.split()
    client._request_json = lambda command,payload: {'route':route,'lasers':{laser:float(loss)}}
    result = client.mems_route_loss(route)
    assert result.lasers[0].value < 1
    assert eval(repr(result),vars(host)) == result
    assert str(result) == repr(result)
print('Route defaults, overrides, NVS restore, and C/Python precision checks passed')

# Protocol routing, bounded plotting, and notebook widget lifecycle without a broker.
import asyncio
import contextlib
import io
import logging
import os
import time
from types import SimpleNamespace
import numpy as np
os.environ.setdefault('MPLCONFIGDIR', str(Path(tempfile.gettempdir())/'hispec-test-matplotlib'))
os.environ.setdefault('XDG_CACHE_HOME', str(Path(tempfile.gettempdir())/'hispec-test-cache'))
import matplotlib
matplotlib.use('Agg')
import matplotlib.pyplot as plt

client = host.HispecFibPcb('localhost', connect=False)
client._connected.set()  # Local delivery only; no MQTT socket is opened.
client.logger = logging.Logger('throughput-test',level=logging.DEBUG)
logs = []
log_handler = logging.Handler()
log_handler.emit = logs.append
client.logger.addHandler(log_handler)
monitor = host.ThroughputMonitor(client,channel='all').start()
json_payload = json.dumps(dataclasses.asdict(sample)).encode()
for _ in range(100):
    for payload in (binary,json_payload):
        client._on_message(None,None,SimpleNamespace(topic=f'dt/{client.device}/yj_tput',payload=payload))
deadline=time.monotonic()+3
while len(monitor.to_recarray())<200 and time.monotonic()<deadline:
    time.sleep(.01)
assert len(monitor.to_recarray())==200 and not logs
monitor._stop_collection()
for channel in ('yj','hk'):
    client._on_message(None,None,SimpleNamespace(topic=f'dt/{client.device}/{channel}_tput',payload=b'not logged'))
assert not logs
client._on_message(None,None,SimpleNamespace(topic=f'dt/{client.device}/warning',
    payload=b'{"code":"test","msg":"visible warning","uptime_ms":12345}'))
client._on_message(None,None,SimpleNamespace(topic=f'cmd/{client.device}/resp/laser',
    payload=b'{"status":"ok"}',properties=None))
assert any('visible warning' in record.getMessage() for record in logs)
assert client.warnings[-1].uptime_ms == 12345
assert any('uptime_ms=12345' in record.getMessage() for record in logs)
assert any('resp/laser' in record.getMessage() for record in logs)

with plt.ioff():
    empty = host.ThroughputMonitor(client,channel='yj')
    fig,anim=empty.plot_live(max_points=10)
    anim._func(0); fig.canvas.draw(); plt.close(fig)
    rec=monitor.to_recarray()[:5].copy()
    rec.t_ms=np.arange(5)*50+1000
    rec.tp=[.01,0,-1,np.nan,2]
    rec.tp_err=[.002,0,.1,np.nan,.5]
    rec.pd_net_mv=[100,0,-5,100,100]
    rec.pd_net_err_mv=[1,0,1,np.nan,2]
    monitor._samples.clear()
    monitor._samples.extend(rec.tolist())
    # Interleave the other channel; selecting YJ must still preserve its time base.
    monitor._samples.extend([dataclasses.replace(sample,channel='hk_m').as_tuple()]*3)
    fig,anim=monitor.plot_live(channel='yj',max_points=5)
    anim._func(0); fig.canvas.draw()
    tp,pd,snr,drive,source,power,atten=fig.axes
    np.testing.assert_allclose(tp.lines[0].get_ydata(),[.01,np.nan,np.nan,np.nan,2],equal_nan=True)
    np.testing.assert_allclose(tp.lines[0].get_xdata(),np.arange(5)*.05)
    np.testing.assert_allclose(snr.lines[0].get_ydata(),[100,np.nan,np.nan,np.nan,50],equal_nan=True)
    np.testing.assert_allclose(snr.lines[1].get_ydata(),[5,np.nan,np.nan,np.nan,4],equal_nan=True)
    assert pd.patches[0].get_y()==400 and pd.patches[0].get_height()==1200
    assert pd.lines[0].get_ydata()==[2000,2000]
    loss_axis=tp.child_axes[0]
    np.testing.assert_allclose(loss_axis._functions[0]([.01,1,10]),[20,0,-10])
    np.testing.assert_allclose(loss_axis._functions[1]([20,0,-10]),[.01,1,10])
    # Verify screen alignment, including negative dB and an inverted zoomed axis.
    for limits in ((.001,10),(10,.001)):
        tp.set_ylim(*limits); fig.canvas.draw()
        np.testing.assert_allclose(
            tp.transData.transform([(0,.01),(0,1),(0,10)])[:,1],
            loss_axis.transData.transform([(0,20),(0,0),(0,-10)])[:,1])
    tp.set_ylim(.001,10)
    for ax in fig.axes: ax.set_autoscale_on(False)
    tp.set_xlim(.1,.3); old=tp.get_xlim(); anim._func(1); assert tp.get_xlim()==old
    anim.pause(); anim.resume()
    for ax in fig.axes: ax.set_autoscale_on(True)
    anim._func(2); fig.canvas.draw(); plt.close(fig)
    np.testing.assert_allclose(monitor.to_recarray().tp[:5],rec.tp,equal_nan=True)
    # Overrange suppresses S/N even if a caller supplied finite errors; gaps
    # use acquisition times, not arrival timing or repeated old sample values.
    over=dataclasses.replace(sample,flags=('overrange',),t_ms=1050)
    normal=dataclasses.replace(sample,t_ms=1000)
    after_gap=dataclasses.replace(sample,t_ms=1200)
    monitor._samples.clear();monitor._samples.extend(x.as_tuple() for x in (normal,over,after_gap))
    fig,anim=monitor.plot_live(channel='yj',interval_s=.01)
    anim._func(0);fig.canvas.draw()
    assert anim.event_source.interval==250
    assert np.isnan(fig.axes[2].lines[0].get_ydata()[1])
    assert np.isnan(fig.axes[0].lines[0].get_ydata()[2])
    assert fig.axes[0].lines[1].get_ydata()[1]==sample.tp
    plt.close(fig)
    passive=host.decode_throughput_payload(bytes.fromhex(sample_wire_lines[9]))
    monitor._samples.clear();monitor._samples.extend(dataclasses.replace(passive,t_ms=1000+i*50).as_tuple() for i in range(3))
    fig,anim=monitor.plot_live(channel='yj');anim._func(0);fig.canvas.draw()
    assert np.isnan(fig.axes[0].lines[0].get_ydata()).all()
    assert np.isfinite(fig.axes[2].lines[0].get_ydata()).all()
    fig.savefig('/private/tmp/hispec-passive-dashboard.png');plt.close(fig)
    monitor._samples.extend([sample.as_tuple()]*20000)
    # The dashboard must not convert the collector's whole array each frame.
    monitor.to_recarray=lambda: (_ for _ in ()).throw(AssertionError('full history conversion'))
    fig,anim=monitor.plot_live(channel='yj',max_points=10)
    anim._func(0); fig.canvas.draw()
    assert len(fig.axes[0].lines[0].get_ydata())==10
    plt.close(fig)

notebook=json.loads((ROOT/'tools/throuput_monitor_lab.ipynb').read_text())
pane_code=next(''.join(c['source']) for c in notebook['cells'] if 'class MessagePaneHandler' in ''.join(c.get('source',[])))
cleanup_code=next(''.join(c['source']) for c in notebook['cells'] if ''.join(c.get('source',[])).startswith('# Optional display/log cleanup.'))
async def check_message_pane():
    previous=client.logger
    ns={'pcb':client}
    with contextlib.redirect_stdout(io.StringIO()): exec(pane_code,ns)
    widget=ns['message_output']; renders=[]
    render_trait='value' if widget.has_trait('value') else 'outputs'
    widget.observe(lambda change:renders.append(time.monotonic()),names=render_trait)
    for i in range(550): client.logger.info('message %d',i)
    for _ in range(100):
        client._on_message(None,None,SimpleNamespace(topic=f'dt/{client.device}/yj_tput',payload=b'no raw bytes'))
    assert not renders  # The MQTT/logging thread never writes the widget.
    await asyncio.sleep(.55)
    from html import unescape
    text=(unescape(widget.value.removeprefix('<pre style="margin:0; white-space:pre;">').removesuffix('</pre>'))
          if render_trait=='value' else ''.join(o.get('text','') for o in widget.outputs))
    assert len(renders)==1 and len(text.splitlines())==500
    assert 'message 49\n' not in text and 'message 50\n' in text and 'message 549\n' in text
    await asyncio.sleep(.55); assert len(renders)==1  # No refresh of unchanged output.
    old_task=ns['message_task']; old_handler=ns['message_handler']
    with contextlib.redirect_stdout(io.StringIO()): exec(pane_code,ns)
    await asyncio.sleep(0)
    assert old_task.cancelled() and old_handler._closed
    task=ns['message_task']
    exec(cleanup_code,ns); await asyncio.sleep(0)
    assert task.cancelled() and client.logger is previous
asyncio.run(check_message_pane())
# CSV export retains power precision, signs, errors and overrange flags.
import csv
import pandas as pd
csv_buffer=io.StringIO()
records=[host.decode_throughput_payload(line).as_tuple() for line in sample_wire_lines[::2]]
writer=csv.writer(csv_buffer);writer.writerow(host.THROUGHPUT_DTYPE.names);writer.writerows(records)
csv_buffer.seek(0);saved=pd.read_csv(csv_buffer)
assert saved.pd_power_err_nw[0]==2e-12
assert np.isnan(saved.tp_err[1]) and 'overrange' in saved['flags'][1]
assert saved.tp[2]==0 and saved.tp[3]==0 and saved.tp_err[3]>0

print('Protocol filtering, dashboard math/rendering, and notebook lifecycle checks passed')

# Exercise production laser current/stop paths with counted Modbus operations.
laser_source = r'''
#include <assert.h>
#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>
#include <errno.h>
#include <math.h>
#include "laser_properties.h"
#define K_FOREVER 0
#define K_MSEC(x) (x)
#define LASER_COMMAND_LOCK_TIMEOUT_MS 250
#define ARRAY_SIZE(a) (sizeof(a)/sizeof((a)[0]))
#define LOG_INF(...) ((void)0)
#define LOG_DBG(...) ((void)0)
#define LOG_WRN(...) ((void)0)
#define LASER_AUTOFF_NO_DEADLINE 0
#define HISPEC_LASER_COUNT 1
enum hispec_laser_id {HISPEC_LASER_1028_Y};
typedef struct {unsigned node_id; bool io_failed; int last_error; int64_t last_response_ms;} maiman_driver_t;
struct hispec_laser_driver_profile {enum hispec_laser_id id; const char *name; unsigned node_id; uint16_t expected_device_id; const laserprops_t *properties;};
struct on_time_runtime {bool active;};
static struct k_mutex laser_io_lock,laser_state_lock;
static bool laser_runtime_initialized=true;
/* Full settings definition inserted below. */
static struct app_laser_channel_settings laser_settings[1];
static bool bank_power_requested_enabled=true;
static int writes, starts, configurations, stops, identity_reads;
static double driver_current;
static bool hardware_started;
static bool fail_write, fail_stop;
static int64_t laser_autooff_deadline_ms[1];
static struct on_time_runtime laser_current_runtime[1],laser_tec_runtime[1];

static const struct hispec_laser_driver_profile profile={0,"test",1,0x1113,&LASER_1028};
static int profile_for_id(enum hispec_laser_id id, const struct hispec_laser_driver_profile **p)
{(void)id; *p=&profile;return 0;}
static int laser_io_lock_with_timeout(int t){return k_mutex_lock(&laser_io_lock,t);}
static const laserprops_t *runtime_props_locked(enum hispec_laser_id id){return &laser_settings[id].properties;}
static bool float_is_valid(double x){return isfinite(x);}
static void ensure_laser_runtime_settings_locked(void){}
static int64_t health_now=1000;
static int64_t k_uptime_get(void) {return health_now;}
#define MAX(a,b) ((a)>(b)?(a):(b))
#define MIN(a,b) ((a)<(b)?(a):(b))
#define ARG_UNUSED(x) (void)(x)
#define LASER_RESPONSE_TIMEOUT_MS 5000
#define LASER_COMM_WARNING_MS 5000
#define K_NO_WAIT 0
struct k_work {int unused;};
static int health_warnings,health_faults,health_recoveries,scheduled;
static bool fail_read;
static const struct hispec_laser_driver_profile laser_profiles[1]={{0,"test",1,0x1113,&LASER_1028}};
static void laser_autooff_reschedule(void) {scheduled++;}
static void hispec_laser_service_autooff(void);
static void laser_health_warning(enum hispec_laser_id id,const char *code,const char *message,int error) {
 (void)id;(void)message;(void)error;
 if(!strcmp(code,"laser_communication_fault")) health_faults++;
 else if(!strcmp(code,"laser_communication_recovered")) health_recoveries++;
 else health_warnings++;
}
static void maiman_init(maiman_driver_t *d,unsigned n){*d=(maiman_driver_t){.node_id=n};}
static bool reply(maiman_driver_t *d,bool ok) {
 if(ok)d->last_response_ms=health_now;else {d->io_failed=true;d->last_error=-EIO;}return ok;
}
static bool maiman_read_tec_started(maiman_driver_t *d,bool *on) {*on=true;return reply(d,!fail_read);}
#define OPERATION_STATE_STARTED 2
#define TEC_OPERATION_STATE_STARTED 2
#define REG_STATE_OF_DEVICE_COMMAND 4
#define REG_LOCK_STATUS 5
#define LOG_ERR(...) ((void)0)
static int ensure_bank_powered_locked(void) {return 0;}
static uint16_t maiman_get_device_id(maiman_driver_t *d) {identity_reads++;reply(d,true);return 0x1113;}
static uint16_t maiman_get_serial_number(maiman_driver_t *d) {identity_reads++;reply(d,true);return 8229;}
static int check_driver_serial_locked(const struct hispec_laser_driver_profile *p,uint16_t actual,uint16_t expected) {
 (void)p;return actual==expected?0:-EADDRNOTAVAIL;
}
static bool maiman_read_raw_tec_status(maiman_driver_t *d,uint16_t *v) {*v=2;return reply(d,true);}
static bool maiman_start_tec(maiman_driver_t *d) {return reply(d,true);}
static bool maiman_read_u16(maiman_driver_t *d,uint16_t a,uint16_t *v) {
 *v=a==REG_STATE_OF_DEVICE_COMMAND && hardware_started?2:0;return reply(d,true);
}
static uint16_t effective_blocking_lock_status(uint16_t l,uint16_t d) {(void)d;return l;}

static bool maiman_set_current(maiman_driver_t *d,double x){++writes;io_barrier();if(!fail_write)driver_current=x;return reply(d,!fail_write);}
static bool maiman_start_device(maiman_driver_t *d){++starts;hardware_started=true;return reply(d,true);}
static bool maiman_stop_device(maiman_driver_t *d){++stops;if(!fail_stop)hardware_started=false;return reply(d,!fail_stop);}
static bool maiman_stop_tec(maiman_driver_t *d){return reply(d,true);}
static int temperature_writes;
static bool maiman_set_tec_temperature(maiman_driver_t *d,double t){(void)t;temperature_writes++;return reply(d,true);}
static void commit_current_runtime_locked(enum hispec_laser_id id,bool p);
static void on_time_runtime_update_locked(struct on_time_runtime *r,unsigned n,enum hispec_laser_id id,bool active)
{(void)n;r[id].active=active;}
'''
laser_source = '#include <string.h>\n'+laser_source.replace('#define K_FOREVER 0',mutex_harness+'\n#define K_FOREVER 0',1)
laser_source = laser_source.replace('/* Full settings definition inserted below. */',block('app_settings.h','struct app_laser_channel_settings {'))
laser_source += quantize_source
laser_source += block('lasers.c','struct laser_output_estimate_state {')
laser_source += 'static struct laser_output_estimate_state laser_output_estimate[1];\nstatic void invalidate_output_locked(enum hispec_laser_id);\n'
for marker in ['static void laser_note_communication_locked(', 'int hispec_laser_output_status(',
               'static void laser_autooff_work_handler(struct k_work *work)\n{',
               'static void output_estimate_set_locked(', 'static void invalidate_output_locked(',
               'static bool output_ready_locked(']:
    laser_source += block('lasers.c',marker)
# The stop function also has a forward declaration; select its definition.
laser_text=(ROOT/'app/src/lasers.c').read_text()
stop_marker='static int stop_output_locked(const struct hispec_laser_driver_profile *profile, bool stop_tec)\n{'
laser_source += r'''
static void commit_current_runtime_locked(enum hispec_laser_id id,bool p) {
    (void)p;laser_current_runtime[id].active=false;
    output_estimate_set_locked(id,0,laser_output_estimate[id].tec_temperature_c);
    laser_output_estimate[id].started=false;laser_autooff_deadline_ms[id]=0;
}
'''
laser_source += r'''
static int apply_runtime_profile_locked(const struct hispec_laser_driver_profile *p,maiman_driver_t *d,const struct app_laser_channel_settings *settings) {
 (void)d;++configurations;laser_output_estimate[p->id].prepared=true;
 laser_output_estimate[p->id].tec_temperature_c=settings->properties.operating_temp_c;return 0;
}
'''
laser_source += block('lasers.c','static int verify_driver_locked(')
laser_source += block('lasers.c','static int prepare_to_operate_locked(')
laser_source += block('lasers.c',stop_marker)
laser_source += block('lasers.c','int hispec_laser_stop_output(')
laser_source += block('lasers.c','static void hispec_laser_service_autooff(void)\n{')
laser_source += block('lasers.c','int hispec_laser_set_current_ma(')
laser_source += block('lasers.c','int hispec_laser_get_channel_settings(')
for marker in ['static bool float_is_positive(', 'static bool float_is_nonzero(',
               'static double clamp_with_flag(', 'double hispec_laser_estimate_power_mw(']:
    laser_source += block('lasers.c',marker)
for name in ['hispec_laser_tune_request','hispec_laser_tune_result']:
    laser_source += block('lasers.h','struct '+name+' {')
for marker in ['int hispec_laser_set_output_mw(', 'int hispec_laser_set_output_percent(',
               'int hispec_laser_tune_wavelength(', 'int hispec_laser_set_output_percent_autooff(']:
    laser_source += block('lasers.c',marker)
laser_harness = laser_source
laser_source += r'''
static void *change_current(void *p) {(void)p;assert(hispec_laser_set_current_ma(0,80)==0);return NULL;}
int main(void){
 init_mutex(&laser_io_lock);init_mutex(&laser_state_lock);
 laser_settings[0].properties=LASER_1028;laser_settings[0].expected_serial=8229;
 assert(hispec_laser_set_current_ma(0,100)==0);
 assert(configurations==1 && writes==1 && starts==1);
 assert(hispec_laser_set_current_ma(0,150)==0);
 assert(hispec_laser_set_current_ma(0,50)==0);
 assert(configurations==1 && writes==3 && starts==1);
 assert(laser_output_estimate[0].current_ma==50);
 fail_write=true;
 assert(hispec_laser_set_current_ma(0,60)==-EIO);
 assert(!laser_output_estimate[0].valid && laser_output_estimate[0].prepared);
 assert(laser_current_runtime[0].active); /* A failed write cannot prove emission stopped. */
 fail_write=false;
 assert(hispec_laser_set_current_ma(0,60)==0 && configurations==1 && starts==2);
 /* Zero is a current update: retain preparation/start and shutdown deadline. */
 laser_autooff_deadline_ms[0]=health_now+1000;
 int nstarts=starts,nstops=stops,nconfigurations=configurations;
 assert(hispec_laser_set_current_ma(0,0)==0);
 assert(!laser_current_runtime[0].active && laser_output_estimate[0].started);
 assert(laser_autooff_deadline_ms[0]==health_now+1000);
 assert(starts==nstarts && stops==nstops && configurations==nconfigurations);
 assert(hispec_laser_set_current_ma(0,60)==0 && starts==nstarts && configurations==nconfigurations);
 fail_stop=true;
 assert(stop_output_locked(&profile,false)==-EIO);
 assert(!laser_current_runtime[0].active && !laser_output_estimate[0].valid);
 assert(laser_output_estimate[0].started && laser_output_estimate[0].current_ma==0);
 assert(laser_autooff_deadline_ms[0]>0); /* Failed stop retains shutdown obligation. */
 assert(hispec_laser_set_current_ma(0,0)==0 && !laser_output_estimate[0].valid);
 fail_stop=false;
 assert(stop_output_locked(&profile,false)==0);
 assert(!laser_current_runtime[0].active && !laser_output_estimate[0].started);
 assert(laser_output_estimate[0].prepared && laser_autooff_deadline_ms[0]==0);
 nstops=stops;int nwrites=writes;
 assert(stop_output_locked(&profile,false)==0 && stops==nstops && writes==nwrites);
 assert(hispec_laser_set_current_ma(0,60)==0);
 laser_autooff_deadline_ms[0]=health_now+100;
 assert(hispec_laser_set_current_ma(0,0)==0);
 health_now+=100;hispec_laser_service_autooff();
 assert(!laser_output_estimate[0].started && laser_autooff_deadline_ms[0]==0);
 assert(hispec_laser_set_current_ma(0,260)==-ERANGE);
 struct app_laser_channel_settings copy;
 laser_runtime_initialized=false;
 assert(hispec_laser_get_channel_settings(0,&copy)==-EINVAL);
 laser_runtime_initialized=true;
 pthread_t writer;alarm(5);atomic_store(&block_io,true);
 assert(!pthread_create(&writer,NULL,change_current,NULL));wait_for_io();
 assert(hispec_laser_get_channel_settings(0,&copy)==0);
 assert(copy.properties.max_current_ma==250 && copy.properties.operating_temp_c==25);
 k_mutex_lock(&laser_state_lock,K_FOREVER);
 assert(laser_output_estimate[0].current_ma==0);
 k_mutex_unlock(&laser_state_lock);
 atomic_store(&release_io,true);assert(!pthread_join(writer,NULL));alarm(0);
 assert(laser_output_estimate[0].current_ma==80 && laser_output_estimate[0].valid);
 bool emitting;
 int64_t deadline=laser_output_estimate[0].response_deadline_ms;
 maiman_driver_t read={.io_failed=true,.last_error=-122};
 laser_note_communication_locked(0,&read);
 assert(hispec_laser_output_status(0,&emitting)==0 && emitting);
 assert(laser_output_estimate[0].valid && laser_output_estimate[0].current_ma==80);
 health_now=deadline-1;assert(hispec_laser_output_status(0,&emitting)==0);
 health_now=deadline;assert(hispec_laser_output_status(0,&emitting)==-ETIMEDOUT);
 fail_read=true;laser_autooff_work_handler(NULL);
 assert(health_faults==1 && laser_output_estimate[0].communication_fault);
 laser_autooff_work_handler(NULL);assert(health_faults==1);
 fail_read=false;health_now+=1000;laser_autooff_work_handler(NULL);
 assert(health_recoveries==1 && hispec_laser_output_status(0,&emitting)==0);
 assert(laser_output_estimate[0].prepared); /* Health failure did not erase configuration. */
 assert(scheduled>0 && health_warnings>0);
 assert(configurations==1 && identity_reads==2);
 maiman_driver_t check;maiman_init(&check,1);
 assert(verify_driver_locked(&profile,&check,NULL,9999)==-EADDRNOTAVAIL && identity_reads==2);
 /* Simulate the bank-off invalidation, then exercise actual first-use verification. */
 laser_output_estimate[0].device_id=laser_output_estimate[0].serial=0;
 laser_output_estimate[0].prepared=false;laser_output_estimate[0].started=false;hardware_started=false;
 assert(hispec_laser_set_current_ma(0,50)==0 && configurations==2 && identity_reads==4);
 /* Rounding is global, bounded, and reflected in the confirmed estimate. */
 assert(hispec_laser_set_current_ma(0,50.04)==0 && driver_current==50);
 nwrites=writes;deadline=laser_output_estimate[0].response_deadline_ms;
 health_now+=10;
 assert(hispec_laser_set_current_ma(0,50.049)==0 && writes==nwrites);
 assert(laser_output_estimate[0].response_deadline_ms==deadline);
 assert(hispec_laser_set_current_ma(0,50.05)==0 && driver_current==50.1);
 assert(laser_output_estimate[0].current_ma==50.1);
 laser_settings[0].properties.max_current_ma=60.06;
 assert(hispec_laser_set_current_ma(0,60.06)==0 && driver_current==60);
 assert(hispec_laser_set_current_ma(0,60.07)==-ERANGE);
 fail_write=true;
 assert(hispec_laser_set_current_ma(0,59.94)==-EIO); /* Failed writes preserve the confirmed setpoint. */
 assert(laser_output_estimate[0].current_ma==60);fail_write=false;
 assert(hispec_laser_set_current_ma(0,0.04)==0 && driver_current==0);
 assert(!laser_current_runtime[0].active && laser_output_estimate[0].current_ma==0);
 assert(hispec_laser_quantize_current_ma(8.14,8.11,8.29)==8.2);
 assert(isnan(hispec_laser_quantize_current_ma(8.15,8.11,8.19)));
 /* Every level/power/tune entry point uses the same current grid. */
 laser_settings[0].properties=LASER_1270;
 laser_settings[0].properties.nominal_current_ma=60.06;
 laser_settings[0].properties.max_current_ma=60.06;
 assert(hispec_laser_set_output_percent(0,100)==0 && driver_current==60);
 assert(hispec_laser_set_output_mw(0,(30.04-8)*.166)==0 && driver_current==30);
 assert(hispec_laser_set_output_percent(0,0)==0 && driver_current==0);
 laser_settings[0].properties=LASER_1270;
 struct hispec_laser_tune_request req={.desired_power_percent=50,.wavelength_nm=1270,
     .use_temperature=true,.use_current=true,.maximum_power_shift_percent=100,.apply=true};
 struct hispec_laser_tune_result result;
 assert(hispec_laser_tune_wavelength(0,&req,&result)==0 && result.target_current_ma==34);
 int ntemp=temperature_writes; nwrites=writes;
 deadline=laser_output_estimate[0].response_deadline_ms;
 health_now+=100;
 assert(!hispec_laser_tune_wavelength(0,&req,&result) && temperature_writes==ntemp && writes==nwrites);
 assert(laser_output_estimate[0].response_deadline_ms==deadline);
 req.wavelength_nm+=.01;
 assert(!hispec_laser_tune_wavelength(0,&req,&result) && temperature_writes==ntemp+1 && writes==nwrites);
 assert(laser_output_estimate[0].response_deadline_ms>deadline);
 /* Stored tuning is optional; a current-only throughput move keeps the TEC target. */
 laser_settings[0].tune_delta_nm=.02;
 ntemp=temperature_writes;
 double tuned_temp=laser_output_estimate[0].tec_temperature_c;
 assert(!hispec_laser_set_output_percent_autooff(0,25,4,false));
 assert(temperature_writes==ntemp && laser_output_estimate[0].tec_temperature_c==tuned_temp);
 assert(laser_autooff_deadline_ms[0]==health_now+4000);
 assert(!hispec_laser_set_output_percent_autooff(0,50,4,true) && temperature_writes>ntemp);
 ntemp=temperature_writes; nwrites=writes; health_now+=100;
 deadline=laser_output_estimate[0].response_deadline_ms;
 assert(!hispec_laser_set_output_percent_autooff(0,50,9,true) && temperature_writes==ntemp && writes==nwrites);
 assert(laser_autooff_deadline_ms[0]==health_now+9000);
 assert(laser_output_estimate[0].response_deadline_ms==deadline);
 laser_output_estimate[0].valid=false;
 assert(!hispec_laser_set_output_percent_autooff(0,50,9,true) && writes>nwrites);
 return 0;
}
'''
with tempfile.TemporaryDirectory() as tmp:
    cfile=Path(tmp)/'laser.c';exe=Path(tmp)/'laser'
    cfile.write_text(laser_source)
    subprocess.run(['cc','-pthread','-D_POSIX_C_SOURCE=200809L','-std=c11','-Wall','-Wextra','-Werror','-I',str(ROOT/'app/src'),str(cfile),'-lm','-o',str(exe)],check=True)
    subprocess.run([str(exe)],check=True)
print('laser current/stop regressions passed')

# Exercise the entire production diagnostic path after an acknowledged STOP.
# Register replies are stubbed; fault/readiness interpretation is production C.
status_source = laser_harness
status_source = status_source.replace('static uint16_t effective_blocking_lock_status(uint16_t l,uint16_t d) {(void)d;return l;}\n', '')
status_source = status_source.replace('#define OPERATION_STATE_STARTED 2\n', '').replace('#define TEC_OPERATION_STATE_STARTED 2\n', '')
maiman_header = (ROOT/'app/src/maiman.h').read_text()
status_bits = maiman_header[maiman_header.index('#define OPERATION_STATE_STARTED'):maiman_header.index('// Modbus command values')]
status_source = status_bits + status_source.replace('static int ensure_bank_powered_locked',
    'static uint16_t effective_blocking_lock_status(uint16_t,uint16_t);\nstatic int ensure_bank_powered_locked', 1)
status_source += block('lasers.h', 'struct hispec_laser_status {')
status_source += r'''
static uint16_t observed_locks,observed_tec;
static double on_time_runtime_seconds_locked(struct on_time_runtime *r,unsigned n,enum hispec_laser_id id)
{(void)r;(void)n;(void)id;return 0;}
static uint16_t maiman_get_raw_status(maiman_driver_t *d)
{reply(d,!fail_read);return hardware_started?OPERATION_STATE_STARTED:0;}
static uint16_t maiman_get_raw_tec_status(maiman_driver_t *d){reply(d,!fail_read);return observed_tec;}
static uint16_t maiman_get_raw_lock_status(maiman_driver_t *d){reply(d,!fail_read);return observed_locks;}
static bool maiman_get_current(maiman_driver_t *d,double *v){*v=driver_current;return reply(d,!fail_read);}
static bool maiman_get_tec_pid(maiman_driver_t *d,tec_pid_t *v){memset(v,0,sizeof(*v));return reply(d,!fail_read);}
'''
for name in ['current_measured', 'current_min', 'current_max', 'current_max_limit',
             'current_protection_threshold', 'current_set_calibration', 'tec_temperature_value',
             'pcb_temperature_measured', 'tec_current_limit', 'ntc_b25_100_coefficient',
             'voltage_measured', 'tec_temperature_measured', 'tec_current_measured', 'tec_voltage']:
    status_source += f'static double maiman_get_{name}(maiman_driver_t *d){{reply(d,!fail_read);return 0;}}\n'
for marker in ['static uint16_t effective_blocking_lock_status(', 'static const char *laser_blocked_reason(',
               'static double level_percent_for_current(', 'double hispec_laser_estimate_wavelength_nm(',
               'static void status_defaults(', 'int hispec_laser_get_status(']:
    status_source += block('lasers.c',marker)
status_source += r'''
int main(void){
 init_mutex(&laser_io_lock);init_mutex(&laser_state_lock);
 laser_settings[0].properties=LASER_1028;laser_settings[0].expected_serial=8229;
 assert(!hispec_laser_set_current_ma(0,100));
 assert(!hispec_laser_stop_output(0,true));
 assert(laser_output_estimate[0].valid && !laser_output_estimate[0].started);
 struct hispec_laser_status s;
 observed_locks=LOCK_STATE_INTERLOCK;
 for(int eng=0;eng<2;eng++){
  int warnings=health_warnings;
  assert(!hispec_laser_get_status(0,eng,&s));
  assert(laser_output_estimate[0].valid && health_warnings==warnings);
  assert(s.lock_status==2 && s.blocking_lock_status==2 && !s.ready_to_operate);
  assert(!strcmp(s.blocked_reason,"tec_not_started") && s.current_set_ma==0);
 }
 const uint16_t hard[]={LOCK_STATE_LD_OVERCURRENT,LOCK_STATE_LD_OVERHEAT,
  LOCK_STATE_EXTERNAL_NTC_INTERLOCK,LOCK_STATE_TEC_ERROR,LOCK_STATE_TEC_SELFHEAT};
 for(unsigned i=0;i<ARRAY_SIZE(hard);i++){
  observed_locks=hard[i]|LOCK_STATE_INTERLOCK;laser_output_estimate[0].valid=true;
  int warnings=health_warnings;assert(!hispec_laser_get_status(0,false,&s));
  assert(!laser_output_estimate[0].valid && health_warnings==warnings+1);
 }
 observed_locks=LOCK_STATE_INTERLOCK;observed_tec=TEC_OPERATION_STATE_STARTED;
 for(int intent=0;intent<2;intent++)for(int observed=0;observed<2;observed++){
  laser_output_estimate[0].started=intent;hardware_started=observed;
  laser_output_estimate[0].valid=true;assert(!hispec_laser_get_status(0,false,&s));
  assert(laser_output_estimate[0].valid==(!intent && !observed));
 }
 /* Unexpected LD or TEC stop remains a fault even without lock bits. */
 observed_locks=0;laser_output_estimate[0].started=true;
 for(int tec=0;tec<2;tec++){
  hardware_started=!tec;observed_tec=tec?TEC_OPERATION_STATE_STARTED:0;
  laser_output_estimate[0].valid=true;assert(!hispec_laser_get_status(0,false,&s));
  assert(!laser_output_estimate[0].valid);
 }
 /* A successful diagnostic never clears an earlier control failure. */
 laser_output_estimate[0].started=false;hardware_started=false;
 assert(!hispec_laser_get_status(0,false,&s) && !laser_output_estimate[0].valid);
 laser_output_estimate[0].valid=true;laser_settings[0].expected_serial=9999;
 assert(hispec_laser_get_status(0,true,&s)==-EADDRNOTAVAIL && !laser_output_estimate[0].valid);
 return 0;
}
'''
with tempfile.TemporaryDirectory() as tmp:
    cfile,exe=Path(tmp)/'status.c',Path(tmp)/'status'
    cfile.write_text(status_source)
    subprocess.run(['cc','-pthread','-D_POSIX_C_SOURCE=200809L','-std=c11','-Wall','-Wextra','-Werror',
                    '-Wno-unused-function','-Wno-unused-parameter','-I',str(ROOT/'app/src'),str(cfile),'-lm','-o',str(exe)],check=True)
    subprocess.run([str(exe)],check=True)
print('Stopped interlock, diagnostic readiness, hard faults and retained control-fault checks passed')

# Profile/settings tests reuse the real current/stop paths above. The TEC stub
# rejects any intermediate envelope that excludes its current target.
profile_stubs = r'''
#define REG_TEC_TEMPERATURE_MAX 0x0071
#define REG_TEC_TEMPERATURE_MIN 0x0072
#define DIVIDER_TEC_TEMPERATURE 100.0
static double tec_min=17,tec_max=38,tec_target=25;
static int config_op,fail_config_op,power_changes,persisted;
static bool config_reply(maiman_driver_t *d){return reply(d,++config_op!=fail_config_op);}
static int check_ocp_limit_locked(const struct hispec_laser_driver_profile *p,maiman_driver_t *d,const laserprops_t *s)
{(void)p;(void)s;return config_reply(d)?0:-EIO;}
static bool maiman_set_current_max(maiman_driver_t *d,double x){configurations++;assert(x==floor(x*10)/10);return config_reply(d);}
#define SCALAR_STUB(name,type) static bool name(maiman_driver_t *d,type x){(void)x;return config_reply(d);}
SCALAR_STUB(maiman_set_current_set_calibration,double)
SCALAR_STUB(maiman_set_tec_current_limit,double)
SCALAR_STUB(maiman_set_tec_pid,tec_pid_t)
SCALAR_STUB(maiman_set_frequency,double)
SCALAR_STUB(maiman_set_duration,double)
SCALAR_STUB(maiman_set_internal_current_control,bool)
SCALAR_STUB(maiman_set_internal_enable_control,bool)
SCALAR_STUB(maiman_set_internal_tec_temperature_control,bool)
SCALAR_STUB(maiman_set_internal_tec_enable_control,bool)
static bool maiman_deny_interlock(maiman_driver_t *d){return config_reply(d);}
static bool maiman_read_scaled(maiman_driver_t *d,unsigned reg,double divider,bool sign,double *v){
 assert(divider==100 && sign);assert(reg==0x71 || reg==0x72);
 *v=reg==0x71?tec_max:tec_min;return config_reply(d);
}
static bool maiman_write_scaled(maiman_driver_t *d,unsigned reg,double divider,bool sign,double v){
 assert(divider==100 && sign);assert(reg==0x71 || reg==0x72);
 if(!config_reply(d))return false;
 if(reg==0x71)tec_max=v;else tec_min=v;
 assert(tec_min<=tec_target && tec_target<=tec_max);return true;
}
static bool maiman_set_tec_temperature(maiman_driver_t *d,double t){
 temperature_writes++;if(!config_reply(d))return false;
 assert(tec_min<=t && t<=tec_max);tec_target=t;return true;
}
#define HISPEC_LASER_BANK_POWER_OVERRIDE_OFF 2
static int bank_power_mode;
static int bank_power_set_locked(bool on,bool *changed,bool force){
 (void)changed;(void)force;power_changes++;bank_power_requested_enabled=on;return 0;
}
static int app_settings_update_laser_channel(uint8_t id,const struct app_laser_channel_settings *s,bool persist){
 (void)id;(void)s;if(persist)persisted++;return 0;
}
'''
config_source=laser_harness.replace(
    'static bool maiman_set_tec_temperature(maiman_driver_t *d,double t){(void)t;temperature_writes++;return reply(d,true);}',
    profile_stubs)
config_source=config_source.replace('(void)p;laser_current_runtime[id].active=false;',
    '(void)p;if(laser_output_estimate[id].started)laser_settings[id].total_emitting_s+=.5;laser_current_runtime[id].active=false;')
start=config_source.index('static int apply_runtime_profile_locked(')
end=config_source.index('static int verify_driver_locked(',start)
config_source=config_source[:start]+block('lasers.c','static int apply_runtime_profile_locked(')+config_source[end:]
for marker in ['static int validate_laser_settings(', 'static bool laser_driver_settings_differ(',
               'int hispec_laser_update_channel_settings(']:
    config_source+=block('lasers.c',marker)
config_source+=r'''
int main(void){
 (void)wait_for_io;
 (void)laser_autooff_work_handler;
 init_mutex(&laser_io_lock);init_mutex(&laser_state_lock);
 struct app_laser_channel_settings base={.properties=LASER_1270,.expected_serial=8229,
     .min_autolevel_current_ma=8.1,.current_set_calibration_pct=100};
 laser_settings[0]=base;
 /* Expand-only, narrow-only, and disjoint envelopes in both directions. */
 const double ranges[][3]={{15,40,25},{20,30,25},{31,38,35},{17,23,20}};
 for(unsigned i=0;i<ARRAY_SIZE(ranges);i++){
   struct app_laser_channel_settings next=base;
   next.properties.operating_temp_range_c=(temp_range_c_t){ranges[i][0],ranges[i][1]};
   next.properties.operating_temp_c=ranges[i][2];
   maiman_driver_t d={0};assert(!apply_runtime_profile_locked(&profile,&d,&next));
   assert(tec_min==ranges[i][0] && tec_max==ranges[i][1] && tec_target==ranges[i][2]);
 }
 /* Every profile I/O failure leaves preparation false and blocks emission. */
 config_op=0;tec_min=17;tec_max=23;tec_target=20;
 assert(!hispec_laser_set_current_ma(0,34)); /* Initially prepared by the calls above. */
 assert(!hispec_laser_stop_output(0,false));
 laser_output_estimate[0].prepared=false;
 config_op=0;assert(!hispec_laser_set_current_ma(0,34));int operations=config_op;
 assert(operations>10);
 for(int op=1;op<=operations;op++){
   assert(!hispec_laser_stop_output(0,false));
   laser_output_estimate[0].prepared=false;
   tec_min=17;tec_max=23;tec_target=20;config_op=0;fail_config_op=op;
   int nstarts=starts;
   assert(hispec_laser_set_current_ma(0,34)==-EIO);
   assert(!laser_output_estimate[0].prepared && starts==nstarts);
   fail_config_op=0;
 }
 /* Every changed model/envelope field stops emission before acceptance. */
 for(int field=0;field<9;field++){
   laser_settings[0]=base;assert(!hispec_laser_set_current_ma(0,34));
   struct app_laser_channel_settings next=base;
   switch(field){
    case 0:next.properties.threshold_current_ma+=1;break;
    case 1:next.properties.nominal_current_ma-=1;break;
    case 2:next.properties.efficiency_mw_per_ma+=.01;break;
    case 3:next.properties.wavelength_nm+=1;break;
    case 4:next.properties.dlambda_dT_nm_per_k+=.01;break;
    case 5:next.properties.dlambda_dA_nm_per_ma+=.01;break;
    case 6:next.min_autolevel_current_ma+=1;break;
    case 7:next.properties.operating_temp_range_c.min_c+=1;break;
    case 8:next.properties.max_current_ma+=1;break;
   }
   int saves=persisted;
   fail_stop=true;
   assert(hispec_laser_update_channel_settings(0,&next,true)==-EIO);
   assert(!memcmp(&laser_settings[0],&base,sizeof(base)) && persisted==saves);
   fail_stop=false;
   assert(!hispec_laser_update_channel_settings(0,&next,true));
   assert(!laser_current_runtime[0].active && !laser_output_estimate[0].started && persisted==saves+1);
   assert(laser_settings[0].total_emitting_s==.5); /* STOP's committed counter survives the settings snapshot. */
   if(field==0)assert(laser_settings[0].min_autolevel_current_ma==9.1);
   if(field==7)assert(!laser_output_estimate[0].prepared);
 }
 /* A range-only edit leaves an idle bank off. Mixed edits program immediately. */
 laser_settings[0]=base;bank_power_requested_enabled=false;
 struct app_laser_channel_settings next=base;next.properties.operating_temp_range_c.min_c=18;
 int powers=power_changes,configs=configurations;
 assert(!hispec_laser_update_channel_settings(0,&next,false));
 assert(power_changes==powers && configurations==configs && !bank_power_requested_enabled);
 next.properties.operating_temp_c=26;
 assert(!hispec_laser_update_channel_settings(0,&next,false));
 assert(power_changes==powers+2 && configurations==configs+1 && !bank_power_requested_enabled);
 assert(tec_min==18 && tec_target==26);
 /* Failure during immediate programming rejects the proposed settings. */
 struct app_laser_channel_settings previous=laser_settings[0];
 next.properties.operating_temp_c=27;config_op=0;fail_config_op=8;
 assert(hispec_laser_update_channel_settings(0,&next,true)==-EIO);
 assert(!memcmp(&laser_settings[0],&previous,sizeof(previous)) && !laser_output_estimate[0].prepared);
 return 0;
}
'''
with tempfile.TemporaryDirectory() as tmp:
    cfile,exe=Path(tmp)/'profile.c',Path(tmp)/'profile'
    cfile.write_text(config_source)
    subprocess.run(['cc','-pthread','-D_POSIX_C_SOURCE=200809L','-std=c11','-Wall','-Wextra','-Werror','-I',str(ROOT/'app/src'),str(cfile),'-lm','-o',str(exe)],check=True)
    subprocess.run([str(exe)],check=True)
print('TEC envelope ordering, programming failures and settings emission-stop checks passed')

maiman_source=r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#define LOG_ERR(...) ((void)0)
#include <stdarg.h>
static void trace(const char *fmt, ...) {(void)fmt;}
#define LOG_LEVEL_DBG 4
#define LOG_DBG(...) do { if (CONFIG_MAIMAN_LOG_LEVEL >= LOG_LEVEL_DBG) trace(__VA_ARGS__); } while (0)
#include <errno.h>
typedef struct {uint8_t node_id;bool io_failed;int last_error;int64_t last_response_ms;} maiman_driver_t;
static int64_t now=1000;
static int64_t k_uptime_get(void){return now;}
#define K_MSEC(x) (x)
static int bus_depth,lock_count,unlock_count;
static const int relay_device=1,temperature_device=2;
static const int *relay_bus=&relay_device,*temperature_bus=&temperature_device;
static void w1_lock_bus(const int *bus){assert(*bus==++bus_depth);++lock_count;}
static void w1_unlock_bus(const int *bus){assert(*bus==bus_depth--);++unlock_count;}
static void k_sleep(unsigned ms){assert(bus_depth==0);now+=ms;}
#define MAIMAN_BUSY_MS 350U
#define REG_STATE_OF_DEVICE_COMMAND 4
#define MODBUS_START_COMMAND_VALUE 8
#define MODBUS_STOP_COMMAND_VALUE 16
#define REG_SAVE_PARAMETERS 9
#define REG_RESET_PARAMETERS 10
#if CONFIG_MAIMAN_LOG_LEVEL >= LOG_LEVEL_DBG
static uint32_t transaction_sequence;
static int64_t last_transaction_end_ms;
#endif
static int maiman_client_iface=0,reply;
static const char *maiman_register_name(uint16_t a){(void)a;return "test";}
static int modbus_read_holding_regs(int i,uint8_t n,uint16_t a,uint16_t *v,int c)
{(void)i;(void)n;(void)a;(void)c;assert(bus_depth==2);*v=42;now+= reply ? 75 : 4;return reply;}
static int modbus_write_holding_regs(int i,uint8_t n,uint16_t a,uint16_t *v,int c)
{(void)i;(void)n;(void)a;(void)v;(void)c;assert(bus_depth==2);now+= reply ? 75 : 4;return reply;}
'''
for marker in ['void maiman_init(', 'bool maiman_read_u16(', 'bool maiman_write_u16(']:
    maiman_source += block('maiman.c',marker)
maiman_source += r'''
int main(void){
 maiman_driver_t d;uint16_t v;
 maiman_init(&d,1);
 reply=2;assert(!maiman_read_u16(&d,4,&v) && d.io_failed);
 reply=0;assert(maiman_read_u16(&d,4,&v) && d.io_failed);
 maiman_init(&d,1);assert(!d.io_failed);
 reply=-5;assert(!maiman_write_u16(&d,8,1) && d.io_failed);
 maiman_init(&d,1);
 reply=3;assert(!maiman_write_u16(&d,8,1) && d.io_failed);
 const uint16_t regs[]={4,4,9,10};const uint16_t values[]={8,16,1,1};
 for(unsigned i=0;i<4;i++) for(unsigned fail=0;fail<2;fail++) {
  maiman_init(&d,1);reply=fail?-ETIMEDOUT:0;
  int64_t start=now;
  assert(maiman_write_u16(&d,regs[i],values[i])==!fail);
  assert(now-start==(fail?75:4)+350);
#if CONFIG_MAIMAN_LOG_LEVEL >= LOG_LEVEL_DBG
  assert(last_transaction_end_ms==start+(fail?75:4));
#endif
  assert(d.last_response_ms==(fail?0:start+4));
  /* Next request begins after quiet release, even following timeout. */
  reply=0;assert(maiman_read_u16(&d,4,&v));assert(now-start==(fail?75:4)+350+4);
 }
 maiman_init(&d,1);reply=0;int64_t start=now;
 assert(maiman_write_u16(&d,8,0) && now-start==4);
 assert(!bus_depth && lock_count==unlock_count && lock_count>20);
 maiman_client_iface=-ENODEV;assert(!maiman_read_u16(&d,4,&v));
 assert(!maiman_write_u16(&d,8,0) && !bus_depth);
 return 0;
}
'''
# Logging must not change exception handling, response timestamps, or busy waits.
with tempfile.TemporaryDirectory() as tmp:
    cfile=Path(tmp)/'maiman.c';exe=Path(tmp)/'maiman'
    cfile.write_text(maiman_source)
    for level in [0,3,4]:  # OFF, normal INFO, diagnostic DEBUG
        subprocess.run(['cc','-std=c11','-Wall','-Wextra','-Werror','-Wno-unused-function',
                        f'-DCONFIG_MAIMAN_LOG_LEVEL={level}',str(cfile),'-o',str(exe)],check=True)
        subprocess.run([str(exe)],check=True)
print('Maiman exception, sticky failure, and busy-wait regressions passed at OFF/INFO/DEBUG')

# Test allocation policy using production code and a linear optical model stub.
allocator_source=r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#define MAX(a,b) ((a)>(b)?(a):(b))
#define MIN(a,b) ((a)<(b)?(a):(b))
#define CLAMP(v,l,h) MIN(MAX(v,l),h)
#define ATTENUATOR_DB_EPSILON 1e-6
#define snprintk snprintf
#define coo_cmd_runtime_emit(...) ((void)0)
struct attenuator_dac_cfg {double voltage,attenuation_db,limit;bool valid;};
struct attenuator_model_coeffs {double scale,max_calibrated_db;};
#define ATTENUATOR_CALIBRATED_MAX_DB 55.0
struct attenuator {struct attenuator_dac_cfg dac_cfg1,dac_cfg2;struct attenuator_model_coeffs coeff1,coeff2;double attenuation_db;};
static int writes,fail_device=-1;
static double attenuator_drive_limit_mv(const struct attenuator_dac_cfg *c){return c->limit;}
static double attenuator_model_voltage_to_db(const struct attenuator_model_coeffs *c,double v){return v*c->scale;}
static bool attenuator_set_physical_db_staged(struct attenuator *a,unsigned i,double db){
 ++writes;if((int)i==fail_device)return false;
 struct attenuator_dac_cfg *d=i?&a->dac_cfg2:&a->dac_cfg1;
 d->voltage=db/(i?a->coeff2.scale:a->coeff1.scale);d->attenuation_db=db;return true;
}
'''
allocator_source += block('attenuator.c','static double attenuator_physical_max_db(')
allocator_source += 'static bool attenuator_read_physical(struct attenuator_dac_cfg *d,const struct attenuator_model_coeffs *c){(void)d;(void)c;return false;}\n'
allocator_source += block('attenuator.c','static bool attenuator_set_db_staged(')
allocator_source += r'''
static struct attenuator pair(double x,double y,double m1,double m2){
 return (struct attenuator){.dac_cfg1={x,999,m1,true},.dac_cfg2={y,999,m2,true},.coeff1={1,m1},.coeff2={1,m2}};
}
int main(void){
 struct attenuator a=pair(36,0,39.2791,34.8723);
 assert(attenuator_set_db_staged(&a,41,false));assert(a.dac_cfg1.attenuation_db==36 && a.dac_cfg2.attenuation_db==5);
 int n=writes;assert(attenuator_set_db_staged(&a,41,false) && writes==n);
 a=pair(39,35,40,40);assert(attenuator_set_db_staged(&a,40,false));
 assert(a.dac_cfg1.attenuation_db==20 && a.dac_cfg2.attenuation_db==20);
 a=pair(35.9263,0,39.2791,34.8723);assert(attenuator_set_db_staged(&a,40.70283,false));
 assert(a.dac_cfg1.attenuation_db<39 && fabs(a.dac_cfg2.attenuation_db-4.77653)<1e-8);
 a=pair(0,0,10,40);assert(attenuator_set_db_staged(&a,45,false));
 assert(a.dac_cfg1.attenuation_db==10 && a.dac_cfg2.attenuation_db==35);
 a=pair(10,10,40,40);fail_device=1;assert(!attenuator_set_db_staged(&a,30,false));
 assert(a.dac_cfg1.attenuation_db==15 && a.dac_cfg2.attenuation_db==10 && a.attenuation_db==25);
 fail_device=-1;
 a=pair(0,0,80,90);a.coeff1.max_calibrated_db=40;a.coeff2.max_calibrated_db=55;
 assert(attenuator_set_db_staged(&a,90,true));
 assert(a.dac_cfg1.attenuation_db==40 && a.dac_cfg2.attenuation_db==50);
 assert(attenuator_set_db_staged(&a,120,true) && a.attenuation_db==95);
 a.dac_cfg1.voltage=70;a.dac_cfg2.voltage=20;
 assert(attenuator_set_db_staged(&a,90,true));
 assert(a.dac_cfg1.attenuation_db==40 && a.dac_cfg2.attenuation_db==50);
 assert(attenuator_set_db_staged(&a,140,false) && a.attenuation_db==140);
 for(int j=0;j<10000;j++){
  double m1=1+rand()%60,m2=1+rand()%60;
  double x=(double)rand()/RAND_MAX*m1,y=(double)rand()/RAND_MAX*m2;
  double target=(double)rand()/RAND_MAX*(m1+m2);
  a=pair(x,y,m1,m2);assert(attenuator_set_db_staged(&a,target,false));
  assert(fabs(a.attenuation_db-target)<1e-5);
  assert(a.dac_cfg1.attenuation_db>=-1e-8 && a.dac_cfg1.attenuation_db<=m1+1e-8);
  assert(a.dac_cfg2.attenuation_db>=-1e-8 && a.dac_cfg2.attenuation_db<=m2+1e-8);
  assert((a.dac_cfg1.attenuation_db-x)*(target-x-y)>=-1e-8);
  assert((a.dac_cfg2.attenuation_db-y)*(target-x-y)>=-1e-8);
 }
 return 0;
}
'''
with tempfile.TemporaryDirectory() as tmp:
    cfile=Path(tmp)/'allocator.c';exe=Path(tmp)/'allocator'
    cfile.write_text(allocator_source)
    subprocess.run(['cc','-std=c11','-Wall','-Wextra','-Werror',str(cfile),'-lm','-o',str(exe)],check=True)
    subprocess.run([str(exe)],check=True)
print('directional attenuator allocation regressions passed')

# Production consumer loop: coalesced wakeups cannot replay measurements or moves.
source = r'''
#include <assert.h>
#include <errno.h>
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <setjmp.h>
#include <stdio.h>
#define PHOTODIODE_CHANNEL_COUNT 2
#define PHOTODIODE_SAMPLE_INTERVAL_MS 50
#define K_FOREVER 0
#define K_MSEC(x) (x)
#define ARG_UNUSED(x) (void)(x)
#define LOG_WRN(...) ((void)0)
enum photodiode_channel {PHOTODIODE_CHANNEL_YJ,PHOTODIODE_CHANNEL_HK};
enum hispec_laser_id {HISPEC_LASER_1028_Y};
'''
for file,names in {
    'photodiode.h':['photodiode_window_result','photodiode_channel_status','photodiode_status'],
    'throughput_monitor.c':['throughput_source_reference','throughput_state'],
    'lasers.h':['hispec_laser_flux_estimate'],
}.items():
    for name in names:
        source+=block(file,'struct '+name+' {')
source+=r'''
static struct throughput_state monitors[2];
static struct photodiode_status throughput_pd_status;
static int monitors_lock,frame,pubs,moves,stops,refreshes,warnings;
static int64_t now;
static jmp_buf done;
static int64_t k_uptime_get(void) {return now;}
static void k_mutex_lock(int *m,int t) {(void)m;(void)t;}
static void k_mutex_unlock(int *m) {(void)m;}
static int photodiode_wait_for_sample(int t) {
    assert(t==50);
    if(++frame==7) longjmp(done,1);
    now=frame*50;return 0;
}
static void photodiode_get_status(struct photodiode_status *s) {
    /* frame 2 repeats; frame 3 began during the preceding move; 4 is fresh. */
    const int64_t acquisitions[]={0,10,10,51,150,200,250};
    s->channel[0].sample_ms=acquisitions[frame];
}
static void attenuator_calibration_tick(struct photodiode_status *s) {(void)s;}
static int pd_power_output(int i) {return i;}
static int housekeeping_power_get_confirmed(int i,bool *p) {(void)i;*p=true;return 0;}
#define hispec_laser_name(i) "1028y"
static int hispec_laser_output_status(enum hispec_laser_id id,bool *on){(void)id;*on=true;return 0;}
static int stop_locked(int i) {monitors[i].phase=TP_INACTIVE;stops++;return 0;}
static int refresh_reference(struct throughput_state *s) {
    refreshes++;
    if(frame==6) return -EIO;
    s->source.laser_current_ma=10;
    if(frame==1 && moves==1) {now+=5;s->input_changed_ms=now;}
    return 0;
}
static void publish_sample(const struct throughput_state *s,const struct photodiode_channel_status *pd) {
    (void)s;(void)pd;pubs++;
    assert(moves==(frame==1?0:frame<=4?1:2)); /* Publication precedes adjustment. */
}
static int autolevel_adjust(struct throughput_state *s,const struct photodiode_channel_status *pd) {
    (void)s;(void)pd;moves++;
    assert(frame==1 || frame==4 || frame==5);return frame==1?1:0;
}
'''
source+=r'''
#define snprintk snprintf
#define COO_CMD_RUNTIME_EMIT_WARNING 1
#define COO_CMD_RUNTIME_EMIT_BEST_EFFORT 0
static const char *photodiode_channel_names[]={"yj","hk"};
struct coo_cmd_runtime_emit_args {int type,delivery;const char *code,*msg,*context;};
static void *command_runtime_get(void){return NULL;}
static int coo_cmd_runtime_emit(void *r,const struct coo_cmd_runtime_emit_args *a){
 (void)r;assert(a->type==COO_CMD_RUNTIME_EMIT_WARNING && a->code && a->msg && a->context);warnings++;return 0;
}
'''
source+=block('throughput_monitor.c','static void warn_fault_stop(')
source+=block('throughput_monitor.c','void throughput_monitor_thread(')
source+=r'''
int main(void) {
    monitors[0]=(struct throughput_state){.phase=TP_RUNNING,.autolevel=true,.has_laser=true};
    if(!setjmp(done)) throughput_monitor_thread(NULL,NULL,NULL);
    assert(pubs==4 && moves==3 && stops==1 && refreshes==7 && warnings==1);
    assert(monitors[0].phase!=TP_RUNNING && monitors[0].last_sample_ms==200);
    puts("Consumer freshness, publication order, source fault checks passed");
}
'''
with tempfile.TemporaryDirectory() as tmp:
    cfile,exe=Path(tmp)/'consumer.c',Path(tmp)/'consumer'
    cfile.write_text(source)
    subprocess.run(['cc','-std=c11','-Wall','-Wextra','-Werror',str(cfile),'-lm','-o',str(exe)],check=True)
    subprocess.run([str(exe)],check=True)

# Command preparation and both physical routes, using production command/monitor
# bodies. Stub parsing (already tested by coo_commons), hardware, and persistence.
source = r'''
#include <assert.h>
#include <errno.h>
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#define ARRAY_SIZE(a) (sizeof(a)/sizeof((a)[0]))
#define MIN(a,b) ((a)<(b)?(a):(b))
#define K_FOREVER 0
#define LOG_ERR(...) ((void)0)
#define snprintk snprintf
#define MEMS_SOURCEDEST_MAX_LEN 24
#define APP_ROUTE_LOSS_ROUTE_MAX_LEN 24
#define COO_JSON_EXTRACT_OK 0
#define COO_JSON_EXTRACT_ERR -1
#define COO_JSON_EXTRACT_MISSING 1
#define PHOTODIODE_CHANNEL_COUNT 2
#define APP_PD_POWER_OVERRIDE_OFF 2
#define ROUTE_DEF(i,o,steps) {{i,o}}
enum photodiode_channel {PHOTODIODE_CHANNEL_YJ,PHOTODIODE_CHANNEL_HK};
enum housekeeping_power_output {YJ,HK};
enum hispec_laser_id {HISPEC_LASER_1028_Y,HISPEC_LASER_1270_J,HISPEC_LASER_1430_YJ,HISPEC_LASER_1430_HK,HISPEC_LASER_1510_H,HISPEC_LASER_2330_K,HISPEC_LASER_UNKNOWN};
static const char *laser_names[]={"1028y","1270j","1430yj","1430hk","1510h","2330k","none"};
static const char *photodiode_channel_names[]={"yj","hk"};
struct photodiode_status {struct {bool dark_pending;} channel[2];};
struct app_photodiode_settings {struct {int power;} channel[2];};
struct mems_route {struct {const char *input_name,*output_name;} key;};
struct fixture {const char *laser,*channel,*fiber,*input,*output,*format,*stop; bool autolevel,laser_stop,no_value,has_autooff,has_initial; double value,initial_level;};
typedef struct {double threshold_current_ma,nominal_current_ma;} laserprops_t;
struct app_laser_channel_settings {uint32_t autooff_s; bool disable_tec_at_autooff;laserprops_t properties;double min_autolevel_current_ma;};
struct coo_cmd_request {const struct fixture *payload;};
struct coo_cmd_response {char error[160];};
struct coo_json_string_choice {const char *name;int value;};
static int coo_json_extract_string(const struct fixture *p,const char *key,char *out,size_t n) {
    const char *value=NULL;
    #define FIELD(f) if(!strcmp(key,#f))value=p->f;
    FIELD(laser) FIELD(channel) FIELD(fiber) FIELD(input) FIELD(output) FIELD(format) FIELD(stop)
    #undef FIELD
    if(!value)return COO_JSON_EXTRACT_MISSING;
    if(strlen(value)>=n)return COO_JSON_EXTRACT_ERR;
    snprintf(out,n,"%s",value);return 0;
}
static int coo_json_match_string_choice(const char *s,const struct coo_json_string_choice *c,size_t n,int *v) {
    for(size_t i=0;i<n;i++) if(!strcmp(s,c[i].name)){*v=c[i].value;return 0;}return -1;
}
static int coo_json_extract_string_choice(const struct fixture *p,const char *key,const struct coo_json_string_choice *c,size_t n,int *v) {
    char s[24];int rc=coo_json_extract_string(p,key,s,sizeof(s));
    return rc==0?coo_json_match_string_choice(s,c,n,v):rc;
}
static int coo_json_extract_optional_bool(const struct fixture *p,const char *key,bool *v,bool *present) {
    (void)present;*v=!strcmp(key,"stop")?p->laser_stop:p->autolevel;return 0;
}
static int coo_json_extract_optional_u32(const struct fixture *p,const char *key,uint32_t *v,bool *present) {
    (void)key;(void)v;if(present)*present=p->has_autooff;return 0;
}
static int coo_json_extract_optional_double_range(const struct fixture *p,const char *key,double *v,bool *present,double lo,double hi) {
    *present=!strcmp(key,"initial_level") && p->has_initial;
    if(*present){if(!(p->initial_level>=lo && p->initial_level<=hi))return -1;*v=p->initial_level;}return 0;
}
static int coo_cmd_error(struct coo_cmd_response *out,const struct coo_cmd_request *cmd,const char *msg) {
    (void)cmd;snprintf(out->error,sizeof(out->error),"%s",msg);return -1;
}
static int coo_cmd_ok(struct coo_cmd_response *out,const struct coo_cmd_request *cmd) {(void)out;(void)cmd;return 0;}
static int hispec_laser_id_from_name(const char *name,enum hispec_laser_id *out) {
    for(int i=0;i<6;i++)if(!strcmp(name,laser_names[i])){*out=i;return 0;}return -EINVAL;
}
static const char *hispec_laser_name(enum hispec_laser_id id) {return laser_names[id];}
static int hispec_laser_output_status(enum hispec_laser_id id,bool *on){(void)id;*on=true;return 0;}
static int command_laser_id_from_payload(const struct coo_cmd_request *cmd,enum hispec_laser_id *id,char *name,size_t n)
{(void)name;(void)n;return hispec_laser_id_from_name(cmd->payload->laser,id);}
static int coo_json_extract_double(const struct fixture *p,const char *key,double *out) {(void)key;if(p->no_value)return COO_JSON_EXTRACT_MISSING;*out=p->value;return 0;}
static int hispec_laser_get_channel_settings(enum hispec_laser_id id,struct app_laser_channel_settings *s) {(void)id;*s=(struct app_laser_channel_settings){.autooff_s=300,.properties={8,60},.min_autolevel_current_ma=8.1};return 0;}
static int laser_cmd_error_rc(struct coo_cmd_response *out,const struct coo_cmd_request *cmd,const char *msg,int rc) {(void)out;(void)cmd;(void)msg;return rc;}
'''
for file,names in {
    'throughput_monitor.h':['throughput_monitor_request','throughput_monitor_status'],
    'throughput_monitor.c':['throughput_source_reference','throughput_state','laser_pd_channel'],
}.items():
    for name in names:
        source+=block(file,'struct '+name+' {')
source+=quantize_source+initial_level_define
for file,marker in [('devices.c','static const struct mems_route tib_routes[] ='),
                    ('throughput_monitor.c','static const struct laser_pd_channel laser_pd_channels[] =')]:
    source+=block(file,marker).rstrip()+';\n'
source+=r'''
static struct throughput_state monitors[2];
static int monitors_lock,router,applied,stops,fail_route,fail_power,fail_source,fail_stop,fail_atten;
static bool dark,calibrating,power_off,inhibited[2],omit_return;
static bool manual_level;
static int level_error,settings_error;
static double written_level;
static bool startup_atten_set;
static const struct mems_route *last_routes[2];
static struct attenuator {int unused;} attenuators[6];
static int64_t k_uptime_get(void) {return 100;}
static void k_mutex_lock(int *p,int t) {(void)p;(void)t;}
static void k_mutex_unlock(int *p) {(void)p;}
static void photodiode_get_status(struct photodiode_status *p) {memset(p,0,sizeof(*p));p->channel[0].dark_pending=dark;}
static bool attenuator_calibration_active(void) {return calibrating;}
static void app_settings_get_photodiode(struct app_photodiode_settings *p) {p->channel[0].power=p->channel[1].power=power_off?APP_PD_POWER_OVERRIDE_OFF:0;}
static int attenuator_index_from_laser_id(enum hispec_laser_id id,uint8_t *out) {*out=id;return 0;}
static int housekeeping_power_set(enum housekeeping_power_output p,bool on) {(void)on;assert(inhibited[p]);return fail_power?-EIO:0;}
static void housekeeping_photodiode_autooff_inhibit(enum housekeeping_power_output p,bool on) {inhibited[p]=on;}
static int hispec_laser_stop_output(enum hispec_laser_id l,bool bank) {(void)l;assert(!bank);stops++;return fail_stop?-EIO:0;}
#define ATTENUATOR_CALIBRATED_MAX_DB 55.0
static bool attenuator_set_db(struct attenuator *a,double db,bool calibrated_only) {assert(calibrated_only && db==110);(void)a;startup_atten_set=true;return !fail_atten;}
static int hispec_laser_set_output_percent_autooff(enum hispec_laser_id l,double p,unsigned off,bool tune) {
    (void)off;if(manual_level)assert(!monitors[l<3?0:1].autolevel && tune);
    else assert(startup_atten_set && !tune);
    if(level_error)return level_error;written_level=p;return 0;
}
static int hispec_laser_update_channel_settings(enum hispec_laser_id id,const struct app_laser_channel_settings *s,bool persist) {
    (void)s;(void)persist;assert(monitors[id<3?0:1].phase==TP_INACTIVE);return settings_error;
}
static int refresh_reference(struct throughput_state *s) {(void)s;return fail_source?-EIO:0;}
static const struct mems_route *mems_router_get_route(int *r,const char *in,const char *out) {
    (void)r;if(omit_return && strstr(out,"_pd"))return NULL;
    for(size_t i=0;i<ARRAY_SIZE(tib_routes);i++)if(!strcmp(in,tib_routes[i].key.input_name)&&!strcmp(out,tib_routes[i].key.output_name))return &tib_routes[i];return NULL;
}
static int mems_router_apply_route(int *r,const struct mems_route *route,bool force,const char **failed,char *state) {
    (void)r;(void)force;(void)failed;(void)state;
    int channel=route->key.input_name[0]=='y'?0:1;
    assert(monitors[channel].phase!=TP_RUNNING); /* Quiesce precedes all routing. */
    last_routes[applied++%2]=route;return applied==fail_route?-EIO:0;
}
static int app_settings_get_route_loss(const char *route,const char *laser,double *tx) {
    if(strstr(route,"_pd"))*tx=strstr(route,"_mm_")?.98:.60;
    else {assert(laser);*tx=1e-6;}return 0;
}
'''
for marker in ['static enum housekeeping_power_output pd_power_output(', 'static int photodiode_channel_for_laser(',
               'static void release_locked(', 'static int stop_locked(',
               'int throughput_monitor_prepare_start(', 'int throughput_monitor_start(', 'int throughput_monitor_stop(',
               'void throughput_monitor_note_laser_changed(', 'void throughput_monitor_note_attenuator_changed(',
               'int throughput_monitor_update_laser_settings(']:
    source+=block('throughput_monitor.c',marker)
source+='static int hispec_laser_set_current_ma(enum hispec_laser_id id,double ma) {return hispec_laser_set_output_percent_autooff(id,ma,0,true); }\n'
source+=block('laser_command.c','int laser_set(')
command=(ROOT/'app/src/throughput_command.c').read_text()
source+=command[command.index('enum throughput_format {'):]
source+=r'''
static struct coo_cmd_response response;
static int run(struct fixture f) {response.error[0]=0;return measure_throughput_set(&(struct coo_cmd_request){&f},&response);}
static void reset(void) {memset(monitors,0,sizeof(monitors));applied=stops=fail_route=fail_power=fail_source=fail_stop=fail_atten=settings_error=0;dark=calibrating=power_off=omit_return=startup_atten_set=false;inhibited[0]=inhibited[1]=false;}
int main(void) {
    struct fixture f={.laser="1028y",.output="yj_ao",.autolevel=true};
    for(int channel=0;channel<2;channel++)for(int fiber=0;fiber<2;fiber++) {
        reset();struct fixture p={.laser="none",.channel=channel?"hk":"yj",.fiber=fiber?"s":"m"};
        assert(!run(p) && applied==1 && monitors[channel].phase==TP_RUNNING);
        assert(!monitors[channel].has_laser && !monitors[channel].stop_laser);
        assert(monitors[channel].pd_route_tx==(fiber?.60:.98));assert(isnan(monitors[channel].laser_route_tx));
        assert(!strcmp(last_routes[0]->key.output_name,channel?"hk_pd":"yj_pd"));
        assert(!throughput_monitor_stop(channel,NULL) && stops==0 && !inhibited[channel]);
        p.input=channel?"hk_cal":"yj_cal";p.output=channel?"hk_fei":"yj_ao";
        assert(!run(p) && applied==3); /* optional passive launch plus return */
    }
    reset();assert(!run(f) && applied==2 && monitors[0].autolevel && monitors[0].stop_laser);
    assert(written_level==50);
    struct fixture initial=f;initial.has_initial=true;initial.initial_level=.25;
    assert(!run(initial) && written_level==25);
    initial.initial_level=0;assert(!run(initial) && fabs(written_level-100*.1/52)<1e-12);
    for(int i=0;i<4;i++) {
        initial.initial_level=(double[]){-1,1.01,NAN,INFINITY}[i];int before=applied;
        assert(run(initial)!=0 && applied==before && monitors[0].autolevel);
    }
    initial.initial_level=.5;initial.autolevel=false;assert(run(initial)!=0);
    reset();assert(!run(f));
    struct fixture bad=f;bad.output="hk_ao";assert(run(bad)!=0 && applied==2 && monitors[0].phase==TP_RUNNING && stops==0);
    bad=f;bad.channel="hk";assert(run(bad)!=0 && applied==2 && monitors[0].phase==TP_RUNNING);
    omit_return=true;assert(run(f)!=0 && applied==2 && monitors[0].phase==TP_RUNNING);omit_return=false;
    dark=true;assert(run(f)!=0 && applied==2 && monitors[0].phase==TP_RUNNING);dark=false;
    calibrating=true;assert(run(f)!=0 && applied==2);calibrating=false;
    power_off=true;assert(run(f)!=0 && applied==2);power_off=false;
    bad=(struct fixture){.laser="1430hk",.output="hk_ao",.autolevel=true};assert(run(bad)!=0 && applied==2 && monitors[0].phase==TP_RUNNING);
    f.autolevel=false;assert(!run(f) && monitors[0].stop_laser && stops==0);
    assert(!throughput_monitor_stop(0,NULL) && stops==1 && monitors[0].phase!=TP_RUNNING);
    for(int failure=1;failure<=2;failure++) {
        reset();f.autolevel=true;assert(!run(f));fail_route=applied+failure;
        assert(run(f)!=0 && monitors[0].phase!=TP_RUNNING && stops==1 && !inhibited[0]);
        assert(strstr(response.error,"MEMS may be partially changed"));
    }
    reset();assert(!run(f));fail_stop=1;fail_route=applied+1;
    assert(run(f)!=0 && monitors[0].phase!=TP_RUNNING && monitors[0].stop_laser && stops==1);
    fail_stop=0;assert(!throughput_monitor_stop(0,NULL) && stops==2 && !monitors[0].stop_laser);
    for(int failure=0;failure<3;failure++) {
        reset();fail_power=failure==0;fail_atten=failure==1;fail_source=failure==2;
        assert(run(f)!=0 && monitors[0].phase!=TP_RUNNING && !inhibited[0]);
        assert(stops==(failure==0?0:1));
    }
    reset();assert(!run(f));bad=f;bad.laser="1270j";assert(!run(bad) && stops==1 && monitors[0].laser==HISPEC_LASER_1270_J);
    struct fixture passive={.laser="none",.channel="yj"};assert(!run(passive) && stops==2 && !monitors[0].stop_laser);
    reset();passive.channel=NULL;assert(run(passive)!=0 && applied==0);
    passive.channel="yj";passive.input="yj_cal";assert(run(passive)!=0 && applied==0);
    puts("Throughput route, passive capture, exclusion and startup failure checks passed");
    for(int automatic=0;automatic<2;automatic++) {
        reset();manual_level=false;f.autolevel=automatic;assert(!run(f));
        monitors[0].off_in_s=3000;monitors[0].last_sample_ms=123;
        struct throughput_state expected=monitors[0];expected.autolevel=false;
        monitors[1]=(struct throughput_state){.phase=TP_RUNNING,.has_laser=true,.laser=HISPEC_LASER_1430_HK,.autolevel=true};
        struct throughput_state other=monitors[1];
        struct fixture level={.laser="1028y",.value=-1};
        struct coo_cmd_request cmd={&level};
        assert(laser_set(&cmd,&response)!=0 && monitors[0].autolevel==automatic);
        manual_level=true;level.value=.2;assert(!laser_set(&cmd,&response) && written_level==20);
        assert(!memcmp(&monitors[0],&expected,sizeof(expected)) && inhibited[0] && stops==0);
        assert(!memcmp(&monitors[1],&other,sizeof(other)));
        level_error=-EBUSY;assert(laser_set(&cmd,&response)==-EBUSY);
        assert(monitors[0].phase==TP_RUNNING && inhibited[0]);level_error=0;
        level.value=0;assert(!laser_set(&cmd,&response) && written_level==0);
        int before_stop=stops;
        level.laser_stop=true;level.no_value=true;
        assert(!laser_set(&cmd,&response) && stops==before_stop+1);
        level.has_autooff=true;assert(laser_set(&cmd,&response)!=0 && stops==before_stop+1);
        level.has_autooff=false;level.no_value=false;level.value=.5;
        assert(laser_set(&cmd,&response)!=0 && stops==before_stop+1);
        level.laser_stop=false;
        level.value=.4;assert(!laser_set(&cmd,&response) && written_level==40);
        throughput_monitor_note_attenuator_changed(0);
        assert(!memcmp(&monitors[0],&expected,sizeof(expected)) && inhibited[0]);
        assert(!throughput_monitor_stop(0,NULL) && stops==automatic+1 && !inhibited[0]);
    }
    manual_level=false;reset();assert(!run(f));
    throughput_monitor_note_laser_changed(HISPEC_LASER_1028_Y,true);
    assert(monitors[0].phase==TP_INACTIVE && !inhibited[0] && stops==0);
    reset();assert(!run(f));settings_error=-EIO;
    struct app_laser_channel_settings settings={0};
    assert(throughput_monitor_update_laser_settings(HISPEC_LASER_1028_Y,&settings,false)==-EIO);
    assert(monitors[0].phase==TP_INACTIVE && monitors[0].stop_laser && !inhibited[0]);
    assert(!throughput_monitor_stop(0,NULL) && stops==1 && !monitors[0].stop_laser);
    reset();assert(!run(f));
    assert(!throughput_monitor_update_laser_settings(HISPEC_LASER_1028_Y,&settings,true));
    assert(monitors[0].phase==TP_INACTIVE && !monitors[0].stop_laser);
    puts("Manual laser/attenuation streaming, command ordering, busy and shutdown ownership checks passed");
}
'''
with tempfile.TemporaryDirectory() as tmp:
    cfile,exe=Path(tmp)/'startup.c',Path(tmp)/'startup'
    cfile.write_text(source)
    subprocess.run(['cc','-std=c11','-Wall','-Wextra','-Werror',str(cfile),'-lm','-o',str(exe)],check=True)
    subprocess.run([str(exe)],check=True)

sent=[]
client._request_ok=lambda command,payload: sent.append((command,payload))
for channel in ('yj','hk'):
    client.measure_throughput('none',channel=channel,autolevel=False)
    assert sent[-1]==('measure_throughput',dict(laser='none',channel=channel,fiber='M',autolevel=False,off_in_s=300,format='binary'))
    client.measure_throughput('none',channel=channel,autolevel=False,input=f'{channel}_cal',output=f'{channel}_ao',format='json')
    assert sent[-1][1]['format']=='json' and sent[-1][1]['output']==f'{channel}_ao'
for kwargs in ({'laser':'none','autolevel':False}, {'laser':'none','channel':'yj'},
               {'laser':'none','channel':'yj','autolevel':False,'input':'yj_cal'},
               {'laser':'1028y','output':'yj_ao','channel':'hk'}, {'laser':'1028y'},
               {'laser':'1028y','output':'yj_pd'}, {'laser':'1028y','output':'hk_ao'}):
    count=len(sent)
    try: client.measure_throughput(**kwargs)
    except host.HispecFibError: pass
    else: raise AssertionError(f'invalid request accepted: {kwargs}')
    assert len(sent)==count
for laser in host.LASER_NAMES:
    channel=host._LASER_TO_PD_CHANNEL[laser]
    client.measure_throughput(laser,output=f'{channel}_fei')
    assert sent[-1][1]['channel']==channel
    assert 'initial_level' not in sent[-1][1]  # Firmware alone owns the default.
    client.measure_throughput(laser,output=f'{channel}_fei',initial_level=0.25)
    assert sent[-1][1]['initial_level']==0.25
for value in (-.01,1.01,float('nan'),float('inf')):
    try: client.measure_throughput('1028y',output='yj_fei',initial_level=value)
    except host.HispecFibError: pass
    else: raise AssertionError(f'invalid initial level accepted: {value}')
try: client.measure_throughput('1028y',output='yj_fei',autolevel=False,initial_level=.5)
except host.HispecFibError: pass
else: raise AssertionError('passive initial_level accepted')
print('Python active/passive measurement command checks passed')

# The shared command catalog validates keys before reaching the handler.
entry=(ROOT/'app/src/command.c').read_text().split('CMD_SPEC_TIB("measure_throughput"',1)[1].split('COO_CMD_HELP_EFFECT',1)[0]
allowed_keys=set(re.search(r'"(laser,[^"\n]+)"',entry).group(1).split(','))
for _,payload in sent: assert set(payload)<=allowed_keys

# Actual calibration classifier, six-term normal solve, evaluator and derivatives.
source = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <math.h>
#include <errno.h>
#define ATTENUATOR_MODEL_CORRECTION_TERMS 6
#define MODEL_CORRECTION_START_DB (-10.0 * log10(.99))
#define ATTEN_CAL_CORRECTION_PIVOT_EPS 1e-12
#define PHOTODIODE_ADC_MAX_MV 2047.9375
#define PHOTODIODE_ADC_USABLE_MV 2000.0
#define PHOTODIODE_ADC_LSB_MV .0625
#define ATTEN_CAL_SNR_USABLE 5.0
'''
for file,marker in [('attenuator.h','struct attenuator_model_coeffs {'),
                    ('photodiode.h','struct photodiode_window_result {'),
                    ('attenuator_calibration.c','enum atten_cal_record_classification {'),
                    ('attenuator_calibration.c','struct atten_cal_measurement {')]:
    source += block(file,marker) + (';\n' if marker.startswith('enum') else '')
for file,marker in [('attenuator.c','static bool attenuator_model_correction_active('),
                    ('attenuator.c','bool atten_model_correction_basis('),
                    ('attenuator.c','static double attenuator_model_chebyshev_db('),
                    ('attenuator.c','static double attenuator_model_correction_db('),
                    ('attenuator_calibration.c','static int solve_correction_normal_equation('),
                    ('attenuator_calibration.c','static void build_measurement_from_pd_window(')]:
    source += block(file,marker)
source += r'''
int main(void) {
 struct photodiode_window_result w={.valid=true,.sample_length=11,.mean_mv=1675.585227,
  .mean_net_mv=1675.585227,.mean_net_err_mv=238.157314,.max_mv=PHOTODIODE_ADC_MAX_MV};
 struct atten_cal_measurement m;
 build_measurement_from_pd_window(&w,&m);assert(m.classification==ATTEN_CAL_CLASSIFICATION_SATURATED);
 w.max_mv=2000;w.mean_mv=w.mean_net_mv=1900;w.mean_net_err_mv=1;
 build_measurement_from_pd_window(&w,&m);assert(m.classification==ATTEN_CAL_CLASSIFICATION_SATURATED);
 w.max_mv=1999.9375;
 build_measurement_from_pd_window(&w,&m);assert(m.classification==ATTEN_CAL_CLASSIFICATION_OK);
 w.failed_samples=11;
 build_measurement_from_pd_window(&w,&m);assert(m.classification==ATTEN_CAL_CLASSIFICATION_ADC_ERROR);
 double normal[6][6]={0},rhs[6]={0};float fit[6]={0};
 struct attenuator_model_coeffs c={.max_atten_db=60,.correction_coeff={1,-2,3,-4,5,-6}};
 for(int i=0;i<80;i++) {
  double base=MODEL_CORRECTION_START_DB+(60-MODEL_CORRECTION_START_DB)*(i+.5)/80;
  double b[6],target=attenuator_model_correction_db(&c,base,NULL,NULL);
  assert(atten_model_correction_basis(base,60,b));
  for(int j=0;j<6;j++) {rhs[j]+=b[j]*target;for(int k=0;k<6;k++)normal[j][k]+=b[j]*b[k];}
 }
 assert(solve_correction_normal_equation(normal,rhs,6,fit)==0);
 for(int j=0;j<6;j++)assert(fabs(fit[j]-c.correction_coeff[j])<1e-5);
 /* Refitting a leading submatrix must clear unused stored terms and solve
  * that smaller system, rather than truncate the six-term solution. */
 assert(solve_correction_normal_equation(normal,rhs,4,fit)==0);
 assert(fit[4]==0 && fit[5]==0);
 for(int j=0;j<4;j++) {double sum=0;for(int k=0;k<4;k++)sum+=normal[j][k]*fit[k];assert(fabs(sum-rhs[j])<1e-6);}
 memset(normal,0,sizeof(normal));assert(solve_correction_normal_equation(normal,rhs,6,fit)==-ERANGE);
 for(int i=1;i<80;i++) {
  double base=60.*i/80,db,dm,h=1e-4;
  double y=attenuator_model_correction_db(&c,base,&db,&dm);
  double fd=(attenuator_model_correction_db(&c,base+h,NULL,NULL)-attenuator_model_correction_db(&c,base-h,NULL,NULL))/(2*h);
  assert(fabs(fd-db)<1e-7);
  c.max_atten_db=60+h;double plus=attenuator_model_correction_db(&c,base,NULL,NULL);
  c.max_atten_db=60-h;double minus=attenuator_model_correction_db(&c,base,NULL,NULL);
  c.max_atten_db=60;assert(fabs((plus-minus)/(2*h)-dm)<1e-7);
  printf("%.12g %.12g\n",base,y);
 }
 assert(attenuator_model_correction_db(&c,0,NULL,NULL)==0);
 assert(attenuator_model_correction_db(&c,60,NULL,NULL)==0);
 /* A single T0 term gives an independently solvable quadratic endpoint. */
 c=(struct attenuator_model_coeffs){.max_atten_db=60,.max_calibrated_db=37,.correction_coeff={4}};
 double start=MODEL_CORRECTION_START_DB,span=60-start;
 double t=2*(37-start)/(span+4+sqrt((span+4)*(span+4)-16*(37-start)));
 double bc=start+span*t,cc=37-bc,h=1e-5,db,dm;
 assert(fabs(bc+attenuator_model_correction_db(&c,bc,NULL,NULL)-37)<1e-10);
 double left=bc-h+attenuator_model_correction_db(&c,bc-h,NULL,NULL);
 double right=bc+h+attenuator_model_correction_db(&c,bc+h,NULL,NULL);
 assert(right>=left && right-left<3*h);
 assert(fabs(attenuator_model_correction_db(&c,(bc+60)/2,&db,&dm)-cc/2)<1e-10);
 assert(fabs(db+cc/(60-bc))<1e-10);
 for(int i=1;i<80;i++) {
  double x=60.*i/80;
  (void)attenuator_model_correction_db(&c,x,&db,&dm);
  double fd=(attenuator_model_correction_db(&c,x+h,NULL,NULL)-attenuator_model_correction_db(&c,x-h,NULL,NULL))/(2*h);
  assert(fabs(fd-db)<1e-7);
  c.max_atten_db=60+h;double plus=attenuator_model_correction_db(&c,x,NULL,NULL);
  c.max_atten_db=60-h;double minus=attenuator_model_correction_db(&c,x,NULL,NULL);
  c.max_atten_db=60;assert(fabs((plus-minus)/(2*h)-dm)<1e-7);
  assert(1+db>0);
 }
 assert(attenuator_model_correction_db(&c,60,NULL,NULL)==0);
 c.correction_coeff[0]=0;
 assert(attenuator_model_correction_db(&c,50,&db,&dm)==0 && db==0 && dm==0);

}
'''
with tempfile.TemporaryDirectory() as tmp:
    cfile,exe=Path(tmp)/'correction.c',Path(tmp)/'correction'
    cfile.write_text(source)
    subprocess.run(['cc','-std=c11','-Wall','-Wextra','-Werror',str(cfile),'-lm','-o',str(exe)],check=True)
    values=np.loadtxt(io.StringIO(subprocess.check_output([str(exe)],text=True)))
    np.testing.assert_allclose(host._atten_correction_db(values[:,0],60,[1,-2,3,-4,5,-6]),values[:,1],atol=1e-10)
assert host.ATTENUATOR_MODEL_CORRECTION_TERMS==6
assert '#define ATTENUATOR_MODEL_CORRECTION_TERMS 6U' in (ROOT/'app/src/attenuator.h').read_text()
assert '#define APP_NVS_SCHEMA_VERSION 13U' in (ROOT/'app/src/app_settings.c').read_text()
print('Clipped windows, six-term recovery, singular fallback, analytic derivatives and C/Python parity passed')

# Exercise all coefficient JSON writers with nonzero fifth/sixth terms.
source=r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <stdarg.h>
#include <math.h>
#include <errno.h>
#define ATTENUATOR_MODEL_CORRECTION_TERMS 6
struct coo_cmd_response {char payload[1024];};
#define MIN(a,b) ((a)<(b)?(a):(b))
static struct coo_cmd_response message;
static int coo_json_append(char *p,size_t n,size_t *off,const char *fmt,...) {
 va_list args;va_start(args,fmt);int count=vsnprintf(p+*off,n-*off,fmt,args);va_end(args);
 if(count<0 || (size_t)count>=n-*off)return -ENOSPC;*off+=count;return 0;
}
static struct coo_cmd_response *atten_cal_telemetry_begin(size_t *off,const char *event){
 *off=0;assert(!coo_json_append(message.payload,sizeof(message.payload),off,"{\"event\":\"%s\"",event));return &message;
}
static const char *physical_name(uint8_t physical){assert(physical==0);return "dac1";}
static void atten_cal_publish_telemetry(struct coo_cmd_response *m){puts(m->payload);}
'''
for file,marker in [('attenuator.h','struct attenuator_model_coeffs {'),
                    ('attenuator_calibration.h','struct attenuator_calibration_fit_metrics {'),
                    ('attenuator_calibration.h','struct attenuator_calibration_status {'),
                    ('attenuator_command.c','static int append_attenuator_physical_coeff_json('),
                    ('attenuator_calibration.c','static int append_fit_json('),
                    ('attenuator_calibration.c','int attenuator_calibration_format_status('),
                    ('attenuator_calibration.c','static void atten_cal_emit_fit(')]:
    if marker.startswith('struct '):
        source+=block(file,marker)
    else:
        # These writers contain unmatched JSON braces inside C strings.
        text=(ROOT/'app/src'/file).read_text()
        start=text.index(marker)
        source+=text[start:text.index('\n}',start)+2]+'\n'
source+=r'''
int main(void){
 struct attenuator_model_coeffs c={.correction_coeff={1,-2,3,-4,5,-6}};
 struct attenuator_calibration_fit_metrics fit={.valid=true,.accepted=true,.correction_coeff={1,-2,3,-4,5,-6}};
 char payload[2048]="{";size_t off=1;
 assert(!append_attenuator_physical_coeff_json(payload,sizeof(payload),&off,"dac1",&c));puts(strcat(payload,"}"));
 strcpy(payload,"{\"base\":0");off=strlen(payload);
 assert(!append_fit_json(payload,sizeof(payload),&off,"dac1",&fit));puts(strcat(payload,"}"));

 atten_cal_emit_fit(0,&fit);
 struct attenuator_calibration_status status={.state="complete",.mode="tib_auto",.physical="dac2",.fit="failed",.point_count=128,.point_index=127,.dwell_ms=2000,.complete_pct=100,.current_mv=3299.194336f,.other_mv=3299.194336f,.last_error=-2147483647};
 for(unsigned i=0;i<2;i++) status.fit_metrics[i]=(struct attenuator_calibration_fit_metrics){
  .valid=true,.accepted=true,.points=128,.fvoa_50pct_mv=10117.7991234,
  .slope_inv_fvoa_mv=1.23456789123e-12,.max_atten_db=99.1234567891,
  .max_calibrated_db=54.123456789,.max_atten_sigma_db=.000123456789,
  .correlation=.999123456,.rms_db=.000123456789,.max_abs_db=.000123456789,
  .correction_coeff={-1.23456789e-12f,-1.23456789e-12f,-1.23456789e-12f,-1.23456789e-12f,-1.23456789e-12f,-1.23456789e-12f}};
 char compact[1024];assert(attenuator_calibration_format_status(compact,sizeof(compact),&status)==0);
 assert(strlen(compact)<sizeof(compact));puts(compact);return 0;
}
'''
with tempfile.TemporaryDirectory() as tmp:
    cfile,exe=Path(tmp)/'coefficient_json.c',Path(tmp)/'coefficient_json'
    cfile.write_text(source)
    subprocess.run(['cc','-std=c11','-Wall','-Wextra','-Werror',str(cfile),'-o',str(exe)],check=True)
    replies=[json.loads(line) for line in subprocess.check_output([str(exe)],text=True).splitlines()]
    for reply in replies[:3]:
        assert reply.get('dac1',reply)['correction_coeff']==[1,-2,3,-4,5,-6]
status = host._decode_atten_cal_status(replies[-1])
assert abs(status.dac1.max_calibrated_db-54.123456789) < 1e-7
assert status.dac1.min_tx is None and status.dac1.fvoa_span_mv is None
assert len(status.dac1.correction_coeff) == 6
print('Six-term writers, calibrated metadata and 1024-byte aggregate status checks passed')

for base in ([2500,0.002,55,1.533,50],):
    values=base+[1,-2,3,-4,5,-6]
    assert host._atten_coeff_tuple('dac1',values)[4]==(1,-2,3,-4,5,-6)
    assert host._atten_physical_coeff_payload('dac1',values)['correction_coeff']==[1,-2,3,-4,5,-6]

# Relay owner: real locks, fake GPIO transport, queued expiry and elapsed health.
source = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <math.h>
#include <errno.h>
#define K_FOREVER 0
#define K_MSEC(x) (x)
#define K_NO_WAIT 0
#define ARG_UNUSED(x) (void)(x)
#define LOG_WRN(...) ((void)0)
#define PHOTODIODE_CHANNEL_COUNT 2
#define HOUSEKEEPING_POWER_OUTPUT_COUNT 3
#define HOUSEKEEPING_PD_AUTOFF_NO_DEADLINE 0
#define HOUSEKEEPING_TEMP_INTERVAL_MS 1000
#define RELAY_RESPONSE_TIMEOUT_MS 5000
#define RELAY_COMM_WARNING_MS 5000
#define GPIO_ACTIVE_LOW 1
#define BIT(x) (1U<<(x))
#define snprintk snprintf
#define COO_CMD_RUNTIME_EMIT_WARNING 1
#define COO_CMD_RUNTIME_EMIT_BEST_EFFORT 0
'''+mutex_harness
source+=block('housekeeping.h','enum housekeeping_power_output {')+';\n'
source+=block('housekeeping.c','struct power_on_time_runtime {')
source+=r'''
struct device {int unused;};
struct gpio_dt_spec {const struct device *port; unsigned pin,dt_flags;};
typedef unsigned gpio_port_value_t;
struct k_work {int unused;};struct k_work_q {int unused;};struct k_work_delayable {int unused;};
struct coo_cmd_runtime_emit_args {int type,delivery; const char *code,*msg,*context;};
static struct k_mutex housekeeping_io_lock,housekeeping_state_lock;
static struct device dev;
static struct k_work_q queue,*housekeeping_work_q=&queue;
static struct k_work_delayable temperature_work,pd_autooff_work;
static const struct gpio_dt_spec yj_power_gpio={&dev,0,0},hk_power_gpio={&dev,1,GPIO_ACTIVE_LOW},heater_power_gpio={&dev,2,0};
static struct power_on_time_runtime power_on_time[3];
static int64_t relay_response_deadline_ms,relay_next_warning_ms,pd_autooff_deadline_ms[2],now=1000;
static bool relay_communication_fault,pd_autooff_inhibited[2],fail_transport;
static unsigned raw=BIT(1);static int writes,reads,warnings,faults,recoveries;
static int64_t k_uptime_get(void){return now;}
static bool devices_relay_gpio_online(void){return true;}
static int devices_relay_gpio_last_error(void){return -ENODEV;}
static void *command_runtime_get(void){return NULL;}
static void coo_cmd_runtime_emit(void *r,const struct coo_cmd_runtime_emit_args *a){
 (void)r;assert(a->type==COO_CMD_RUNTIME_EMIT_WARNING);assert(strstr(a->context,"rc="));
 if(!strcmp(a->code,"relay_communication_fault"))faults++;
 else if(!strcmp(a->code,"relay_communication_recovered"))recoveries++;else warnings++;
}
static int gpio_pin_get_dt(const struct gpio_dt_spec *g){reads++;return fail_transport?-EIO:!!(raw&BIT(g->pin))^!!(g->dt_flags&GPIO_ACTIVE_LOW);}
static int gpio_pin_set_dt(const struct gpio_dt_spec *g,int on){
 writes++;io_barrier();if(fail_transport)return -EIO;
 if(!!on^!!(g->dt_flags&GPIO_ACTIVE_LOW))raw|=BIT(g->pin);else raw&=~BIT(g->pin);return 0;
}
static int gpio_port_get_raw(const struct device *d,gpio_port_value_t *p){(void)d;reads++;*p=raw;return fail_transport?-EIO:0;}
static int k_work_cancel_delayable(struct k_work_delayable *w){(void)w;return 0;}
static int k_work_reschedule_for_queue(struct k_work_q *q,struct k_work_delayable *w,int64_t t){(void)q;(void)w;(void)t;return 0;}
static int temperature_sample_once(void){return 0;}
static void power_on_time_update_locked(enum housekeeping_power_output,bool);
'''
for marker in ['static const struct gpio_dt_spec *power_gpio(', 'static bool power_output_is_photodiode(',
               'static bool power_output_to_pd_index(', 'static void relay_health_warning(',
               'static void relay_note_response_locked(', 'int housekeeping_relay_error(',
               'static int power_get_locked(', 'static void power_on_time_update_locked(enum housekeeping_power_output output,\n',
               'static int power_set_locked(', 'int housekeeping_power_set(', 'int housekeeping_power_get(',
               'int housekeeping_power_get_confirmed(', 'double housekeeping_power_on_time_s(',
               'static int64_t pd_next_autooff_deadline_locked(', 'static void pd_autooff_reschedule_locked(',
               'int housekeeping_photodiode_auto_enable(', 'void housekeeping_photodiode_autooff_cancel(',
               'void housekeeping_photodiode_autooff_inhibit(', 'int64_t housekeeping_photodiode_autooff_remaining_s(',
               'static void temperature_work_handler(struct k_work *work)\n{',
               'static void pd_autooff_work_handler(struct k_work *work)\n{']:
    source+=block('housekeeping.c',marker)
source+=r'''
static void *turn_off(void *p){(void)p;assert(!housekeeping_power_set(0,false));return NULL;}
int main(void){
 init_mutex(&housekeeping_io_lock);init_mutex(&housekeeping_state_lock);
 relay_response_deadline_ms=now+5000;
 assert(!housekeeping_power_set(0,true));bool on,was_off;
 assert(!housekeeping_power_get_confirmed(0,&on)&&on);
 int old_reads=reads;assert(!housekeeping_power_get_confirmed(0,&on)&&reads==old_reads);
 assert(!housekeeping_photodiode_auto_enable(0,1,&was_off)&&!was_off);
 now+=1500;housekeeping_photodiode_autooff_inhibit(0,true);
 int old_writes=writes;pd_autooff_work_handler(NULL);assert(writes==old_writes);
 assert(housekeeping_photodiode_autooff_remaining_s(0)==-1);
 housekeeping_photodiode_autooff_inhibit(0,false);pd_autooff_work_handler(NULL);
 assert(!housekeeping_power_get_confirmed(0,&on)&&!on);
 assert(housekeeping_power_on_time_s(0)==0);
 assert(!housekeeping_power_set(0,true));housekeeping_photodiode_autooff_cancel(0);
 fail_transport=true;assert(housekeeping_power_get(0,&on)==-EIO);
 assert(!housekeeping_power_get_confirmed(0,&on)&&on);
 now=relay_response_deadline_ms;assert(housekeeping_power_get_confirmed(0,&on)==-ETIMEDOUT&&on);
 temperature_work_handler(NULL);assert(faults==1&&warnings>0);
 temperature_work_handler(NULL);assert(faults==1);
 fail_transport=false;now+=1000;temperature_work_handler(NULL);
 assert(recoveries==1&&!housekeeping_relay_error());
 assert(!housekeeping_power_get_confirmed(1,&on)&&!on); /* Active-low decoding. */
 pthread_t writer;alarm(5);atomic_store(&block_io,true);
 assert(!pthread_create(&writer,NULL,turn_off,NULL));wait_for_io();
 assert(!housekeeping_power_get_confirmed(0,&on)&&on); /* State copy cannot wait for GPIO. */
 atomic_store(&release_io,true);assert(!pthread_join(writer,NULL));alarm(0);
 assert(!housekeeping_power_get_confirmed(0,&on)&&!on);
 puts("Relay timeout/recovery, queued auto-off, polarity and concurrent state reads passed");
}
'''
with tempfile.TemporaryDirectory() as tmp:
    cfile,exe=Path(tmp)/'relay.c',Path(tmp)/'relay'
    cfile.write_text(source)
    subprocess.run(['cc','-pthread','-D_POSIX_C_SOURCE=200809L','-std=c11','-Wall','-Wextra','-Werror',str(cfile),'-lm','-o',str(exe)],check=True)
    subprocess.run([str(exe)],check=True)

# Calibration lifetime uses its real start/stop/error paths, with transport stubs.
source=r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <errno.h>
#include <stdio.h>
#define K_FOREVER 0
#define PHOTODIODE_CHANNEL_COUNT 2
#define ATTEN_CAL_MIN_DWELL_MS 100
#define ATTEN_CAL_DEFAULT_DWELL_MS 400
#define ATTEN_CAL_MAX_DWELL_MS 2000
#define ATTENUATOR_DRIVE_MAX_MV 3300
#define MIN(a,b) ((a)<(b)?(a):(b))
#define COO_CMD_RUNTIME_EMIT_WARNING 1
#define COO_CMD_RUNTIME_EMIT_BEST_EFFORT 0
#define ATTEN_CAL_MODE_TIB_AUTO 1
#define ATTEN_CAL_PHASE_NONE 0
#define ATTEN_CAL_PHASE_WAIT_WINDOW 1
enum hispec_laser_id {L0,L1};enum photodiode_channel {YJ,HK};
enum housekeeping_power_output {P0,P1};
struct coo_cmd_runtime_emit_args {int type,delivery;const char *code,*msg;};
struct attenuator_calibration_status {int state,error;};
'''
source+=block('attenuator_calibration.c','enum atten_cal_state {')+';\n'
source+=block('attenuator_calibration.h','struct attenuator_calibration_auto_request {')
source+=r'''
static struct {int state,phase,mode,attenuator_index,physical_index,dwell_ms,laser_percent,last_error;
 bool persistent,shutdown_pending;enum photodiode_channel channel;enum hispec_laser_id laser;} cal;
static int cal_lock,router,fail_stop=-1,fail_route,release_count,stop_count[2];
static bool inhibited[2],powered[2]={true,true},emitting[2];
static void k_mutex_lock(int *m,int t){(void)m;(void)t;}static void k_mutex_unlock(int *m){(void)m;}
static void *command_runtime_get(void){return NULL;}
static void coo_cmd_runtime_emit(void *r,const struct coo_cmd_runtime_emit_args *a){(void)r;(void)a;}
static bool devices_attenuator_channel_available(int i){return i<2;}
static void housekeeping_photodiode_autooff_inhibit(enum housekeeping_power_output i,bool on){if(inhibited[i]&&!on)release_count++;inhibited[i]=on;}
static int housekeeping_power_get(enum housekeeping_power_output i,bool *on){assert(inhibited[i]);*on=powered[i];return 0;}
static bool throughput_monitor_any_active(void){return false;}
static int throughput_monitor_stop(int c,void *s){(void)c;(void)s;return 0;}
static int mems_router_apply_named_route(void *r,const char *a,const char *b,bool c,void *d,void *e){(void)r;(void)a;(void)b;(void)c;(void)d;(void)e;return fail_route?-EIO:0;}
static bool set_physical_pair(int i,int p,int a,int b){(void)i;(void)p;(void)a;(void)b;return true;}
static int hispec_laser_stop_output(enum hispec_laser_id id,bool tec){(void)tec;stop_count[id]++;if(fail_stop==(int)id)return -EIO;emitting[id]=false;return 0;}
static void copy_status_locked(struct attenuator_calibration_status *s){if(s){s->state=cal.state;s->error=cal.last_error;}}
static void reset_locked(enum atten_cal_state state){memset(&cal,0,sizeof(cal));cal.state=state;}
static void atten_cal_emit_simple(const char *e){(void)e;}
static void auto_start_next_physical_locked(void){assert(!emitting[1-cal.laser]);emitting[cal.laser]=true;cal.phase=ATTEN_CAL_PHASE_WAIT_WINDOW;}
'''
for marker in ['static void auto_error_locked(', 'int attenuator_calibration_start_auto(', 'int attenuator_calibration_stop(']:
    source+=block('attenuator_calibration.c',marker)
source+=r'''
int main(void){
 struct attenuator_calibration_auto_request r={.laser=L0,.channel=YJ,.route_input="laser",.output="out",.pd_input="mm",.pd_output="pd",.dwell_ms=550};
 struct attenuator_calibration_status status;
 assert(!attenuator_calibration_start_auto(&r,&status)&&inhibited[0]);
 int releases=release_count;assert(!attenuator_calibration_start_auto(&r,&status)&&release_count==releases);
 r.laser=L1;assert(!attenuator_calibration_start_auto(&r,&status)&&!emitting[0]&&emitting[1]);
 fail_stop=1;auto_error_locked(-ETIMEDOUT);
 assert(!inhibited[0]&&cal.shutdown_pending&&cal.laser==L1);
 assert(attenuator_calibration_stop(&status)==-EIO&&cal.shutdown_pending&&cal.laser==L1);
 fail_stop=-1;assert(!attenuator_calibration_stop(&status)&&!emitting[1]&&!cal.shutdown_pending);
 assert(!attenuator_calibration_start_auto(&r,&status));
 assert(!attenuator_calibration_stop(&status)&&!inhibited[0]);
 fail_stop=1;assert(attenuator_calibration_start_auto(&r,&status)==-EIO&&cal.shutdown_pending&&cal.laser==L1);
 fail_stop=-1;assert(!attenuator_calibration_stop(&status));
 powered[0]=false;assert(attenuator_calibration_start_auto(&r,&status)==-EIO&&!inhibited[0]);
 powered[0]=true;fail_route=1;assert(attenuator_calibration_start_auto(&r,&status)==-EIO&&!inhibited[0]);
 puts("Calibration inhibition, restart, source replacement and failed-stop identity passed");
}
'''
with tempfile.TemporaryDirectory() as tmp:
    cfile,exe=Path(tmp)/'cal_lifetime.c',Path(tmp)/'cal_lifetime'
    cfile.write_text(source)
    subprocess.run(['cc','-std=c11','-Wall','-Wextra','-Werror',str(cfile),'-o',str(exe)],check=True)
    subprocess.run([str(exe)],check=True)

# Stop keyword is serialized unambiguously and conflicts never reach MQTT.
client=host.HispecFibPcb('localhost',connect=False)
sent=[]
client._request_ok=lambda key,payload: sent.append((key,payload))
client.laser('1028y',stop=True)
assert sent[-1]==('laser',{'name':'1028y','stop':True})
client.laser('1028y',value=0)
assert sent[-1]==('laser',{'name':'1028y','value':0.0})
for kwargs in ({'stop':True,'value':.1},{'stop':True,'autooff_s':1},{'stop':'true'}):
    count=len(sent)
    try: client.laser('1028y',**kwargs)
    except host.HispecFibError: pass
    else: raise AssertionError(kwargs)
    assert len(sent)==count
print('Python explicit stop and zero-level API checks passed')
