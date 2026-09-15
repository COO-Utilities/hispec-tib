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
    return text[start:end] + (';' if marker.startswith('struct ') else '') + '\n'


source = r'''
#include <assert.h>
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#define MAX(a,b) ((a)>(b)?(a):(b))
#define MIN(a,b) ((a)<(b)?(a):(b))
#define LOG_WRN(...) ((void)0)
#define PHOTODIODE_CHANNEL_COUNT 2
#define PD_WINDOW_MAX_SAMPLES 25
#define PD_THROUGHPUT_SAMPLES 25
#define PHOTODIODE_FIXED_WINDOW_MS 500U
#define PHOTODIODE_ADC_USABLE_MV 2000.0
#define TP_LOW_FRACTION 0.2
#define TP_HIGH_FRACTION 0.8
#define TP_INSTANT_BAD_SAMPLES 5U
#define TP_MIN_ATTEN_TX 1e-9
#define K_FOREVER 0
static int pd_runtime_lock;
static int64_t clock_ms = 100;
static int64_t k_uptime_get(void) { return clock_ms; }
static void k_mutex_lock(int *p, int t) { (void)p; (void)t; }
static void k_mutex_unlock(int *p) { (void)p; }
enum photodiode_channel {PHOTODIODE_CHANNEL_YJ, PHOTODIODE_CHANNEL_HK};
enum hispec_laser_id {HISPEC_LASER_1028_Y};
struct app_pd_channel_settings { struct { double rms_mv; } dark; };
'''
for file, names in {
    'photodiode.h': ['photodiode_window_result', 'photodiode_throughput_reference',
                    'photodiode_throughput_result', 'photodiode_channel_status'],
    'photodiode.c': ['pd_window_runtime', 'photodiode_dark_action', 'photodiode_runtime_channel'],
    'lasers.h': ['hispec_laser_flux_estimate'],
    'attenuator.h': ['attenuator_transmission_estimate'],
    'throughput_monitor.c': ['throughput_state'],
}.items():
    for name in names:
        source += block(file, 'struct ' + name + ' {')
source += r'''
static struct photodiode_runtime_channel pd_runtime[2];
static int attenuators[6];
static double written_tx;
static bool attenuator_set_linear(int *a, double tx) { (void)a; written_tx=tx; return true; }
static int hispec_laser_set_output_percent_autooff(enum hispec_laser_id l, double p, unsigned t)
{ (void)l; (void)p; (void)t; return 0; }
static int laser_estimate_flux(enum hispec_laser_id l,
                              struct hispec_laser_flux_estimate *f)
{ (void)l; f->flux_ph_s=100; return 0; }
static double photodiode_power_uw_from_mv(double mv, const struct app_pd_channel_settings *s)
{ (void)s; return mv; }
static void pd_window_result_clear(struct photodiode_window_result *w) { memset(w,0,sizeof(*w)); }
'''
for name in ['pd_window_recompute', 'pd_window_add_sample', 'pd_throughput_recompute']:
    source += block('photodiode.c', 'static void ' + name + '(')
