//
// Created by Jeb Bailey on 5/19/25.
//


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

enum hispec_board_type {
	HISPEC_BOARD_UNKNOWN = 0,
	HISPEC_BOARD_TIB,
	HISPEC_BOARD_CAL_BLUE,
	HISPEC_BOARD_CAL_RED,
	HISPEC_BOARD_AS,
	HISPEC_BOARD_FAULT,
};


// extern const struct device *modbus;
extern const struct device *adc_dev;
extern const struct device *dac_dev;
extern const struct device *gpio_dev;

extern const struct gpio_dt_spec power_gpio;

extern struct mems_switch mems_switches[];
extern struct mems_router router;
extern struct attenuator attenuators[];

/**
 * @brief Read the solder-strap GPIOs and select the active PCB profile.
 *
 * The straps are active-low GPIO inputs with pullups. Exactly one strap must
 * be active. More than one active strap is treated as a board fault because the
 * assembled PCB identity should not change at runtime.
 */
int devices_detect_board_type(void);

/** @brief Return true after board-type strap detection has completed once. */
bool devices_board_type_checked(void);

/** @brief Return the detected board type enum. */
enum hispec_board_type devices_board_type(void);

/** @brief Return the short stable board type name used in logs/settings. */
const char *devices_board_type_name(void);

/** @brief Return true when the selected PCB is safe to configure. */
bool devices_board_type_valid(void);

/** @brief Return true when this PCB has a laser bank and Modbus control. */
bool devices_has_laser_bank(void);

/** @brief Return true when this PCB has the laser-bank power GPIO. */
bool devices_has_laser_power_control(void);

/** @brief Return true when this PCB has the ADS1115 photodiode channels. */
bool devices_has_photodiodes(void);

/** @brief Return true when this PCB has any DAC-driven attenuator channel. */
bool devices_has_attenuators(void);

/** @brief Return true when a logical attenuator index is populated on this PCB. */
bool devices_attenuator_index_available(uint8_t index);

/** @brief Return the number of MEMS switches expected for this PCB profile. */
uint8_t devices_mems_switch_count(void);

bool devices_ready(void);
void setup_mems_switches_and_routes(void);
void setup_attenuators(void);

#endif
