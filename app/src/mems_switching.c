/**
 * @file mems_switching.c
 * @brief MEMS pulse timing, duty-cycle quantization, and route-state readback.
 */

#include "mems_switching.h"
#include "app_settings.h"

#include <ctype.h>
#include <errno.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/util.h>
#include <string.h>
LOG_MODULE_REGISTER(mems_switching, LOG_LEVEL_DBG);

#define MEMS_ROUTER_STACK_SIZE 1024
#define MEMS_ROUTER_PRIORITY 1
#define MEMS_TIMING_STATS_INTERVAL_MS 10000U

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

static const char *split_channel_names[MEMS_SPLIT_CHANNEL_COUNT] = {"yj", "hk"};
static const char *split_route_inputs[MEMS_SPLIT_CHANNEL_COUNT] = {"yj_calin", "hk_calin"};
static const char *split_route_outputs[MEMS_SPLIT_CHANNEL_COUNT] = {"yj_split", "hk_split"};
static const char *split_output_loss_keys[MEMS_SPLIT_OUTPUT_COUNT] = {
    "split1", "split2", "split3",
};
static struct mems_split_state g_split_state[MEMS_SPLIT_CHANNEL_COUNT];
static K_MUTEX_DEFINE(split_state_lock);

struct mems_timing_stats {
    uint32_t service_events;
    uint32_t missed_base_ticks;
    uint32_t late_service_events;
    uint32_t late_pulse_events;
    uint32_t stale_pulse_skips;
    uint32_t cleanup_late_events;
    uint32_t worst_missed_base_ticks;
    uint32_t worst_late_service_cycles;
    uint32_t worst_cleanup_late_ms;
};

static struct mems_timing_stats mems_timing_stats;
static int64_t mems_timing_next_log_ms;

extern bool app_timing_summary_logs_enabled(void);

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

static bool time_reached_u32(uint32_t now_ms, uint32_t deadline_ms)
{
    return (int32_t)(now_ms - deadline_ms) >= 0;
}

static void mems_timing_note_base_ticks(uint32_t elapsed_ticks)
{
    uint32_t missed;

    mems_timing_stats.service_events++;
    if (elapsed_ticks <= 1U) {
        return;
    }

    missed = elapsed_ticks - 1U;
    mems_timing_stats.missed_base_ticks += missed;
    if (missed > mems_timing_stats.worst_missed_base_ticks) {
        mems_timing_stats.worst_missed_base_ticks = missed;
    }
}

static void mems_timing_note_late_service(uint32_t late_service_cycles, bool pulse_due)
{
    if (late_service_cycles == 0U) {
        return;
    }

    if (pulse_due) {
        mems_timing_stats.late_pulse_events++;
    } else {
        mems_timing_stats.late_service_events++;
    }
    if (late_service_cycles > mems_timing_stats.worst_late_service_cycles) {
        mems_timing_stats.worst_late_service_cycles = late_service_cycles;
    }
}

static void mems_timing_note_stale_pulse_skip(uint32_t late_service_cycles)
{
    mems_timing_stats.stale_pulse_skips++;
    if (late_service_cycles > mems_timing_stats.worst_late_service_cycles) {
        mems_timing_stats.worst_late_service_cycles = late_service_cycles;
    }
}

static void mems_timing_note_cleanup_late(uint32_t late_ms)
{
    mems_timing_stats.cleanup_late_events++;
    if (late_ms > mems_timing_stats.worst_cleanup_late_ms) {
        mems_timing_stats.worst_cleanup_late_ms = late_ms;
    }
}

static bool mems_timing_has_warning(void)
{
    return mems_timing_stats.late_pulse_events > 0U ||
           mems_timing_stats.stale_pulse_skips > 0U;
}

static bool mems_timing_has_variation(void)
{
    return mems_timing_stats.missed_base_ticks > 0U ||
           mems_timing_stats.late_service_events > 0U ||
           mems_timing_stats.late_pulse_events > 0U ||
           mems_timing_stats.stale_pulse_skips > 0U ||
           mems_timing_stats.cleanup_late_events > 0U;
}

