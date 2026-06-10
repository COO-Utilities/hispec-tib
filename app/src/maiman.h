/**
 * @file maiman.h
 * @brief Thin Modbus register wrapper for Maiman SF8025 laser drivers.
 *
 * This layer exposes raw and scaled register operations. Higher-level safety,
 * power sequencing, diode limits, and persistence are owned by lasers.c and the
 * Maiman module EEPROM, not this wrapper.
 */

//todo see https://github.com/CaltechOpticalObservatories/hispec-fib/blob/develop/ait/photonic_testing/photonic_testing.py
//todo see https://github.com/CaltechOpticalObservatories/hispec-fib/blob/develop/ait/photonic_testing/maiman_modbus/...

#ifndef MAIMAN_H
#define MAIMAN_H

#include <stdint.h>
#include <stdbool.h>
#include "laser_properties.h"
#include <zephyr/modbus/modbus.h>

/* Maiman MODBUS RTU holding-register addresses.
 *
 * These are the register addresses from maiman_modbus_py/config/modbus_config.yaml,
 * cross-checked against the SF8025 family user manual's parameter table. The PDF
 * lists legacy text-command parameter numbers such as 0300h and 0A10h; the
 * MODBUS library maps those to the compact holding-register block below.
 */
#define REG_DEVICE_ID                       0x0001
#define REG_CHANGEABLE_PARAMETERS           0x0002
#define REG_SERIAL_NUMBER                   0x0003
#define REG_STATE_OF_DEVICE_COMMAND         0x0004
#define REG_LOCK_STATUS                     0x0005
#define REG_FREQUENCY                       0x0006
#define REG_DURATION                        0x0007
#define REG_CURRENT                         0x0008
#define REG_SAVE_PARAMETERS                 0x0009
#define REG_RESET_PARAMETERS                0x000A

#define REG_FREQUENCY_MIN                   0x0020
#define REG_FREQUENCY_MAX                   0x0021
#define REG_DURATION_MIN                    0x0022
#define REG_DURATION_MAX                    0x0023
#define REG_CURRENT_MIN                     0x0024
#define REG_CURRENT_MAX                     0x0025
#define REG_NTC_TEMPERATURE_MIN             0x0026
#define REG_NTC_TEMPERATURE_MAX             0x0027
#define REG_CURRENT_MAX_LIMIT               0x0029
#define REG_CURRENT_PROTECTION_THRESHOLD    0x002A

#define REG_CURRENT_MEASURED                0x0040
#define REG_VOLTAGE_MEASURED                0x0041
#define REG_NTC_TEMPERATURE_MEASURED        0x0042
#define REG_PCB_TEMPERATURE_MEASURED        0x0043

#define REG_TEC_TEMPERATURE_VALUE           0x0070
#define REG_TEC_TEMPERATURE_MAX             0x0071
#define REG_TEC_TEMPERATURE_MIN             0x0072
#define REG_TEC_TEMPERATURE_MAX_LIMIT       0x0073
#define REG_TEC_TEMPERATURE_MIN_LIMIT       0x0074
#define REG_TEC_TEMPERATURE_MEASURED        0x0075
#define REG_TEC_CURRENT_MEASURED            0x0076
#define REG_TEC_CURRENT_LIMIT               0x0077
#define REG_TEC_VOLTAGE                     0x0078
#define REG_STATE_OF_TEC_COMMAND            0x007A
#define REG_TEC_CURRENT_SET_CALIBRATION     0x007E
#define REG_INTERNAL_LD_NTC_SENSOR          0x007F

#define REG_RS_SETTING                      0x0080
#define REG_CURRENT_SET_CALIBRATION         0x0088
#define REG_NTC_COEFFICIENT                 0x008A
#define REG_TEC_P_COEFFICIENT               0x0091
#define REG_TEC_I_COEFFICIENT               0x0092
#define REG_TEC_D_COEFFICIENT               0x0093

typedef uint16_t laser_address_t;

/* Finds the register, returns true and sets *address_out if found. */
bool maiman_get_register_address(const char *name, laser_address_t *address_out);

/*
 * Selects the Zephyr Modbus client interface used by subsequent blocking
 * Maiman register transactions. Device setup owns the interface lookup.
 */
int maiman_set_client_iface(int iface);


/* Divider constants from the SF8025 v5.4 device metadata used by the
 * validation notebooks. Values returned by this layer are engineering units:
 * mA, V, Hz, ms, deg C, A, or raw PID/register units as named.
 */
