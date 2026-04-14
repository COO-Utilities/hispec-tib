//
// Created by Jeb Bailey on 4/23/25.
//
// mems_switching.c

#include "mems_switching.h"
#include <ctype.h>
#include <errno.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/util.h>
#include <string.h>
LOG_MODULE_REGISTER(mems_switching, LOG_LEVEL_DBG);

static uint32_t min_toggle_period_cycles(void)
{
    const float min_period_ms = 1000.0f / MEMS_SWITCH_MAX_TOGGLE_HZ;
    const float cycles = min_period_ms / (float)MEMS_SWITCH_ELECTRICAL_PULSE_MS;
    uint32_t min_cycles = (uint32_t)cycles;

    if ((float)min_cycles < cycles) {
        min_cycles += 1U;
    }
    if (min_cycles < 2U) {
        min_cycles = 2U;
    }
    return min_cycles;
}

static uint32_t quantize_toggle_period_cycles(float requested_rate_hz)
{
    uint32_t period_cycles;
    const uint32_t min_cycles = min_toggle_period_cycles();

    if (requested_rate_hz <= 0.0f) {
        return min_cycles;
    }

    period_cycles = (uint32_t)((1000.0f / (requested_rate_hz * (float)MEMS_SWITCH_ELECTRICAL_PULSE_MS)) + 0.5f);
    if (period_cycles < min_cycles) {
        period_cycles = min_cycles;
    }
    if (period_cycles < 2U) {
        period_cycles = 2U;
    }
    return period_cycles;
}

static float attained_toggle_rate_hz(uint32_t period_cycles)
{
    if (period_cycles == 0U) {
        return 0.0f;
    }
    return 1000.0f / ((float)period_cycles * (float)MEMS_SWITCH_ELECTRICAL_PULSE_MS);
}

static uint32_t seconds_to_cycles(uint32_t seconds)
{
    uint64_t cycles = ((uint64_t)seconds * 1000ULL) / (uint64_t)MEMS_SWITCH_ELECTRICAL_PULSE_MS;
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
    sw->target_state = state;
    sw->configured_state = state;
    sw->duty_cycle = (state == 'A') ? 1.0f : 0.0f;
    sw->toggle_rate_hz = 0.0f;
    sw->toggle_period_cycles = 0U;
    sw->a_state_cycles = 0U;
    sw->cycles_until_toggle = 0U;
    sw->remaining_toggle_cycles = 0U;
}

static void mems_switch_stop_toggling_locked(struct mems_switch *sw)
{
    sw->toggle_rate_hz = 0.0f;
    sw->toggle_period_cycles = 0U;
    sw->a_state_cycles = 0U;
    sw->cycles_until_toggle = 0U;
    sw->remaining_toggle_cycles = 0U;


    // TODO this is undocumented yet appears to contain vital state machine assumptions
    //  if we are stopping toggle then we've either been through at least one cycle and so state is known and A or B
    //  or the impossible happened and we were commanded to stop before even on cycle...in which case we could fairly activate whatever state we want.
    //  so when we kick off toggling we immediately set the pin state and free ourselves from this nonsense.
    if (sw->state_known_this_boot && (sw->state == 'A' || sw->state == 'B')) {
        sw->target_state = sw->state;
    }

    if (sw->target_state == 'A' || sw->target_state == 'B') {
        sw->configured_state = sw->target_state;
        sw->duty_cycle = (sw->target_state == 'A') ? 1.0f : 0.0f;
    } else {
        sw->configured_state = 'U';
        sw->duty_cycle = 0.0f;
    }
}

