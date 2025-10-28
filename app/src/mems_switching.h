//
// Created by Jeb Bailey on 4/23/25.
//
// mems_switching.h
//
// Zephyr/C version: supports switch and router abstractions with named lookup and route tables.

#ifndef MEMS_SWITCHING_H
#define MEMS_SWITCHING_H

#include <zephyr/device.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/logging/log.h>
#include <stdint.h>
#define MEMS_SOURCEDEST_MAX_LEN 24
#define MEMS_SWITCH_PULSE_DELAY_MS 2
#define MEMS_SWITCH_NAME_LEN 24
#define MEMS_ROUTER_MAX_SWITCHES 8
#define MEMS_ROUTER_MAX_ROUTES   18  // TIB=18, CAL=12, AS=2
#define MEMS_ROUTER_MAX_ROUTE_PATH 5 // cal has 5 deep
#define MEMS_ROUTER_MAX_ACTIVE_ROUTES 6


// -----------------------
// Switch Abstraction
// -----------------------
struct mems_switch {
    const struct device *gpio_dev;
    gpio_pin_t pin_a;
    gpio_pin_t pin_b;
    char state; // 'A', 'B', or 'U'
    char name[MEMS_SWITCH_NAME_LEN];
};

// -----------------------
// Route Definitions
// -----------------------
struct mems_route_step {
    const char *switch_name; // Points to .name in mems_switch
    char state;              // 'A' or 'B'
};

struct mems_route_id {
    const char input[MEMS_SOURCEDEST_MAX_LEN];
    const char output[MEMS_SOURCEDEST_MAX_LEN];
};

struct mems_route_key {
    const char *input_name;
    const char *output_name;
};

struct mems_active_routes {
    const struct mems_route_key *routes;
};

struct mems_route {
    struct mems_route_key key;
    struct mems_route_step steps[MEMS_ROUTER_MAX_ROUTE_PATH];
    uint8_t num_steps;
};

// -----------------------
// Router Abstraction
// -----------------------
struct mems_router {
    struct mems_switch *switches[MEMS_ROUTER_MAX_SWITCHES];
    uint8_t num_switches;

    struct mems_route routes[MEMS_ROUTER_MAX_ROUTES];
    uint8_t num_routes;
};

// -----------------------
// Switch Methods
// -----------------------
void mems_switch_init(struct mems_switch *sw, const struct device *gpio_dev,
                      gpio_pin_t pin_a, gpio_pin_t pin_b, const char *name);
int mems_switch_set_state(struct mems_switch *sw, char state);
int mems_switch_get_state(const struct mems_switch *sw, char *out_state);


// -----------------------
// Router Methods
// -----------------------

// Initialize router from an array of pointers to switches
void mems_router_init(struct mems_router *router, struct mems_switch **switches, uint8_t num_switches);

struct mems_switch *mems_router_find_switch(const struct mems_router *router, const char *name);

const struct mems_route *mems_router_get_route(const struct mems_router *router,
                                               const char *input, const char *output);


// // Set a switch by name
// int mems_router_set_switch(struct mems_router *router, const char *name, char state);
//
// // Get state of a switch by name
// int mems_router_get_switch(const struct mems_router *router, const char *name, char *out_state);

// Define a route from input to output with a path (sequence of switch/state pairs)
int mems_router_define_route(struct mems_router *router,
                            const char *input, const char *output,
                            const struct mems_route_step *steps, uint8_t num_steps);


// (Optional) List all active routes
uint8_t mems_router_active_routes(const struct mems_router *router,
                                 struct mems_route_key *out_keys, uint8_t max_keys);

#endif // MEMS_SWITCHING_H
