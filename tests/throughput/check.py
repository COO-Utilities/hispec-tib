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
static int laser_estimate_flux(enum hispec_laser_id l, double a, double b,
                              struct hispec_laser_flux_estimate *f)
{ (void)l; (void)a; (void)b; f->flux_ph_s=100; return 0; }
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
