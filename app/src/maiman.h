/**
 * @file maiman.h
 * @brief Thin Modbus register wrapper for Maiman SF8025 laser drivers.
 *
 * This layer exposes raw and scaled register operations. Higher-level safety,
 * power sequencing, diode limits, and persistence are owned by lasers.c and the
 * Maiman module EEPROM, not this wrapper.
 *
 * Except local initialization/name lookup, calls below perform blocking Modbus
 * I/O. The laser owner serializes them; never call from the Modbus RX workqueue.
 * Checked bool operations return true only on success. Convenience getters use
 * sentinels that can overlap valid signed values: inspect drv->io_failed rather
 * than interpreting a returned zero/false/-1 as a confirmed physical state.
 * No getter programs registers. Setters do not write app NVS; SAVE/RESET explicitly
 * affect controller EEPROM. LD START/STOP and SAVE/RESET include a 350 ms busy wait.
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
/* Internal LD thermistor B (K), used for TEC temperature conversion. */
#define REG_INTERNAL_LD_NTC_SENSOR          0x007F

#define REG_RS_SETTING                      0x0080
#define REG_CURRENT_SET_CALIBRATION         0x0088
/* Optional external NTC B (K); that sensor is absent in HISPEC. */
#define REG_NTC_COEFFICIENT                 0x008A
#define REG_TEC_P_COEFFICIENT               0x0091
#define REG_TEC_I_COEFFICIENT               0x0092
#define REG_TEC_D_COEFFICIENT               0x0093

typedef uint16_t laser_address_t;

/** Finds the register, returns true and sets *address_out if found. */
bool maiman_get_register_address(const char *name, laser_address_t *address_out);

/**
 * Initializes the Zephyr Modbus client without sending a controller request.
 * Device setup owns interface lookup and calls this before workers start.
 * Runtime register calls own timeout cancellation and deferred initialization;
 * callers must hold the laser I/O mutex and run outside the Modbus RX workqueue.
 */
int maiman_init_client(int iface);


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
	/* Sticky for this operation's endpoint lifetime; later reads cannot hide
	 * an earlier transport/exception failure. maiman_init() starts a new operation.
	 */
	bool io_failed;
	/* Operation-local transport facts, consumed by the laser owner. */
	int last_error;
	int64_t last_response_ms;
} maiman_driver_t;

/**
 * Initialize an operation's endpoint and clear its communication results; no I/O.
 */
void maiman_init(maiman_driver_t *drv, uint8_t node_id);

/** Read/write one raw holding register. Calls block on Modbus RTU I/O. */
bool maiman_read_u16(maiman_driver_t *drv, uint16_t address, uint16_t *value);
/** Write one holding register; false preserves the operation-local error. May sleep. */
bool maiman_write_u16(maiman_driver_t *drv, uint16_t address, uint16_t value);
/** Read/write scaled engineering units. Calls block on Modbus RTU I/O. */
bool maiman_read_scaled(maiman_driver_t *drv, uint16_t address, double divider,
                        bool signed_value, double *value);
/** Quantize engineering units by divider and write; false on range or I/O failure. */
bool maiman_write_scaled(maiman_driver_t *drv, uint16_t address, double divider,
                         bool signed_value, double value);


