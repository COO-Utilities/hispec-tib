//
// Created by Jeb Bailey on 5/19/25.
//
/*
 * Copyright (c) 2024 Analog Devices, Inc.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef __DEVICE_C__
#define __DEVICE_C__

#include "devices.h"
#include "mems_switching.h"

#include <errno.h>
#include <string.h>
#include <zephyr/sys/util.h>

LOG_MODULE_REGISTER(devices, LOG_LEVEL_INF);

#define USER_NODE DT_PATH(zephyr_user)
#define MAX_NUM_MEMS_SWITCHES 8U
#define CAL_ATTENUATOR_INDEX 4U

/* Devices */

const struct gpio_dt_spec power_gpio = GPIO_DT_SPEC_GET(USER_NODE, power_gpios);

#if DT_NODE_HAS_PROP(USER_NODE, board_type_tib_gpios)
static const struct gpio_dt_spec board_type_tib_gpio =
	GPIO_DT_SPEC_GET(USER_NODE, board_type_tib_gpios);
#else
static const struct gpio_dt_spec board_type_tib_gpio = {0};
#endif

#if DT_NODE_HAS_PROP(USER_NODE, board_type_cal_blue_gpios)
static const struct gpio_dt_spec board_type_cal_blue_gpio =
	GPIO_DT_SPEC_GET(USER_NODE, board_type_cal_blue_gpios);
#else
static const struct gpio_dt_spec board_type_cal_blue_gpio = {0};
#endif

#if DT_NODE_HAS_PROP(USER_NODE, board_type_cal_red_gpios)
static const struct gpio_dt_spec board_type_cal_red_gpio =
	GPIO_DT_SPEC_GET(USER_NODE, board_type_cal_red_gpios);
#else
static const struct gpio_dt_spec board_type_cal_red_gpio = {0};
#endif

#if DT_NODE_HAS_PROP(USER_NODE, board_type_as_gpios)
static const struct gpio_dt_spec board_type_as_gpio =
	GPIO_DT_SPEC_GET(USER_NODE, board_type_as_gpios);
#else
static const struct gpio_dt_spec board_type_as_gpio = {0};
#endif

#if DT_HAS_COMPAT_STATUS_OKAY(zephyr_modbus_serial)
#define MODBUS_NODE DT_COMPAT_GET_ANY_STATUS_OKAY(zephyr_modbus_serial)
static const char modbus_name[] = DEVICE_DT_NAME(MODBUS_NODE);
#endif

#if DT_NODE_EXISTS(DT_NODELABEL(adc1115))
const struct device *adc_dev = DEVICE_DT_GET(DT_NODELABEL(adc1115));
#else
const struct device *adc_dev = NULL;
#endif

#if DT_NODE_EXISTS(DT_NODELABEL(dac7578))
const struct device *dac_dev = DEVICE_DT_GET(DT_NODELABEL(dac7578));
#else
const struct device *dac_dev = NULL;
#endif

#if DT_NODE_EXISTS(DT_NODELABEL(pcal6416a))
const struct device *gpio_dev = DEVICE_DT_GET(DT_NODELABEL(pcal6416a));
#else
const struct device *gpio_dev = NULL;
#endif

struct attenuator attenuators[NUM_ATTENUATORS];
struct mems_switch mems_switches[MEMS_ROUTER_MAX_SWITCHES];
struct mems_router router;

static const char *const tib_switch_names[8] = {
	"yj_cal_laser", "hk_cal_laser",
	"yj_ao_fei", "hk_ao_fei",
	"yj_forward_retro", "hk_forward_retro",
	"yj_mm_sm", "hk_mm_sm",
};

static const char *const as_switch_names[6] = {
	"yj_as1", "yj_as2", "yj_as3",
	"hk_as1", "hk_as2", "hk_as3",
};

/* TODO decide on final CAL route/switch names once the fiber path names are finalized. */
static const char *const cal_switch_names[7] = {
	"cal1", "cal2", "cal3", "cal4", "cal5", "cal6", "cal7",
};

/* The GPIO expander wiring is the same 2-pin sequence on each populated board.
 * Board profiles below limit how many of these pairs are instantiated.
 */
