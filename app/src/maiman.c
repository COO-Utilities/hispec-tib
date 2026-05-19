/**
 * @file maiman.c
 * @brief Blocking Modbus RTU transactions for Maiman SF8025 registers.
 */

#include "maiman.h"

#include <errno.h>
#include <stdint.h>

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(maiman, LOG_LEVEL_DBG);

/* c.f. as needed:
 * https://docs.zephyrproject.org/apidoc/latest/group__modbus.html
 * https://docs.zephyrproject.org/latest/samples/subsys/modbus/rtu_client/README.html
 */

void maiman_init(maiman_driver_t *drv, uint8_t node_id)
{
	if (drv == NULL) {
		return;
	}

	drv->node_id = node_id;
	drv->serial_number = 0U;
}

/**
 * Read a single 16-bit holding register via Zephyr's Modbus client API.
 * The call blocks until the RTU transaction completes or times out.
 */
bool maiman_read_u16(maiman_driver_t *drv, uint16_t address, uint16_t *value)
{
	int err;

	if (drv == NULL || value == NULL) {
		return false;
	}

	err = modbus_read_holding_regs(CLIENT_IFACE, drv->node_id, address, value, 1);
	if (err < 0) {
		LOG_ERR("Modbus read node=%u reg=0x%04x failed: %d",
			drv->node_id, address, err);
		return false;
	}
	return true;
}

/**
 * Write a single 16-bit holding register via Zephyr's Modbus client API.
 * This changes device state or an EEPROM-backed parameter depending on the
 * selected register; callers own any higher-level safety sequencing.
 */
bool maiman_write_u16(maiman_driver_t *drv, uint16_t address, uint16_t value)
{
	int err;

	if (drv == NULL) {
		return false;
	}

	err = modbus_write_holding_regs(CLIENT_IFACE, drv->node_id, address, &value, 1);
	if (err < 0) {
		LOG_ERR("Modbus write node=%u reg=0x%04x value=0x%04x failed: %d",
			drv->node_id, address, value, err);
		return false;
	}
	return true;
}

static int16_t maiman_to_signed(uint16_t value)
{
	return (int16_t)value;
}

static bool maiman_scaled_to_raw(float value, float divider, bool signed_value, uint16_t *raw)
{
	float scaled;
	int32_t signed_rounded;
	uint32_t unsigned_rounded;

	if (raw == NULL || !(divider > 0.0f) || !(value == value)) {
		return false;
	}

	scaled = value * divider;
	if (signed_value) {
		if (scaled < -32768.0f || scaled > 32767.0f) {
			return false;
		}
		signed_rounded = (int32_t)(scaled >= 0.0f ? scaled + 0.5f : scaled - 0.5f);
		*raw = (uint16_t)((int16_t)signed_rounded);
		return true;
	}

	if (scaled < 0.0f || scaled > 65535.0f) {
		return false;
	}
	unsigned_rounded = (uint32_t)(scaled + 0.5f);
	if (unsigned_rounded > UINT16_MAX) {
		return false;
	}
	*raw = (uint16_t)unsigned_rounded;
	return true;
}

bool maiman_read_scaled(maiman_driver_t *drv, uint16_t address, float divider,
			bool signed_value, float *value)
{
	uint16_t raw;

	if (value == NULL || !(divider > 0.0f)) {
		return false;
	}

	if (!maiman_read_u16(drv, address, &raw)) {
		return false;
	}

	*value = signed_value ? ((float)maiman_to_signed(raw) / divider) :
				((float)raw / divider);
	return true;
}

bool maiman_write_scaled(maiman_driver_t *drv, uint16_t address, float divider,
			 bool signed_value, float value)
{
	uint16_t raw;

	if (!maiman_scaled_to_raw(value, divider, signed_value, &raw)) {
		LOG_ERR("Scaled write reg=0x%04x out of range", address);
		return false;
	}

	return maiman_write_u16(drv, address, raw);
}

float maiman_get_tec_temperature_measured(maiman_driver_t *drv)
{
	float value;

	if (maiman_read_scaled(drv, REG_TEC_TEMPERATURE_MEASURED,
			       DIVIDER_TEC_TEMPERATURE, true, &value)) {
		return value;
	}
	return -273.15f;
}

float maiman_get_pcb_temperature_measured(maiman_driver_t *drv)
{
	float value;

	if (maiman_read_scaled(drv, REG_PCB_TEMPERATURE_MEASURED,
			       DIVIDER_PCB_TEMPERATURE, true, &value)) {
		return value;
	}
	return -273.15f;
}

