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
    command, payload = client.set_laser_settings(name, fractional_noise=0.03, constant_noise_mw=0.1, persist=True)
    assert command == 'laser/settings' and payload['settings'] == {'fractional_noise':0.03, 'constant_noise_mw':0.1}
    assert payload['persist']
for key in ('fractional_noise','constant_noise_mw'):
    for value in (-1, float('nan'), float('inf')):
        try:
            client.set_laser_settings(host.LASER_NAMES[0], **{key:value})
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
