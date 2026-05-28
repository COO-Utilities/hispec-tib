/**
 * @file devices.h
 * @brief Devicetree device handles and board-profile setup.
 *
 * `hardware.md` owns physical wiring. This module selects the active firmware
 * profile from board straps, initializes only hardware expected on that board,
 * and exposes the shared MEMS router and attenuator objects.
 */


#ifndef DEVICES_H
#define DEVICES_H


#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/logging/log.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/drivers/adc.h>
#include <zephyr/drivers/dac.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/modbus/modbus.h>
#include <zephyr/drivers/uart.h>
#include "mems_switching.h"
#include "attenuator.h"

#define MODBUS_BAUD 115200
#define MODBUS_PARITY UART_CFG_PARITY_NONE
#define MODBUS_STOPBITS UART_CFG_STOP_BITS_2
#define MODBUS_RX_TIMEOUT_MS 10
#define DAC_RESOLUTION 12

#define NUM_ATTENUATORS 6
#define HISPEC_ATTENUATOR_LFC_INDEX 4U

enum hispec_board_type {
	HISPEC_BOARD_UNKNOWN = 0,
	HISPEC_BOARD_TIB,
	HISPEC_BOARD_CAL_YJ,
	HISPEC_BOARD_CAL_HK,
	HISPEC_BOARD_AS
};


// extern const struct device *modbus;
extern const struct device *adc_dev;
extern const struct device *gpio_dev;

extern const struct gpio_dt_spec laser_power_gpio;
extern const struct gpio_dt_spec heater_power_gpio;
extern const struct gpio_dt_spec yj_power_gpio;
extern const struct gpio_dt_spec hk_power_gpio;

extern struct mems_switch mems_switches[];
extern struct mems_router router;
extern struct attenuator attenuators[];

/**
 * @brief Read the solder-strap GPIOs and select the active PCB profile.
 *
 * The straps are active-low GPIO inputs with pullups. Exactly one strap must
 * be active. Missing or conflicting straps leave the board type unknown because
 * the assembled PCB identity should not change at runtime.
 */
int devices_detect_board_type(void);

/** @brief Return true after board-type strap detection has completed once. */
bool devices_board_type_checked(void);

/** @brief Return the detected board type enum. */
enum hispec_board_type devices_board_type(void);

/** @brief Return the short stable board type name used in logs/settings. */
const char *devices_board_type_name(void);

/**
 * @brief Return true when a logical attenuator belongs to the active profile.
 *
 * This is a profile/board-presence check only. It does not probe DAC readiness
 * or perform I2C; callers still need to handle transient DAC failures.
 */
bool devices_attenuator_channel_available(uint8_t attenuator_index);

/** @brief Check/configure devices required by the detected board profile. */
bool devices_ready(void);

/** @brief Return true when the off-board DS2408 relay GPIO expander is usable. */
bool devices_relay_gpio_online(void);

/** @brief Last DS2408 relay GPIO setup error, or 0 when online. */
int devices_relay_gpio_last_error(void);

/**
 * @brief Capture and clear hardware reset-cause flags for this boot.
 *
 * On STM32 this uses Zephyr hwinfo to read RCC reset flags, including
 * watchdog reset. Call once early in boot before later code can clear them.
 */
void devices_capture_boot_reset_cause(void);

/**
 * @brief Queue retained watchdog boot telemetry for MQTT retry.
 *
 * Uses the captured reset cause and enqueues a non-best-effort MQTT telemetry
 * message if the prior reset included watchdog expiration.
 */
void devices_queue_boot_reset_telemetry(void);

/** @brief Build MEMS switch objects and select the board-specific route table. */
void setup_mems_switches_and_routes(void);

/** @brief Initialize profile-available logical attenuator channels from settings. */
void setup_attenuators(void);

#endif