float maiman_get_tec_temperature_value(maiman_driver_t *drv)
{
	float value;

	if (maiman_read_scaled(drv, REG_TEC_TEMPERATURE_VALUE,
			       DIVIDER_TEC_TEMPERATURE, true, &value)) {
		return value;
	}
	return -273.15f;
}

float maiman_get_current_measured(maiman_driver_t *drv)
{
	float value;

	if (maiman_read_scaled(drv, REG_CURRENT_MEASURED, DIVIDER_CURRENT, false, &value)) {
		return value;
	}
	return -1.0f;
}

bool maiman_get_current(maiman_driver_t *drv, float *value)
{
	return maiman_read_scaled(drv, REG_CURRENT, DIVIDER_CURRENT, false, value);
}

float maiman_get_voltage_measured(maiman_driver_t *drv)
{
	float value;

	if (maiman_read_scaled(drv, REG_VOLTAGE_MEASURED, DIVIDER_VOLTAGE, false, &value)) {
		return value;
	}
	return -1.0f;
}

float maiman_get_current_min(maiman_driver_t *drv)
{
	float value;

	if (maiman_read_scaled(drv, REG_CURRENT_MIN, DIVIDER_CURRENT, false, &value)) {
		return value;
	}
	return -1.0f;
}

float maiman_get_current_max(maiman_driver_t *drv)
{
	float value;

	if (maiman_read_scaled(drv, REG_CURRENT_MAX, DIVIDER_CURRENT, false, &value)) {
		return value;
	}
	return -1.0f;
}

float maiman_get_current_max_limit(maiman_driver_t *drv)
{
	float value;

	if (maiman_read_scaled(drv, REG_CURRENT_MAX_LIMIT, DIVIDER_CURRENT, false, &value)) {
		return value;
	}
	return -1.0f;
}

float maiman_get_current_protection_threshold(maiman_driver_t *drv)
{
	float value;

	if (maiman_read_scaled(drv, REG_CURRENT_PROTECTION_THRESHOLD,
			       DIVIDER_CURRENT, false, &value)) {
		return value;
	}
	return -1.0f;
}

float maiman_get_current_set_calibration(maiman_driver_t *drv)
{
	float value;

	if (maiman_read_scaled(drv, REG_CURRENT_SET_CALIBRATION,
			       DIVIDER_CURRENT_SET_CALIBRATION, false, &value)) {
		return value;
	}
	return -1.0f;
}

float maiman_get_ntc_b25_100_coefficient(maiman_driver_t *drv)
{
	float value;

	if (maiman_read_scaled(drv, REG_NTC_COEFFICIENT,
			       DIVIDER_NTC_COEFFICIENT, false, &value)) {
		return value;
	}
	return -1.0f;
}

float maiman_get_tec_current_measured(maiman_driver_t *drv)
{
	float value;

	if (maiman_read_scaled(drv, REG_TEC_CURRENT_MEASURED,
			       DIVIDER_TEC_CURRENT, true, &value)) {
		return value;
	}
	return -1.0f;
}

float maiman_get_tec_current_limit(maiman_driver_t *drv)
{
	float value;

	if (maiman_read_scaled(drv, REG_TEC_CURRENT_LIMIT,
			       DIVIDER_TEC_CURRENT, false, &value)) {
		return value;
	}
	return -1.0f;
}

float maiman_get_tec_current_set_calibration(maiman_driver_t *drv)
{
	float value;

	if (maiman_read_scaled(drv, REG_TEC_CURRENT_SET_CALIBRATION,
			       DIVIDER_TEC_CURRENT_SET_CALIBRATION, false, &value)) {
		return value;
	}
	return -1.0f;
}

float maiman_get_tec_voltage(maiman_driver_t *drv)
{
	float value;

	if (maiman_read_scaled(drv, REG_TEC_VOLTAGE, DIVIDER_TEC_VOLTAGE, true, &value)) {
		return value;
	}
	return -1.0f;
}

uint16_t maiman_get_device_id(maiman_driver_t *drv)
{
	uint16_t raw;

	if (maiman_read_u16(drv, REG_DEVICE_ID, &raw)) {
		return raw;
	}
	return 0U;
}

uint16_t maiman_get_serial_number(maiman_driver_t *drv)
{
	uint16_t raw;

	if (maiman_read_u16(drv, REG_SERIAL_NUMBER, &raw)) {
		if (drv != NULL) {
			drv->serial_number = raw;
		}
		return raw;
	}
	return 0U;
}

uint16_t maiman_get_raw_status(maiman_driver_t *drv)
{
	uint16_t raw;

	if (maiman_read_u16(drv, REG_STATE_OF_DEVICE_COMMAND, &raw)) {
		return raw;
	}
	return 0U;
}