static void mems_timing_log_snapshot(bool warning)
{
    if (warning) {
        LOG_WRN("MEMS toggle timing: events=%u late_rise=%u skipped_rise=%u "
                "worst_late_cycles=%u base_miss=%u",
                (unsigned int)mems_timing_stats.service_events,
                (unsigned int)mems_timing_stats.late_pulse_events,
                (unsigned int)mems_timing_stats.stale_pulse_skips,
                (unsigned int)mems_timing_stats.worst_late_service_cycles,
                (unsigned int)mems_timing_stats.missed_base_ticks);
        return;
    }

    LOG_INF("MEMS timing: events=%u base_miss=%u svc_late=%u "
            "late_rise=%u skipped_rise=%u late_fall=%u "
            "worst_base=%u worst_late=%u worst_fall_ms=%u",
            (unsigned int)mems_timing_stats.service_events,
            (unsigned int)mems_timing_stats.missed_base_ticks,
            (unsigned int)mems_timing_stats.late_service_events,
            (unsigned int)mems_timing_stats.late_pulse_events,
            (unsigned int)mems_timing_stats.stale_pulse_skips,
            (unsigned int)mems_timing_stats.cleanup_late_events,
            (unsigned int)mems_timing_stats.worst_missed_base_ticks,
            (unsigned int)mems_timing_stats.worst_late_service_cycles,
            (unsigned int)mems_timing_stats.worst_cleanup_late_ms);
}

static void mems_timing_maybe_log(int64_t now_ms)
{
    bool warning;

    if (mems_timing_next_log_ms == 0) {
        mems_timing_next_log_ms = now_ms + MEMS_TIMING_STATS_INTERVAL_MS;
        return;
    }
    if (now_ms < mems_timing_next_log_ms) {
        return;
    }

    warning = mems_timing_has_warning();
    if (warning || mems_timing_has_variation() || app_timing_summary_logs_enabled()) {
        mems_timing_log_snapshot(warning);
    }

    memset(&mems_timing_stats, 0, sizeof(mems_timing_stats));
    mems_timing_next_log_ms = now_ms + MEMS_TIMING_STATS_INTERVAL_MS;
}

static uint32_t mems_switch_min_actuation_cycles(uint32_t pulse_ms)
{
    const double min_period_ms = 1000.0 / MEMS_SWITCH_MAX_TOGGLE_HZ;
    const double cycles = min_period_ms / (double)pulse_ms;
    uint32_t whole_cycles = (uint32_t)cycles;

    if ((double)whole_cycles < cycles) {
        whole_cycles++;
    }
    if (whole_cycles < 1U) {
        whole_cycles = 1U;
    }

    return whole_cycles;
}

static uint32_t quantize_cycle_period_cycles(const struct mems_switch *sw,
                                             double requested_cycle_ms)
{
    uint32_t period_cycles;
    const uint32_t min_cycles =
        2U * mems_switch_min_actuation_cycles(mems_switch_pulse_ms(sw));

    if (requested_cycle_ms <= 0.0) {
        return min_cycles;
    }

    period_cycles =
        (uint32_t)((requested_cycle_ms / (double)mems_switch_pulse_ms(sw)) + 0.5);
    if (period_cycles < 2U) {
        period_cycles = 2U;
    }
    return period_cycles;
}

static int quantize_mixed_duty_ticks(double duty_cycle,
                                     uint32_t min_cycles,
                                     bool fixed_period,
                                     uint32_t *period_cycles,
                                     uint32_t *a_cycles)
{
    uint32_t ticks;
    double required_period_cycles;

    if (period_cycles == NULL || a_cycles == NULL ||
        duty_cycle <= 0.0 || duty_cycle >= 1.0) {
        return -EINVAL;
    }

    if (!fixed_period) {
        required_period_cycles = (double)*period_cycles;
        required_period_cycles =
            MAX(required_period_cycles, (double)min_cycles / duty_cycle);
        required_period_cycles =
            MAX(required_period_cycles, (double)min_cycles / (1.0 - duty_cycle));
        if (required_period_cycles > (double)UINT32_MAX) {
            return -ERANGE;
        }

        *period_cycles = (uint32_t)required_period_cycles;
        if ((double)*period_cycles < required_period_cycles) {
            (*period_cycles)++;
        }
    }

    if (*period_cycles < 2U * min_cycles) {
        return -ERANGE;
    }

    ticks = (uint32_t)(duty_cycle * (double)*period_cycles + 0.5);
    ticks = CLAMP(ticks, min_cycles, *period_cycles - min_cycles);
    *a_cycles = ticks;
    return 0;
}

static uint32_t cycles_to_ms(const struct mems_switch *sw, uint32_t cycles)
{
    uint64_t ms = (uint64_t)cycles * (uint64_t)mems_switch_pulse_ms(sw);

    if (ms > UINT32_MAX) {
        return UINT32_MAX;
    }
    return (uint32_t)ms;
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


static struct mems_switch *mems_router_find_switch_locked(const struct mems_router *router, const char *name)
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
    sw->target_state = state;
    sw->a_state_cycles = (state == 'A') ? sw->switching_period_cycles : 0U;
    sw->cycles_until_toggle = 0U;
    sw->remaining_toggle_cycles = 0U;
}