source += block('photodiode.c', 'void photodiode_set_throughput_reference(')
source += block('throughput_monitor.c', 'static bool autolevel_adjust(')
source += r'''
static void close_to(double a, double b) { assert(fabs(a-b) < 1e-10 * MAX(1.0, fabs(b))); }
int main(void) {
    struct photodiode_runtime_channel *r = &pd_runtime[0];
    struct app_pd_channel_settings settings = {.dark.rms_mv=0.5};
    r->fixed_window.target_samples=25;
    /* All 25 ADC samples, spanning several different process inputs. */
    for (int i=0; i<25; ++i) {
        double scale = i < 12 ? 0.001 : 0.003;
        r->references[r->fixed_window.index] = (struct photodiode_throughput_reference){scale, 0.1};
        pd_window_add_sample(&r->fixed_window, 0, 1, 0.01/scale, 0.01/scale, &settings, i*20);
        pd_throughput_recompute(r, &settings);
        close_to(r->throughput.mean, 0.01);
        assert(r->throughput.samples == i+1);
    }
    close_to(r->throughput.pd_error, 0.5*(12*0.001+13*0.003)/25);
    close_to(r->throughput.error, hypot(r->throughput.pd_error, 0.001));
    photodiode_set_throughput_reference(0, (struct photodiode_throughput_reference){0.004,0.1}, false);
    assert(r->throughput.samples == 25); /* Input changes retain history. */
    photodiode_set_throughput_reference(0, (struct photodiode_throughput_reference){0.002,0.1}, true);
    assert(r->throughput.samples == 0 && isnan(r->throughput.mean));
    assert(r->fixed_window.filled == 25); /* Restart leaves raw diagnostics alone. */
    /* At constant input, normalization commutes with the previous PD statistics. */
    for (int i=0; i<25; ++i) {
        double mv=100+i;
        r->references[r->fixed_window.index]=r->reference;
        pd_window_add_sample(&r->fixed_window, i==2 ? -1 : 0, 1, mv, mv, &settings, i*20);
    }
    pd_throughput_recompute(r,&settings);
    assert(r->throughput.samples == 24);
    close_to(r->throughput.mean, r->fixed_window.current.mean_net_mv*0.002);
    close_to(r->throughput.pd_error, r->fixed_window.current.mean_net_err_mv*0.002);
    close_to(r->throughput.error, hypot(r->throughput.pd_error, r->throughput.mean*0.1));
    memset(r->fixed_window.good,0,sizeof(r->fixed_window.good));
    pd_throughput_recompute(r,&settings);
    assert(r->throughput.samples == 0 && isnan(r->throughput.error));

    struct throughput_state state = {.acquiring=true, .level_percent=100};
    struct photodiode_channel_status pd = {.net_mv=0,
        .fixed_window={.valid=true, .mean_net_mv=0, .end_ms=100}};
    struct attenuator_transmission_estimate atten = {.linear=0.001};
    assert(autolevel_adjust(&state,&pd,&atten)); /* Immediate startup at 100 ms. */
    close_to(written_tx, 0.003);
    pd.net_mv=1000; pd.fixed_window.mean_net_mv=1000;
    assert(!autolevel_adjust(&state,&pd,&atten));
    assert(!state.acquiring);
    state.input_changed_ms=100;
    pd.fixed_window.mean_net_mv=300; pd.fixed_window.end_ms=599;
    assert(!autolevel_adjust(&state,&pd,&atten)); /* Ordinary mean waits full window. */
    pd.fixed_window.end_ms=600;
    assert(autolevel_adjust(&state,&pd,&atten));
    state.input_changed_ms=600; pd.fixed_window.end_ms=620;
    pd.net_mv=0;
    for (int i=0; i<4; ++i) assert(!autolevel_adjust(&state,&pd,&atten));
    assert(autolevel_adjust(&state,&pd,&atten)); /* Low-count bypass survives. */
    pd.net_mv=2000; pd.raw=INT16_MAX; state.high_count=0;
    for (int i=0; i<4; ++i) assert(!autolevel_adjust(&state,&pd,&atten));
    assert(autolevel_adjust(&state,&pd,&atten)); /* Bright wins over stale low mean. */
    close_to(written_tx,atten.linear/3);
    puts("throughput C regressions passed");
}
'''
with tempfile.TemporaryDirectory() as tmp:
    cfile = Path(tmp) / 'check.c'
    exe = Path(tmp) / 'check'
    cfile.write_text(source)
    subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror', str(cfile), '-lm', '-o', str(exe)], check=True)
    subprocess.run([str(exe)], check=True)

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
#define float_is_valid(x) isfinite(x)
#define float_is_positive(x) (isfinite(x) && (x)>0)
static int laser_lock;
static void k_mutex_lock(int *p,int t) { (void)p; (void)t; }
static void k_mutex_unlock(int *p) { (void)p; }
static void ensure_laser_runtime_settings_locked(void) {}
'''
for line in (ROOT/'app/src/lasers.h').read_text().splitlines():
    if line.startswith('#define HISPEC_LASER_DEFAULT_'):
        source += line + '\n'
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
for marker in ['static int validate_laser_settings(', 'static bool laser_driver_settings_differ(',
               'double hispec_laser_estimate_power_mw(', 'double hispec_laser_estimate_wavelength_nm(',
               'int laser_estimate_flux(']:
    source += block('lasers.c', marker)
for marker in ['static void laser_policy_from_settings(', 'static void app_nvs_apply_laser_policy(']:
    source += block('app_settings.c', marker)
source += r'''
int main(void) {
    struct app_settings_snapshot defaults;
    laser_defaults(&defaults);
    double expected[] = {0.435675,0.086320,0.086320,0.086320,0.086320,0.029481};
    for (int i=0;i<HISPEC_LASER_COUNT;++i) {
        struct app_laser_channel_settings *s=&defaults.laser.channel[i], restored=*s;
        struct app_nvs_laser_policy stored;
        struct hispec_laser_driver_profile profile={.properties=default_laser_props[i]};
        assert(s->fractional_noise == 0.03);
        assert(fabs(s->constant_noise_mw-expected[i])<1e-12);
        assert(validate_laser_settings(&profile,s)==0);
        s->fractional_noise=0.02+i*0.01;
        s->constant_noise_mw=0.01+i*0.01;
        assert(!laser_driver_settings_differ(s,&restored));
        laser_policy_from_settings(&stored,s);
        app_nvs_apply_laser_policy(&restored,&stored);
        assert(restored.fractional_noise==s->fractional_noise);
        assert(restored.constant_noise_mw==s->constant_noise_mw);
        laser_settings[i]=*s;
        laser_output_estimate[i].valid=true;
        laser_output_estimate[i].current_ma=s->properties.max_current_ma;
        laser_output_estimate[i].tec_temperature_c=s->properties.operating_temp_c;
        struct hispec_laser_flux_estimate estimate;
        assert(laser_estimate_flux(i,&estimate)==0);
        double power=(s->properties.max_current_ma-s->properties.threshold_current_ma)*s->properties.efficiency_mw_per_ma;
        assert(fabs(estimate.power_mw-power)<1e-12);
        double nominal_flux=estimate.flux_ph_s;
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
        restored.fractional_noise=NAN;
        assert(validate_laser_settings(&profile,&restored)==-ERANGE);
        restored.fractional_noise=0;
        restored.constant_noise_mw=INFINITY;
        assert(validate_laser_settings(&profile,&restored)==-ERANGE);
    }
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
for key in ('fractional_noise','constant_noise_mw'):
    for value in (-1, float('nan'), float('inf')):
        try:
            client.laser_settings(host.LASER_NAMES[0], **{key:value})
        except host.HispecFibError:
            pass
        else:
            raise AssertionError(f'accepted invalid {key}: {value}')
settings = {field.name:0 for field in dataclasses.fields(host.LaserSettings)}
settings.update(model='test',expected_serial=123,tec_pid={'p':0,'i':0,'d':0},
                operating_temp_range_c=[17,38],fractional_noise=0.03,constant_noise_mw=0.435675)
client._request_json = lambda command, payload: {'name':payload['name'], 'settings':settings}
result = client.laser_settings(host.LASER_NAMES[0])
assert result.fractional_noise == 0.03 and result.constant_noise_mw == 0.435675
binary = host._THROUGHPUT_BINARY.pack(b'yj_m',123,*[0.01,0.002,0.001,20,1,2000,300,1,1,0.2],12,*[10,9,8,0.1,50,6,1028],1,2)
sample = host.decode_throughput_payload(binary)
jsample = host.decode_throughput_payload(json.dumps({'tp':0.01,'tp_err':0.002,'tp_rms_err':0.001}))
assert (sample.tp,sample.tp_err,sample.tp_rms_err) == (jsample.tp,jsample.tp_err,jsample.tp_rms_err)
print('Python laser settings and JSON/binary telemetry checks passed')

# Physical-model uncertainty and accepted-fit installation. Stub only hardware
# and flash writes; exercise the production replacement/rollback code.
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
for line in (ROOT/'app/src/attenuator.h').read_text().splitlines():
    if line.startswith(('#define ATTENUATOR_', '#define FVOA_DEFAULT_')):
        source += line+'\n'
for file, names in {
    'attenuator.h': ['attenuator_model_coeffs','attenuator_dac_cfg','attenuator_status',
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
static void app_settings_update_attenuator_channel(int i,const struct app_attenuator_channel_settings *s,bool persist)
{ (void)i; assert(persist); saved=*s; ++saves; }
static bool attenuator_get(struct attenuator *a, struct attenuator_status *out)
{ (void)a; *out=(struct attenuator_status){.linear=sample_tx,.attenuation_db=30,.voltage1=123,.voltage2=456}; return true; }
static bool attenuator_set_db(struct attenuator *a,double db) { (void)a; (void)db; return !fail_write; }
static double attenuator_model_floor_linear(const struct attenuator_model_coeffs *c)
{ return pow(10,-c->max_atten_db/10); }
static double attenuator_model_voltage_to_db(const struct attenuator_model_coeffs *c,float mv)
{ (void)mv; return c->max_atten_db; }
/* JSON extraction stubs select absent/explicit RMS; the parser's replacement
 * semantics and validation, not the shared JSON library, are under test here. */
static int rms_status=COO_JSON_EXTRACT_MISSING;
static double parsed_rms;
static int coo_json_extract_object(const char *j,const char *key,char *out,size_t n)
{ (void)j; (void)key; (void)n; out[0]=0; return COO_JSON_EXTRACT_OK; }
static int coo_json_extract_double(const char *j,const char *key,double *out) {
    (void)j;
    if (!strcmp(key,"rms_db")) {if (rms_status==0) *out=parsed_rms; return rms_status;}
    if (!strcmp(key,"fvoa_50pct_mv")) *out=2500;
    if (!strcmp(key,"slope_inv_fvoa_mv")) *out=0.002;
    if (!strcmp(key,"max_atten_db")) *out=55;
    if (!strcmp(key,"gain")) *out=1.533;
    return COO_JSON_EXTRACT_OK;
}
static int coo_json_extract_double_array(const char *j,const char *key,double *out,size_t n,size_t *len)
{ (void)j;(void)key;(void)out;(void)n;(void)len;return COO_JSON_EXTRACT_MISSING; }
'''
for marker in ['static bool attenuator_model_coeff_valid(', 'bool attenuator_model_coefficients_valid(',
               'bool attenuator_estimate_transmission(', 'int attenuator_apply_coefficients_preserve_db(']:
    source += block('attenuator.c',marker)
source += block('app_settings.c','static bool attenuator_channel_valid(')
source += block('attenuator_calibration.c','static int apply_fit_to_settings_locked(')
source += block('attenuator_command.c','static int parse_attenuator_coeff_object(')
source += r'''
int main(void) {
    struct attenuator *a=&attenuators[0];
    a->coeff1=(struct attenuator_model_coeffs){.fvoa_50pct_mv=2500,.slope_inv_fvoa_mv=0.002,
        .max_atten_db=55,.gain=1.533,.rms_db=ATTENUATOR_DEFAULT_RMS_DB};
    a->coeff2=a->coeff1;
    struct attenuator_transmission_estimate out;
    for(int i=0;i<3;++i) {
        sample_tx=pow(10,-i*3);
        assert(attenuator_estimate_transmission(a,&out));
        assert(out.linear==sample_tx && out.attenuation_db==30 && out.voltage1==123);
        assert(fabs(out.linear_err/out.linear-log(10)/10*hypot(2,2))<1e-12);
    }
    cal.persistent=true;
    for(int i=0;i<2;++i) cal.fit[i]=(struct attenuator_calibration_fit_metrics){
        .accepted=true,.fvoa_50pct_mv=2600+i,.slope_inv_fvoa_mv=0.003,.max_atten_db=50,
        .rms_db=0.75+i};
    assert(apply_fit_to_settings_locked()==0 && saves==1);
    assert(saved.physical[0].rms_db==0.75 && saved.physical[1].rms_db==1.75);
    assert(a->coeff1.rms_db==0.75 && a->coeff2.rms_db==1.75);
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
    puts("attenuator uncertainty C regressions passed");
}
'''
with tempfile.TemporaryDirectory() as tmp:
    cfile, exe = Path(tmp)/'atten.c', Path(tmp)/'atten'
    cfile.write_text(source)
    subprocess.run(['cc','-std=c11','-Wall','-Wextra','-Werror',str(cfile),'-lm','-o',str(exe)],check=True)
    subprocess.run([str(exe)],check=True)

coeff = {'fvoa_50pct_mv':2500,'slope_inv_fvoa_mv':0.002,'max_atten_db':55,
         'gain':1.533,'correction_coeff':[0,0,0,0],'rms_db':0.75}
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
assert 'rms_db' not in host._atten_physical_coeff_payload('dac1',(2500,0.002,55))
print('Python attenuator RMS checks passed')

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
settings_text = (ROOT/'app/src/app_settings.c').read_text()
start = settings_text.rfind('static const struct {',0,settings_text.index('} default_route_losses[]'))
source += settings_text[start:settings_text.index('\n};',start)+3] + '\n'
for marker in ['static bool route_loss_record_valid(', 'static void app_nvs_load_route_loss(',
               'static int route_loss_record_index_locked(', 'int app_settings_get_route_loss(',
               'int app_settings_set_route_loss(']:
    source += block('app_settings.c',marker)
source += block('mems_command.c','static int route_loss_append_loss(')
source += r'''
int main(void) {
    const char *lasers[]={"1028y","1270j","1430yj","1430hk","1510h","2330k"};
    const char *inputs[]={"yj_laser","yj_laser","yj_1430","hk_1430","hk_laser","hk_laser"};
    const double db[]={73,40,100,100,33,3};
    const unsigned switches[]={2,2,3,3,2,2};
    double tx; char route[24],json[64]; size_t off;
    for(unsigned i=0;i<6;i++) {
        const char *channel=i<3?"yj":"hk";
        double expected=pow(i<3?.88:.83,switches[i])*pow(10,-db[i]/10);
        for(unsigned j=0;j<2;j++) {
            snprintf(route,sizeof(route),"%s_to_%s_%s",inputs[i],channel,j?"fei":"ao");
            assert(app_settings_get_route_loss(route,lasers[i],&tx)==0);
            assert(fabs(tx/expected-1)<1e-11);
            off=0; assert(route_loss_append_loss(json,sizeof(json),&off,tx)==0);
            double loss=strtod(json,NULL);
            assert(loss<1 && fabs((1-loss)/tx-1)<2e-6);
            printf("%s %s %s\n",route,lasers[i],json);
            snprintf(route,sizeof(route),"%s_%s_to_%s_pd",channel,j?"sm":"mm",channel);
            assert(app_settings_get_route_loss(route,lasers[i],&tx)==0 && tx==(j?.60:.98));
        }
    }
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
    payload=b'{"code":"test","msg":"visible warning","uptime_s":1}'))
client._on_message(None,None,SimpleNamespace(topic=f'cmd/{client.device}/resp/laser',
    payload=b'{"status":"ok"}',properties=None))
assert any('visible warning' in record.getMessage() for record in logs)
assert any('resp/laser' in record.getMessage() for record in logs)

with plt.ioff():
    empty = host.ThroughputMonitor(client,channel='yj')
    fig,anim=empty.plot_live(max_points=10)
    anim._func(0); fig.canvas.draw(); plt.close(fig)
    rec=monitor.to_recarray()[:5].copy()
    rec.t_ms=np.arange(5)*100+1000
    rec.tp=[.01,0,-1,np.nan,2]
    rec.tp_err=[.002,0,.1,np.nan,.5]
    rec.pd_mean_net_mv=[100,0,-5,100,100]
    rec.pd_mean_net_err_mv=[1,0,1,np.nan,2]
    monitor._samples.clear()
    monitor._samples.extend(rec.tolist())
    # Interleave the other channel; selecting YJ must still preserve its time base.
    monitor._samples.extend([dataclasses.replace(sample,channel='hk_m').as_tuple()]*3)
    fig,anim=monitor.plot_live(channel='yj',max_points=5)
    anim._func(0); fig.canvas.draw()
    tp,pd,snr,drive,flux,atten=fig.axes
    np.testing.assert_allclose(tp.lines[0].get_ydata(),[.01,np.nan,np.nan,np.nan,2],equal_nan=True)
    np.testing.assert_allclose(tp.lines[0].get_xdata(),np.arange(5)*.1)
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
    widget.observe(lambda change:renders.append(time.monotonic()),names='outputs')
    for i in range(550): client.logger.info('message %d',i)
    for _ in range(100):
        client._on_message(None,None,SimpleNamespace(topic=f'dt/{client.device}/yj_tput',payload=b'no raw bytes'))
    assert not renders  # The MQTT/logging thread never writes the widget.
    await asyncio.sleep(.55)
    text=widget.outputs[0]['text']
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
print('Protocol filtering, dashboard math/rendering, and notebook lifecycle checks passed')

# Exercise production laser current/stop paths with counted Modbus operations.
laser_source = r'''
#include <assert.h>
#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>
#include <errno.h>
#include <math.h>
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
typedef struct {double max_current_ma; double operating_temp_c;} laserprops_t;
typedef struct {unsigned node_id; bool io_failed;} maiman_driver_t;
struct hispec_laser_driver_profile {enum hispec_laser_id id; const char *name; unsigned node_id;};
struct on_time_runtime {bool active;};
static int laser_lock;
static bool bank_power_requested_enabled=true;
static int writes, starts, prepares, stops;
static bool fail_write, fail_stop;
static int64_t laser_autooff_deadline_ms[1];
static struct on_time_runtime laser_current_runtime[1],laser_tec_runtime[1];
static const laserprops_t props={250,25};
static const struct hispec_laser_driver_profile profile={0,"test",1};
static int profile_for_id(enum hispec_laser_id id, const struct hispec_laser_driver_profile **p)
{(void)id; *p=&profile;return 0;}
static void k_mutex_lock(int *p,int t){(void)p;(void)t;}
static void k_mutex_unlock(int *p){(void)p;}
static int laser_lock_with_timeout(int t){(void)t;return 0;}
static const laserprops_t *runtime_props_locked(enum hispec_laser_id id){(void)id;return &props;}
static bool float_is_valid(double x){return isfinite(x);}
static void ensure_laser_runtime_settings_locked(void){}
static void maiman_init(maiman_driver_t *d,unsigned n){d->node_id=n;d->io_failed=false;}
static int prepare_to_operate_locked(const struct hispec_laser_driver_profile *p,maiman_driver_t *d,bool v)
{(void)v;++prepares;maiman_init(d,p->node_id);return 0;}
static int verify_driver_locked(const struct hispec_laser_driver_profile *p,maiman_driver_t *d,void *o)
{(void)p;(void)d;(void)o;return 0;}
static bool maiman_set_current(maiman_driver_t *d,double x){(void)d;(void)x;++writes;return !fail_write;}
static bool maiman_start_device(maiman_driver_t *d){(void)d;++starts;return true;}
static bool maiman_stop_device(maiman_driver_t *d){(void)d;++stops;return !fail_stop;}
static bool maiman_stop_tec(maiman_driver_t *d){(void)d;return true;}
static void commit_current_runtime_locked(enum hispec_laser_id id,bool p)
{(void)p;laser_current_runtime[id].active=false;}
static void on_time_runtime_update_locked(struct on_time_runtime *r,unsigned n,enum hispec_laser_id id,bool active)
{(void)n;r[id].active=active;}
'''
laser_source += block('lasers.c','struct laser_output_estimate_state {')
laser_source += 'static struct laser_output_estimate_state laser_output_estimate[1];\n'
for marker in ['static void output_estimate_set_locked(', 'static void invalidate_output_locked(',
               'static bool output_ready_locked(']:
    laser_source += block('lasers.c',marker)
# The stop function also has a forward declaration; select its definition.
laser_text=(ROOT/'app/src/lasers.c').read_text()
stop_marker='static int stop_output_locked(const struct hispec_laser_driver_profile *profile, bool stop_tec)\n{'
laser_source += block('lasers.c',stop_marker)
laser_source += block('lasers.c','int hispec_laser_set_current_ma(')
laser_source += r'''
int main(void){
 assert(hispec_laser_set_current_ma(0,100)==0);
 assert(prepares==1 && writes==1 && starts==1);
 assert(hispec_laser_set_current_ma(0,150)==0);
 assert(hispec_laser_set_current_ma(0,50)==0);
 assert(prepares==1 && writes==3 && starts==1);
 assert(laser_output_estimate[0].current_ma==50);
 fail_write=true;
 assert(hispec_laser_set_current_ma(0,60)==-EIO);
 assert(!laser_output_estimate[0].valid && !laser_output_estimate[0].prepared);
 assert(laser_current_runtime[0].active); /* A failed write cannot prove emission stopped. */
 fail_write=false;
 assert(hispec_laser_set_current_ma(0,60)==0 && prepares==2 && starts==2);
 fail_stop=true;
 assert(hispec_laser_set_current_ma(0,0)==-EIO);
 assert(laser_current_runtime[0].active && !laser_output_estimate[0].valid);
 fail_stop=false;
 assert(hispec_laser_set_current_ma(0,0)==0);
 assert(!laser_current_runtime[0].active && laser_output_estimate[0].current_ma==0);
 assert(hispec_laser_set_current_ma(0,260)==-ERANGE);
 return 0;
}
'''
with tempfile.TemporaryDirectory() as tmp:
    cfile=Path(tmp)/'laser.c';exe=Path(tmp)/'laser'
    cfile.write_text(laser_source)
    subprocess.run(['cc','-std=c11','-Wall','-Wextra','-Werror',str(cfile),'-lm','-o',str(exe)],check=True)
    subprocess.run([str(exe)],check=True)
print('laser current/stop regressions passed')

maiman_source=r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#define LOG_ERR(...) ((void)0)
#define LOG_INF(...) ((void)0)
typedef struct {uint8_t node_id;bool verbose;bool io_failed;} maiman_driver_t;
static int maiman_client_iface=0,reply;
static const char *maiman_register_name(uint16_t a){(void)a;return "test";}
static int modbus_read_holding_regs(int i,uint8_t n,uint16_t a,uint16_t *v,int c)
{(void)i;(void)n;(void)a;(void)c;*v=42;return reply;}
static int modbus_write_holding_regs(int i,uint8_t n,uint16_t a,uint16_t *v,int c)
{(void)i;(void)n;(void)a;(void)v;(void)c;return reply;}
'''
for marker in ['void maiman_init_verbose(', 'bool maiman_read_u16(', 'bool maiman_write_u16(']:
    maiman_source += block('maiman.c',marker)
maiman_source += r'''
int main(void){
 maiman_driver_t d;uint16_t v;
 maiman_init_verbose(&d,1,false);
 reply=2;assert(!maiman_read_u16(&d,4,&v) && d.io_failed);
 reply=0;assert(maiman_read_u16(&d,4,&v) && d.io_failed);
 maiman_init_verbose(&d,1,false);assert(!d.io_failed);
 reply=-5;assert(!maiman_write_u16(&d,8,1) && d.io_failed);
 maiman_init_verbose(&d,1,false);
 reply=3;assert(!maiman_write_u16(&d,8,1) && d.io_failed);
 return 0;
}
'''
# LOG_* macros consume arguments on target; stubs intentionally do not.
with tempfile.TemporaryDirectory() as tmp:
    cfile=Path(tmp)/'maiman.c';exe=Path(tmp)/'maiman'
    cfile.write_text(maiman_source)
    subprocess.run(['cc','-std=c11','-Wall','-Wextra','-Werror','-Wno-unused-function',str(cfile),'-o',str(exe)],check=True)
    subprocess.run([str(exe)],check=True)
print('Maiman exception and sticky failure regressions passed')
