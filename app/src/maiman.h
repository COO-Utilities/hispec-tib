//
// Created by Jeb Bailey on 4/28/25.
//

#ifndef MAIMAN_H
#define MAIMAN_H

#include <stdint.h>
#include <stdbool.h>
#include <zephyr/modbus/modbus.h>

#define CLIENT_IFACE 0


// Register addresses
#define REG_TEC_TEMPERATURE_MEASURED        0x1000
#define REG_PCB_TEMPERATURE_MEASURED        0x1001
#define REG_TEC_TEMPERATURE_VALUE           0x1002
#define REG_CURRENT_MEASURED                0x1003
#define REG_CURRENT                         0x1004
#define REG_VOLTAGE_MEASURED                0x1005
#define REG_CURRENT_MAX_LIMIT               0x1006
#define REG_CURRENT_PROTECTION_THRESHOLD    0x1007
#define REG_CURRENT_SET_CALIBRATION         0x1008
#define REG_NTC_COEFFICIENT                 0x1009
#define REG_TEC_CURRENT_MEASURED            0x100A
#define REG_TEC_VOLTAGE                     0x100B
#define REG_SERIAL_NUMBER                   0x100C
#define REG_FREQUENCY                       0x100D
#define REG_DURATION                        0x100E
#define REG_STATE_OF_DEVICE_COMMAND         0x1010

typedef uint16_t laser_address_t;

typedef struct {
    const char *name;
    const laser_address_t address;
} MaimanRegister;

static const MaimanRegister register_table[] = {
    {"TEC_TEMPERATURE_MEASURED",     REG_TEC_TEMPERATURE_MEASURED},
    {"PCB_TEMPERATURE_MEASURED",     REG_PCB_TEMPERATURE_MEASURED},
    {"TEC_TEMPERATURE_VALUE",        REG_TEC_TEMPERATURE_VALUE},
    {"CURRENT_MEASURED",             REG_CURRENT_MEASURED},
    {"CURRENT",                      REG_CURRENT},
    {"VOLTAGE_MEASURED",             REG_VOLTAGE_MEASURED},
    {"CURRENT_MAX_LIMIT",            REG_CURRENT_MAX_LIMIT},
    {"CURRENT_PROTECTION_THRESHOLD", REG_CURRENT_PROTECTION_THRESHOLD},
    {"CURRENT_SET_CALIBRATION",      REG_CURRENT_SET_CALIBRATION},
    {"NTC_COEFFICIENT",              REG_NTC_COEFFICIENT},
    {"TEC_CURRENT_MEASURED",         REG_TEC_CURRENT_MEASURED},
    {"TEC_VOLTAGE",                  REG_TEC_VOLTAGE},
    {"SERIAL_NUMBER",                REG_SERIAL_NUMBER},
    {"FREQUENCY",                    REG_FREQUENCY},
    {"DURATION",                     REG_DURATION},
    {"STATE_OF_DEVICE_COMMAND",      REG_STATE_OF_DEVICE_COMMAND}
};


// Finds the register, returns true and sets *address if found
bool maiman_get_register_address(const char *name, laser_address_t *address_out);


// Divider constants
#define DIVIDER_TEMPERATURE             10.0f
#define DIVIDER_CURRENT                 100.0f
#define DIVIDER_VOLTAGE                 100.0f
#define DIVIDER_DURATION                1000.0f
#define DIVIDER_FREQUENCY               1000.0f
#define DIVIDER_NTC_COEFFICIENT         1000.0f

// Device state bitmasks
#define OPERATION_STATE_STARTED         0x0001
#define CURRENT_SET_INTERNAL            0x0002
#define ENABLE_INTERNAL                 0x0004
#define EXTERNAL_NTC_INTERLOCK_DENIED   0x0008
#define INTERLOCK_DENIED                0x0010

// Modbus command values
#define MODBUS_START_COMMAND_VALUE      0x0001
#define MODBUS_STOP_COMMAND_VALUE       0x0000


/**
 * Structure representing a Maiman device instance.
 */
typedef struct {
    uint8_t node_id;
} maiman_driver_t;

/**
 * Initialize the driver with the target Modbus node ID.
 */
void maiman_init(maiman_driver_t *drv, uint8_t node_id);

/**
 * Read a single 16-bit register via Modbus.
 * @param drv     Pointer to driver instance
 * @param address Register address
 * @param value   Out parameter for register value
 * @return true on success, false on error
 */
bool maiman_read_u16(maiman_driver_t *drv, uint16_t address, uint16_t *value);

/**
 * Write a single 16-bit register via Modbus.
 * @param drv     Pointer to driver instance
 * @param address Register address
 * @param value   Value to write
 * @return true on success, false on error
 */
bool maiman_write_u16(maiman_driver_t *drv, uint16_t address, uint16_t value);

/**
 * Convert a raw unsigned 16-bit value to signed.
 */
int16_t maiman_to_signed(uint16_t value);

/* ----- Measurement getters ----- */
float maiman_get_tec_temperature_measured(maiman_driver_t *drv);
float maiman_get_pcb_temperature_measured(maiman_driver_t *drv);
float maiman_get_tec_temperature_value(maiman_driver_t *drv);
float maiman_get_current_measured(maiman_driver_t *drv);
float maiman_get_frequency(maiman_driver_t *drv);
float maiman_get_duration(maiman_driver_t *drv);
bool  maiman_get_current(maiman_driver_t *drv, float *value);
float maiman_get_voltage_measured(maiman_driver_t *drv);
float maiman_get_current_max_limit(maiman_driver_t *drv);
float maiman_get_current_protection_threshold(maiman_driver_t *drv);
float maiman_get_current_set_calibration(maiman_driver_t *drv);
float maiman_get_ntc_b25_100_coefficient(maiman_driver_t *drv);
float maiman_get_tec_current_measured(maiman_driver_t *drv);
float maiman_get_tec_voltage(maiman_driver_t *drv);

/* ----- Status and control ----- */
uint16_t maiman_get_serial_number(maiman_driver_t *drv);
uint16_t maiman_get_raw_status(maiman_driver_t *drv);
bool maiman_is_bit_set(maiman_driver_t *drv, uint16_t bitmask);
bool maiman_is_operation_started(maiman_driver_t *drv);
bool maiman_is_current_set_internal(maiman_driver_t *drv);
bool maiman_is_enable_internal(maiman_driver_t *drv);
bool maiman_is_external_ntc_denied(maiman_driver_t *drv);
bool maiman_is_interlock_denied(maiman_driver_t *drv);

/* ----- Setpoint and commands ----- */
bool maiman_set_current(maiman_driver_t *drv, float current);
bool maiman_set_frequency(maiman_driver_t *drv, float frequency);
bool maiman_set_duration(maiman_driver_t *drv, float duration);
bool maiman_start_device(maiman_driver_t *drv);
bool maiman_stop_device(maiman_driver_t *drv);

#endif //MAIMAN_H
