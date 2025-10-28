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


// extern const struct device *modbus;
extern const struct device *adc_dev;
extern const struct device *dac_dev;
extern const struct device *gpio_dev;

extern const struct gpio_dt_spec power_gpio;
extern const struct gpio_dt_spec mems0_A;
extern const struct gpio_dt_spec mems0_B;
extern const struct gpio_dt_spec mems1_A;
extern const struct gpio_dt_spec mems1_B;
extern const struct gpio_dt_spec mems2_A;
extern const struct gpio_dt_spec mems2_B;
extern const struct gpio_dt_spec mems3_A;
extern const struct gpio_dt_spec mems3_B;
extern const struct gpio_dt_spec mems4_A;
extern const struct gpio_dt_spec mems4_B;
extern const struct gpio_dt_spec mems5_A;
extern const struct gpio_dt_spec mems5_B;
extern const struct gpio_dt_spec mems6_A;
extern const struct gpio_dt_spec mems6_B;
extern const struct gpio_dt_spec mems7_A;
extern const struct gpio_dt_spec mems7_B;


extern struct mems_switch mems_switches[];
extern struct mems_router router;
extern struct mems_switch *mems_switch_ptrs[];
extern struct attenuator attenuators[];


bool devices_ready(void);
void setup_mems_switches_and_routes();
void setup_attenuators();

#endif