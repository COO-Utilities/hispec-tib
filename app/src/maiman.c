//
// Created by Jeb Bailey on 4/28/25.
//

#include "maiman.h"
#include <zephyr/logging/log.h>
#include <ctype.h>

LOG_MODULE_REGISTER(maiman, LOG_LEVEL_DBG);

//c.f. as needed: https://docs.zephyrproject.org/apidoc/latest/group__modbus.html
// https://docs.zephyrproject.org/latest/samples/subsys/modbus/rtu_client/README.html#modbus-rtu-client

static bool strcaseeq(const char *a, const char *b) {
    while (*a && *b) {
        if (tolower((unsigned char)*a) != tolower((unsigned char)*b)) return false;
        a++;
        b++;
    }
    return *a == *b;
}


// Finds the register, returns true and sets *address if found
bool maiman_get_register_address(const char *name, laser_address_t *address_out) {
    size_t n = sizeof(register_table)/sizeof(register_table[0]);
    for (size_t i = 0; i < n; ++i) {
        if (strcaseeq(name, register_table[i].name)) {
            if (address_out) {
                *address_out = (register_table[i].address);
            }
            return true;
        }
    }
    return false;
}



void maiman_init(maiman_driver_t *drv, uint8_t node_id) {
    drv->node_id = node_id;
}

bool maiman_read_u16(maiman_driver_t *drv, uint16_t address, uint16_t *value) {
    int err = modbus_read_holding_regs(CLIENT_IFACE,
                                       drv->node_id,
                                       address,
                                       value,
                                       1);
    if (err < 0) {
        LOG_ERR("Modbus read failed: %d", err);
        return false;
    }
    return true;
}

bool maiman_write_u16(maiman_driver_t *drv, uint16_t address, uint16_t value) {
    int err = modbus_write_holding_regs(CLIENT_IFACE,
                                        drv->node_id,
                                        address,
                                        &value,
                                        1);
    if (err < 0) {
        LOG_ERR("Modbus write failed: %d", err);
        return false;
    }
    return true;
}


int16_t maiman_to_signed(uint16_t value) {
    return (int16_t)value;
}

float maiman_get_tec_temperature_measured(maiman_driver_t *drv) {
    uint16_t raw;
    if (maiman_read_u16(drv, REG_TEC_TEMPERATURE_MEASURED, &raw)) {
        return maiman_to_signed(raw) / DIVIDER_TEMPERATURE;
    }
    return -273.15f;
}

float maiman_get_pcb_temperature_measured(maiman_driver_t *drv) {
    uint16_t raw;
    if (maiman_read_u16(drv, REG_PCB_TEMPERATURE_MEASURED, &raw)) {
        return maiman_to_signed(raw) / DIVIDER_TEMPERATURE;
    }
    return -273.15f;
}

float maiman_get_tec_temperature_value(maiman_driver_t *drv) {
    uint16_t raw;
    if (maiman_read_u16(drv, REG_TEC_TEMPERATURE_VALUE, &raw)) {
        return maiman_to_signed(raw) / DIVIDER_TEMPERATURE;
    }
    return -273.15f;
}

float maiman_get_current_measured(maiman_driver_t *drv) {
    uint16_t raw;
    if (maiman_read_u16(drv, REG_CURRENT_MEASURED, &raw)) {
        return raw / DIVIDER_CURRENT;
    }
    return -1.0f;
}

float maiman_get_frequency(maiman_driver_t *drv) {
    uint16_t raw;
    if (maiman_read_u16(drv, REG_FREQUENCY, &raw)) {
        return raw / DIVIDER_FREQUENCY;
    }
    return -1.0f;
}

float maiman_get_duration(maiman_driver_t *drv) {
    uint16_t raw;
    if (maiman_read_u16(drv, REG_DURATION, &raw)) {
        return raw / DIVIDER_DURATION;
    }
    return -1.0f;
}

bool maiman_get_current(maiman_driver_t *drv, float *value) {
    uint16_t raw;
    if (maiman_read_u16(drv, REG_CURRENT, &raw)) {
        *value = raw / DIVIDER_CURRENT;
        return true;
    }
    return false;
}

float maiman_get_voltage_measured(maiman_driver_t *drv) {
    uint16_t raw;
    if (maiman_read_u16(drv, REG_VOLTAGE_MEASURED, &raw)) {
        return raw / DIVIDER_VOLTAGE;
    }
    return -1.0f;
}

