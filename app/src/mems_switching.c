/**
 * @file mems_switching.c
 * @brief MEMS pulse timing, duty-cycle quantization, and route-state readback.
 */

#include "mems_switching.h"
#include <ctype.h>
#include <errno.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/util.h>
#include <string.h>
LOG_MODULE_REGISTER(mems_switching, LOG_LEVEL_DBG);

#define MEMS_ROUTER_STACK_SIZE 1024
#define MEMS_ROUTER_PRIORITY 2

BUILD_ASSERT((MEMS_SWITCH_ELECTRICAL_PULSE_MS % MEMS_SWITCH_ROUTER_TICK_MS) == 0U,
             "FFSW pulse width must be an integer number of router ticks");
BUILD_ASSERT((MEMS_SWITCH_ELECTRICAL_PULSE_FFLS_MS % MEMS_SWITCH_ROUTER_TICK_MS) == 0U,
             "FFLS pulse width must be an integer number of router ticks");

static void mems_router_timer_handler(struct k_timer *timer);
static void mems_router_thread(void *p1, void *p2, void *p3);

static K_SEM_DEFINE(mems_router_start_sem, 0, 1);
static K_SEM_DEFINE(mems_router_tick_sem, 0, 1);
static K_TIMER_DEFINE(mems_router_timer, mems_router_timer_handler, NULL);

static struct mems_router *active_router;

K_THREAD_DEFINE(mems_router_tid, MEMS_ROUTER_STACK_SIZE,
                mems_router_thread, NULL, NULL, NULL,
                MEMS_ROUTER_PRIORITY, 0, 0);

static uint32_t mems_switch_pulse_ms(const struct mems_switch *sw)
{
    return sw->switch_type == MEMS_SWITCH_TYPE_FFLS ?
           MEMS_SWITCH_ELECTRICAL_PULSE_FFLS_MS :
           MEMS_SWITCH_ELECTRICAL_PULSE_MS;
}

static uint8_t mems_switch_work_ticks(const struct mems_switch *sw)
{
    return (uint8_t)(mems_switch_pulse_ms(sw) / MEMS_SWITCH_ROUTER_TICK_MS);
}

static uint32_t min_toggle_period_cycles(const struct mems_switch *sw)
{
    const float min_period_ms = 1000.0f / MEMS_SWITCH_MAX_TOGGLE_HZ;
    const float cycles = min_period_ms / (float)mems_switch_pulse_ms(sw);
    uint32_t min_cycles = (uint32_t)cycles;

    if ((float)min_cycles < cycles) {
        min_cycles += 1U;
    }
    if (min_cycles < 2U) {
        min_cycles = 2U;
    }
    return min_cycles;
}

static uint32_t quantize_toggle_period_cycles(const struct mems_switch *sw,
                                              float requested_rate_hz)
{
    uint32_t period_cycles;
    const uint32_t min_cycles = min_toggle_period_cycles(sw);

    if (requested_rate_hz <= 0.0f) {
        return min_cycles;
    }

    period_cycles = (uint32_t)((1000.0f / (requested_rate_hz * (float)mems_switch_pulse_ms(sw))) + 0.5f);
    if (period_cycles < min_cycles) {
        period_cycles = min_cycles;
    }
    if (period_cycles < 2U) {
        period_cycles = 2U;
    }
    return period_cycles;
}

static float attained_toggle_rate_hz(const struct mems_switch *sw,
                                     uint32_t period_cycles)
{
    if (period_cycles == 0U) {
        return 0.0f;
    }
    return 1000.0f / ((float)period_cycles * (float)mems_switch_pulse_ms(sw));
}

static uint32_t seconds_to_cycles(const struct mems_switch *sw, uint32_t seconds)
{
    uint64_t cycles = ((uint64_t)seconds * 1000ULL) / (uint64_t)mems_switch_pulse_ms(sw);
    if (cycles == 0ULL) {
        cycles = 1ULL;
    }
    if (cycles > UINT32_MAX) {
        cycles = UINT32_MAX;
    }
    return (uint32_t)cycles;
}