/* ----- Measurement getters ----- */
/** Checked signed TEC temperature read in degrees C; output is valid only on true. */
bool maiman_read_tec_temperature_measured(maiman_driver_t *drv, double *value);
/** Read TEC temperature in degrees C; -273.15 on failure. */
double maiman_get_tec_temperature_measured(maiman_driver_t *drv);
/** Read controller PCB temperature in degrees C; -273.15 on failure. */
double maiman_get_pcb_temperature_measured(maiman_driver_t *drv);
/** Read external NTC temperature in degrees C; -273.15 on failure. Sensor is absent on HISPEC. */
double maiman_get_ntc_temperature_measured(maiman_driver_t *drv);
/** Read TEC target in degrees C; -273.15 on failure. */
double maiman_get_tec_temperature_value(maiman_driver_t *drv);
/** Read measured diode current in mA; -1 on failure. */
double maiman_get_current_measured(maiman_driver_t *drv);
/** Read modulation frequency in Hz; -1 on failure. */
double maiman_get_frequency(maiman_driver_t *drv);
/** Read pulse duration in ms; -1 on failure. */
double maiman_get_duration(maiman_driver_t *drv);
/** Checked diode current setpoint read in mA; output is valid only on true. */
bool  maiman_get_current(maiman_driver_t *drv, double *value);
/** Read diode voltage in V; -1 on failure. */
double maiman_get_voltage_measured(maiman_driver_t *drv);
/** Read minimum diode current in mA; -1 on failure. */
double maiman_get_current_min(maiman_driver_t *drv);
/** Read configured maximum diode current in mA; -1 on failure. */
double maiman_get_current_max(maiman_driver_t *drv);
/** Read absolute diode-current ceiling in mA; -1 on failure. */
double maiman_get_current_max_limit(maiman_driver_t *drv);
/** Read hardware overcurrent protection threshold in mA; -1 on failure. */
double maiman_get_current_protection_threshold(maiman_driver_t *drv);
/** Read LD current calibration in percent; -1 on failure. */
double maiman_get_current_set_calibration(maiman_driver_t *drv);
/** Reads internal LD thermistor B (K) over Modbus; -1 on read failure. */
double maiman_get_ntc_b25_100_coefficient(maiman_driver_t *drv);
/** Read signed TEC current in A; -1 also denotes failure, so check io_failed. */
double maiman_get_tec_current_measured(maiman_driver_t *drv);
/** Read TEC current limit in A; -1 on failure. */
double maiman_get_tec_current_limit(maiman_driver_t *drv);
/** Read TEC calibration register in percent; -1 on failure. */
double maiman_get_tec_current_set_calibration(maiman_driver_t *drv);
/** Read signed TEC voltage in V; -1 also denotes failure, so check io_failed. */
double maiman_get_tec_voltage(maiman_driver_t *drv);

/* ----- Status and control ----- */
/** Read device identifier; zero on failure, with io_failed set. */
uint16_t maiman_get_device_id(maiman_driver_t *drv);
/** Read controller serial number; zero on failure, with io_failed set. */
uint16_t maiman_get_serial_number(maiman_driver_t *drv);
/** Read device-state bits; zero on failure is not proof of stopped operation. */
uint16_t maiman_get_raw_status(maiman_driver_t *drv);
/** Read lock bits; zero on failure is not proof that interlocks are clear. */
uint16_t maiman_get_raw_lock_status(maiman_driver_t *drv);
/** Checked TEC-state read; output is valid only on true. */
bool maiman_read_raw_tec_status(maiman_driver_t *drv, uint16_t *status);
/** Read TEC-state bits; zero on failure, with io_failed set. */
uint16_t maiman_get_raw_tec_status(maiman_driver_t *drv);
/** Checked TEC-started observation; false return leaves the observation unavailable. */
bool maiman_read_tec_started(maiman_driver_t *drv, bool *started);
/** Read device-state register and test any masked bit; false can also mean I/O failure. */
bool maiman_is_bit_set(maiman_driver_t *drv, uint16_t bitmask);
/** Read device-state bits for this predicate; false can also indicate I/O failure. */
bool maiman_is_operation_started(maiman_driver_t *drv);
/** Read device-state bits for this predicate; false can also indicate I/O failure. */
bool maiman_is_current_set_internal(maiman_driver_t *drv);
/** Read device-state bits for this predicate; false can also indicate I/O failure. */
bool maiman_is_enable_internal(maiman_driver_t *drv);
/** Read device-state bits for this predicate; false can also indicate I/O failure. */
bool maiman_is_external_ntc_denied(maiman_driver_t *drv);
/** Read device-state bits for this predicate; false can also indicate I/O failure. */
bool maiman_is_interlock_denied(maiman_driver_t *drv);
/** Read TEC-state bits for this predicate; false can also indicate I/O failure. */
bool maiman_is_tec_started(maiman_driver_t *drv);
/** Read TEC-state bits for this predicate; false can also indicate I/O failure. */
bool maiman_is_tec_set_internal(maiman_driver_t *drv);
/** Read TEC-state bits for this predicate; false can also indicate I/O failure. */
bool maiman_is_tec_enable_internal(maiman_driver_t *drv);
/** Read lock bits for this predicate; false can also indicate I/O failure. */
bool maiman_is_lockstate_interlock(maiman_driver_t *drv);
/** Read lock bits for this predicate; false can also indicate I/O failure. */
bool maiman_is_lockstate_ld_overcurrent(maiman_driver_t *drv);
/** Read lock bits for this predicate; false can also indicate I/O failure. */
bool maiman_is_lockstate_ld_overheat(maiman_driver_t *drv);
/** Read lock bits for this predicate; false can also indicate I/O failure. */
bool maiman_is_lockstate_external_ntc_interlock(maiman_driver_t *drv);
/** Read lock bits for this predicate; false can also indicate I/O failure. */
bool maiman_is_lockstate_tec_error(maiman_driver_t *drv);
/** Read lock bits for this predicate; false can also indicate I/O failure. */
bool maiman_is_lockstate_tec_selfheat(maiman_driver_t *drv);