#define DIVIDER_FREQUENCY                   10.0
#define DIVIDER_DURATION                    10.0
#define DIVIDER_CURRENT                     10.0
#define DIVIDER_VOLTAGE                     10.0
#define DIVIDER_PCB_TEMPERATURE             10.0
#define DIVIDER_NTC_TEMPERATURE             10.0
#define DIVIDER_TEC_TEMPERATURE             100.0
#define DIVIDER_TEC_CURRENT                 10.0
#define DIVIDER_TEC_VOLTAGE                 10.0
#define DIVIDER_CURRENT_SET_CALIBRATION     100.0
#define DIVIDER_TEC_CURRENT_SET_CALIBRATION 100.0
#define DIVIDER_NTC_COEFFICIENT             1.0

// Device state bitmasks
#define DEVICE_STATE_POWERED                0x0001
#define OPERATION_STATE_STARTED             0x0002
#define CURRENT_SET_INTERNAL                0x0004
#define ENABLE_INTERNAL                     0x0010
#define EXTERNAL_NTC_INTERLOCK_DENIED       0x0040
#define INTERLOCK_DENIED                    0x0080

// TEC state bitmasks
#define TEC_OPERATION_STATE_STARTED         0x0002
#define TEC_SET_INTERNAL                    0x0004
#define TEC_ENABLE_INTERNAL                 0x0010

// Lock-status bitmasks
#define LOCK_STATE_INTERLOCK                0x0002
#define LOCK_STATE_LD_OVERCURRENT          0x0008
#define LOCK_STATE_LD_OVERHEAT             0x0010
#define LOCK_STATE_EXTERNAL_NTC_INTERLOCK  0x0020
#define LOCK_STATE_TEC_ERROR               0x0040
#define LOCK_STATE_TEC_SELFHEAT            0x0080

#define LOCK_STATE_BLOCKING_MASK \
    (LOCK_STATE_INTERLOCK | LOCK_STATE_LD_OVERCURRENT | \
     LOCK_STATE_LD_OVERHEAT | LOCK_STATE_EXTERNAL_NTC_INTERLOCK | \
     LOCK_STATE_TEC_ERROR | LOCK_STATE_TEC_SELFHEAT)

// Modbus command values
#define MODBUS_START_COMMAND_VALUE              0x0008
#define MODBUS_STOP_COMMAND_VALUE               0x0010
#define MODBUS_INTERNAL_CURRENT_SET_VALUE       0x0020
#define MODBUS_EXTERNAL_CURRENT_SET_VALUE       0x0040
#define MODBUS_EXTERNAL_ENABLE_VALUE            0x0200
#define MODBUS_INTERNAL_ENABLE_VALUE            0x0400
#define MODBUS_ALLOW_INTERLOCK_VALUE            0x1000
#define MODBUS_DENY_INTERLOCK_VALUE             0x2000
#define MODBUS_DENY_EXTERNAL_NTC_INTERLOCK_VALUE 0x4000
#define MODBUS_ALLOW_EXTERNAL_NTC_INTERLOCK_VALUE 0x8000

#define MODBUS_START_TEC_COMMAND_VALUE          0x0008
#define MODBUS_STOP_TEC_COMMAND_VALUE           0x0010
#define MODBUS_INTERNAL_TEMPERATURE_SET_VALUE   0x0020
#define MODBUS_EXTERNAL_TEMPERATURE_SET_VALUE   0x0040
#define MODBUS_EXTERNAL_TEC_ENABLE_VALUE        0x0200
#define MODBUS_INTERNAL_TEC_ENABLE_VALUE        0x0400

#define MODBUS_SAVE_PARAMETERS_VALUE            0x0001
#define MODBUS_RESET_PARAMETERS_VALUE           0x0001


/**
 * Structure representing a Maiman Modbus endpoint.
 */
typedef struct {
	uint8_t node_id;
	bool verbose;
} maiman_driver_t;

/**
 * Initialize the driver with the target Modbus node ID.
 */
void maiman_init(maiman_driver_t *drv, uint8_t node_id);
void maiman_init_verbose(maiman_driver_t *drv, uint8_t node_id, bool verbose);

/** Read/write one raw holding register. Calls block on Modbus RTU I/O. */
bool maiman_read_u16(maiman_driver_t *drv, uint16_t address, uint16_t *value);
bool maiman_write_u16(maiman_driver_t *drv, uint16_t address, uint16_t value);
/** Read/write scaled engineering units. Calls block on Modbus RTU I/O. */
bool maiman_read_scaled(maiman_driver_t *drv, uint16_t address, double divider,
                        bool signed_value, double *value);
bool maiman_write_scaled(maiman_driver_t *drv, uint16_t address, double divider,
                         bool signed_value, double value);


