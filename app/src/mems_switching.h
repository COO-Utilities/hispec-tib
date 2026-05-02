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
#define MEMS_ROUTER_MAX_ROUTES   18  // TIB=16, CAL=12, AS=4
#define MEMS_ROUTER_MAX_ROUTE_PATH 5 // cal has 5 deep
#define MEMS_ROUTER_MAX_ACTIVE_ROUTES 6
#define MEMS_SWITCH_MAX_TOGGLE_HZ 5.0f
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
    char state; // 'A', 'B' may report with a ? if ~state_known_this_boot
    char target_state; // desired state applied by toggler on next tick
    bool state_known_this_boot;
    float requested_toggle_rate_hz;
    float actual_toggle_rate_hz;
    uint32_t switching_period_cycles; // Quantized switching period in MEMS tick cycles
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
    /* Exact A-state duty numerator and denominator in MEMS tick counts. */
    uint32_t duty_numerator;
    uint32_t duty_denominator;
    /* Duration of one MEMS tick, currently the delayable-work period. */
    uint32_t tick_duration_ms;
    float requested_toggle_rate_hz;
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
    const struct mems_route_step *steps;
    uint8_t num_steps;
};

// -----------------------
// Router Abstraction
// -----------------------
struct mems_router {
    struct mems_switch *switches[MEMS_ROUTER_MAX_SWITCHES];
    uint8_t num_switches;

    const struct mems_route *routes;
    uint8_t num_routes;
    struct k_mutex lock;
    struct k_work_delayable toggler_work;
};

// -----------------------
// Switch Methods
// -----------------------
void mems_switch_init(struct mems_switch *sw, const struct device *gpio_dev,
                      gpio_pin_t pin_a, gpio_pin_t pin_b, const char *name,
                      float configured_toggle_rate_hz, char initial_state);
/**
 * @brief Queue a static or toggling MEMS switch state change.
 *
 * The router-owned delayable work applies pulses on its next tick. A positive
 * @p requested_toggle_rate_hz updates the stored requested rate and is
 * quantized to the nearest firmware tick period before toggling starts.
 */
int mems_switch_set_state(struct mems_switch *sw, char state,
                          float duty_cycle, uint32_t stop_after_s,
                          float requested_toggle_rate_hz);
/**
 * @brief Queue a MEMS state change using exact duty-cycle tick counts.
 *
 * @p state_ticks is the number of ticks spent in @p state during each
 * @p period_ticks cycle. The function converts that to the internal A-state
 * numerator, updates the switch period, and lets the router-owned delayable
 * work apply pulses on subsequent ticks.
 */
int mems_switch_set_state_ticks(struct mems_switch *sw, char state,
                                uint32_t state_ticks, uint32_t period_ticks,
                                uint32_t stop_after_s);
/**
 * @brief Read a MEMS switch state snapshot.
 *
 * The snapshot is taken under the router mutex when the switch belongs to a
 * router. `duty_cycle` and `duty_numerator` describe the internal A-state
 * duty; callers that care about a route's B-state duty must invert the
 * numerator against `duty_denominator`.
 */
void mems_switch_get_status(const struct mems_switch *sw, struct mems_switch_status *out);


// -----------------------
// Router Methods
// -----------------------

/**
 * @brief Initialize a MEMS router from active switches and a static route table.
 *
 * @p routes points at immutable board-specific route data. The router does not
 * copy it; the selected board profile chooses which compile-time table is used.
 */
void mems_router_init(struct mems_router *router, struct mems_switch **switches,
                      uint8_t num_switches, const struct mems_route *routes,
                      uint8_t num_routes);

struct mems_switch *mems_router_find_switch(const struct mems_router *router, const char *name);

const struct mems_route *mems_router_get_route(const struct mems_router *router,
                                               const char *input, const char *output);


//  List all active routes
uint8_t mems_router_active_routes(const struct mems_router *router,
                                  struct mems_route_key *out_keys, uint8_t max_keys);

#endif // MEMS_SWITCHING_H