float maiman_get_current_max_limit(maiman_driver_t *drv) {
    uint16_t raw;
    if (maiman_read_u16(drv, REG_CURRENT_MAX_LIMIT, &raw)) {
        return raw / DIVIDER_CURRENT;
    }
    return -1.0f;
}

float maiman_get_current_protection_threshold(maiman_driver_t *drv) {
    uint16_t raw;
    if (maiman_read_u16(drv, REG_CURRENT_PROTECTION_THRESHOLD, &raw)) {
        return raw / DIVIDER_CURRENT;
    }
    return -1.0f;
}

float maiman_get_current_set_calibration(maiman_driver_t *drv) {
    uint16_t raw;
    if (maiman_read_u16(drv, REG_CURRENT_SET_CALIBRATION, &raw)) {
        return raw / DIVIDER_CURRENT;
    }
    return -1.0f;
}

float maiman_get_ntc_b25_100_coefficient(maiman_driver_t *drv) {
    uint16_t raw;
    if (maiman_read_u16(drv, REG_NTC_COEFFICIENT, &raw)) {
        return raw / DIVIDER_NTC_COEFFICIENT;
    }
    return -1.0f;
}

float maiman_get_tec_current_measured(maiman_driver_t *drv) {
    uint16_t raw;
    if (maiman_read_u16(drv, REG_TEC_CURRENT_MEASURED, &raw)) {
        return raw / DIVIDER_CURRENT;
    }
    return -1.0f;
}

float maiman_get_tec_voltage(maiman_driver_t *drv) {
    uint16_t raw;
    if (maiman_read_u16(drv, REG_TEC_VOLTAGE, &raw)) {
        return raw / DIVIDER_VOLTAGE;
    }
    return -1.0f;
}

uint16_t maiman_get_serial_number(maiman_driver_t *drv) {
    uint16_t raw;
    if (maiman_read_u16(drv, REG_SERIAL_NUMBER, &raw)) {
        return raw;
    }
    return 0;
}

uint16_t maiman_get_raw_status(maiman_driver_t *drv) {
    uint16_t raw;
    if (maiman_read_u16(drv, REG_STATE_OF_DEVICE_COMMAND, &raw)) {
        return raw;
    }
    return 0;
}

bool maiman_is_bit_set(maiman_driver_t *drv, uint16_t bitmask) {
    uint16_t status = maiman_get_raw_status(drv);
    return (status & bitmask) != 0;
}

bool maiman_is_operation_started(maiman_driver_t *drv) {
    return maiman_is_bit_set(drv, OPERATION_STATE_STARTED);
}

bool maiman_is_current_set_internal(maiman_driver_t *drv) {
    return maiman_is_bit_set(drv, CURRENT_SET_INTERNAL);
}

bool maiman_is_enable_internal(maiman_driver_t *drv) {
    return maiman_is_bit_set(drv, ENABLE_INTERNAL);
}

bool maiman_is_external_ntc_denied(maiman_driver_t *drv) {
    return maiman_is_bit_set(drv, EXTERNAL_NTC_INTERLOCK_DENIED);
}

bool maiman_is_interlock_denied(maiman_driver_t *drv) {
    return maiman_is_bit_set(drv, INTERLOCK_DENIED);
}

bool maiman_set_current(maiman_driver_t *drv, float current) {
    uint16_t value = (uint16_t)(current * DIVIDER_CURRENT);
    return maiman_write_u16(drv, REG_CURRENT, value);
}

bool maiman_set_frequency(maiman_driver_t *drv, float frequency) {
    uint16_t value = (uint16_t)(frequency * DIVIDER_FREQUENCY);
    return maiman_write_u16(drv, REG_FREQUENCY, value);
}

bool maiman_set_duration(maiman_driver_t *drv, float duration) {
    uint16_t value = (uint16_t)(duration * DIVIDER_DURATION);
    return maiman_write_u16(drv, REG_DURATION, value);
}

bool maiman_start_device(maiman_driver_t *drv) {
    return maiman_write_u16(drv, REG_STATE_OF_DEVICE_COMMAND, MODBUS_START_COMMAND_VALUE);
}

bool maiman_stop_device(maiman_driver_t *drv) {
    return maiman_write_u16(drv, REG_STATE_OF_DEVICE_COMMAND, MODBUS_STOP_COMMAND_VALUE);
}
