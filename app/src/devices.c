/**
 * @file devices.c
 * @brief Board strap detection and board-profile-specific device setup.
 */
/*
 * Copyright (c) 2024 Analog Devices, Inc.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef __DEVICE_C__
#define __DEVICE_C__

#include "devices.h"
#include "app_identity.h"
#include "app_settings.h"
#include "command.h"
#include "mems_switching.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <zephyr/drivers/hwinfo.h>
#include <zephyr/sys/util.h>

LOG_MODULE_REGISTER(devices, LOG_LEVEL_INF);

#define USER_NODE DT_PATH(zephyr_user)
#define MAX_NUM_MEMS_SWITCHES 8U
#define CAL_ATTENUATOR_INDEX 4U

BUILD_ASSERT(APP_ATTENUATOR_CHANNEL_COUNT == NUM_ATTENUATORS,
	     "Persistent attenuator settings must match logical attenuator count");

/* Devices */
const struct gpio_dt_spec laser_power_gpio = GPIO_DT_SPEC_GET(USER_NODE, laser_power_gpios);
const struct gpio_dt_spec heater_power_gpio = GPIO_DT_SPEC_GET(USER_NODE, heater_power_gpios);
const struct gpio_dt_spec yj_power_gpio = GPIO_DT_SPEC_GET(USER_NODE, yj_power_gpios);
const struct gpio_dt_spec hk_power_gpio = GPIO_DT_SPEC_GET(USER_NODE, hk_power_gpios);


#if DT_NODE_HAS_PROP(USER_NODE, board_type_tib_gpios)
static const struct gpio_dt_spec board_type_tib_gpio =
	GPIO_DT_SPEC_GET(USER_NODE, board_type_tib_gpios);
#else
static const struct gpio_dt_spec board_type_tib_gpio = {0};
#endif

#if DT_NODE_HAS_PROP(USER_NODE, board_type_cal_yj_gpios)
static const struct gpio_dt_spec board_type_cal_yj_gpio =
	GPIO_DT_SPEC_GET(USER_NODE, board_type_cal_yj_gpios);
#else
static const struct gpio_dt_spec board_type_cal_yj_gpio = {0};
#endif

#if DT_NODE_HAS_PROP(USER_NODE, board_type_cal_hk_gpios)
static const struct gpio_dt_spec board_type_cal_hk_gpio =
	GPIO_DT_SPEC_GET(USER_NODE, board_type_cal_hk_gpios);
#else
static const struct gpio_dt_spec board_type_cal_hk_gpio = {0};
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

#if DT_NODE_EXISTS(DT_NODELABEL(dac7578_b))
const struct device *dac_dev_b = DEVICE_DT_GET(DT_NODELABEL(dac7578_b));
#else
const struct device *dac_dev_b = NULL;
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


static const char *const cal_switch_names[7] = {
	"cal1", "cal2", "cal3", "cal4", "cal5", "cal6", "cal7",
};

/* The GPIO expander wiring is the same 2-pin sequence on each populated board.
 * Board profiles below limit how many of these pairs are instantiated.
 */
static const gpio_pin_t mems_switch_pin_pairs[MAX_NUM_MEMS_SWITCHES][2] = {
	{6, 7},  	//sw1
	{8, 9}, 		//sw2
	{4, 5},		//sw3
	{10, 11},	//sw4
	{12, 13},	//sw5
	{14, 15},	//sw6
	{2, 3},		//sw7
	{0, 1},		//sw8
};


struct attenuator_dac_pair {
	const struct device *dev;
	uint8_t channel1;
	uint8_t channel2;
};

struct board_profile {
	enum hispec_board_type board;
	const char *name;
	uint8_t mems_switch_count;
	const char *const *switch_names;
	uint8_t attenuator_first;
	uint8_t attenuator_count;
};

static const struct board_profile unknown_profile = {
	.board = HISPEC_BOARD_UNKNOWN,
	.name = "SET BOARD JUMPER!",
};


static const struct board_profile tib_profile = {
	.board = HISPEC_BOARD_TIB,
	.name = "tib",
	.mems_switch_count = 8,
	.switch_names = tib_switch_names,
	.attenuator_first = 0,
	.attenuator_count = NUM_ATTENUATORS,
};