static struct mems_switch *mems_router_find_switch_unlocked(const struct mems_router *router, const char *name)
{
    for (uint8_t i = 0; i < router->num_switches; ++i) {
        if (strncmp(router->switches[i]->name, name, MEMS_SWITCH_NAME_LEN) == 0) {
            return router->switches[i];
        }
    }
    return NULL;
}



static void mems_switch_set_static_locked(struct mems_switch *sw, char state)
{
    sw->target_state = state;;
    sw->a_state_cycles = (state == 'A') ? sw->switching_period_cycles : 0U;
    sw->cycles_until_toggle = 0U;
    sw->remaining_toggle_cycles = 0U;
}

static void mems_switch_stop_toggling_locked(struct mems_switch *sw)
{
    sw->target_state=sw->state;
    sw->a_state_cycles = (sw->state == 'A') ? sw->switching_period_cycles : 0U;
    sw->cycles_until_toggle = 0U;
    sw->remaining_toggle_cycles = 0U;
}

static void mems_switch_apply_requested_rate_locked(struct mems_switch *sw,
                                                    float requested_rate_hz)
{
    if (requested_rate_hz <= 0.0f) {
        return;
    }

    sw->requested_toggle_rate_hz = requested_rate_hz;
    sw->switching_period_cycles = quantize_toggle_period_cycles(sw, requested_rate_hz);
    sw->actual_toggle_rate_hz = attained_toggle_rate_hz(sw, sw->switching_period_cycles);
}

static void mems_switch_apply_exact_rate_locked(struct mems_switch *sw,
                                                uint32_t period_cycles)
{
    sw->switching_period_cycles = period_cycles;
    sw->actual_toggle_rate_hz = attained_toggle_rate_hz(sw, period_cycles);
    sw->requested_toggle_rate_hz = sw->actual_toggle_rate_hz;
}

static int mems_switch_apply_profile_locked(struct mems_switch *sw,
                                            uint32_t a_cycles,
                                            uint32_t previous_period_cycles,
                                            uint32_t stop_after_s)
{
    uint32_t requested_duration_s;
    bool same_profile;

    if (a_cycles > sw->switching_period_cycles) {
        a_cycles = sw->switching_period_cycles;
    }

    if (a_cycles == 0U) {
        mems_switch_set_static_locked(sw, 'B');
        return 0;
    }

    if (a_cycles >= sw->switching_period_cycles) {
        mems_switch_set_static_locked(sw, 'A');
        return 0;
    }

    /* Mixed duty cycles are applied by the router-owned MEMS tick thread.
     * A zero duration means "run until the firmware safety maximum".
     */
    requested_duration_s = stop_after_s;
    if (requested_duration_s == 0U) {
        requested_duration_s = MEMS_SWITCH_MAX_TOGGLE_DURATION_S;
    }
    same_profile = (sw->remaining_toggle_cycles > 0U) &&
                   (sw->a_state_cycles == a_cycles) &&
                   (previous_period_cycles == sw->switching_period_cycles);

    sw->remaining_toggle_cycles = seconds_to_cycles(sw, requested_duration_s);
    if (!same_profile) {
        sw->a_state_cycles = a_cycles;
        sw->target_state = 'A';
        sw->cycles_until_toggle = a_cycles;
    }

    return 0;
}

static int mems_switch_set_state_internal(struct mems_switch *sw, char state,
                                          float duty_cycle, uint32_t stop_after_s,
                                          float requested_toggle_rate_hz)
{
    struct mems_router *router;
    uint32_t previous_period_cycles;
    uint32_t period_cycles;
    uint32_t a_cycles;
    int rc;
    bool locked = false;

    if (sw == NULL) {
        return -EINVAL;
    }

    state = (char)toupper((unsigned char)state);
    if (state != 'A' && state != 'B') {
        return -EINVAL;
    }

    if (duty_cycle!=0.0f && state != 'A') {
        duty_cycle = 1.0f - duty_cycle;
        state='A';
    }

    if (duty_cycle < 0.0f || duty_cycle > 1.0f || stop_after_s > MEMS_SWITCH_MAX_TOGGLE_DURATION_S) {
        return -ERANGE;
    }

    router = sw->owner;
    if (router != NULL) {
        k_mutex_lock(&router->lock, K_FOREVER);
        locked = true;
    }

    previous_period_cycles = sw->switching_period_cycles;
    mems_switch_apply_requested_rate_locked(sw, requested_toggle_rate_hz);

    period_cycles = sw->switching_period_cycles;
    a_cycles = (uint32_t)(duty_cycle * (float)period_cycles + 0.5f);
    rc = mems_switch_apply_profile_locked(sw, a_cycles, previous_period_cycles,
                                          stop_after_s);

    if (locked) {
        k_mutex_unlock(&router->lock);
    }
    return rc;
}