static const gpio_pin_t mems_switch_pin_pairs[MAX_NUM_MEMS_SWITCHES][2] = {
	{0, 1}, {2, 3}, {4, 5}, {6, 7},
	{8, 9}, {10, 11}, {12, 13}, {14, 15},
};

/* Per-switch compile-time nominal toggle rates (Hz), quantized in mems_switch_init(). */
static const float mems_switch_toggle_rate_hz[MAX_NUM_MEMS_SWITCHES] = {
	5.0f, 5.0f, 5.0f, 5.0f, 5.0f, 5.0f, 5.0f, 5.0f,
};

struct board_profile {
	enum hispec_board_type type;
	const char *name;
	uint8_t mems_switch_count;
	const char *const *switch_names;
	uint8_t attenuator_first;
	uint8_t attenuator_count;
	bool has_laser_bank;
	bool has_photodiodes;
	bool has_laser_power_control;
	bool has_relay_box;
};

static const struct board_profile unknown_profile = {
	.type = HISPEC_BOARD_UNKNOWN,
	.name = "unknown",
};

static const struct board_profile fault_profile = {
	.type = HISPEC_BOARD_FAULT,
	.name = "fault",
};

static const struct board_profile tib_profile = {
	.type = HISPEC_BOARD_TIB,
	.name = "tib",
	.mems_switch_count = 8,
	.switch_names = tib_switch_names,
	.attenuator_first = 0,
	.attenuator_count = NUM_ATTENUATORS,
	.has_laser_bank = true,
	.has_photodiodes = true,
	.has_laser_power_control = true,
	.has_relay_box = true,
};

static const struct board_profile cal_blue_profile = {
	.type = HISPEC_BOARD_CAL_BLUE,
	.name = "cal_blue",
	.mems_switch_count = 7,
	.switch_names = cal_switch_names,
	.attenuator_first = CAL_ATTENUATOR_INDEX,
	.attenuator_count = 1,
};

static const struct board_profile cal_red_profile = {
	.type = HISPEC_BOARD_CAL_RED,
	.name = "cal_red",
	.mems_switch_count = 7,
	.switch_names = cal_switch_names,
	.attenuator_first = CAL_ATTENUATOR_INDEX,
	.attenuator_count = 1,
};

static const struct board_profile as_profile = {
	.type = HISPEC_BOARD_AS,
	.name = "as",
	.mems_switch_count = 6,
	.switch_names = as_switch_names,
};

static K_MUTEX_DEFINE(board_profile_lock);
static const struct board_profile *active_profile = &unknown_profile;
static bool board_type_checked;

struct board_strap {
	const struct gpio_dt_spec *gpio;
	enum hispec_board_type type;
	const char *name;
};

static const struct board_strap board_straps[] = {
	{&board_type_tib_gpio, HISPEC_BOARD_TIB, "tib"},
	{&board_type_cal_blue_gpio, HISPEC_BOARD_CAL_BLUE, "cal_blue"},
	{&board_type_cal_red_gpio, HISPEC_BOARD_CAL_RED, "cal_red"},
	{&board_type_as_gpio, HISPEC_BOARD_AS, "as"},
};

struct route_definition {
	const char *input;
	const char *output;
	const struct mems_route_step *steps;
	uint8_t step_count;
};

#define ROUTE_DEF(input_, output_, steps_) \
	{ .input = (input_), .output = (output_), .steps = (steps_), .step_count = ARRAY_SIZE(steps_) }