static int mems_switch_set_state_internal(struct mems_switch *sw,
                                 char state,
                                 float duty_cycle,
                                 uint32_t stop_after_s)
{
    struct mems_router *router;
    uint32_t period_cycles;
    bool locked = false;

    if (sw == NULL) {
        return -EINVAL;
    }

    state = (char)toupper((unsigned char)state);
    if (state != 'A' && state != 'B') {
        return -EINVAL;
    }

    if (duty_cycle!=0.0f && state != 'A') {
        return -EINVAL;
    }

    if (duty_cycle < 0.0f || duty_cycle > 1.0f || stop_after_s > MEMS_SWITCH_MAX_TOGGLE_DURATION_S) {
        return -ERANGE;
    }

    router = sw->owner;
    if (router != NULL) {
        k_mutex_lock(&router->lock, K_FOREVER);
        locked = true;
    }

    period_cycles = sw->configured_toggle_period_cycles;
    uint32_t a_cycles = (uint32_t)(duty_cycle * (float)period_cycles + 0.5f);
    if (a_cycles > period_cycles) {
        a_cycles = period_cycles;
    }
    duty_cycle = (float)a_cycles / (float)period_cycles;

    if (a_cycles == 0U) {
        mems_switch_set_static_locked(sw, 'B');
        if (locked) {
            k_mutex_unlock(&router->lock);
        }
        return 0;
    }

    if (a_cycles >= period_cycles) {
        mems_switch_set_static_locked(sw, 'A');
        if (locked) {
            k_mutex_unlock(&router->lock);
        }
        return 0;
    }

    uint32_t requested_duration_s = stop_after_s;
    if (requested_duration_s == 0U) {
        requested_duration_s = MEMS_SWITCH_MAX_TOGGLE_DURATION_S;
    }
    uint32_t requested_duration_cycles = seconds_to_cycles(requested_duration_s);
    bool same_profile = (sw->remaining_toggle_cycles > 0U) &&
                        (sw->toggle_period_cycles == period_cycles) &&
                        (sw->a_state_cycles == a_cycles);

    sw->configured_state = 'T'; //TODO I'm inclined that this should be A with a dutycycle
    sw->duty_cycle = duty_cycle;

    if (same_profile) {
        if (requested_duration_cycles > sw->remaining_toggle_cycles) {
            sw->remaining_toggle_cycles = requested_duration_cycles;
        }
    } else {
        sw->toggle_period_cycles = period_cycles;
        sw->toggle_rate_hz = sw->switching_frequency_hz;  //TODO why are there two, what does this distinction achieve. Also toggle_rate_hz and toggle_period_cycles are prima facie redundant, not good additional complexity to cary
        sw->a_state_cycles = a_cycles;
        sw->remaining_toggle_cycles = requested_duration_cycles;
        sw->target_state = 'A';
        sw->cycles_until_toggle = a_cycles;
    }

    if (locked) {
        k_mutex_unlock(&router->lock);
    }
    return 0;
}

static void mems_switch_tick_locked(struct mems_switch *sw)
{
    const bool toggling = (sw->remaining_toggle_cycles > 0U) &&
                          (sw->toggle_rate_hz > 0.0f) &&
                          (sw->a_state_cycles > 0U) &&
                          (sw->a_state_cycles < sw->toggle_period_cycles); //TODO this last seems like something that should be a don't care

    //TODO clean up this conditional, target_state should only ever be A | B, if that isn't the case then we've got problems elsewhere.
    if ((sw->target_state == 'A' || sw->target_state == 'B') &&
        (!sw->state_known_this_boot || sw->state != sw->target_state)) {

        gpio_pin_t pin = (sw->target_state == 'A') ? sw->pin_a : sw->pin_b;

        if (gpio_pin_set(sw->gpio_dev, pin, 1) != 0) {
            LOG_ERR("Pulse set failed on %s pin %u", sw->name, (unsigned int)pin);
        }
        else {
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
            next_cycles = sw->toggle_period_cycles - sw->a_state_cycles;
        }

        if (next_cycles == 0U) {
            mems_switch_stop_toggling_locked(sw);
            return;
        }

        sw->cycles_until_toggle = next_cycles;
    }
}

static void mems_router_toggler_work_handler(struct k_work *work)
{
    struct k_work_delayable *dwork = k_work_delayable_from_work(work);
    struct mems_router *router = CONTAINER_OF(dwork, struct mems_router, toggler_work);

    k_mutex_lock(&router->lock, K_FOREVER);

    for (uint8_t i = 0; i < router->num_switches; ++i) {
        struct mems_switch *sw = router->switches[i];
        (void)gpio_pin_set(sw->gpio_dev, sw->pin_a, 0);
        (void)gpio_pin_set(sw->gpio_dev, sw->pin_b, 0);
    }

    for (uint8_t i = 0; i < router->num_switches; ++i) {
        mems_switch_tick_locked(router->switches[i]);
    }

    k_mutex_unlock(&router->lock);

    (void)k_work_reschedule(&router->toggler_work, K_MSEC(MEMS_SWITCH_ELECTRICAL_PULSE_MS));
}

// -----------------------
// Switch Methods
// -----------------------