static void mems_switch_stop_toggling_locked(struct mems_switch *sw)
{
    sw->target_state = sw->state;
    sw->a_state_cycles = (sw->state == 'A') ? sw->switching_period_cycles : 0U;
    sw->cycles_until_toggle = 0U;
    sw->remaining_toggle_cycles = 0U;
}

static void mems_switch_apply_exact_period_locked(struct mems_switch *sw,
                                                  uint32_t period_cycles)
{
    sw->switching_period_cycles = period_cycles;
}

static int mems_switch_apply_profile_locked(struct mems_switch *sw,
                                            uint32_t a_cycles,
                                            uint32_t previous_period_cycles,
                                            uint32_t off_in_s)
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
    requested_duration_s = off_in_s;
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

int mems_switch_set_state(struct mems_switch *sw, char state,
                          double duty_cycle, uint32_t off_in_s,
                          double requested_cycle_ms)
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

    if (duty_cycle!=0.0 && state != 'A') {
        duty_cycle = 1.0 - duty_cycle;
        state='A';
    }

    if (duty_cycle < 0.0 || duty_cycle > 1.0 || off_in_s > MEMS_SWITCH_MAX_TOGGLE_DURATION_S) {
        return -ERANGE;
    }

    router = sw->owner;
    if (router != NULL) {
        k_mutex_lock(&router->lock, K_FOREVER);
        locked = true;
    }

    previous_period_cycles = sw->switching_period_cycles;
    period_cycles = quantize_cycle_period_cycles(sw, requested_cycle_ms);
    if (duty_cycle > 0.0 && duty_cycle < 1.0) {
        const uint32_t min_cycles =
            mems_switch_min_actuation_cycles(mems_switch_pulse_ms(sw));
        rc = quantize_mixed_duty_ticks(duty_cycle, min_cycles,
                                       requested_cycle_ms > 0.0,
                                       &period_cycles, &a_cycles);
        if (rc != 0) {
            if (locked) {
                k_mutex_unlock(&router->lock);
            }
            return rc;
        }
    } else {
        a_cycles = (uint32_t)(duty_cycle * (double)period_cycles + 0.5);
    }

    sw->switching_period_cycles = period_cycles;
    rc = mems_switch_apply_profile_locked(sw, a_cycles, previous_period_cycles,
                                          off_in_s);

    if (locked) {
        k_mutex_unlock(&router->lock);
    }
    return rc;
}