static const struct mems_route_step tib_yj_1430_to_yj_ao[] = {
	{"yj_cal_laser", 'B'},
	{"yj_forward_retro", 'A'},
	{"yj_ao_fei", 'A'},
};
static const struct mems_route_step tib_yj_1430_to_yj_fei[] = {
	{"yj_cal_laser", 'B'},
	{"yj_forward_retro", 'A'},
	{"yj_ao_fei", 'B'},
};
static const struct mems_route_step tib_yj_cal_to_yj_ao[] = {
	{"yj_cal_laser", 'A'},
	{"yj_ao_fei", 'A'},
};
static const struct mems_route_step tib_yj_cal_to_yj_fei[] = {
	{"yj_cal_laser", 'A'},
	{"yj_ao_fei", 'B'},
};
static const struct mems_route_step tib_yj_laser_to_yj_ao[] = {
	{"yj_cal_laser", 'B'},
	{"yj_ao_fei", 'A'},
};
static const struct mems_route_step tib_yj_laser_to_yj_fei[] = {
	{"yj_cal_laser", 'B'},
	{"yj_ao_fei", 'B'},
};
static const struct mems_route_step tib_yj_mm_to_yj_pd[] = {
	{"yj_mm_sm", 'A'},
};
static const struct mems_route_step tib_yj_sm_to_yj_pd[] = {
	{"yj_mm_sm", 'B'},
};
static const struct mems_route_step tib_hk_1430_to_hk_ao[] = {
	{"hk_cal_laser", 'B'},
	{"hk_forward_retro", 'A'},
	{"hk_ao_fei", 'A'},
};
static const struct mems_route_step tib_hk_1430_to_hk_fei[] = {
	{"hk_cal_laser", 'B'},
	{"hk_forward_retro", 'A'},
	{"hk_ao_fei", 'B'},
};
static const struct mems_route_step tib_hk_cal_to_hk_ao[] = {
	{"hk_cal_laser", 'A'},
	{"hk_ao_fei", 'A'},
};
static const struct mems_route_step tib_hk_cal_to_hk_fei[] = {
	{"hk_cal_laser", 'A'},
	{"hk_ao_fei", 'B'},
};
static const struct mems_route_step tib_hk_laser_to_hk_ao[] = {
	{"hk_cal_laser", 'B'},
	{"hk_ao_fei", 'A'},
};
static const struct mems_route_step tib_hk_laser_to_hk_fei[] = {
	{"hk_cal_laser", 'B'},
	{"hk_ao_fei", 'B'},
};
static const struct mems_route_step tib_hk_mm_to_hk_pd[] = {
	{"hk_mm_sm", 'A'},
};
static const struct mems_route_step tib_hk_sm_to_hk_pd[] = {
	{"hk_mm_sm", 'B'},
};

static const struct route_definition tib_routes[] = {
	ROUTE_DEF("yj_1430", "yj_ao", tib_yj_1430_to_yj_ao),
	ROUTE_DEF("yj_1430", "yj_fei", tib_yj_1430_to_yj_fei),
	ROUTE_DEF("yj_cal", "yj_ao", tib_yj_cal_to_yj_ao),
	ROUTE_DEF("yj_cal", "yj_fei", tib_yj_cal_to_yj_fei),
	ROUTE_DEF("yj_laser", "yj_ao", tib_yj_laser_to_yj_ao),
	ROUTE_DEF("yj_laser", "yj_fei", tib_yj_laser_to_yj_fei),
	ROUTE_DEF("yj_mm", "yj_pd", tib_yj_mm_to_yj_pd),
	ROUTE_DEF("yj_sm", "yj_pd", tib_yj_sm_to_yj_pd),
	ROUTE_DEF("hk_1430", "hk_ao", tib_hk_1430_to_hk_ao),
	ROUTE_DEF("hk_1430", "hk_fei", tib_hk_1430_to_hk_fei),
	ROUTE_DEF("hk_cal", "hk_ao", tib_hk_cal_to_hk_ao),
	ROUTE_DEF("hk_cal", "hk_fei", tib_hk_cal_to_hk_fei),
	ROUTE_DEF("hk_laser", "hk_ao", tib_hk_laser_to_hk_ao),
	ROUTE_DEF("hk_laser", "hk_fei", tib_hk_laser_to_hk_fei),
	ROUTE_DEF("hk_mm", "hk_pd", tib_hk_mm_to_hk_pd),
	ROUTE_DEF("hk_sm", "hk_pd", tib_hk_sm_to_hk_pd),
};

/* AS splitter routes define the switch order used by splitting_set().
 * Step 1 selects output 1, step 2 is held on the splitter branch, and
 * step 3 is extended by step 1's deadtime before selecting output 2.
 */