static void mems_switch_tick_locked(struct mems_switch *sw)
{
    const bool toggling = (sw->remaining_toggle_cycles > 0U);

    if ((!sw->state_known_this_boot || sw->state != sw->target_state)) {

        gpio_pin_t pin = (sw->target_state == 'A') ? sw->pin_a : sw->pin_b;

        /* gpio_pin_set() is raw because the PCAL6416A pins are passed as
         * gpio_pin_t values, not gpio_dt_spec. The overlay active flags are
         * therefore not applied here.
         */
        if (gpio_pin_set(sw->gpio_dev, pin, 1) != 0) {
            LOG_ERR("Pulse set failed on %s pin %u", sw->name, (unsigned int)pin);
        }
        else {
            sw->pulse_ticks_remaining = mems_switch_work_ticks(sw);
            sw->state = sw->target_state;
            sw->state_known_this_boot = true;
        }
    }

    if (!toggling) {
        return;
    }

    sw->remaining_toggle_cycles -= 1U;
    if (sw->remaining_toggle_cycles == 0U) {
        mems_switch_stop_toggling_locked(sw);
        return;
    }

    if (sw->cycles_until_toggle > 0U) {
        sw->cycles_until_toggle -= 1U;
    }

    if (sw->cycles_until_toggle == 0U) {
        uint32_t next_cycles;

        sw->target_state = (sw->target_state == 'A') ? 'B' : 'A';
        if (sw->target_state == 'A') {
            next_cycles = sw->a_state_cycles;
        } else {
            next_cycles = sw->switching_period_cycles - sw->a_state_cycles;
        }

        if (next_cycles == 0U) {
            mems_switch_stop_toggling_locked(sw);
            return;
        }

        sw->cycles_until_toggle = next_cycles;
    }
}

static void mems_switch_clear_finished_pulse_elapsed_locked(struct mems_switch *sw,
                                                            uint32_t elapsed_ticks)
{
    uint8_t previous_ticks;

    if (sw->pulse_ticks_remaining == 0U || elapsed_ticks == 0U) {
        return;
    }

    previous_ticks = sw->pulse_ticks_remaining;
    if (elapsed_ticks < previous_ticks) {
        sw->pulse_ticks_remaining -= elapsed_ticks;
        return;
    }

    sw->pulse_ticks_remaining = 0U;
    {
        const gpio_pin_t pin = (sw->state == 'A') ? sw->pin_a : sw->pin_b;
        (void)gpio_pin_set(sw->gpio_dev, pin, 0);
    }

    if (elapsed_ticks > previous_ticks) {
        LOG_WRN("MEMS %s pulse cleanup was %u router ticks late",
                sw->name, (unsigned int)(elapsed_ticks - previous_ticks));
    }
}

static uint32_t mems_switch_target_hold_cycles(const struct mems_switch *sw)
{
    if (sw->switching_period_cycles == 0U) {
        return 0U;
    }

    if (sw->target_state == 'A') {
        return sw->a_state_cycles;
    }
    return sw->switching_period_cycles - sw->a_state_cycles;
}

static void mems_switch_drop_fully_missed_pulse_locked(struct mems_switch *sw,
                                                       uint32_t late_service_cycles)
{
    if (sw->remaining_toggle_cycles <= late_service_cycles) {
        mems_switch_stop_toggling_locked(sw);
        return;
    }

    sw->remaining_toggle_cycles -= late_service_cycles;

    /* A low-to-high pulse opportunity is real-time. If the full requested high
     * window elapsed before this thread ran, do not emit a stale pulse.
     */
    sw->target_state = sw->state;
    sw->cycles_until_toggle = 1U;
}

