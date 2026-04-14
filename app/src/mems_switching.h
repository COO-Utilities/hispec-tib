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
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <stdbool.h>
#include <stdint.h>
#define MEMS_SOURCEDEST_MAX_LEN 24
#define MEMS_SWITCH_ELECTRICAL_PULSE_MS 2U
#define MEMS_SWITCH_NAME_LEN 24
#define MEMS_ROUTER_MAX_SWITCHES 8
#define MEMS_ROUTER_MAX_ROUTES   18  // TIB=18, CAL=12, AS=2
#define MEMS_ROUTER_MAX_ROUTE_PATH 5 // cal has 5 deep
#define MEMS_ROUTER_MAX_ACTIVE_ROUTES 6
#define MEMS_SWITCH_MAX_TOGGLE_HZ 50.0f
#define MEMS_SWITCH_MAX_TOGGLE_DURATION_S (4U * 60U * 60U)

struct mems_router;


// -----------------------
// Switch Abstraction
// -----------------------
struct mems_switch {
    const struct device *gpio_dev;
    //todo shouldn't these pins be constant?
    gpio_pin_t pin_a;
    gpio_pin_t pin_b;
    char state; // 'A', 'B', or 'U'
    //todo the target state isn't necessary, if toggling we switch states, if not we change if the configured state doesn't match
    char target_state; // desired state applied by toggler on next tick
    char configured_state; // route/effective-command state: 'A'/'B'/'T'/'U'
    bool state_known_this_boot;
    float duty_cycle;
    //TODO these names are VERY confusing to the uninitiated and suggest significat redundancy. Refactor
    float toggle_rate_hz; // active attained rate; zero means static mode, can't we axe this and use the duty cycle
    float switching_frequency_hz; // compile-time quantized attained rate
    uint32_t toggle_period_cycles;
    uint32_t configured_toggle_period_cycles; //todo how does this differ from toggle_period_cycles?
    uint32_t a_state_cycles;
    uint32_t cycles_until_toggle;
    uint32_t remaining_toggle_cycles; // zero means not toggling
    struct mems_router *owner;
    char name[MEMS_SWITCH_NAME_LEN];
};

struct mems_switch_status {
    char state;
    bool state_known_this_boot;
    float duty_cycle;
    float toggle_rate_hz;
    uint16_t stopafter_s;
};

// -----------------------
// Route Definitions
// -----------------------
struct mems_route_step {
    const char *switch_name; // Points to .name in mems_switch
    char state;              // 'A' or 'B'
};

struct mems_route_id {
    char input[MEMS_SOURCEDEST_MAX_LEN];
    char output[MEMS_SOURCEDEST_MAX_LEN];
};

struct mems_route_key {
    const char *input_name;
    const char *output_name;
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
    struct k_mutex lock;
    struct k_work_delayable toggler_work;
};

// -----------------------
// Switch Methods
// -----------------------
void mems_switch_init(struct mems_switch *sw, const struct device *gpio_dev,
                      gpio_pin_t pin_a, gpio_pin_t pin_b, const char *name,
                      float configured_toggle_rate_hz);
int mems_switch_set_state(struct mems_switch *sw,
                          char state,
                          float duty_cycle,
                          uint32_t stop_after_s);
void mems_switch_get_status(const struct mems_switch *sw, struct mems_switch_status *out);


// -----------------------
// Router Methods
// -----------------------

// Initialize router from an array of pointers to switches
void mems_router_init(struct mems_router *router, struct mems_switch **switches, uint8_t num_switches);

struct mems_switch *mems_router_find_switch(const struct mems_router *router, const char *name);

const struct mems_route *mems_router_get_route(const struct mems_router *router,
                                               const char *input, const char *output);


// Define a route from input to output with a path (sequence of switch/state pairs)
int mems_router_define_route(struct mems_router *router,
                            const char *input, const char *output,
                            const struct mems_route_step *steps, uint8_t num_steps);


//  List all active routes
uint8_t mems_router_active_routes(const struct mems_router *router,
                                  struct mems_route_key *out_keys, uint8_t max_keys);

#endif // MEMS_SWITCHING_H
