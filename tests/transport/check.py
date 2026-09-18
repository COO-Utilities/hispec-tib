"""Deterministic interleavings of the actual patched RTU C functions.

Only the kernel/UART boundary is simulated. Also exercise the patch apply command
against disposable checkouts; never reset or modify the developer's checkout.
"""
from pathlib import Path
import hashlib
import subprocess
import tempfile
import yaml

ROOT = Path(__file__).resolve().parents[2]
ZEPHYR = ROOT.parent / 'zephyr'


def block(path, marker):
    text = path.read_text()
    start = text.index(marker)
    opening = text.index('{', start)
    depth, end = 1, opening + 1
    while depth:
        depth += (text[end] == '{') - (text[end] == '}')
        end += 1
    return text[start:end] + '\n'


def run_c(source, name):
    with tempfile.TemporaryDirectory() as tmp:
        src, exe = Path(tmp)/f'{name}.c', Path(tmp)/name
        src.write_text(source)
        subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror',
                        '-Wno-unused-function', str(src), '-o', str(exe)], check=True)
        subprocess.run([str(exe)], check=True)


serial = ZEPHYR/'subsys/modbus/modbus_serial.c'
core = ZEPHYR/'subsys/modbus/modbus_core.c'
source = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <errno.h>
#define CONFIG_MODBUS_SERIAL_ASYNC_API 0
#define CONFIG_MODBUS_SERIAL 1
#define CONFIG_MODBUS_RAW_ADU 0
#define CONFIG_MODBUS_SERVER 0
#define CONFIG_MODBUS_ASCII_MODE 0
#define CONFIG_MODBUS_BUFFER_SIZE 32
#define IS_ENABLED(x) (x)
#define MODBUS_STATE_RX_ENABLED 1
#define MODBUS_MODE_RTU 0
#define MODBUS_MODE_ASCII 1
#define MODBUS_MODE_RAW 2
#define MODBUS_RTU_MIN_MSG_SIZE 4
#define MODBUS_ASCII_START_FRAME_CHAR ':'
#define MODBUS_ASCII_END_FRAME_CHAR2 '\n'
#define K_USEC(x) (x)
#define K_NO_WAIT 0
#define LOG_ERR(...) ((void)0)
#define LOG_WRN(...) ((void)0)
#define LOG_DBG(...) ((void)0)
#define CONTAINER_OF(p,t,m) ((t *)((char *)(p)-offsetof(t,m)))
struct device {int unused;};
struct k_timer {bool armed;void *ctx;};
struct k_work {bool queued,running;};
struct k_work_sync {int unused;};
struct k_sem {int count;};
struct adu {uint8_t unit_id,fc,data[32];uint16_t length,crc;};
struct modbus_serial_config {
 const struct device *dev;const void *re,*de;unsigned rtu_timeout;
 struct k_timer rtu_timer;uint8_t uart_buf[32],*uart_buf_ptr;unsigned uart_buf_ctr;
};
struct modbus_context {
 struct modbus_serial_config *cfg;bool client;int mode,state,rx_adu_err,rxwait_to;
 struct k_work server_work;struct k_sem client_wait_sem;struct adu rx_adu;
};
static bool rx_enabled,tx_enabled;static unsigned irq_depth;
static int scenario,submits,cancels;static uint8_t fifo[32];static unsigned fifo_count;
static unsigned irq_lock(void){return irq_depth++;}
static void irq_unlock(unsigned key){assert(irq_depth==key+1);irq_depth=key;}
static void atomic_set_bit(int *v,int n){*v|=1<<n;}
static void atomic_clear_bit(int *v,int n){*v&=~(1<<n);}
static bool atomic_test_bit(int *v,int n){return !!(*v&(1<<n));}
static void gpio_pin_set_dt(const void *p,int v){(void)p;(void)v;}
static void uart_irq_rx_disable(const struct device *d){(void)d;rx_enabled=false;}
static void uart_irq_tx_disable(const struct device *d){(void)d;tx_enabled=false;}
static void uart_rx_disable(const struct device *d){(void)d;assert(false);}
static void k_timer_stop(struct k_timer *t){t->armed=false;}
static void k_timer_start(struct k_timer *t,int a,int b){(void)a;(void)b;t->armed=true;}
static void *k_timer_user_data_get(struct k_timer *t){return t->ctx;}
static int uart_fifo_read(const struct device *d,uint8_t *b,unsigned n){
 (void)d;if(n>fifo_count)n=fifo_count;memcpy(b,fifo,n);
 memmove(fifo,fifo+n,fifo_count-n);fifo_count-=n;return (int)n;
}
static void k_sem_reset(struct k_sem *s){s->count=0;}
static void k_sem_give(struct k_sem *s){s->count=1;}
static void modbus_rx_handler(struct k_work *w);
static void cb_handler_rx(struct modbus_context *ctx);
static void modbus_work_submit(struct k_work *w){
 struct modbus_context *c=CONTAINER_OF(w,struct modbus_context,server_work);
 w->queued=true;++submits;
 /* Inject a byte at the handoff, before the delayed worker can run. */
 if(scenario==1){
  unsigned count=c->cfg->uart_buf_ctr;fifo[0]=0x55;fifo_count=1;
  cb_handler_rx(c);assert(c->cfg->uart_buf_ctr==count);
  assert(!c->cfg->rtu_timer.armed);
 }
}
static void k_work_cancel_sync(struct k_work *w,struct k_work_sync *s){
 (void)s;++cancels;assert(!irq_depth && !rx_enabled && !tx_enabled);
 w->queued=false;
 /* A worker already parsing may finish and signal while cancellation waits. */
 if(w->running){modbus_rx_handler(w);w->running=false;}
}
static uint16_t sys_get_le16(const uint8_t *p){return p[0]|((uint16_t)p[1]<<8);}
static uint16_t crc16_ansi(const uint8_t *p,unsigned n){
 uint16_t crc=0xffff;while(n--){crc^=*p++;for(int b=0;b<8;b++)crc=(crc>>1)^((crc&1)?0xa001:0);}return crc;
}
static int modbus_ascii_rx_adu(struct modbus_context *c){(void)c;return 0;}
static int modbus_raw_rx_adu(struct modbus_context *c){(void)c;return 0;}
static bool modbus_server_handler(struct modbus_context *c){(void)c;return false;}
static void modbus_serial_rx_enable(struct modbus_context *c){(void)c;rx_enabled=true;}
static void modbus_tx_adu(struct modbus_context *c);
static int k_sem_take(struct k_sem *s,int t);
'''
for marker in ['static void modbus_serial_tx_off(', 'static void modbus_serial_rx_fifo_drain(',
               'static void modbus_serial_rx_off(', 'void modbus_serial_rx_disable(',
               'static int modbus_rtu_rx_adu(', 'int modbus_serial_rx_adu(',
               'static void cb_handler_rx(', 'static void rtu_tmr_handler(',
               'void modbus_serial_client_quiesce(']:
    source += block(serial, marker)
source += block(core, 'static void modbus_rx_handler(')
source += block(core, 'int modbus_tx_wait_rx_adu(')
source += r'''
static struct modbus_serial_config cfg;
static struct modbus_context ctx;
static void receive_frame(void){
 uint8_t b[]={5,3,2,0,42,0,0};uint16_t crc=crc16_ansi(b,5);
 b[5]=crc;b[6]=crc>>8;memcpy(fifo,b,7);fifo_count=7;
 rx_enabled=true;atomic_set_bit(&ctx.state,MODBUS_STATE_RX_ENABLED);
 cb_handler_rx(&ctx);assert(cfg.uart_buf_ctr==7 && cfg.rtu_timer.armed);
 rtu_tmr_handler(&cfg.rtu_timer);
}
static void modbus_tx_adu(struct modbus_context *c){
 assert(!irq_depth && !c->server_work.queued && !c->server_work.running);
 assert(!c->client_wait_sem.count && !cfg.uart_buf_ctr && !cfg.rtu_timer.armed);
 tx_enabled=true;rx_enabled=true;atomic_set_bit(&c->state,MODBUS_STATE_RX_ENABLED);
}
static int k_sem_take(struct k_sem *s,int t){
 (void)t;receive_frame();
 assert(!rx_enabled && !cfg.rtu_timer.armed);
 if(scenario==2)return -EAGAIN; /* queued parser at ACK timeout */
 if(scenario==3){ctx.server_work.queued=false;ctx.server_work.running=true;return -EAGAIN;}
 if(scenario==4)cfg.uart_buf[4]^=1; /* preserve CRC failure */
 if(scenario==5)cfg.uart_buf_ctr=2; /* preserve short-frame failure */
 ctx.server_work.queued=false;modbus_rx_handler(&ctx.server_work);
 assert(s->count);s->count=0;return 0;
}
static void reset(void){
 memset(&cfg,0,sizeof(cfg));memset(&ctx,0,sizeof(ctx));
 ctx.cfg=&cfg;ctx.client=true;ctx.mode=MODBUS_MODE_RTU;ctx.rxwait_to=75;
 cfg.rtu_timer.ctx=&ctx;cfg.uart_buf_ptr=cfg.uart_buf;
 rx_enabled=tx_enabled=false;fifo_count=0;
}
int main(void){
 for(scenario=0;scenario<=5;scenario++){
  reset();
  /* Entry must drain unsolicited/old work as well as completion cleanup. */
  ctx.server_work.queued=true;ctx.client_wait_sem.count=1;cfg.rtu_timer.armed=true;
  cfg.uart_buf_ctr=9;rx_enabled=tx_enabled=true;
  int rc=modbus_tx_wait_rx_adu(&ctx);
  int expected=scenario==2||scenario==3?-ETIMEDOUT:scenario==4?-EIO:scenario==5?-EMSGSIZE:0;
  assert(rc==expected && !rx_enabled && !tx_enabled);
  assert(!ctx.server_work.queued && !ctx.server_work.running && !ctx.client_wait_sem.count);
  assert(!cfg.uart_buf_ctr && cfg.uart_buf_ptr==cfg.uart_buf && !cfg.rtu_timer.armed);
  if(!rc)assert(ctx.rx_adu.data[2]==42 && ctx.rx_adu.unit_id==5);
  int saved=scenario;scenario=0;assert(!modbus_tx_wait_rx_adu(&ctx));scenario=saved;
 }
 /* Full RX buffer already disabled reception: timer must still submit it. */
 reset();cfg.uart_buf_ctr=CONFIG_MODBUS_BUFFER_SIZE;int previous=submits;
 rtu_tmr_handler(&cfg.rtu_timer);assert(submits==previous+1);
 /* Non-client and non-RTU cleanup remain untouched. */
 int previous_cancels=cancels;ctx.client=false;modbus_serial_client_quiesce(&ctx);
 ctx.client=true;ctx.mode=MODBUS_MODE_ASCII;modbus_serial_client_quiesce(&ctx);
 ctx.mode=MODBUS_MODE_RAW;modbus_serial_client_quiesce(&ctx);
 assert(cancels==previous_cancels);
 return 0;
}
'''
run_c(source, 'rtu')
print('RTU frame handoff, queued/running timeout cleanup, error preservation and next-request checks passed')

# The actual lazy configure/conversion/sample path must lock every raw bus
# operation and release before the 750 ms conversion sleep, including failures.
ds = ZEPHYR/'drivers/sensor/maxim/ds18b20/ds18b20.c'
source = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <errno.h>
#define LOG_ERR(...) ((void)0)
#define LOG_DBG(...) ((void)0)
#define __ASSERT_NO_MSG(x) assert(x)
#define type_ds18b20 1
#define DS18B20_CMD_CONVERT_T 0x44
struct device {const void *config;void *data;const char *name;};
struct rom {int family;};
struct w1_slave_config {struct rom rom;};
struct ds18b20_config {const struct device *bus;int family,chip,resolution;};
struct ds18b20_data {struct w1_slave_config config;int scratchpad;bool lazy_loaded;};
enum sensor_channel {SENSOR_CHAN_ALL,SENSOR_CHAN_AMBIENT_TEMP};
static int locked,presence=1,convert_error,sleeps;
static int w1_lock_bus(const struct device *d){(void)d;assert(!locked++);return 0;}
static int w1_unlock_bus(const struct device *d){(void)d;assert(locked--==1);return 0;}
static int w1_reset_bus(const struct device *d){(void)d;assert(locked);return presence;}
static int w1_get_slave_count(const struct device *d){(void)d;return 1;}
static uint64_t w1_rom_to_uint64(const struct rom *r){return r->family;}
static int w1_read_rom(const struct device *d,struct rom *r){(void)d;r->family=1;return 0;}
static void ds18b20_set_resolution(const struct device *d,int r){(void)d;(void)r;}
static int ds18b20_write_scratchpad(const struct device *d,int s){(void)d;(void)s;return 0;}
static int w1_reset_select(const struct device *d,struct w1_slave_config *s){(void)d;(void)s;assert(locked);return convert_error;}
static int w1_write_byte(const struct device *d,int b){(void)d;(void)b;assert(locked);return 0;}
static int measure_wait_ms(const struct device *d){(void)d;return 750;}
static void k_msleep(int t){assert(t==750 && !locked);++sleeps;}
static int ds18b20_read_scratchpad(const struct device *d,int *s){(void)d;(void)s;assert(!locked);return 0;}
'''
for m in ['static int ds18b20_configure(', 'static int ds18b20_temperature_convert(',
          'static int ds18b20_sample_fetch(']:
    # configure has a forward declaration; start at its definition.
    text = ds.read_text(); marker = text.index(m)
    if ';' in text[marker:text.index('{', marker)]:
        marker = text.index(m, marker+1)
        with tempfile.TemporaryDirectory() as tmp:
            p=Path(tmp)/'ds.c';p.write_text(text[marker:]);source += block(p,m)
    else:
        source += block(ds,m)