/* ----- Measurement getters ----- */
bool maiman_read_tec_temperature_measured(maiman_driver_t *drv, double *value);
double maiman_get_tec_temperature_measured(maiman_driver_t *drv);
double maiman_get_pcb_temperature_measured(maiman_driver_t *drv);
double maiman_get_ntc_temperature_measured(maiman_driver_t *drv);
double maiman_get_tec_temperature_value(maiman_driver_t *drv);
double maiman_get_current_measured(maiman_driver_t *drv);
double maiman_get_frequency(maiman_driver_t *drv);
double maiman_get_duration(maiman_driver_t *drv);
bool  maiman_get_current(maiman_driver_t *drv, double *value);
double maiman_get_voltage_measured(maiman_driver_t *drv);
double maiman_get_current_min(maiman_driver_t *drv);
double maiman_get_current_max(maiman_driver_t *drv);
double maiman_get_current_max_limit(maiman_driver_t *drv);
double maiman_get_current_protection_threshold(maiman_driver_t *drv);
double maiman_get_current_set_calibration(maiman_driver_t *drv);
double maiman_get_ntc_b25_100_coefficient(maiman_driver_t *drv);
double maiman_get_tec_current_measured(maiman_driver_t *drv);
double maiman_get_tec_current_limit(maiman_driver_t *drv);
double maiman_get_tec_current_set_calibration(maiman_driver_t *drv);
double maiman_get_tec_voltage(maiman_driver_t *drv);

/* ----- Status and control ----- */
uint16_t maiman_get_device_id(maiman_driver_t *drv);
uint16_t maiman_get_serial_number(maiman_driver_t *drv);
uint16_t maiman_get_raw_status(maiman_driver_t *drv);
uint16_t maiman_get_raw_lock_status(maiman_driver_t *drv);
bool maiman_read_raw_tec_status(maiman_driver_t *drv, uint16_t *status);
uint16_t maiman_get_raw_tec_status(maiman_driver_t *drv);
bool maiman_read_tec_started(maiman_driver_t *drv, bool *started);
bool maiman_is_bit_set(maiman_driver_t *drv, uint16_t bitmask);
bool maiman_is_operation_started(maiman_driver_t *drv);
bool maiman_is_current_set_internal(maiman_driver_t *drv);
bool maiman_is_enable_internal(maiman_driver_t *drv);
bool maiman_is_external_ntc_denied(maiman_driver_t *drv);
bool maiman_is_interlock_denied(maiman_driver_t *drv);
bool maiman_is_tec_started(maiman_driver_t *drv);
bool maiman_is_tec_set_internal(maiman_driver_t *drv);
bool maiman_is_tec_enable_internal(maiman_driver_t *drv);
bool maiman_is_lockstate_interlock(maiman_driver_t *drv);
bool maiman_is_lockstate_ld_overcurrent(maiman_driver_t *drv);
bool maiman_is_lockstate_ld_overheat(maiman_driver_t *drv);
bool maiman_is_lockstate_external_ntc_interlock(maiman_driver_t *drv);
bool maiman_is_lockstate_tec_error(maiman_driver_t *drv);
bool maiman_is_lockstate_tec_selfheat(maiman_driver_t *drv);

/* ----- Setpoint and commands ----- */
bool maiman_set_current(maiman_driver_t *drv, double current);
bool maiman_set_current_max(maiman_driver_t *drv, double current);
bool maiman_set_current_set_calibration(maiman_driver_t *drv, double calibration_percent);
bool maiman_set_frequency(maiman_driver_t *drv, double frequency);
bool maiman_set_duration(maiman_driver_t *drv, double duration);
bool maiman_set_tec_temperature(maiman_driver_t *drv, double temperature_c);
bool maiman_set_tec_current_limit(maiman_driver_t *drv, double current_a);
bool maiman_get_tec_pid(maiman_driver_t *drv, tec_pid_t *pid);
bool maiman_set_tec_pid(maiman_driver_t *drv, tec_pid_t pid);
bool maiman_start_device(maiman_driver_t *drv);
bool maiman_stop_device(maiman_driver_t *drv);
bool maiman_start_tec(maiman_driver_t *drv);
bool maiman_stop_tec(maiman_driver_t *drv);
bool maiman_set_internal_current_control(maiman_driver_t *drv, bool internal);
bool maiman_set_internal_enable_control(maiman_driver_t *drv, bool internal);
bool maiman_set_internal_tec_temperature_control(maiman_driver_t *drv, bool internal);
bool maiman_set_internal_tec_enable_control(maiman_driver_t *drv, bool internal);
bool maiman_allow_interlock(maiman_driver_t *drv);
bool maiman_deny_interlock(maiman_driver_t *drv);
bool maiman_allow_external_ntc_interlock(maiman_driver_t *drv);
bool maiman_deny_external_ntc_interlock(maiman_driver_t *drv);
bool maiman_save_parameters(maiman_driver_t *drv);
bool maiman_reset_parameters(maiman_driver_t *drv);

#endif //MAIMAN_H
