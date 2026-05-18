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
 * Parses `atten/<laser>/<value|valuedb|coeff>`, checks that the mapped logical
 * attenuator belongs to the active board profile, and may block on DAC I2C for
 * value readback. It does not modify hardware or settings.
 */
struct coo_cmd_response atten_setting_get(const struct coo_cmd_request *cmd);

/**
 * @brief Update one logical attenuator's value or model coefficients.
 *
 * Parses and validates the command payload, writes DAC-backed attenuator state,
 * and optionally persists coefficient changes through app settings. It may
 * block on DAC I2C and can enqueue attenuator range warnings through the domain
 * driver.
 */
struct coo_cmd_response atten_setting_set(const struct coo_cmd_request *cmd);

#endif /* HISPEC_ATTENUATOR_COMMAND_H */