source += r'''
int main(void){
 struct ds18b20_config c={.chip=type_ds18b20};struct ds18b20_data d={0};
 struct device dev={.config=&c,.data=&d};
 for(presence=-1;presence<=0;presence++)assert(ds18b20_sample_fetch(&dev,SENSOR_CHAN_ALL)==-ENODEV && !locked);
 presence=1;assert(!ds18b20_sample_fetch(&dev,SENSOR_CHAN_ALL) && d.lazy_loaded && sleeps==1);
 convert_error=-EIO;assert(ds18b20_sample_fetch(&dev,SENSOR_CHAN_ALL)==-EIO && !locked && sleeps==1);
 return 0;
}
'''
run_c(source, 'ds18b20')
print('DS18B20 cold presence/conversion lock and unlocked conversion wait checks passed')

metadata = yaml.safe_load((ROOT/'zephyr/patches.yml').read_text())
assert metadata['checkout-command'] == metadata['clean-command'] == ''
for patch in metadata['patches']:
    path = ROOT/'zephyr/patches'/patch['path']
    data = path.read_bytes()
    assert hashlib.sha256(data).hexdigest() == patch['sha256sum']
    with tempfile.TemporaryDirectory() as tmp:
        checkout = Path(tmp)
        subprocess.run(['git', 'init', '-q', str(checkout)], check=True)
        originals = {}
        for line in data.decode().splitlines():
            if line.startswith('--- a/'):
                name = line[6:]
                originals[name] = subprocess.check_output(['git','show',f'HEAD:{name}'],cwd=ZEPHYR)
                dest=checkout/name;dest.parent.mkdir(parents=True,exist_ok=True);dest.write_bytes(originals[name])
        sentinel=checkout/'unrelated-edit';sentinel.write_text('keep me')
        command=['cmake','-P',str(ROOT/'app/cmake/apply_zephyr_patch.cmake'),str(path)]
        for _ in range(2):
            subprocess.run(command,cwd=checkout,check=True,capture_output=True)
        assert sentinel.read_text()=='keep me'
        # Partial application across files must fail without discarding anything.
        if len(originals)>1:
            name=next(iter(originals));(checkout/name).write_bytes(originals[name])
        else:
            name=next(iter(originals));(checkout/name).write_text('conflicting local content\n')
        before={p:p.read_bytes() for p in checkout.rglob('*.c')}
        result=subprocess.run(command,cwd=checkout,capture_output=True,text=True)
        assert result.returncode and 'neither applicable nor already applied' in result.stderr
        assert all(p.read_bytes()==b for p,b in before.items())
        assert sentinel.read_text()=='keep me'
print('Patch checksums, clean/already-applied and non-destructive conflict/partial checks passed')