static void mems_switch_service_elapsed_locked(struct mems_switch *sw,
                                               uint32_t elapsed_ticks)
{
    const uint8_t service_period_ticks = mems_switch_work_ticks(sw);
    uint32_t late_service_cycles;
    bool pulse_due;

    if (elapsed_ticks == 0U) {
        return;
    }

    if (sw->service_ticks_remaining > elapsed_ticks) {
        sw->service_ticks_remaining -= elapsed_ticks;
        return;
    }

    late_service_cycles =
        (elapsed_ticks - sw->service_ticks_remaining) / service_period_ticks;
    pulse_due = !sw->state_known_this_boot || sw->state != sw->target_state;

    if (late_service_cycles > 0U && pulse_due) {
        uint32_t hold_cycles = mems_switch_target_hold_cycles(sw);

        if (hold_cycles <= late_service_cycles) {
            LOG_WRN("MEMS %s skipped stale %c pulse after %u missed service ticks",
                    sw->name, sw->target_state, late_service_cycles);
            mems_switch_drop_fully_missed_pulse_locked(sw, late_service_cycles);
            sw->service_ticks_remaining = service_period_ticks;
            return;
        }

        LOG_WRN("MEMS %s applying late %c pulse after %u missed service ticks",
                sw->name, sw->target_state, late_service_cycles);
    } else if (late_service_cycles > 0U) {
        LOG_WRN("MEMS %s service tick was %u cycles late",
                sw->name, late_service_cycles);
    }

    mems_switch_tick_locked(sw);
    if (late_service_cycles > 0U && pulse_due && sw->remaining_toggle_cycles > 0U) {
        if (sw->remaining_toggle_cycles > late_service_cycles) {
            sw->remaining_toggle_cycles -= late_service_cycles;
        } else {
            mems_switch_stop_toggling_locked(sw);
        }

        if (sw->cycles_until_toggle > late_service_cycles) {
            sw->cycles_until_toggle -= late_service_cycles;
        } else if (sw->remaining_toggle_cycles > 0U) {
            sw->cycles_until_toggle = 1U;
        }
    }
    sw->service_ticks_remaining = service_period_ticks;
}

static void mems_router_process_ticks(struct mems_router *router, uint32_t elapsed_ticks)
{
    if (router == NULL || elapsed_ticks == 0U) {
        return;
    }

    if (elapsed_ticks > 1U) {
        LOG_WRN("MEMS router missed %u base ticks",
                (unsigned int)(elapsed_ticks - 1U));
    }

    k_mutex_lock(&router->lock, K_FOREVER);

    for (uint8_t i = 0; i < router->num_switches; ++i) {
        mems_switch_clear_finished_pulse_elapsed_locked(router->switches[i],
                                                        elapsed_ticks);
    }

    for (uint8_t i = 0; i < router->num_switches; ++i) {
        mems_switch_service_elapsed_locked(router->switches[i], elapsed_ticks);
    }

    k_mutex_unlock(&router->lock);
}

static void mems_router_timer_handler(struct k_timer *timer)
{
    ARG_UNUSED(timer);

    /* Timer expiry is interrupt context. GPIO-expander writes happen in the
     * dedicated MEMS thread so bus I/O never runs in the ISR.
     */
    k_sem_give(&mems_router_tick_sem);
}

static void mems_router_thread(void *p1, void *p2, void *p3)
{
    ARG_UNUSED(p1);
    ARG_UNUSED(p2);
    ARG_UNUSED(p3);

    k_sem_take(&mems_router_start_sem, K_FOREVER);

    while (1) {
        uint32_t elapsed_ticks;

        k_sem_take(&mems_router_tick_sem, K_FOREVER);
        elapsed_ticks = k_timer_status_get(&mems_router_timer);
        if (elapsed_ticks == 0U) {
            elapsed_ticks = 1U;
        }

        mems_router_process_ticks(active_router, elapsed_ticks);
    }
}