static const struct board_profile cal_yj_profile = {
	.board = HISPEC_BOARD_CAL_YJ,
	.name = "cal_yj",
	.mems_switch_count = 7,
	.switch_names = cal_switch_names,
	.attenuator_first = CAL_ATTENUATOR_INDEX,
	.attenuator_count = 1,
};

static const struct board_profile cal_hk_profile = {
	.board = HISPEC_BOARD_CAL_HK,
	.name = "cal_hk",
	.mems_switch_count = 7,
	.switch_names = cal_switch_names,
	.attenuator_first = CAL_ATTENUATOR_INDEX,
	.attenuator_count = 1,
};

static const struct board_profile as_profile = {
	.board = HISPEC_BOARD_AS,
	.name = "as",
	.mems_switch_count = 6,
	.switch_names = as_switch_names,
};

static K_MUTEX_DEFINE(board_profile_lock);
static const struct board_profile *active_profile = &unknown_profile;
static bool board_type_checked;
static K_MUTEX_DEFINE(relay_gpio_lock);
static bool relay_gpio_online;
static int relay_gpio_last_error = -ENODEV;
static bool relay_gpio_warning_emitted;
static uint32_t boot_reset_cause;
static bool boot_reset_cause_valid;

struct board_strap {
	const struct gpio_dt_spec *gpio;
	enum hispec_board_type board;
	const char *name;
};

static const struct board_strap board_straps[] = {
	{&board_type_tib_gpio, HISPEC_BOARD_TIB, "tib"},
	{&board_type_cal_yj_gpio, HISPEC_BOARD_CAL_YJ, "cal_yj"},
	{&board_type_cal_hk_gpio, HISPEC_BOARD_CAL_HK, "cal_hk"},
	{&board_type_as_gpio, HISPEC_BOARD_AS, "as"},
};

static const char *reset_cause_name(uint32_t bit)
{
	switch (bit) {
	case RESET_PIN:
		return "pin";
	case RESET_SOFTWARE:
		return "software";
	case RESET_BROWNOUT:
		return "brownout";
	case RESET_POR:
		return "power_on";
	case RESET_WATCHDOG:
		return "watchdog";
	case RESET_DEBUG:
		return "debug";
	case RESET_SECURITY:
		return "security";
	case RESET_LOW_POWER_WAKE:
		return "low_power_wake";
	case RESET_CPU_LOCKUP:
		return "cpu_lockup";
	case RESET_PARITY:
		return "parity";
	case RESET_PLL:
		return "pll";
	case RESET_CLOCK:
		return "clock";
	case RESET_HARDWARE:
		return "hardware";
	case RESET_USER:
		return "user";
	case RESET_TEMPERATURE:
		return "temperature";
	case RESET_BOOTLOADER:
		return "bootloader";
	case RESET_FLASH:
		return "flash";
	default:
		return NULL;
	}
}

static void format_reset_cause_list(uint32_t cause, char *buf, size_t buf_len)
{
	size_t off = 0U;
	bool first = true;

	if (buf == NULL || buf_len == 0U) {
		return;
	}

	buf[0] = '\0';
	if (cause == 0U) {
		(void)snprintk(buf, buf_len, "unknown");
		return;
	}

	for (uint32_t bit = BIT(0); bit != 0U; bit <<= 1) {
		const char *name;

		if ((cause & bit) == 0U) {
			continue;
		}

		name = reset_cause_name(bit);
		if (name == NULL) {
			continue;
		}

		off += snprintk(&buf[off], buf_len - off, "%s%s",
				first ? "" : ",", name);
		if (off >= buf_len) {
			buf[buf_len - 1U] = '\0';
			return;
		}
		first = false;
	}

	if (first) {
		(void)snprintk(buf, buf_len, "unknown");
	}
}