/* ----- Setpoint and commands ----- */
/** Write diode current setpoint in mA; does not independently enforce diode policy. */
bool maiman_set_current(maiman_driver_t *drv, double current);
/** Write configured maximum diode current in mA; owner validates the physical limit. */
bool maiman_set_current_max(maiman_driver_t *drv, double current);
/** Write LD current calibration percent; owner controls calibration intent. */
bool maiman_set_current_set_calibration(maiman_driver_t *drv, double calibration_percent);
/** Write modulation frequency in Hz. */
bool maiman_set_frequency(maiman_driver_t *drv, double frequency);
/** Write pulse duration in ms. */
bool maiman_set_duration(maiman_driver_t *drv, double duration);
/** Write signed TEC target in degrees C. */
bool maiman_set_tec_temperature(maiman_driver_t *drv, double temperature_c);
/** Write TEC current limit in A. */
bool maiman_set_tec_current_limit(maiman_driver_t *drv, double current_a);
/** Read P/I/D registers sequentially; false can leave a partial output. */
bool maiman_get_tec_pid(maiman_driver_t *drv, tec_pid_t *pid);
/** Write P/I/D registers sequentially; failure does not roll back earlier writes. */
bool maiman_set_tec_pid(maiman_driver_t *drv, tec_pid_t pid);
/** Request LD START and wait through the controller busy interval, including failed ACKs. */
bool maiman_start_device(maiman_driver_t *drv);
/** Request LD STOP and wait through the controller busy interval, including failed ACKs. */
bool maiman_stop_device(maiman_driver_t *drv);
/** Request TEC START; no thermal qualification is performed here. */
bool maiman_start_tec(maiman_driver_t *drv);
/** Request TEC STOP; no independent temperature or current observation. */
bool maiman_stop_tec(maiman_driver_t *drv);
/** Select internal/external LD current control through a device command. */
bool maiman_set_internal_current_control(maiman_driver_t *drv, bool internal);
/** Select internal/external LD enable control through a device command. */
bool maiman_set_internal_enable_control(maiman_driver_t *drv, bool internal);
/** Select internal/external TEC target control through a TEC command. */
bool maiman_set_internal_tec_temperature_control(maiman_driver_t *drv, bool internal);
/** Select internal/external TEC enable control through a TEC command. */
bool maiman_set_internal_tec_enable_control(maiman_driver_t *drv, bool internal);
/** Write the controller ALLOW_INTERLOCK command; laser policy owns its use. */
bool maiman_allow_interlock(maiman_driver_t *drv);
/** Write the controller DENY_INTERLOCK command; laser policy owns its use. */
bool maiman_deny_interlock(maiman_driver_t *drv);
/** Write ALLOW_EXTERNAL_NTC_INTERLOCK; the external sensor is absent on HISPEC. */
bool maiman_allow_external_ntc_interlock(maiman_driver_t *drv);
/** Write DENY_EXTERNAL_NTC_INTERLOCK; laser policy owns its use. */
bool maiman_deny_external_ntc_interlock(maiman_driver_t *drv);
/** Request controller EEPROM SAVE and wait 350 ms after the attempt; no app NVS write. */
bool maiman_save_parameters(maiman_driver_t *drv);
/** Request controller parameter RESET and wait 350 ms; no app settings reset. */
bool maiman_reset_parameters(maiman_driver_t *drv);

#endif //MAIMAN_H
