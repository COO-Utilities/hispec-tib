/**
 * @file mems_switching.h
 * @brief MEMS switch pulse scheduling and board-local route tables.
 *
 * A kernel timer provides the MEMS cadence and a dedicated MEMS thread performs
 * GPIO-expander writes. The thread clears pulse pins, applies requested
 * static/toggling switch states, and quantizes requested dwell timing into
 * fixed MEMS ticks. Public calls can sleep on the router mutex but do not
 * publish MQTT or persist state.
 */

#ifndef MEMS_SWITCHING_H
#define MEMS_SWITCHING_H

#include <zephyr/device.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#define MEMS_SOURCEDEST_MAX_LEN 24
#define MEMS_SWITCH_ELECTRICAL_PULSE_MS 20U  //datasheet says pulse width >=15ms
#define MEMS_SWITCH_ELECTRICAL_PULSE_FFLS_MS 50U //datasheet says pulse width of 50ms typical (ch 7&8 on tib)
#define MEMS_SWITCH_ROUTER_TICK_MS 10U
#define MEMS_SWITCH_NAME_LEN 24
#define MEMS_ROUTER_MAX_SWITCHES 8
#define MEMS_ROUTER_MAX_ROUTES   18  // TIB=16, CAL=12, AS=4
#define MEMS_ROUTER_MAX_ROUTE_PATH 5 // cal has 5 deep
#define MEMS_ROUTER_MAX_ACTIVE_ROUTES 6
#define MEMS_SWITCH_MAX_TOGGLE_HZ 5.0 /* Datasheet max actuation pulses per second. */
#define MEMS_SWITCH_MAX_TOGGLE_DURATION_S (4U * 60U * 60U)
#define MEMS_SPLIT_CHANNEL_COUNT 2
#define MEMS_SPLIT_OUTPUT_COUNT 3
#define MEMS_SPLIT_ROUTE_SWITCH_COUNT 3

struct mems_router;

enum mems_switch_type {
    MEMS_SWITCH_TYPE_FFSW,
    MEMS_SWITCH_TYPE_FFLS,
};

/** Runtime state for one dual-coil MEMS switch.
 *
 * `switch_type` is assigned once by mems_switch_init() from the active board
 * profile. It controls the electrical pulse width used by router work ticks.
 */
struct mems_switch {
    /* Assigned once by mems_switch_init() from the active board profile. */
    struct gpio_dt_spec gpio_a;
    struct gpio_dt_spec gpio_b;
    enum mems_switch_type switch_type;
    char state; // 'A', 'B' may report with a ? if ~state_known_this_boot
    char target_state; // desired state applied by toggler on next tick
    bool state_known_this_boot;
    uint32_t switching_period_cycles; // Quantized switching period in MEMS tick cycles
    uint32_t a_state_cycles;
    uint32_t cycles_until_toggle;
    uint32_t remaining_toggle_cycles; // zero means not toggling
    uint32_t pulse_clear_at_ms;
    /* Start time of the last A or B actuation pulse. */
    uint32_t last_pulse_at_ms;
    bool pulse_active;
    bool force_pulse_pending;
    uint8_t service_ticks_remaining;
    struct mems_router *owner;
    char name[MEMS_SWITCH_NAME_LEN];
};

struct mems_switch_status {
    char state;
    bool state_known_this_boot;
    double duty_cycle;
    uint32_t cycle_ms;
    uint32_t a_ms;
    uint32_t b_ms;
    uint32_t stop_in_s;
};

/** One required switch state within a named route. */
struct mems_route_step {
    const char *switch_name; // Points to .name in mems_switch
    char state;              // 'A' or 'B'
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

struct mems_split_switch_duty {
    char name[MEMS_SWITCH_NAME_LEN];
    char state;
    double duty_cycle;
    uint32_t a_ms;
    uint32_t b_ms;
};

struct mems_split_state {
    double requested[MEMS_SPLIT_OUTPUT_COUNT];
    double actual[MEMS_SPLIT_OUTPUT_COUNT];
    double output[MEMS_SPLIT_OUTPUT_COUNT];
    double transmission[MEMS_SPLIT_OUTPUT_COUNT];
    struct mems_split_switch_duty switches[MEMS_SPLIT_ROUTE_SWITCH_COUNT];
    uint32_t cycle_ms;
    uint32_t stop_in_s;
};

/** Board-selected router state and immutable route table pointer. */
struct mems_router {
    struct mems_switch *switches[MEMS_ROUTER_MAX_SWITCHES];
    uint8_t num_switches;