static const struct mems_route_step as_yj_split_to_as[] = {
	{"yj_as1", 'A'},
	{"yj_as2", 'B'},
	{"yj_as3", 'A'},
};
static const struct mems_route_step as_hk_split_to_as[] = {
	{"hk_as1", 'A'},
	{"hk_as2", 'B'},
	{"hk_as3", 'A'},
};
static const struct mems_route_step as_yj_split_to_cal[] = {
	{"yj_as1", 'A'},
	{"yj_as2", 'A'},
};
static const struct mems_route_step as_hk_split_to_cal[] = {
	{"hk_as1", 'A'},
	{"hk_as2", 'A'},
};

static const struct route_definition as_routes[] = {
	ROUTE_DEF("yj_calin", "yj_split", as_yj_split_to_as),
	ROUTE_DEF("hk_calin", "hk_split", as_hk_split_to_as),
	ROUTE_DEF("yj_calin", "yj_cal", as_yj_split_to_cal),
	ROUTE_DEF("hk_calin", "hk_cal", as_hk_split_to_cal),
};

static const struct board_profile *profile_for_type(enum hispec_board_type type)
{
	switch (type) {
	case HISPEC_BOARD_TIB:
		return &tib_profile;
	case HISPEC_BOARD_CAL_BLUE:
		return &cal_blue_profile;
	case HISPEC_BOARD_CAL_RED:
		return &cal_red_profile;
	case HISPEC_BOARD_AS:
		return &as_profile;
	case HISPEC_BOARD_FAULT:
		return &fault_profile;
	case HISPEC_BOARD_UNKNOWN:
	default:
		return &unknown_profile;
	}
}

static const struct board_profile *current_profile(void)
{
	const struct board_profile *profile;

	k_mutex_lock(&board_profile_lock, K_FOREVER);
	profile = active_profile;
	k_mutex_unlock(&board_profile_lock);

	return profile;
}

static void set_current_profile(const struct board_profile *profile, bool checked)
{
	k_mutex_lock(&board_profile_lock, K_FOREVER);
	active_profile = profile != NULL ? profile : &unknown_profile;
	board_type_checked = checked;
	k_mutex_unlock(&board_profile_lock);
}

static bool all_board_straps_mapped(void)
{
	for (uint8_t i = 0; i < ARRAY_SIZE(board_straps); ++i) {
		if (board_straps[i].gpio->port == NULL) {
			return false;
		}
	}

	return true;
}

static int board_strap_read_active(const struct board_strap *strap, bool *active)
{
	int value;
	int rc;

	if (strap == NULL || active == NULL || strap->gpio->port == NULL) {
		return -ENODEV;
	}
	if (!gpio_is_ready_dt(strap->gpio)) {
		return -ENODEV;
	}

	/* gpio_pin_configure_dt() applies the GPIO_ACTIVE_LOW and GPIO_PULL_UP
	 * flags from the overlay. gpio_pin_get_dt() then returns logical active
	 * state, so an active-low jumper shorted to ground reads as true.
	 */
	rc = gpio_pin_configure_dt(strap->gpio, GPIO_INPUT);
	if (rc != 0) {
		return rc;
	}

	value = gpio_pin_get_dt(strap->gpio);
	if (value < 0) {
		return value;
	}

	*active = (value != 0);
	return 0;
}