void mems_switch_init(struct mems_switch *sw, const struct device *gpio_dev,
                      gpio_pin_t pin_a, gpio_pin_t pin_b, const char *name,
                      float switching_frequency_hz)
{
    uint32_t configured_period_cycles;

    sw->gpio_dev = gpio_dev;
    sw->pin_a = pin_a;
    sw->pin_b = pin_b;
    sw->state = 'U';
    sw->target_state = 'U';
    sw->configured_state = 'U';
    sw->state_known_this_boot = false;
    sw->owner = NULL;
    sw->duty_cycle = 0.0f;
    sw->toggle_rate_hz = 0.0f;
    sw->toggle_period_cycles = 0U;
    sw->a_state_cycles = 0U;
    sw->cycles_until_toggle = 0U;
    sw->remaining_toggle_cycles = 0U;

    configured_period_cycles = quantize_toggle_period_cycles(switching_frequency_hz);
    sw->configured_toggle_period_cycles = configured_period_cycles;
    sw->switching_frequency_hz = attained_toggle_rate_hz(configured_period_cycles);

    strncpy(sw->name, name, MEMS_SWITCH_NAME_LEN-1);
    sw->name[MEMS_SWITCH_NAME_LEN-1] = '\0';

    (void)gpio_pin_configure(gpio_dev, pin_a, GPIO_OUTPUT_INACTIVE);
    (void)gpio_pin_configure(gpio_dev, pin_b, GPIO_OUTPUT_INACTIVE);
}


int mems_switch_set_state(struct mems_switch *sw,
                                    char state,
                                    float duty_cycle,
                                    uint32_t stop_after_s)
{
    // todo is this static/nonstatic needed?!?
    return mems_switch_set_state_internal(sw, state, duty_cycle, stop_after_s);
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

    out->state = sw->state;
    out->state_known_this_boot = sw->state_known_this_boot;
    out->duty_cycle = sw->duty_cycle;
    out->toggle_rate_hz = sw->toggle_rate_hz;

    if (sw->remaining_toggle_cycles!=0) {
        out->stopafter_s = ((sw->remaining_toggle_cycles+999U) * MEMS_SWITCH_ELECTRICAL_PULSE_MS)/ 1000U;
    }

    if (sw->owner != NULL) {
        k_mutex_unlock(&sw->owner->lock);
    }
}

// -----------------------
// Router Methods
// -----------------------

void mems_router_init(struct mems_router *router, struct mems_switch **switches, uint8_t num_switches)
{
    k_mutex_init(&router->lock);
    router->num_switches = (num_switches > MEMS_ROUTER_MAX_SWITCHES) ? MEMS_ROUTER_MAX_SWITCHES : num_switches;
    for (uint8_t i = 0; i < router->num_switches; ++i) {
        router->switches[i] = switches[i];
        router->switches[i]->owner = router;
    }
    router->num_routes = 0;

    //It is vital that at this point in the code the switches be initialized such that they WILL NOT toggle. Their
    // needed states and potentially restored states must all align.
    //TODO Verify this is the case.

    k_work_init_delayable(&router->toggler_work, mems_router_toggler_work_handler);
    (void)k_work_reschedule(&router->toggler_work, K_MSEC(MEMS_SWITCH_ELECTRICAL_PULSE_MS));
}

struct mems_switch *mems_router_find_switch(const struct mems_router *router, const char *name)
{
    //todo what is this mess? why, this nesting is cryptic and unapproachable. an unexplained stripping of static
    return mems_router_find_switch_unlocked(router, name);
}



int mems_router_define_route(struct mems_router *router,
                            const char *input, const char *output,
                            const struct mems_route_step *steps, uint8_t num_steps)
{
    if (router->num_routes >= MEMS_ROUTER_MAX_ROUTES) return -1;
    if (!input || !output || !steps || num_steps == 0 || num_steps > MEMS_ROUTER_MAX_ROUTE_PATH) return -2;

    struct mems_route *route = &router->routes[router->num_routes];
    route->key.input_name = input;
    route->key.output_name = output;
    route->num_steps = num_steps;
    for (uint8_t i = 0; i < num_steps; ++i) {
        if (steps[i].switch_name == NULL) return -3;
        route->steps[i] = steps[i];
    }
    router->num_routes += 1;
    return 0;
}

// Find route and return pointer/step count, or NULL/-1 if not found
const struct mems_route *mems_router_get_route(const struct mems_router *router,
                                               const char *input, const char *output)
{
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

    for (uint8_t i = 0; i < router->num_routes && n_found < max_keys; ++i) {
        const struct mems_route *route = &router->routes[i];
        bool match = true;
        for (uint8_t j = 0; j < route->num_steps; ++j) {
            const struct mems_route_step *step = &route->steps[j];
            struct mems_switch *sw = mems_router_find_switch_unlocked(router, step->switch_name);
            if (!sw || sw->configured_state != step->state) {
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