uint16_t maiman_get_raw_lock_status(maiman_driver_t *drv)
{
	uint16_t raw;

	if (maiman_read_u16(drv, REG_LOCK_STATUS, &raw)) {
		return raw;
	}
	return 0U;
}

uint16_t maiman_get_raw_tec_status(maiman_driver_t *drv)
{
	uint16_t raw;

	if (maiman_read_u16(drv, REG_STATE_OF_TEC_COMMAND, &raw)) {
		return raw;
	}
	return 0U;
}

bool maiman_is_bit_set(maiman_driver_t *drv, uint16_t bitmask)
{
	uint16_t status = maiman_get_raw_status(drv);

	return (status & bitmask) != 0U;
}

bool maiman_is_operation_started(maiman_driver_t *drv)
{
	return maiman_is_bit_set(drv, OPERATION_STATE_STARTED);
}

bool maiman_is_current_set_internal(maiman_driver_t *drv)
{
	return maiman_is_bit_set(drv, CURRENT_SET_INTERNAL);
}

bool maiman_is_enable_internal(maiman_driver_t *drv)
{
	return maiman_is_bit_set(drv, ENABLE_INTERNAL);
}

bool maiman_is_interlock_denied(maiman_driver_t *drv)
{
	return maiman_is_bit_set(drv, INTERLOCK_DENIED);
}

static bool maiman_is_tec_bit_set(maiman_driver_t *drv, uint16_t bitmask)
{
	uint16_t status = maiman_get_raw_tec_status(drv);

	return (status & bitmask) != 0U;
}

bool maiman_is_tec_started(maiman_driver_t *drv)
{
	return maiman_is_tec_bit_set(drv, TEC_OPERATION_STATE_STARTED);
}

bool maiman_is_tec_set_internal(maiman_driver_t *drv)
{
	return maiman_is_tec_bit_set(drv, TEC_SET_INTERNAL);
}

bool maiman_is_tec_enable_internal(maiman_driver_t *drv)
{
	return maiman_is_tec_bit_set(drv, TEC_ENABLE_INTERNAL);
}

static bool maiman_is_lock_bit_set(maiman_driver_t *drv, uint16_t bitmask)
{
	uint16_t status = maiman_get_raw_lock_status(drv);

	return (status & bitmask) != 0U;
}

bool maiman_is_lockstate_interlock(maiman_driver_t *drv)
{
	return maiman_is_lock_bit_set(drv, LOCK_STATE_INTERLOCK);
}

bool maiman_is_lockstate_ld_overcurrent(maiman_driver_t *drv)
{
	return maiman_is_lock_bit_set(drv, LOCK_STATE_LD_OVERCURRENT);
}

bool maiman_is_lockstate_ld_overheat(maiman_driver_t *drv)
{
	return maiman_is_lock_bit_set(drv, LOCK_STATE_LD_OVERHEAT);
}

bool maiman_is_lockstate_tec_error(maiman_driver_t *drv)
{
	return maiman_is_lock_bit_set(drv, LOCK_STATE_TEC_ERROR);
}

bool maiman_is_lockstate_tec_selfheat(maiman_driver_t *drv)
{
	return maiman_is_lock_bit_set(drv, LOCK_STATE_TEC_SELFHEAT);
}

bool maiman_set_current(maiman_driver_t *drv, float current)
{
	return maiman_write_scaled(drv, REG_CURRENT, DIVIDER_CURRENT, false, current);
}

bool maiman_set_current_max(maiman_driver_t *drv, float current)
{
	return maiman_write_scaled(drv, REG_CURRENT_MAX, DIVIDER_CURRENT, false, current);
}

bool maiman_set_current_set_calibration(maiman_driver_t *drv, float calibration_percent)
{
	return maiman_write_scaled(drv, REG_CURRENT_SET_CALIBRATION,
				   DIVIDER_CURRENT_SET_CALIBRATION, false,
				   calibration_percent);
}

bool maiman_set_frequency(maiman_driver_t *drv, float frequency)
{
	return maiman_write_scaled(drv, REG_FREQUENCY, DIVIDER_FREQUENCY, false, frequency);
}

bool maiman_set_duration(maiman_driver_t *drv, float duration)
{
	return maiman_write_scaled(drv, REG_DURATION, DIVIDER_DURATION, false, duration);
}

bool maiman_set_tec_temperature(maiman_driver_t *drv, float temperature_c)
{
	return maiman_write_scaled(drv, REG_TEC_TEMPERATURE_VALUE,
				   DIVIDER_TEC_TEMPERATURE, true, temperature_c);
}

bool maiman_set_tec_current_limit(maiman_driver_t *drv, float current_a)
{
	return maiman_write_scaled(drv, REG_TEC_CURRENT_LIMIT,
				   DIVIDER_TEC_CURRENT, false, current_a);
}