// -----------------------
// Switch Methods
// -----------------------

void mems_switch_init(struct mems_switch *sw, const struct device *gpio_dev,
                      gpio_pin_t pin_a, gpio_pin_t pin_b, const char *name,
                      enum mems_switch_type switch_type,
                      float switching_frequency_hz, char initial_state)
{

    sw->gpio_dev = gpio_dev;
    sw->pin_a = pin_a;
    sw->pin_b = pin_b;
    sw->switch_type = switch_type;
    sw->state = toupper(initial_state)=='A'? 'A': 'B';
    sw->target_state = sw->state;
    sw->state_known_this_boot = false;
    sw->owner = NULL;
    sw->a_state_cycles = 0U;
    sw->cycles_until_toggle = 0U;
    sw->remaining_toggle_cycles = 0U;
    sw->pulse_ticks_remaining = 0U;
    sw->service_ticks_remaining = 0U;
    sw->requested_toggle_rate_hz = switching_frequency_hz;
    sw->switching_period_cycles = quantize_toggle_period_cycles(sw, switching_frequency_hz);
    sw->actual_toggle_rate_hz = attained_toggle_rate_hz(sw, sw->switching_period_cycles);

    strncpy(sw->name, name, MEMS_SWITCH_NAME_LEN-1);
    sw->name[MEMS_SWITCH_NAME_LEN-1] = '\0';

    /* PCAL stays push-pull because no GPIO_SINGLE_ENDED/OPEN_DRAIN flag is
     * used. Mirror the devicetree hog: pull-up enabled, output inactive/low.
     */
    (void)gpio_pin_configure(gpio_dev, pin_a,
                             GPIO_OUTPUT_INACTIVE | GPIO_PULL_UP);
    (void)gpio_pin_configure(gpio_dev, pin_b,
                             GPIO_OUTPUT_INACTIVE | GPIO_PULL_UP);
}


int mems_switch_set_state(struct mems_switch *sw,
                                    char state,
                                    float duty_cycle,
                                    uint32_t stop_after_s,
                                    float requested_toggle_rate_hz)
{
    // todo is this static/nonstatic nesting needed?
    return mems_switch_set_state_internal(sw, state, duty_cycle, stop_after_s,
                                          requested_toggle_rate_hz);
}

int mems_switch_set_state_ticks(struct mems_switch *sw, char state,
                                uint32_t state_ticks, uint32_t period_ticks,
                                uint32_t stop_after_s)
{
    struct mems_router *router;
    uint32_t previous_period_cycles;
    uint32_t a_cycles;
    int rc;
    bool locked = false;

    if (sw == NULL) {
        return -EINVAL;
    }

    state = (char)toupper((unsigned char)state);
    if (state != 'A' && state != 'B') {
        return -EINVAL;
    }
    if (period_ticks < min_toggle_period_cycles(sw) ||
        state_ticks > period_ticks ||
        stop_after_s > MEMS_SWITCH_MAX_TOGGLE_DURATION_S) {
        return -ERANGE;
    }

    a_cycles = (state == 'A') ? state_ticks : period_ticks - state_ticks;

    router = sw->owner;
    if (router != NULL) {
        k_mutex_lock(&router->lock, K_FOREVER);
        locked = true;
    }

    /* Exact tick callers coordinate phase across multiple switches, so force
     * mixed-duty profiles to restart even if the individual switch profile did
     * not change.
     */
    previous_period_cycles = 0U;
    mems_switch_apply_exact_rate_locked(sw, period_ticks);
    rc = mems_switch_apply_profile_locked(sw, a_cycles, previous_period_cycles,
                                          stop_after_s);

    if (locked) {
        k_mutex_unlock(&router->lock);
    }
    return rc;
}