void devices_capture_boot_reset_cause(void)
{
	uint32_t cause = 0U;
	char cause_text[128];
	int rc;

	rc = hwinfo_get_reset_cause(&cause);
	if (rc != 0) {
		LOG_WRN("Reset cause unavailable (%d)", rc);
		return;
	}

	boot_reset_cause = cause;
	boot_reset_cause_valid = true;
	format_reset_cause_list(cause, cause_text, sizeof(cause_text));

	if ((cause & RESET_WATCHDOG) != 0U) {
		LOG_WRN("Previous boot ended in watchdog reset; reset_cause=%s", cause_text);
	} else {
		LOG_INF("Reset cause: %s", cause_text);
	}

	rc = hwinfo_clear_reset_cause();
	if (rc != 0) {
		LOG_WRN("Failed to clear reset cause flags (%d)", rc);
	}
}

void devices_queue_boot_reset_telemetry(void)
{
	struct coo_cmd_response msg = {0};
	char cause_text[128];
	int rc;

	if (!boot_reset_cause_valid || (boot_reset_cause & RESET_WATCHDOG) == 0U) {
		return;
	}

	format_reset_cause_list(boot_reset_cause, cause_text, sizeof(cause_text));
	msg.target = COO_CMD_OUT_MQTT;
	msg.qos = 0;
	rc = coo_cmd_format_data_topic(app_mqtt_device_id(), "boot",
				       msg.topic, sizeof(msg.topic));
	if (rc != 0) {
		LOG_WRN("Failed to format boot telemetry topic (%d)", rc);
		return;
	}

	msg.payload_len = snprintk(msg.payload, sizeof(msg.payload),
				   "{\"event\":\"boot\",\"reset_cause\":\"%s\","
				   "\"watchdog\":true,\"raw_reset_cause\":%u}",
				   cause_text, boot_reset_cause);
	if (msg.payload_len >= sizeof(msg.payload)) {
		LOG_WRN("Boot telemetry payload too large");
		return;
	}

	if (k_msgq_put(&outbound_queue, &msg, K_FOREVER) != 0) {
		LOG_WRN("Outbound queue full; boot watchdog telemetry not queued");
	}
}

#define ROUTE_DEF(input_, output_, steps_) \
	{ .key = { .input_name = (input_), .output_name = (output_) }, \
	  .steps = (steps_), .num_steps = ARRAY_SIZE(steps_) }

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

