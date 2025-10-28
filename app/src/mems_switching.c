//
// Created by Jeb Bailey on 4/23/25.
//
// mems_switching.c

#include "mems_switching.h"
#include <zephyr/kernel.h>
#include <string.h>
#include <stdio.h>
LOG_MODULE_REGISTER(mems_switching, LOG_LEVEL_DBG);

// -----------------------
// Switch Methods
// -----------------------

void mems_switch_init(struct mems_switch *sw, const struct device *gpio_dev,
                      gpio_pin_t pin_a, gpio_pin_t pin_b, const char *name)
{
    sw->gpio_dev = gpio_dev;
    sw->pin_a = pin_a;
    sw->pin_b = pin_b;
    sw->state = 'U';
    strncpy(sw->name, name, MEMS_SWITCH_NAME_LEN-1);
    sw->name[MEMS_SWITCH_NAME_LEN-1] = '\0';

    // Set both control pins to output, default low
    gpio_pin_configure(gpio_dev, pin_a, GPIO_OUTPUT_INACTIVE);
    gpio_pin_configure(gpio_dev, pin_b, GPIO_OUTPUT_INACTIVE);
}


int mems_switch_set_state(struct mems_switch *sw, char state)
{
    gpio_pin_t pin;
    if (state == 'A') {
        pin = sw->pin_a;
    } else if (state == 'B') {
        pin = sw->pin_b;
    } else {
        LOG_ERR("Invalid MEMS state: %c for %s\n", state, sw->name);
        return -1;
    }

    // Pulse the control pin: low → high → low (with ms delays)
    gpio_pin_set(sw->gpio_dev, pin, 0);
    k_msleep(MEMS_SWITCH_PULSE_DELAY_MS);
    gpio_pin_set(sw->gpio_dev, pin, 1);
    k_msleep(MEMS_SWITCH_PULSE_DELAY_MS);
    gpio_pin_set(sw->gpio_dev, pin, 0);
    k_msleep(MEMS_SWITCH_PULSE_DELAY_MS);

    sw->state = state;
    return 0;
}

int mems_switch_get_state(const struct mems_switch *sw, char *out_state)
{
    if (!sw || !out_state) return -1;
    *out_state = sw->state;
    return (sw->state == 'A' || sw->state == 'B') ? 0 : -1;
}

// -----------------------
// Router Methods
// -----------------------

void mems_router_init(struct mems_router *router, struct mems_switch **switches, uint8_t num_switches)
{
    router->num_switches = (num_switches > MEMS_ROUTER_MAX_SWITCHES) ? MEMS_ROUTER_MAX_SWITCHES : num_switches;
    for (uint8_t i = 0; i < router->num_switches; ++i) {
        router->switches[i] = switches[i];
    }
    router->num_routes = 0;
}

struct mems_switch *mems_router_find_switch(const struct mems_router *router, const char *name)
{
    for (uint8_t i = 0; i < router->num_switches; ++i) {
        if (strncmp(router->switches[i]->name, name, MEMS_SWITCH_NAME_LEN) == 0) {
            return router->switches[i];
        }
    }
    return NULL;
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
    for (uint8_t i = 0; i < router->num_routes && n_found < max_keys; ++i) {
        const struct mems_route *route = &router->routes[i];
        bool match = true;
        for (uint8_t j = 0; j < route->num_steps; ++j) {
            const struct mems_route_step *step = &route->steps[j];
            struct mems_switch *sw = mems_router_find_switch(router, step->switch_name);
            if (!sw || sw->state != step->state) {
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
    return n_found;
}