    const struct mems_route *routes;
    uint8_t num_routes;
    struct k_mutex lock;
};

/**
 * @brief Initialize a MEMS switch object and configure its two GPIO outputs inactive.
 *
 * The hardware state is not known after boot until the delayable tick sends a
 * logical active pulse. Pulse cleanup uses a kernel-uptime deadline from the
 * successful GPIO set, so a delayed router tick may extend but not shorten the
 * physical pulse.
 */
void mems_switch_init(struct mems_switch *sw,
                      const struct gpio_dt_spec *gpio_a,
                      const struct gpio_dt_spec *gpio_b,
                      const char *name,
                      enum mems_switch_type switch_type,
                      char initial_state);
/**
 * @brief Queue a static or toggling MEMS switch state change.
 *
 * The router-owned MEMS thread applies pulses on its next tick. A positive
 * @p requested_cycle_ms is quantized to MEMS service ticks but not stretched;
 * the duty cycle is quantized inside that fixed cycle so A/B actuation pulses
 * satisfy the datasheet spacing limit. A zero value selects the fastest safe
 * cycle for the requested duty. @p force queues one same-state actuation pulse
 * for static profiles; repeated force requests coalesce until the pulse fires.
 */
int mems_switch_set_state(struct mems_switch *sw, char state,
                          double duty_cycle, uint32_t off_in_s,
                          double requested_cycle_ms, bool force);
/**
 * @brief Queue a MEMS state change using exact duty-cycle tick counts.
 *
 * @p state_ticks is the number of ticks spent in @p state during each
 * @p period_ticks cycle. The function converts that to the internal A-state
 * dwell count, updates the switch period, and lets the router-owned MEMS thread
 * apply pulses on subsequent ticks.
 */
int mems_switch_set_state_ticks(struct mems_switch *sw, char state,
                                uint32_t state_ticks, uint32_t period_ticks,
                                uint32_t off_in_s);
/**
 * @brief Read a MEMS switch state snapshot.
 *
 * The snapshot is taken under the router mutex when the switch belongs to a
 * router. `duty_cycle` is the A-state fraction for normal switch commands;
 * split responses convert this same A/B timing into the selected route-state
 * fraction.
 */
void mems_switch_get_status(const struct mems_switch *sw, struct mems_switch_status *out);


/**
 * @brief Initialize a MEMS router from active switches and a static route table.
 *
 * @p routes points at immutable board-specific route data. The router does not
 * copy it; the selected board profile chooses which compile-time table is used.
 */
void mems_router_init(struct mems_router *router, struct mems_switch **switches,
                      uint8_t num_switches, const struct mems_route *routes,
                      uint8_t num_routes);

/** @brief Find a switch by command/API name; returns NULL if not present. */
struct mems_switch *mems_router_find_switch(const struct mems_router *router, const char *name);

/** @brief Find a route by input/output labels in the board-selected route table. */
const struct mems_route *mems_router_get_route(const struct mems_router *router,
                                               const char *input, const char *output);

/**
 * @brief Apply every switch step in one static route.
 *
 * This queues switch state changes through mems_switch_set_state() and can
 * sleep on the router mutex. The caller owns route lookup and command response
 * formatting. On a per-switch failure, @p failed_switch and @p failed_state
 * identify the route step that failed when non-NULL. @p force queues one
 * same-state actuation pulse for each route step.
 */
int mems_router_apply_route(const struct mems_router *router,
                            const struct mems_route *route,
                            bool force,
                            const char **failed_switch,
                            char *failed_state);

/**
 * @brief Look up and apply a named input/output route.
 *
 * This can sleep on the router mutex through MEMS switch operations. It keeps
 * command modules from duplicating route lookup before setting a simple static
 * route. @p force is passed through to every route step.
 */
int mems_router_apply_named_route(const struct mems_router *router,
                                  const char *input,
                                  const char *output,
                                  bool force,
                                  const char **failed_switch,
                                  char *failed_state);

/** @brief List static routes whose switches currently match all required states. */
uint8_t mems_router_active_routes(const struct mems_router *router,
                                  struct mems_route_key *out_keys, uint8_t max_keys);

/** @brief Return the API name for one AS split channel, or NULL if invalid. */
const char *mems_split_channel_name(uint8_t channel_index);

/** @brief Map an AS split channel name such as "yj" or "hk" to an index. */
int mems_split_channel_index(const char *channel, uint8_t *index);

/** @brief Format the app-settings route name used by one AS split channel. */
int mems_split_route_name(uint8_t channel_index, char *out, size_t out_len);

/** @brief Return the app-settings key for one split output transmission. */
const char *mems_split_output_loss_key(uint8_t output_index);

/**
 * @brief Read current AS split route state into @p out.
 *
 * The route is selected from the board MEMS route table. This can sleep on the
 * router mutex while reading switch snapshots and on settings while reading
 * split transmissions. If @p requested is non-NULL it becomes the stored
 * requested ratio for future responses; otherwise the last requested ratio is
 * retained.
 */
int mems_split_read_channel_state(const struct mems_router *router,
                                  uint8_t channel_index,
                                  const double requested[MEMS_SPLIT_OUTPUT_COUNT],
                                  struct mems_split_state *out);

/**
 * @brief Apply one AS split channel as three output ratios.
 *
 * The user-facing command provides ratio1 and ratio2; this domain helper
 * receives all three normalized output ratios, applies route-loss transmission
 * correction, and converts the corrected duty targets to exact MEMS ticks. An
 * explicit cycle is not stretched; output dwell ticks are solved within the
 * fixed PCB switch-boundary order. It can sleep on the router mutex through
 * MEMS switch operations and on settings while reading split transmissions. It
 * does not publish warnings or parse command payloads.
 */
int mems_split_apply_channel(const struct mems_router *router,
                             uint8_t channel_index,
                             const double requested[MEMS_SPLIT_OUTPUT_COUNT],
                             uint32_t cycle_ms,
                             uint32_t off_in_s,
                             struct mems_split_state *out,
                             const char **failed_switch);

#endif // MEMS_SWITCHING_H