static const struct mems_route tib_routes[] = {
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

static const struct mems_route as_routes[] = {
	ROUTE_DEF("yj_calin", "yj_split", as_yj_split_to_as),
	ROUTE_DEF("hk_calin", "hk_split", as_hk_split_to_as),
	ROUTE_DEF("yj_calin", "yj_cal", as_yj_split_to_cal),
	ROUTE_DEF("hk_calin", "hk_cal", as_hk_split_to_cal),
};


//CAL switch connections
// 	{"cal1", 'B'}, //A=LFC to cal4A, B=Etalon to cal4A
// 	{"cal2", 'B'}, //A=CathGad to cal4B, B=cal3b to cal4B
// 	{"cal3", 'A'}, //A=BB to cal2, B=BB to cal6
// 	{"cal4", 'A'}, //A=cal1 to cal5B, B=cal2 to cal5B
// 	{"cal5", 'B'}, //A=NM to cal7, B=cal4A to cal7
// 	{"cal6", 'A'}, //A=cal3 to IS (used for BB to IS), B=NM to IS
// 	{"cal7", 'A'}, //A=cal5 to spec, B=cal5 to tib

static const struct mems_route_step etalon_to_spec[] = {
	{"cal1", 'B'}, //A=LFC to cal4A, B=Etalon to cal4A
	{"cal4", 'A'}, //A=cal1 to cal5B, B=cal2 to cal5B
	{"cal5", 'B'}, //A=NM to cal7, B=cal4 to cal7
	{"cal7", 'A'}, //A=cal5 to spec, B=cal5 to tib
};

static const struct mems_route_step lfc_to_spec[] = {
	{"cal1", 'A'}, //A=LFC to cal4A, B=Etalon to cal4A
	{"cal4", 'A'}, //A=cal1 to cal5B, B=cal2 to cal5B
	{"cal5", 'B'}, //A=NM to cal7, B=cal4 to cal7
	{"cal7", 'A'}, //A=cal5 to spec, B=cal5 to tib
};

static const struct mems_route_step bb_to_spec[] = {
	{"cal2", 'B'}, //A=CathGas to cal4B, B=cal3 to cal4
	{"cal3", 'A'}, //A=BB to cal2, B=BB to cal6
	{"cal4", 'B'}, //A=cal1 to cal5B, B=cal2 to cal5B
	{"cal5", 'B'}, //A=NM to cal7, B=cal4 to cal7
	{"cal7", 'A'}, //A=cal5 to spec, B=cal5 to tib
};

static const struct mems_route_step cathgas_to_spec[] = {
	{"cal2", 'A'}, //A=CathGas to cal4B, B=cal3 to cal4
	{"cal4", 'B'}, //A=cal1 to cal5B, B=cal2 to cal5B
	{"cal5", 'B'}, //A=NM to cal7, B=cal4 to cal7
	{"cal7", 'A'}, //A=cal5 to spec, B=cal5 to tib
};

static const struct mems_route_step nm_to_spec[] = {
	{"cal5", 'A'}, //A=NM to cal7, B=cal4 to cal7
	{"cal7", 'A'}, //A=cal5 to spec, B=cal5 to tib
};

static const struct mems_route_step etalon_to_tib[] = {
	{"cal1", 'B'}, //A=LFC to cal4A, B=Etalon to cal4A
	{"cal4", 'A'}, //A=cal1 to cal5B, B=cal2 to cal5B
	{"cal5", 'B'}, //A=NM to cal7, B=cal4A to cal7
	{"cal7", 'B'}, //A=cal5 to spec, B=cal5 to tib
};

static const struct mems_route_step lfc_to_tib[] = {
	{"cal1", 'A'}, //A=LFC to cal4A, B=Etalon to cal4A
	{"cal4", 'A'}, //A=cal1 to cal5B, B=cal2 to cal5B
	{"cal5", 'B'}, //A=NM to cal7, B=cal4A to cal7
	{"cal7", 'B'}, //A=cal5 to spec, B=cal5 to tib
};

static const struct mems_route_step bb_to_tib[] = {
	{"cal2", 'B'}, //A=CathGas to cal4B, B=cal3 to cal4
	{"cal3", 'A'}, //A=BB to cal2, B=BB to cal6
	{"cal4", 'B'}, //A=cal1 to cal5B, B=cal2 to cal5B
	{"cal5", 'B'}, //A=NM to cal7, B=cal4 to cal7
	{"cal7", 'B'}, //A=cal5 to spec, B=cal5 to tib
};

static const struct mems_route_step cathgas_to_tib[] = {
	{"cal2", 'A'}, //A=CathGas to cal4B, B=cal3 to cal4
	{"cal4", 'B'}, //A=cal1 to cal5B, B=cal2 to cal5B
	{"cal5", 'B'}, //A=NM to cal7, B=cal4 to cal7
	{"cal7", 'B'}, //A=cal5 to spec, B=cal5 to tib
};

static const struct mems_route_step nm_to_tib[] = {
	{"cal5", 'A'}, //A=NM to cal7, B=cal4 to cal7
	{"cal7", 'B'}, //A=cal5 to spec, B=cal5 to tib
};

static const struct mems_route_step bb_to_is[] = {
	{"cal3", 'B'}, //A=BB to cal2, B=BB to cal6
	{"cal6", 'A'}, //A=cal3 to IS (used for BB to IS), B=NM to IS
};


static const struct mems_route_step nm_to_is[] = {
	{"cal6", 'B'}, //A=cal3 to IS (used for BB to IS), B=NM to IS
};



static const struct mems_route cal_routes[] = {
	ROUTE_DEF("bb", "is", bb_to_is),
	ROUTE_DEF("nm", "is", nm_to_is),

	ROUTE_DEF("etalon", "spec", etalon_to_spec),
	ROUTE_DEF("lfc", "spec", lfc_to_spec),
	ROUTE_DEF("bb", "spec", bb_to_spec),
	ROUTE_DEF("cathgas", "spec", cathgas_to_spec),
	ROUTE_DEF("nm", "spec", nm_to_spec),

	ROUTE_DEF("etalon", "tib", etalon_to_tib),
	ROUTE_DEF("lfc", "tib", lfc_to_tib),
	ROUTE_DEF("bb", "tib", bb_to_tib),
	ROUTE_DEF("cathgas", "tib", cathgas_to_tib),
	ROUTE_DEF("nm", "tib", nm_to_tib),
};

BUILD_ASSERT(ARRAY_SIZE(tib_routes) <= MEMS_ROUTER_MAX_ROUTES,
	     "TIB route count must fit command route-status buffers");
BUILD_ASSERT(ARRAY_SIZE(as_routes) <= MEMS_ROUTER_MAX_ROUTES,
	     "AS route count must fit command route-status buffers");
BUILD_ASSERT(ARRAY_SIZE(cal_routes) <= MEMS_ROUTER_MAX_ROUTES,
	     "CAL route count must fit command route-status buffers");

static const struct board_profile *profile_for_type(enum hispec_board_type board)
{
	switch (board) {
	case HISPEC_BOARD_TIB:
		return &tib_profile;
	case HISPEC_BOARD_CAL_YJ:
		return &cal_yj_profile;
	case HISPEC_BOARD_CAL_HK:
		return &cal_hk_profile;
	case HISPEC_BOARD_AS:
		return &as_profile;
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

static int board_strap_read_active(const struct board_strap *strap, bool *active, int *raw_level)
{
	int raw;
	int rc;

	if (strap == NULL || active == NULL || raw_level == NULL || strap->gpio->port == NULL) {
		return -ENODEV;
	}
	/* gpio_pin_configure_dt() applies the pull-up from the overlay. Board straps
	 * are physical solder jumpers, so read the physical level and apply the
	 * active-low flag explicitly: unconnected pulled-up pins are inactive, and
	 * straps shorted to ground are active.
	 */
	rc = gpio_pin_configure_dt(strap->gpio, GPIO_INPUT);
	if (rc != 0) {
		return rc;
	}

	raw = gpio_pin_get_raw(strap->gpio->port, strap->gpio->pin);
	if (raw < 0) {
		return raw;
	}

	*active = (strap->gpio->dt_flags & GPIO_ACTIVE_LOW) != 0 ?
		  (raw == 0) : (raw != 0);
	*raw_level = raw;
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
		int raw_level = -1;
		int rc = board_strap_read_active(&board_straps[i], &active, &raw_level);

		if (rc != 0) {
			LOG_ERR("Failed to read board strap %s (%d)", board_straps[i].name, rc);
			if (first_error == 0) {
				first_error = rc;
			}
			continue;
		}

		LOG_INF("Board strap %s flags=0x%x raw=%d active=%d",
			board_straps[i].name,
			board_straps[i].gpio->dt_flags,
			raw_level, active ? 1 : 0);
		if (active) {
			active_count++;
			detected = board_straps[i].board;
		}
	}

	if (first_error != 0) {
		set_current_profile(&unknown_profile, true);
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
		set_current_profile(&unknown_profile, true);
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
	return current_profile()->board;
}

const char *devices_board_type_name(void)
{
	return current_profile()->name;
}

bool devices_attenuator_channel_available(uint8_t attenuator_index)
{
	const struct board_profile *profile = current_profile();
	uint8_t first = profile->attenuator_first;
	uint8_t count = profile->attenuator_count;

	return attenuator_index < NUM_ATTENUATORS &&
	       attenuator_index >= first &&
	       attenuator_index < first + count;
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

static bool configure_gpio_output_inactive_or_log(const struct gpio_dt_spec *gpio,
						  const char *label)
{
	int rc;

	if (gpio == NULL) {
		LOG_ERR("%s GPIO is not mapped in devicetree", label);
		return false;
	}

	/* gpio_pin_configure_dt() applies the devicetree active flag and sets the
	 * output inactive so relays/supplies default off during firmware setup.
	 */
	rc = gpio_pin_configure_dt(gpio, GPIO_OUTPUT_INACTIVE);
	if (rc != 0) {
		LOG_ERR("Failed to configure %s GPIO (%d)", label, rc);
		return false;
	}

	return true;
}

static void set_relay_gpio_status(bool online, int error)
{
	k_mutex_lock(&relay_gpio_lock, K_FOREVER);
	relay_gpio_online = online;
	relay_gpio_last_error = online ? 0 : error;
	k_mutex_unlock(&relay_gpio_lock);
}

bool devices_relay_gpio_online(void)
{
	bool online;

	k_mutex_lock(&relay_gpio_lock, K_FOREVER);
	online = relay_gpio_online;
	k_mutex_unlock(&relay_gpio_lock);

	return online;
}

int devices_relay_gpio_last_error(void)
{
	int error;

	k_mutex_lock(&relay_gpio_lock, K_FOREVER);
	error = relay_gpio_last_error;
	k_mutex_unlock(&relay_gpio_lock);

	return error;
}

static void emit_relay_gpio_offline_warning_once(int error)
{
	char context[24];

	k_mutex_lock(&relay_gpio_lock, K_FOREVER);
	if (relay_gpio_warning_emitted) {
		k_mutex_unlock(&relay_gpio_lock);
		return;
	}
	relay_gpio_warning_emitted = true;
	k_mutex_unlock(&relay_gpio_lock);

	snprintf(context, sizeof(context), "rc=%d", error);
	coo_cmd_runtime_warning_emit(command_runtime_get(), "relay_gpio_offline",
			 "off-board relay GPIO expander is offline; photodiode relay commands are ignored and laser bank heater is unavailable",
			 context);
}

static bool configure_relay_gpio_outputs(void)
{
	const struct device *relay_port = yj_power_gpio.port;
	int error = -ENODEV;

	if (relay_port == NULL || !device_is_ready(relay_port)) {
		LOG_WRN("Relay GPIO expander is offline at boot");
		set_relay_gpio_status(false, error);
		emit_relay_gpio_offline_warning_once(error);
		return false;
	}

	if (!configure_gpio_output_inactive_or_log(&yj_power_gpio,
						   "YJ photodiode power")) {
		error = -EIO;
		goto offline;
	}
	if (!configure_gpio_output_inactive_or_log(&hk_power_gpio,
						   "HK photodiode power")) {
		error = -EIO;
		goto offline;
	}
	if (!configure_gpio_output_inactive_or_log(&heater_power_gpio,
						   "laser bank heater")) {
		error = -EIO;
		goto offline;
	}

	set_relay_gpio_status(true, 0);
	LOG_INF("Relay-box GPIO outputs configured inactive");
	return true;

offline:
	LOG_WRN("Relay GPIO expander setup failed");
	set_relay_gpio_status(false, error);
	emit_relay_gpio_offline_warning_once(error);
	return false;
}

static bool setup_modbus_client(void)
{
#if DT_HAS_COMPAT_STATUS_OKAY(zephyr_modbus_serial)
	struct modbus_iface_param modbus_cfg = {
		.mode = MODBUS_MODE_RTU,
		.serial = {
			.baud = MODBUS_BAUD,
			.parity = MODBUS_PARITY,
			.stop_bits = MODBUS_STOPBITS,
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

static bool attenuator_dac_pair_for_index(uint8_t attenuator_index,
					  struct attenuator_dac_pair *out)
{
	uint8_t local_index;

	if (out == NULL || attenuator_index >= NUM_ATTENUATORS) {
		return false;
	}

	if (attenuator_index < 3U) {
		local_index = attenuator_index;
		out->dev = dac_dev;
	} else {
		local_index = attenuator_index - 3U;
		out->dev = dac_dev_b;
	}

	out->channel1 = local_index * 2U;
	out->channel2 = out->channel1 + 1U;

	return true;
}

void setup_attenuators(void)
{
	const struct board_profile *profile = current_profile();
	struct app_attenuator_settings atten_settings;

	if (profile->attenuator_count == 0U) {
		LOG_INF("Board %s has no attenuator channels", profile->name);
		return;
	}

	app_settings_get_attenuator(&atten_settings);

	for (uint8_t i = 0; i < profile->attenuator_count; ++i) {
		uint8_t attenuator_index = profile->attenuator_first + i;
		struct attenuator_dac_pair dac_pair;

		if (attenuator_index >= NUM_ATTENUATORS) {
			LOG_ERR("Profile %s attenuator index %u is out of range",
				profile->name, attenuator_index);
			continue;
		}

		if (!attenuator_dac_pair_for_index(attenuator_index, &dac_pair)) {
			LOG_ERR("Profile %s attenuator index %u has no DAC pair",
				profile->name, attenuator_index);
			continue;
		}

		if (!attenuator_init(&attenuators[attenuator_index],
				     dac_pair.dev, dac_pair.channel1,
				     dac_pair.dev, dac_pair.channel2)) {
			continue;
		}

		attenuators[attenuator_index].coeff1.slope =
			atten_settings.channel[attenuator_index].physical[0].slope;
		attenuators[attenuator_index].coeff1.offset =
			atten_settings.channel[attenuator_index].physical[0].offset;
		attenuators[attenuator_index].coeff2.slope =
			atten_settings.channel[attenuator_index].physical[1].slope;
		attenuators[attenuator_index].coeff2.offset =
			atten_settings.channel[attenuator_index].physical[1].offset;
	}
}

void setup_mems_switches_and_routes(void)
{
	const struct board_profile *profile = current_profile();
	struct mems_switch *mems_switch_ptrs[MEMS_ROUTER_MAX_SWITCHES] = {0};
	const struct mems_route *routes = NULL;
	uint8_t route_count = 0U;

	if (profile->board == HISPEC_BOARD_UNKNOWN) {
		LOG_ERR("Cannot configure MEMS routes: board type is %s", profile->name);
		mems_router_init(&router, mems_switch_ptrs, 0, NULL, 0);
		return;
	}

	if (!device_ready_or_log(gpio_dev, "MEMS GPIO expander")) {
		mems_router_init(&router, mems_switch_ptrs, 0, NULL, 0);
		return;
	}

	for (uint8_t i = 0; i < profile->mems_switch_count; ++i) {
		enum mems_switch_type switch_type = MEMS_SWITCH_TYPE_FFSW;

		if (profile->board == HISPEC_BOARD_TIB &&
		    i >= profile->mems_switch_count - 2U) {
			switch_type = MEMS_SWITCH_TYPE_FFLS;
		}

		mems_switch_init(&mems_switches[i],
				 gpio_dev,
				 mems_switch_pin_pairs[i][0],
				 mems_switch_pin_pairs[i][1],
				 profile->switch_names[i],
				 switch_type,
				 MEMS_SWITCH_MAX_TOGGLE_HZ,
				 'A');
		mems_switch_ptrs[i] = &mems_switches[i];
	}

	switch (profile->board) {
	case HISPEC_BOARD_TIB:
		routes = tib_routes;
		route_count = ARRAY_SIZE(tib_routes);
		break;
	case HISPEC_BOARD_AS:
		routes = as_routes;
		route_count = ARRAY_SIZE(as_routes);
		break;
	case HISPEC_BOARD_CAL_YJ:
	case HISPEC_BOARD_CAL_HK:
		routes = cal_routes;
		route_count = ARRAY_SIZE(cal_routes);
		break;
	case HISPEC_BOARD_UNKNOWN:
	default:
		routes = NULL;
		route_count = 0U;
		break;
	}

	mems_router_init(&router, mems_switch_ptrs, profile->mems_switch_count,
			 routes, route_count);

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

	if (profile->board == HISPEC_BOARD_UNKNOWN) {
		LOG_ERR("Board type %s is not valid for device setup", profile->name);
		return false;
	}

	if (!device_ready_or_log(gpio_dev, "MEMS GPIO expander")) {
		rc = false;
	}

	if (profile->board == HISPEC_BOARD_TIB) {
		if (!configure_gpio_output_inactive_or_log(&laser_power_gpio,
							   "laser bank power")) {
			rc = false;
		}
		(void)configure_relay_gpio_outputs();
	}

	if (profile->board == HISPEC_BOARD_TIB && !setup_modbus_client()) {
		rc = false;
	}

	if (profile->attenuator_count > 0U) {
		for (uint8_t i = 0; i < profile->attenuator_count; ++i) {
			uint8_t attenuator_index = profile->attenuator_first + i;
			struct attenuator_dac_pair dac_pair;

			if (!attenuator_dac_pair_for_index(attenuator_index, &dac_pair) ||
			    !device_ready_or_log(dac_pair.dev, "DAC")) {
				rc = false;
			}
		}
	}

	if (profile->board == HISPEC_BOARD_TIB && !device_ready_or_log(adc_dev, "ADC")) {
		rc = false;
	}

	return rc;
}

#endif /* __DEVICE_C__ */
