"""Exercise stock Zephyr's public Modbus disable/init lifecycle.

Kernel/UART/GPIO boundaries simulate queued and running executions of the single
shared parser work item. These are forced interleavings, not PCB observations.
"""
from pathlib import Path
import subprocess
import tempfile

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
#define CONFIG_MODBUS_CLIENT 1
#define CONFIG_MODBUS_RAW_ADU 0
#define CONFIG_MODBUS_SERVER 0
#define CONFIG_MODBUS_ASCII_MODE 0
#define CONFIG_UART_USE_RUNTIME_CONFIGURE 1
#define CONFIG_MODBUS_BUFFER_SIZE 32
#define IS_ENABLED(x) (x)
#define MODBUS_STATE_CONFIGURED 0
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
#define LOG_INF(...) ((void)0)
#define ARRAY_SIZE(a) (sizeof(a)/sizeof((a)[0]))
#define CONTAINER_OF(p,t,m) ((t *)((char *)(p)-offsetof(t,m)))
struct device {const char *name;};
struct k_timer {bool armed;void *ctx;void (*handler)(struct k_timer *);};
struct k_work {bool queued,running;void (*handler)(struct k_work *);};
struct k_work_sync {int unused;};
struct k_mutex {int unused;};
struct k_sem {int count;};
struct adu {uint8_t unit_id,fc,data[32];uint16_t length,crc;};
struct modbus_iface_param {int mode;struct {unsigned baud;} serial;unsigned rx_timeout;};
struct modbus_serial_config {
 const struct device *dev;const void *re,*de;unsigned rtu_timeout;
 struct k_timer rtu_timer;uint8_t uart_buf[32],*uart_buf_ptr;unsigned uart_buf_ctr;
};
struct modbus_context {
 struct modbus_serial_config *cfg;bool client;int mode,state,rx_adu_err,rxwait_to,unit_id;
 void *mbs_user_cb;struct k_mutex iface_lock;
 struct k_work server_work;struct k_sem client_wait_sem;struct adu rx_adu;
};
static struct modbus_serial_config cfg;
static struct modbus_context mb_ctx_tbl[1]={{.cfg=&cfg}};
static bool rx_enabled,tx_enabled;
static int scenario,cancels,transmits,parsed;static uint8_t fifo[32];static unsigned fifo_count;
static void atomic_set_bit(int *v,int n){*v|=1<<n;}
static void atomic_clear_bit(int *v,int n){*v&=~(1<<n);}
static bool atomic_test_bit(int *v,int n){return !!(*v&(1<<n));}
static bool atomic_test_and_set_bit(int *v,int n){bool old=atomic_test_bit(v,n);atomic_set_bit(v,n);return old;}
static bool device_is_ready(const struct device *d){(void)d;return true;}
static void gpio_pin_set_dt(const void *p,int v){(void)p;(void)v;}
static void uart_irq_rx_disable(const struct device *d){(void)d;rx_enabled=false;}
static void uart_irq_rx_enable(const struct device *d){(void)d;rx_enabled=true;}
static void uart_irq_tx_disable(const struct device *d){(void)d;tx_enabled=false;}
static bool uart_irq_tx_complete(const struct device *d){(void)d;return true;}
static int uart_fifo_fill(const struct device *d,const uint8_t *b,unsigned n){(void)d;(void)b;return n;}
static int uart_rx_enable(const struct device *d,uint8_t *b,unsigned n,unsigned t)
{(void)d;(void)b;(void)n;(void)t;assert(false);return 0;}
static void uart_rx_disable(const struct device *d){(void)d;assert(false);}
static int uart_fifo_read(const struct device *d,uint8_t *b,unsigned n){
 (void)d;if(n>fifo_count)n=fifo_count;memcpy(b,fifo,n);
 memmove(fifo,fifo+n,fifo_count-n);fifo_count-=n;return (int)n;
}
static void uart_cb_handler(const struct device *d,void *ctx){(void)d;(void)ctx;}
static void uart_cb_async_handler(const struct device *d,void *ctx){(void)d;(void)ctx;}
static int uart_irq_callback_user_data_set(const struct device *d,void (*cb)(const struct device *,void *),void *ctx)
{(void)d;(void)cb;(void)ctx;return 0;}
#define uart_callback_set uart_irq_callback_user_data_set
static int configure_uart(struct modbus_context *c,struct modbus_iface_param *p){(void)c;(void)p;return 0;}
static int configure_gpio(struct modbus_context *c){(void)c;return 0;}
static void k_timer_stop(struct k_timer *t){t->armed=false;}
static void k_timer_start(struct k_timer *t,int a,int b){(void)a;(void)b;t->armed=true;}
static void k_timer_init(struct k_timer *t,void (*h)(struct k_timer *),void *stop)
{assert(!t->armed);(void)stop;t->handler=h;}
static void k_timer_user_data_set(struct k_timer *t,void *ctx){t->ctx=ctx;}
static void *k_timer_user_data_get(struct k_timer *t){return t->ctx;}
static void k_mutex_init(struct k_mutex *m){(void)m;}
static void k_sem_reset(struct k_sem *s){s->count=0;}
static void k_sem_init(struct k_sem *s,int n,int limit){(void)limit;s->count=n;}
static void k_sem_give(struct k_sem *s){s->count=1;++parsed;}
static void k_work_init(struct k_work *w,void (*h)(struct k_work *)){
 assert(!w->queued && !w->running);w->handler=h;
}
static void modbus_work_submit(struct k_work *w){w->queued=true;}
static void k_work_cancel_sync(struct k_work *w,struct k_work_sync *s){
 (void)s;++cancels;assert(!rx_enabled && !tx_enabled && !cfg.rtu_timer.armed);
 w->queued=false;
 /* A running parser may still complete and signal while cancellation waits. */
 if(w->running){w->handler(w);w->running=false;}
}
static uint16_t sys_get_le16(const uint8_t *p){return p[0]|((uint16_t)p[1]<<8);}
static uint16_t crc16_ansi(const uint8_t *p,unsigned n){
 uint16_t crc=0xffff;while(n--){crc^=*p++;for(int b=0;b<8;b++)crc=(crc>>1)^((crc&1)?0xa001:0);}return crc;
}
static int modbus_ascii_rx_adu(struct modbus_context *c){(void)c;return 0;}
static int modbus_raw_rx_adu(struct modbus_context *c){(void)c;return 0;}
static int modbus_raw_init(struct modbus_context *c,struct modbus_iface_param p){(void)c;(void)p;return 0;}
static bool modbus_server_handler(struct modbus_context *c){(void)c;return false;}
static void modbus_tx_adu(struct modbus_context *c);
static int k_sem_take(struct k_sem *s,int t);
'''
for marker in ['static void modbus_serial_tx_off(', 'static void modbus_serial_rx_fifo_drain(',
               'static void modbus_serial_rx_on(', 'static void modbus_serial_rx_off(',
               'void modbus_serial_rx_disable(', 'void modbus_serial_rx_enable(',
               'static int modbus_rtu_rx_adu(', 'int modbus_serial_rx_adu(',
               'static void cb_handler_rx(', 'static void cb_handler_tx(',
               'static void rtu_tmr_handler(', 'int modbus_serial_init(',
               'void modbus_serial_disable(']:
    source += block(serial, marker)
for marker in ['static void modbus_rx_handler(', 'int modbus_tx_wait_rx_adu(',
               'struct modbus_context *modbus_get_context(',
               'static struct modbus_context *modbus_init_iface(',
               'int modbus_init_client(', 'int modbus_disable(']:
    source += block(core, marker)
source += r'''
static void modbus_tx_adu(struct modbus_context *c){
 assert(!c->server_work.queued && !c->server_work.running && !c->client_wait_sem.count);
 assert(!cfg.uart_buf_ctr && !cfg.rtu_timer.armed);
 ++transmits;cfg.uart_buf_ctr=8;tx_enabled=true;cb_handler_tx(c);cb_handler_tx(c);
 assert(!tx_enabled && rx_enabled && !fifo_count); /* TX completion drains idle garbage. */
}
static int k_sem_take(struct k_sem *s,int t){
 assert(t==75000);struct modbus_context *ctx=&mb_ctx_tbl[0];
 if(scenario==1)return -EAGAIN; /* No response. */
 uint8_t b[]={5,3,2,0,42,0,0};uint16_t crc=crc16_ansi(b,5);
 b[5]=crc;b[6]=crc>>8;memcpy(fifo,b,7);fifo_count=scenario==6?2:7;
 cb_handler_rx(ctx);assert(cfg.rtu_timer.armed);
 if(scenario==6)return -EAGAIN; /* Partial frame, framing timer still armed. */
 cfg.rtu_timer.armed=false;cfg.rtu_timer.handler(&cfg.rtu_timer);
 assert(ctx->server_work.queued);
 if(scenario==2)return -EAGAIN; /* Queued parser at timeout. */
 ctx->server_work.queued=false;ctx->server_work.running=true;
 if(scenario==3)return -EAGAIN; /* Parser running at timeout. */
 if(scenario==4)cfg.uart_buf[4]^=1;
 if(scenario==5)cfg.uart_buf_ctr=2;
 ctx->server_work.handler(&ctx->server_work);ctx->server_work.running=false;
 assert(s->count);s->count=0;return 0;
}
int main(void){
 struct modbus_context *ctx=&mb_ctx_tbl[0];
 struct modbus_iface_param param={.mode=MODBUS_MODE_RTU,.serial.baud=115200,.rx_timeout=75000};
 for(scenario=0;scenario<=6;scenario++){
  int sent=transmits,canceled=cancels;
  assert(!modbus_init_client(0,param));
  assert(!rx_enabled && !tx_enabled && transmits==sent);
  int rc=modbus_tx_wait_rx_adu(ctx);
  int expected=scenario==4?-EIO:scenario==5?-EMSGSIZE:scenario==0?0:-ETIMEDOUT;
  assert(rc==expected && transmits==sent+1 && cancels==canceled);
  if(!rc)assert(ctx->rx_adu.data[2]==42 && ctx->rx_adu.unit_id==5);
  int completions=parsed;
  assert(!modbus_disable(0));
  assert(cancels==canceled+1 && !modbus_get_context(0));
  assert(!ctx->server_work.queued && !ctx->server_work.running);
  assert(!rx_enabled && !tx_enabled && !cfg.rtu_timer.armed);
  assert(parsed==completions+(scenario==3)); /* Running work finishes; queued work is canceled. */
  assert(ctx->client_wait_sem.count==(scenario==3));
  /* Remain disabled with idle garbage. Initialization sends nothing and keeps RX off. */
  memset(fifo,0x55,8);fifo_count=8;
  assert(!modbus_init_client(0,param));
  assert(!ctx->client_wait_sem.count && !cfg.uart_buf_ctr && cfg.uart_buf_ptr==cfg.uart_buf);
  assert(!rx_enabled && !tx_enabled && fifo_count==8 && transmits==sent+1);
  /* A new request requires its own response, not the canceled parser's signal. */
  int saved=scenario;scenario=0;
  assert(!modbus_tx_wait_rx_adu(ctx) && transmits==sent+2 && parsed==completions+(saved==3)+1);
  assert(!modbus_disable(0));scenario=saved;
 }
 return 0;
}
'''
run_c(source, 'rtu')
print('Stock Modbus disable/init: queued/running parser, partial/no response, idle garbage and next-request checks passed')