static void mems_switch_tick_locked(struct mems_switch *sw)
{
    const bool toggling = (sw->remaining_toggle_cycles > 0U);
    bool emitted_pulse = false;

    if ((!sw->state_known_this_boot || sw->state != sw->target_state)) {
        const struct gpio_dt_spec *gpio =
            (sw->target_state == 'A') ? &sw->gpio_a : &sw->gpio_b;
        uint32_t now_ms = k_uptime_get_32();

        if (sw->state_known_this_boot &&
            !time_reached_u32(now_ms,
                              sw->last_pulse_at_ms +
                              (mems_switch_min_actuation_cycles(mems_switch_pulse_ms(sw)) *
                               mems_switch_pulse_ms(sw)))) {
            return;
        }

        /* gpio_pin_set_dt(..., 1) emits the board-defined active pulse. The
         * Nucleo MEMS drive stage is active-low at the PCAL pin, so the
         * external switch-control line pulses high.
         */
        if (gpio_pin_set_dt(gpio, 1) != 0) {
            LOG_ERR("Pulse set failed on %s pin %u", sw->name,
                    (unsigned int)gpio->pin);
        }
        else {
            sw->pulse_clear_at_ms = now_ms + mems_switch_pulse_ms(sw);
            sw->last_pulse_at_ms = now_ms;
            sw->pulse_active = true;
            sw->state = sw->target_state;
            sw->state_known_this_boot = true;
            emitted_pulse = true;
        }
    }

    if (!toggling) {
        return;
    }

    if (emitted_pulse || sw->state == sw->target_state) {
        sw->remaining_toggle_cycles -= 1U;
    } else {
        return;
    }
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
                                                            uint32_t now_ms)
{
    if (!sw->pulse_active || !time_reached_u32(now_ms, sw->pulse_clear_at_ms)) {
        return;
    }

    sw->pulse_active = false;
    {
        const struct gpio_dt_spec *gpio =
            (sw->state == 'A') ? &sw->gpio_a : &sw->gpio_b;

        (void)gpio_pin_set_dt(gpio, 0);
    }

    if (time_reached_u32(now_ms, sw->pulse_clear_at_ms + MEMS_SWITCH_ROUTER_TICK_MS)) {
        mems_timing_note_cleanup_late(now_ms - sw->pulse_clear_at_ms);
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
    const bool toggling = (sw->remaining_toggle_cycles > 0U);
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

    if (late_service_cycles > 0U && pulse_due && toggling) {
        uint32_t hold_cycles = mems_switch_target_hold_cycles(sw);

        if (hold_cycles <= late_service_cycles) {
            mems_timing_note_stale_pulse_skip(late_service_cycles);
            mems_switch_drop_fully_missed_pulse_locked(sw, late_service_cycles);
            sw->service_ticks_remaining = service_period_ticks;
            return;
        }

        mems_timing_note_late_service(late_service_cycles, true);
    } else if (late_service_cycles > 0U) {
        mems_timing_note_late_service(late_service_cycles, false);
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
    uint32_t now_ms;

    if (router == NULL || elapsed_ticks == 0U) {
        return;
    }

    mems_timing_note_base_ticks(elapsed_ticks);

    k_mutex_lock(&router->lock, K_FOREVER);

    now_ms = k_uptime_get_32();

    for (uint8_t i = 0; i < router->num_switches; ++i) {
        mems_switch_clear_finished_pulse_elapsed_locked(router->switches[i],
                                                        now_ms);
    }

    for (uint8_t i = 0; i < router->num_switches; ++i) {
        mems_switch_service_elapsed_locked(router->switches[i], elapsed_ticks);
    }

    k_mutex_unlock(&router->lock);
    mems_timing_maybe_log(k_uptime_get());
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

void mems_switch_init(struct mems_switch *sw,
                      const struct gpio_dt_spec *gpio_a,
                      const struct gpio_dt_spec *gpio_b,
                      const char *name,
                      enum mems_switch_type switch_type,
                      char initial_state)
{

    sw->gpio_a = *gpio_a;
    sw->gpio_b = *gpio_b;
    sw->switch_type = switch_type;
    sw->state = toupper(initial_state)=='A'? 'A': 'B';
    sw->target_state = sw->state;
    sw->state_known_this_boot = false;
    sw->owner = NULL;
    sw->cycles_until_toggle = 0U;
    sw->remaining_toggle_cycles = 0U;
    sw->pulse_clear_at_ms = 0U;
    sw->last_pulse_at_ms = 0U;
    sw->pulse_active = false;
    sw->service_ticks_remaining = 0U;
    sw->switching_period_cycles = quantize_cycle_period_cycles(sw, 0.0);
    /* Keep status readback consistent before the first router pulse. */
    sw->a_state_cycles = (sw->state == 'A') ? sw->switching_period_cycles : 0U;

    strncpy(sw->name, name, MEMS_SWITCH_NAME_LEN-1);
    sw->name[MEMS_SWITCH_NAME_LEN-1] = '\0';

    /* gpio_pin_configure_dt() applies the board polarity from the overlay.
     * For Nucleo MEMS lines, logical inactive is the external idle-low state.
     */
    (void)gpio_pin_configure_dt(&sw->gpio_a, GPIO_OUTPUT_INACTIVE);
    (void)gpio_pin_configure_dt(&sw->gpio_b, GPIO_OUTPUT_INACTIVE);
}


int mems_switch_set_state_ticks(struct mems_switch *sw, char state,
                                uint32_t state_ticks, uint32_t period_ticks,
                                uint32_t off_in_s)
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
    if (period_ticks == 0U ||
        state_ticks > period_ticks ||
        off_in_s > MEMS_SWITCH_MAX_TOGGLE_DURATION_S) {
        return -ERANGE;
    }

    a_cycles = (state == 'A') ? state_ticks : period_ticks - state_ticks;
    if (a_cycles > 0U && a_cycles < period_ticks) {
        const uint32_t min_cycles =
            mems_switch_min_actuation_cycles(mems_switch_pulse_ms(sw));

        if (a_cycles < min_cycles || period_ticks - a_cycles < min_cycles) {
            return -ERANGE;
        }
    }
    if (a_cycles > period_ticks) {
        return -ERANGE;
    }

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
    mems_switch_apply_exact_period_locked(sw, period_ticks);
    rc = mems_switch_apply_profile_locked(sw, a_cycles, previous_period_cycles,
                                          off_in_s);

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
    out->duty_cycle = (double)sw->a_state_cycles / (double)sw->switching_period_cycles; //actual attained duty cycle
    out->cycle_ms = cycles_to_ms(sw, sw->switching_period_cycles);
    out->a_ms = cycles_to_ms(sw, sw->a_state_cycles);
    out->b_ms = out->cycle_ms > out->a_ms ? out->cycle_ms - out->a_ms : 0U;

    if (sw->remaining_toggle_cycles!=0) {
        out->stop_in_s =
            (sw->remaining_toggle_cycles * mems_switch_pulse_ms(sw) + 999U)/ 1000U;
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
    struct mems_switch *sw;

    if (router == NULL || name == NULL) {
        return NULL;
    }

    k_mutex_lock((struct k_mutex *)&router->lock, K_FOREVER);
    sw = mems_router_find_switch_locked(router, name);
    k_mutex_unlock((struct k_mutex *)&router->lock);

    return sw;
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

int mems_router_apply_route(const struct mems_router *router,
                            const struct mems_route *route,
                            const char **failed_switch,
                            char *failed_state)
{
    if (router == NULL || route == NULL) {
        return -EINVAL;
    }

    for (uint8_t i = 0U; i < route->num_steps; ++i) {
        const struct mems_route_step *step = &route->steps[i];
        struct mems_switch *sw = mems_router_find_switch(router, step->switch_name);
        int rc;

        if (sw == NULL) {
            if (failed_switch != NULL) {
                *failed_switch = step->switch_name;
            }
            if (failed_state != NULL) {
                *failed_state = step->state;
            }
            return -ENOENT;
        }

        rc = mems_switch_set_state(sw, step->state, 1.0, 0U, 0.0);
        if (rc != 0) {
            if (failed_switch != NULL) {
                *failed_switch = step->switch_name;
            }
            if (failed_state != NULL) {
                *failed_state = step->state;
            }
            return rc;
        }
    }

    return 0;
}

int mems_router_apply_named_route(const struct mems_router *router,
                                  const char *input,
                                  const char *output,
                                  const char **failed_switch,
                                  char *failed_state)
{
    const struct mems_route *route;

    if (router == NULL || input == NULL || output == NULL) {
        return -EINVAL;
    }

    route = mems_router_get_route(router, input, output);
    if (route == NULL) {
        return -ENOENT;
    }

    return mems_router_apply_route(router, route, failed_switch, failed_state);
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
            struct mems_switch *sw = mems_router_find_switch_locked(router, step->switch_name);
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

//TODO: TFD or relocation to _command.c
const char *mems_split_channel_name(uint8_t channel_index)
{
    if (channel_index >= MEMS_SPLIT_CHANNEL_COUNT) {
        return NULL;
    }

    return split_channel_names[channel_index];
}

//TODO: TFD or relocation to _command.c and/or combination with mems_split_channel_name
//TODO GLOBALLY: the names that llm is coming up with sometimes make it profoundly hard to reason about where the actual work is done. to with, this is merely  channel index getter by name but reads like an action "'Split mems channel-index.' or "Split mems channel (by index)." In the case of splitting using split (action) and splitting (domain/namespace hint) may help
int mems_split_channel_index(const char *channel, uint8_t *index)
{
    if (channel == NULL || index == NULL) {
        return -EINVAL;
    }

    for (uint8_t i = 0U; i < MEMS_SPLIT_CHANNEL_COUNT; ++i) {
        if (strcmp(channel, split_channel_names[i]) == 0) {
            *index = i;
            return 0;
        }
    }

    return -ENOENT;
}

int mems_split_route_name(uint8_t channel_index, char *out, size_t out_len)
{
    int written;

    if (out == NULL || out_len == 0U || channel_index >= MEMS_SPLIT_CHANNEL_COUNT) {
        return -EINVAL;
    }

    written = snprintk(out, out_len, "%s_to_%s",
                       split_route_inputs[channel_index],
                       split_route_outputs[channel_index]);
    return (written < 0 || written >= (int)out_len) ? -ENOSPC : 0;
}

const char *mems_split_output_loss_key(uint8_t output_index)
{
    if (output_index >= MEMS_SPLIT_OUTPUT_COUNT) {
        return NULL;
    }

    return split_output_loss_keys[output_index];
}

static const struct mems_route *split_route_for_channel(const struct mems_router *router,
                                                        uint8_t channel_index)
{
    if (channel_index >= MEMS_SPLIT_CHANNEL_COUNT) {
        return NULL;
    }

    return mems_router_get_route(router,
                                 split_route_inputs[channel_index],
                                 split_route_outputs[channel_index]);
}

static uint32_t split_period_ticks(void)
{
    return 2U * mems_switch_min_actuation_cycles(MEMS_SWITCH_ELECTRICAL_PULSE_MS);
}

static uint32_t split_cycle_ms_to_ticks(uint32_t cycle_ms)
{
    uint64_t ticks;

    if (cycle_ms == 0U) {
        return split_period_ticks();
    }

    ticks = ((uint64_t)cycle_ms + MEMS_SWITCH_ELECTRICAL_PULSE_MS / 2U) /
            MEMS_SWITCH_ELECTRICAL_PULSE_MS;
    if (ticks < 2U) {
        ticks = 2U;
    }
    if (ticks > UINT32_MAX) {
        ticks = UINT32_MAX;
    }
    return (uint32_t)ticks;
}

static uint32_t split_ratio_to_ticks(double ratio, uint32_t period_ticks)
{
    uint32_t ticks = (uint32_t)(ratio * (double)period_ticks + 0.5);

    return MIN(ticks, period_ticks);
}

static int split_quantize_boundary_ticks(double ratio,
                                         uint32_t period_ticks,
                                         uint32_t min_ticks,
                                         uint32_t *boundary_ticks)
{
    uint32_t ticks;

    if (boundary_ticks == NULL || period_ticks == 0U) {
        return -EINVAL;
    }

    if (ratio <= 0.0) {
        *boundary_ticks = 0U;
        return 0;
    }
    if (ratio >= 1.0) {
        *boundary_ticks = period_ticks;
        return 0;
    }
    if (period_ticks < 2U * min_ticks) {
        return -ERANGE;
    }

    ticks = split_ratio_to_ticks(ratio, period_ticks);
    ticks = CLAMP(ticks, min_ticks, period_ticks - min_ticks);
    *boundary_ticks = ticks;
    return 0;
}

static int split_quantize_output_ticks(uint32_t period_ticks,
                                       uint32_t min_ticks,
                                       const double corrected[MEMS_SPLIT_OUTPUT_COUNT],
                                       uint32_t output_ticks[MEMS_SPLIT_OUTPUT_COUNT])
{
    uint32_t boundary1_ticks;
    uint32_t boundary3_ticks;
    int rc;

    if (corrected == NULL || output_ticks == NULL) {
        return -EINVAL;
    }

    rc = split_quantize_boundary_ticks(corrected[0], period_ticks, min_ticks,
                                       &boundary1_ticks);
    if (rc != 0) {
        return rc;
    }
    rc = split_quantize_boundary_ticks(corrected[0] + corrected[1],
                                       period_ticks, min_ticks,
                                       &boundary3_ticks);
    if (rc != 0) {
        return rc;
    }
    if (boundary3_ticks < boundary1_ticks) {
        boundary3_ticks = boundary1_ticks;
    }

    output_ticks[0] = boundary1_ticks;
    output_ticks[1] = boundary3_ticks - boundary1_ticks;
    output_ticks[2] = period_ticks - boundary3_ticks;
    return 0;
}

static void split_read_transmissions(const char *route_name,
                                     double transmission[MEMS_SPLIT_OUTPUT_COUNT])
{
    if (route_name == NULL || transmission == NULL) {
        return;
    }

    for (uint8_t i = 0U; i < MEMS_SPLIT_OUTPUT_COUNT; ++i) {
        const char *loss_key = mems_split_output_loss_key(i);
        double tx = 1.0;

        (void)app_settings_get_route_loss(route_name, loss_key, &tx);
        transmission[i] = (double)tx;
    }
}

static void split_output_ratio_from_actual(const double actual[MEMS_SPLIT_OUTPUT_COUNT],
                                           const double transmission[MEMS_SPLIT_OUTPUT_COUNT],
                                           double output[MEMS_SPLIT_OUTPUT_COUNT])
{
    double delivered[MEMS_SPLIT_OUTPUT_COUNT];
    double total = 0.0;

    for (uint8_t i = 0U; i < MEMS_SPLIT_OUTPUT_COUNT; ++i) {
        delivered[i] = (double)actual[i] * (double)transmission[i];
        total += delivered[i];
    }

    if (!(total > 0.0)) {
        memset(output, 0, MEMS_SPLIT_OUTPUT_COUNT * sizeof(output[0]));
        return;
    }

    for (uint8_t i = 0U; i < MEMS_SPLIT_OUTPUT_COUNT; ++i) {
        output[i] = (double)(delivered[i] / total);
    }
}

static int split_correct_for_transmission(const double requested[MEMS_SPLIT_OUTPUT_COUNT],
                                          const double transmission[MEMS_SPLIT_OUTPUT_COUNT],
                                          double corrected[MEMS_SPLIT_OUTPUT_COUNT])
{
    const double ra = requested[0];
    const double rb = requested[1];
    const double ta = transmission[0];
    const double tb = transmission[1];
    const double tc = transmission[2];
    double denom;

    if (!(ta > 0.0 && tb > 0.0 && tc > 0.0)) {
        return -ERANGE;
    }

    /* Solve for MEMS duty cycles whose transmitted outputs normalize back to
     * the requested ratios after the three split-path transmissions are applied.
     */
    denom = ta * tb - ra * ta * tb - rb * ta * tb +
            rb * ta * tc + ra * tb * tc;
    if (!(denom > 0.0)) {
        return -ERANGE;
    }

    corrected[0] = (ra * tb * tc) / denom;
    corrected[1] = (rb * ta * tc) / denom;
    corrected[2] = 1.0 - corrected[0] - corrected[1];

    if (corrected[0] < -0.000001 || corrected[1] < -0.000001 ||
        corrected[2] < -0.000001 || corrected[0] > 1.000001 ||
        corrected[1] > 1.000001 || corrected[2] > 1.000001) {
        return -ERANGE;
    }

    for (uint8_t i = 0U; i < MEMS_SPLIT_OUTPUT_COUNT; ++i) {
        corrected[i] = CLAMP(corrected[i], 0.0, 1.0);
    }

    return 0;
}

static uint32_t split_selected_ms(const struct mems_switch_status *status,
                                  char state)
{
    if (state == 'A') {
        return status->a_ms;
    }

    return status->b_ms;
}

static void split_clamp_actual(double actual[MEMS_SPLIT_OUTPUT_COUNT])
{
    double used;

    for (uint8_t i = 0U; i < MEMS_SPLIT_OUTPUT_COUNT; ++i) {
        if (actual[i] < 0.0) {
            actual[i] = 0.0;
        }
        if (actual[i] > 1.0) {
            actual[i] = 1.0;
        }
    }

    used = actual[0] + actual[1];
    if (used > 1.0) {
        actual[1] = 1.0 - actual[0];
        used = 1.0;
    }
    actual[2] = 1.0 - used;
}

int mems_split_read_channel_state(const struct mems_router *router,
                                  uint8_t channel_index,
                                  const double requested[MEMS_SPLIT_OUTPUT_COUNT],
                                  struct mems_split_state *out)
{
    const struct mems_route *route;
    struct mems_split_state next = {0};
    char route_name[APP_ROUTE_LOSS_ROUTE_MAX_LEN] = {0};
    double sw1_duty;
    double sw3_duty;

    if (router == NULL || channel_index >= MEMS_SPLIT_CHANNEL_COUNT) {
        return -EINVAL;
    }

    route = split_route_for_channel(router, channel_index);
    if (route == NULL || route->num_steps != MEMS_SPLIT_ROUTE_SWITCH_COUNT) {
        return -EINVAL;
    }
    (void)mems_split_route_name(channel_index, route_name, sizeof(route_name));

    if (requested != NULL) {
        memcpy(next.requested, requested, sizeof(next.requested));
    } else {
        k_mutex_lock(&split_state_lock, K_FOREVER);
        next = g_split_state[channel_index];
        k_mutex_unlock(&split_state_lock);
    }

    for (uint8_t i = 0U; i < MEMS_SPLIT_ROUTE_SWITCH_COUNT; ++i) {
        const struct mems_route_step *step = &route->steps[i];
        struct mems_switch *sw = mems_router_find_switch(router, step->switch_name);
        struct mems_switch_status status = {0};
        uint32_t selected_ms;

        if (sw == NULL) {
            LOG_ERR("Split route %s->%s references missing switch %s",
                    route->key.input_name, route->key.output_name,
                    step->switch_name);
            return -EINVAL;
        }

        mems_switch_get_status(sw, &status);
        selected_ms = split_selected_ms(&status, step->state);

        snprintk(next.switches[i].name, sizeof(next.switches[i].name),
                 "%s", step->switch_name);
        next.switches[i].state = step->state;
        next.switches[i].a_ms = status.a_ms;
        next.switches[i].b_ms = status.b_ms;
        next.switches[i].duty_cycle =
            status.cycle_ms == 0U ? 0.0 :
            (double)selected_ms / (double)status.cycle_ms;
        next.cycle_ms = MAX(next.cycle_ms, status.cycle_ms);
        next.stop_in_s = MAX(next.stop_in_s, status.stop_in_s);
    }

    sw1_duty = next.switches[0].duty_cycle;
    sw3_duty = next.switches[2].duty_cycle;
    next.actual[0] = sw1_duty;
    next.actual[1] = sw3_duty > sw1_duty ? sw3_duty - sw1_duty : 0.0;
    split_clamp_actual(next.actual);
    split_read_transmissions(route_name, next.transmission);
    split_output_ratio_from_actual(next.actual, next.transmission, next.output);

    k_mutex_lock(&split_state_lock, K_FOREVER);
    g_split_state[channel_index] = next;
    k_mutex_unlock(&split_state_lock);

    if (out != NULL) {
        *out = next;
    }

    LOG_INF("Split %s actual %.4f %.4f %.4f output %.4f %.4f %.4f",
            split_channel_names[channel_index],
            (double)next.actual[0],
            (double)next.actual[1],
            (double)next.actual[2],
            (double)next.output[0],
            (double)next.output[1],
            (double)next.output[2]);

    return 0;
}

int mems_split_apply_channel(const struct mems_router *router,
                             uint8_t channel_index,
                             const double requested[MEMS_SPLIT_OUTPUT_COUNT],
                             uint32_t cycle_ms,
                             uint32_t off_in_s,
                             struct mems_split_state *out,
                             const char **failed_switch)
{
    const struct mems_route *route;
    uint32_t period_ticks;
    uint32_t output_ticks[MEMS_SPLIT_OUTPUT_COUNT];
    uint32_t switch_ticks[MEMS_SPLIT_ROUTE_SWITCH_COUNT];
    char route_name[APP_ROUTE_LOSS_ROUTE_MAX_LEN] = {0};
    double transmission[MEMS_SPLIT_OUTPUT_COUNT];
    double corrected[MEMS_SPLIT_OUTPUT_COUNT];
    uint32_t min_split_ticks;
    int rc;

    if (router == NULL || requested == NULL ||
        channel_index >= MEMS_SPLIT_CHANNEL_COUNT) {
        return -EINVAL;
    }
    if (requested[0] < 0.0 || requested[0] > 1.0 ||
        requested[1] < 0.0 || requested[1] > 1.0 ||
        requested[2] < 0.0 || requested[2] > 1.0 ||
        requested[0] + requested[1] > 1.000001 ||
        off_in_s > MEMS_SWITCH_MAX_TOGGLE_DURATION_S) {
        return -ERANGE;
    }

    route = split_route_for_channel(router, channel_index);
    if (route == NULL || route->num_steps != MEMS_SPLIT_ROUTE_SWITCH_COUNT) {
        return -EINVAL;
    }

    (void)mems_split_route_name(channel_index, route_name, sizeof(route_name));
    split_read_transmissions(route_name, transmission);
    rc = split_correct_for_transmission(requested, transmission, corrected);
    if (rc != 0) {
        return rc;
    }

    min_split_ticks = mems_switch_min_actuation_cycles(MEMS_SWITCH_ELECTRICAL_PULSE_MS);
    period_ticks = split_cycle_ms_to_ticks(cycle_ms);
    if (cycle_ms == 0U) {
        double required_period_ticks = (double)period_ticks;

        /* With no external cycle constraint, choose the fastest period that can
         * realize the transmission-corrected split without violating the MEMS
         * actuation spacing on SW1 or SW3.
         */
        if (corrected[0] > 0.0 && corrected[0] < 1.0) {
            required_period_ticks =
                MAX(required_period_ticks, (double)min_split_ticks / corrected[0]);
            required_period_ticks =
                MAX(required_period_ticks, (double)min_split_ticks / (1.0 - corrected[0]));
        }
        if (corrected[0] + corrected[1] > 0.0 &&
            corrected[0] + corrected[1] < 1.0) {
            const double switch3_ratio = corrected[0] + corrected[1];

            required_period_ticks =
                MAX(required_period_ticks, (double)min_split_ticks / switch3_ratio);
            required_period_ticks =
                MAX(required_period_ticks, (double)min_split_ticks / (1.0 - switch3_ratio));
        }
        if (required_period_ticks > (double)UINT32_MAX) {
            return -ERANGE;
        }
        period_ticks = (uint32_t)required_period_ticks;
        if ((double)period_ticks < required_period_ticks) {
            period_ticks++;
        }
    }

    rc = split_quantize_output_ticks(period_ticks, min_split_ticks, corrected,
                                     output_ticks);
    if (rc != 0) {
        return rc;
    }

    switch_ticks[0] = output_ticks[0];
    switch_ticks[1] = period_ticks;
    switch_ticks[2] = output_ticks[0] + output_ticks[1];

    for (uint8_t i = 0U; i < MEMS_SPLIT_ROUTE_SWITCH_COUNT; ++i) {
        const struct mems_route_step *step = &route->steps[i];
        struct mems_switch *sw = mems_router_find_switch(router, step->switch_name);
        int rc;

        if (sw == NULL) {
            if (failed_switch != NULL) {
                *failed_switch = step->switch_name;
            }
            return -ENOENT;
        }

        rc = mems_switch_set_state_ticks(sw, step->state, switch_ticks[i],
                                         period_ticks,
                                         i == 1U ? 0U : off_in_s);
        if (rc != 0) {
            if (failed_switch != NULL) {
                *failed_switch = step->switch_name;
            }
            return rc;
        }
    }

    return mems_split_read_channel_state(router, channel_index, requested, out);
}