void mems_switch_get_status(const struct mems_switch *sw, struct mems_switch_status *out)
{
    if (sw == NULL || out == NULL) {
        return;
    }

    memset(out, 0, sizeof(*out));

    if (sw->owner != NULL) {
        k_mutex_lock(&sw->owner->lock, K_FOREVER);
    }

    out->state = sw->remaining_toggle_cycles>0 ? 'A': sw->state;
    out->state_known_this_boot = sw->state_known_this_boot;
    out->duty_cycle = (float)sw->a_state_cycles / (float)sw->switching_period_cycles; //actual attained duty cycle
    out->duty_numerator = sw->a_state_cycles;
    out->duty_denominator = sw->switching_period_cycles;
    out->tick_duration_ms = mems_switch_pulse_ms(sw);
    out->requested_toggle_rate_hz = sw->requested_toggle_rate_hz;
    out->toggle_rate_hz = sw->actual_toggle_rate_hz;

    if (sw->remaining_toggle_cycles!=0) {
        out->stopafter_s = (sw->remaining_toggle_cycles * mems_switch_pulse_ms(sw) + 999U)/ 1000U;
    }

    if (sw->owner != NULL) {
        k_mutex_unlock(&sw->owner->lock);
    }
}

// -----------------------
// Router Methods
// -----------------------

void mems_router_init(struct mems_router *router, struct mems_switch **switches,
                      uint8_t num_switches, const struct mems_route *routes,
                      uint8_t num_routes)
{
    k_mutex_init(&router->lock);
    router->num_switches = (num_switches > MEMS_ROUTER_MAX_SWITCHES) ? MEMS_ROUTER_MAX_SWITCHES : num_switches;
    for (uint8_t i = 0; i < router->num_switches; ++i) {
        router->switches[i] = switches[i];
        router->switches[i]->owner = router;
    }
    router->routes = routes;
    router->num_routes = num_routes;

    if (router->num_switches > 0U) {
        active_router = router;
        k_timer_start(&mems_router_timer,
                      K_MSEC(MEMS_SWITCH_ROUTER_TICK_MS),
                      K_MSEC(MEMS_SWITCH_ROUTER_TICK_MS));
        k_sem_give(&mems_router_start_sem);
    }
}

struct mems_switch *mems_router_find_switch(const struct mems_router *router, const char *name)
{
    //todo Why not just make mems_router_find_switch_unlocked a non-static function. This nesting is cryptic and unapproachable to maintainers coming from python
    // and should be documented
    return mems_router_find_switch_unlocked(router, name);
}

// Find route and return pointer/step count, or NULL/-1 if not found
const struct mems_route *mems_router_get_route(const struct mems_router *router,
                                               const char *input, const char *output)
{
    if (router == NULL || router->routes == NULL) {
        return NULL;
    }

    for (uint8_t i = 0; i < router->num_routes; ++i) {
        if (strncmp(router->routes[i].key.input_name, input, MEMS_SWITCH_NAME_LEN) == 0 &&
            strncmp(router->routes[i].key.output_name, output, MEMS_SWITCH_NAME_LEN) == 0) {
            return &router->routes[i];
        }
    }
    return NULL;
}

// List all routes whose switches are ALL in the expected state.
// Returns the number of active routes found, up to max_keys.
// Each result is an (input, output) pair.
uint8_t mems_router_active_routes(const struct mems_router *router,
                                 struct mems_route_key *out_keys, uint8_t max_keys)
{
    uint8_t n_found = 0;

    k_mutex_lock((struct k_mutex *)&router->lock, K_FOREVER);

    for (uint8_t i = 0; router->routes != NULL && i < router->num_routes && n_found < max_keys; ++i) {
        const struct mems_route *route = &router->routes[i];
        bool match = true;
        for (uint8_t j = 0; j < route->num_steps; ++j) {
            const struct mems_route_step *step = &route->steps[j];
            struct mems_switch *sw = mems_router_find_switch_unlocked(router, step->switch_name);
            if (!sw || sw->state != step->state || sw->remaining_toggle_cycles>0) {
                match = false;
                break;
            }
        }
        if (match) {
            out_keys[n_found].input_name = route->key.input_name;
            out_keys[n_found].output_name = route->key.output_name;
            n_found++;
        }
    }

    k_mutex_unlock((struct k_mutex *)&router->lock);
    return n_found;
}
