/*
 * Copyright (c) 2026 Caltech Optical Observatories
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef HISPEC_ATTENUATOR_COMMAND_H
#define HISPEC_ATTENUATOR_COMMAND_H

#include "command.h"

/**
 * @file attenuator_command.h
 * @brief Command adapters for logical attenuator values and calibration.
 */

/**
 * @brief Return one logical attenuator's value or model coefficients.
 *
 * Parses `atten/<laser>` or `atten/<laser>/coeff`, checks that the mapped
 * logical attenuator belongs to the active board profile, and may block on DAC
 * I2C for value readback. It does not modify hardware or settings.
 */
int atten_setting_get(const struct coo_cmd_request *cmd, struct coo_cmd_response *out);

/**
 * @brief Update one logical attenuator's value or model coefficients.
 *
 * Parses and validates the command payload, writes DAC-backed attenuator state,
 * and optionally persists coefficient changes through app settings. It may
 * block on DAC I2C and can enqueue attenuator range warnings through the domain
 * driver. Compact `atten/<laser>` value commands return DAC-readback state.
 */
int atten_setting_set(const struct coo_cmd_request *cmd, struct coo_cmd_response *out);

/** @brief Query compact attenuator-calibration state. */
int atten_calibration_get(const struct coo_cmd_request *cmd, struct coo_cmd_response *out);

/** @brief Query retained attenuator-calibration records as binary MQTT chunks. */
int atten_calibration_records_get(const struct coo_cmd_request *cmd,
				  struct coo_cmd_response *out);

/** @brief Start or stop automatic attenuator calibration. */
int atten_calibration_set(const struct coo_cmd_request *cmd, struct coo_cmd_response *out);

#endif /* HISPEC_ATTENUATOR_COMMAND_H */