bool maiman_get_tec_pid(maiman_driver_t *drv, tec_pid_t *pid)
{
	if (pid == NULL) {
		return false;
	}

	return maiman_read_u16(drv, REG_TEC_P_COEFFICIENT, &pid->kp) &&
	       maiman_read_u16(drv, REG_TEC_I_COEFFICIENT, &pid->ki) &&
	       maiman_read_u16(drv, REG_TEC_D_COEFFICIENT, &pid->kd);
}

bool maiman_set_tec_pid(maiman_driver_t *drv, tec_pid_t pid)
{
	return maiman_write_u16(drv, REG_TEC_P_COEFFICIENT, pid.kp) &&
	       maiman_write_u16(drv, REG_TEC_I_COEFFICIENT, pid.ki) &&
	       maiman_write_u16(drv, REG_TEC_D_COEFFICIENT, pid.kd);
}

bool maiman_start_device(maiman_driver_t *drv)
{
	return maiman_write_u16(drv, REG_STATE_OF_DEVICE_COMMAND, MODBUS_START_COMMAND_VALUE);
}

bool maiman_stop_device(maiman_driver_t *drv)
{
	return maiman_write_u16(drv, REG_STATE_OF_DEVICE_COMMAND, MODBUS_STOP_COMMAND_VALUE);
}

bool maiman_start_tec(maiman_driver_t *drv)
{
	return maiman_write_u16(drv, REG_STATE_OF_TEC_COMMAND, MODBUS_START_TEC_COMMAND_VALUE);
}

bool maiman_stop_tec(maiman_driver_t *drv)
{
	return maiman_write_u16(drv, REG_STATE_OF_TEC_COMMAND, MODBUS_STOP_TEC_COMMAND_VALUE);
}

bool maiman_set_internal_current_control(maiman_driver_t *drv, bool internal)
{
	return maiman_write_u16(drv, REG_STATE_OF_DEVICE_COMMAND,
				internal ? MODBUS_INTERNAL_CURRENT_SET_VALUE :
					   MODBUS_EXTERNAL_CURRENT_SET_VALUE);
}

bool maiman_set_internal_enable_control(maiman_driver_t *drv, bool internal)
{
	return maiman_write_u16(drv, REG_STATE_OF_DEVICE_COMMAND,
				internal ? MODBUS_INTERNAL_ENABLE_VALUE :
					   MODBUS_EXTERNAL_ENABLE_VALUE);
}

bool maiman_set_internal_tec_temperature_control(maiman_driver_t *drv, bool internal)
{
	return maiman_write_u16(drv, REG_STATE_OF_TEC_COMMAND,
				internal ? MODBUS_INTERNAL_TEMPERATURE_SET_VALUE :
					   MODBUS_EXTERNAL_TEMPERATURE_SET_VALUE);
}

bool maiman_set_internal_tec_enable_control(maiman_driver_t *drv, bool internal)
{
	return maiman_write_u16(drv, REG_STATE_OF_TEC_COMMAND,
				internal ? MODBUS_INTERNAL_TEC_ENABLE_VALUE :
					   MODBUS_EXTERNAL_TEC_ENABLE_VALUE);
}

bool maiman_allow_interlock(maiman_driver_t *drv)
{
	return maiman_write_u16(drv, REG_STATE_OF_DEVICE_COMMAND,
				MODBUS_ALLOW_INTERLOCK_VALUE);
}

bool maiman_deny_interlock(maiman_driver_t *drv)
{
	return maiman_write_u16(drv, REG_STATE_OF_DEVICE_COMMAND,
				MODBUS_DENY_INTERLOCK_VALUE);
}

bool maiman_save_parameters(maiman_driver_t *drv)
{
	if (!maiman_write_u16(drv, REG_SAVE_PARAMETERS, MODBUS_SAVE_PARAMETERS_VALUE)) {
		return false;
	}

	/* The SF8025 manual says parameter save makes the device unresponsive for
	 * about 300 ms. This Zephyr sleep yields the calling thread during that
	 * EEPROM commit window before any follow-up Modbus transaction.
	 */
	k_sleep(K_MSEC(300));
	return true;
}

bool maiman_reset_parameters(maiman_driver_t *drv)
{
	if (!maiman_write_u16(drv, REG_RESET_PARAMETERS, MODBUS_RESET_PARAMETERS_VALUE)) {
		return false;
	}

	/* Resetting EEPROM-backed parameters has the same device-side commit delay
	 * as saving according to the SF8025 manual's digital-control notes.
	 */
	k_sleep(K_MSEC(300));
	return true;
}