int devices_detect_board_type(void)
{
	enum hispec_board_type detected = HISPEC_BOARD_UNKNOWN;
	uint8_t active_count = 0U;
	int first_error = 0;

	if (!all_board_straps_mapped()) {
		LOG_ERR("Board type strap GPIOs are not mapped in devicetree");
		set_current_profile(&unknown_profile, true);
		return -ENODEV;
	}

	for (uint8_t i = 0; i < ARRAY_SIZE(board_straps); ++i) {
		bool active = false;
		int rc = board_strap_read_active(&board_straps[i], &active);

		if (rc != 0) {
			LOG_ERR("Failed to read board strap %s (%d)", board_straps[i].name, rc);
			if (first_error == 0) {
				first_error = rc;
			}
			continue;
		}

		LOG_DBG("Board strap %s active=%d", board_straps[i].name, active ? 1 : 0);
		if (active) {
			active_count++;
			detected = board_straps[i].type;
		}
	}

	if (first_error != 0) {
		set_current_profile(&fault_profile, true);
		return first_error;
	}

	if (active_count == 1U) {
		const struct board_profile *profile = profile_for_type(detected);

		set_current_profile(profile, true);
		LOG_INF("Detected PCB board type: %s", profile->name);
		return 0;
	}

	if (active_count > 1U) {
		LOG_ERR("Multiple board type straps are active; refusing board-specific setup");
		set_current_profile(&fault_profile, true);
		return -EIO;
	}

	LOG_ERR("No board type strap is active; refusing board-specific setup");
	set_current_profile(&unknown_profile, true);
	return -ENODEV;
}

bool devices_board_type_checked(void)
{
	bool checked;

	k_mutex_lock(&board_profile_lock, K_FOREVER);
	checked = board_type_checked;
	k_mutex_unlock(&board_profile_lock);

	return checked;
}

enum hispec_board_type devices_board_type(void)
{
	return current_profile()->type;
}

const char *devices_board_type_name(void)
{
	return current_profile()->name;
}

bool devices_board_type_valid(void)
{
	enum hispec_board_type type = devices_board_type();

	return type == HISPEC_BOARD_TIB ||
	       type == HISPEC_BOARD_CAL_BLUE ||
	       type == HISPEC_BOARD_CAL_RED ||
	       type == HISPEC_BOARD_AS;
}

bool devices_has_laser_bank(void)
{
	return current_profile()->has_laser_bank;
}

bool devices_has_laser_power_control(void)
{
	return current_profile()->has_laser_power_control;
}

bool devices_has_photodiodes(void)
{
	return current_profile()->has_photodiodes;
}

bool devices_has_attenuators(void)
{
	return current_profile()->attenuator_count > 0U;
}

bool devices_attenuator_index_available(uint8_t index)
{
	const struct board_profile *profile = current_profile();

	return index >= profile->attenuator_first &&
	       index < profile->attenuator_first + profile->attenuator_count;
}

uint8_t devices_mems_switch_count(void)
{
	return current_profile()->mems_switch_count;
}

static bool device_ready_or_log(const struct device *dev, const char *label)
{
	if (dev == NULL) {
		LOG_ERR("%s device is not in devicetree", label);
		return false;
	}
	if (!device_is_ready(dev)) {
		LOG_ERR("Device %s is not ready", dev->name);
		return false;
	}

	LOG_INF("Device %s is ready", dev->name);
	return true;
}

static bool setup_modbus_client(void)
{
#if DT_HAS_COMPAT_STATUS_OKAY(zephyr_modbus_serial)
	struct modbus_iface_param modbus_cfg = {
		.mode = MODBUS_MODE_RTU,
		.serial = {
			.baud = 115200,
			.parity = UART_CFG_PARITY_NONE,
			.stop_bits = UART_CFG_STOP_BITS_1,
		},
		.rx_timeout = MODBUS_RX_TIMEOUT_MS,
	};

	int client_iface = modbus_iface_get_by_name(modbus_name);

	if (client_iface < 0) {
		LOG_ERR("Modbus interface %s not found", modbus_name);
		return false;
	}
	if (modbus_init_client(client_iface, modbus_cfg) == 0) {
		LOG_INF("Modbus client initialized on %s", modbus_name);
		return true;
	}

	LOG_ERR("Modbus init failed");
	return false;
#else
	LOG_ERR("Modbus serial device is not configured");
	return false;
#endif
}

void setup_attenuators(void)
{
	const struct board_profile *profile = current_profile();

	if (!devices_has_attenuators()) {
		LOG_INF("Board %s has no attenuator channels", profile->name);
		return;
	}
	if (!device_ready_or_log(dac_dev, "DAC")) {
		return;
	}

	for (uint8_t i = 0; i < profile->attenuator_count; ++i) {
		uint8_t attenuator_index = profile->attenuator_first + i;

		if (attenuator_index >= NUM_ATTENUATORS) {
			LOG_ERR("Profile %s attenuator index %u is out of range",
				profile->name, attenuator_index);
			continue;
		}

		attenuator_init(&attenuators[attenuator_index], attenuator_index);
	}
}

static void register_routes(const struct route_definition *routes, size_t route_count)
{
	for (size_t i = 0; i < route_count; ++i) {
		int rc = mems_router_define_route(&router,
						  routes[i].input,
						  routes[i].output,
						  routes[i].steps,
						  routes[i].step_count);

		if (rc != 0) {
			LOG_ERR("Failed to register MEMS route %s -> %s (%d)",
				routes[i].input, routes[i].output, rc);
		}
	}
}

void setup_mems_switches_and_routes(void)
{
	const struct board_profile *profile = current_profile();
	struct mems_switch *mems_switch_ptrs[MEMS_ROUTER_MAX_SWITCHES] = {0};

	if (!devices_board_type_valid()) {
		LOG_ERR("Cannot configure MEMS routes: board type is %s", profile->name);
		mems_router_init(&router, mems_switch_ptrs, 0);
		return;
	}

	if (!device_ready_or_log(gpio_dev, "MEMS GPIO expander")) {
		mems_router_init(&router, mems_switch_ptrs, 0);
		return;
	}

	for (uint8_t i = 0; i < profile->mems_switch_count; ++i) {
		mems_switch_init(&mems_switches[i],
				 gpio_dev,
				 mems_switch_pin_pairs[i][0],
				 mems_switch_pin_pairs[i][1],
				 profile->switch_names[i],
				 mems_switch_toggle_rate_hz[i],
				 'A');
		mems_switch_ptrs[i] = &mems_switches[i];
	}

	mems_router_init(&router, mems_switch_ptrs, profile->mems_switch_count);

	switch (profile->type) {
	case HISPEC_BOARD_TIB:
		register_routes(tib_routes, ARRAY_SIZE(tib_routes));
		break;
	case HISPEC_BOARD_AS:
		register_routes(as_routes, ARRAY_SIZE(as_routes));
		break;
	case HISPEC_BOARD_CAL_BLUE:
	case HISPEC_BOARD_CAL_RED:
		/* CAL route names/paths are hardware-fixed but not yet documented. */
		LOG_WRN("CAL MEMS routes are not defined yet");
		break;
	case HISPEC_BOARD_UNKNOWN:
	case HISPEC_BOARD_FAULT:
	default:
		break;
	}

	LOG_INF("Configured %u MEMS switches and %u routes for %s",
		router.num_switches, router.num_routes, profile->name);
}

bool devices_ready(void)
{
	const struct board_profile *profile = current_profile();
	bool rc = true;

	if (!devices_board_type_checked()) {
		int detect_rc = devices_detect_board_type();

		if (detect_rc != 0) {
			rc = false;
		}
		profile = current_profile();
	}

	if (!devices_board_type_valid()) {
		LOG_ERR("Board type %s is not valid for device setup", profile->name);
		return false;
	}

	if (!device_ready_or_log(gpio_dev, "MEMS GPIO expander")) {
		rc = false;
	}

	if (profile->has_laser_power_control) {
		if (!gpio_is_ready_dt(&power_gpio)) {
			LOG_ERR("Power GPIO is not ready");
			rc = false;
		} else if (gpio_pin_configure_dt(&power_gpio, GPIO_OUTPUT_INACTIVE) != 0) {
			LOG_ERR("Failed to configure power GPIO");
			rc = false;
		}
	}

	if (profile->has_laser_bank && !setup_modbus_client()) {
		rc = false;
	}

	if (profile->attenuator_count > 0U && !device_ready_or_log(dac_dev, "DAC")) {
		rc = false;
	}

	if (profile->has_photodiodes && !device_ready_or_log(adc_dev, "ADC")) {
		rc = false;
	}

	if (profile->has_relay_box) {
		LOG_INF("Relay-box profile enabled; DS2408 command support is still pending");
	}

	return rc;
}

#endif /* __DEVICE_C__ */
