/*
 * Copyright (c) 2026 Caltech Optical Observatories
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef HISPEC_PHOTODIODE_COMMAND_H
#define HISPEC_PHOTODIODE_COMMAND_H

#include "command.h"

/**
 * @file photodiode_command.h
 * @brief Command adapters for photodiode status, calibration, and settings.
 */

/**
 * @brief Return photodiode power, ADC, noise, and timing status.
 *
 * This command adapter reads cached photodiode state and app-level calibration
 * settings. It does not perform ADC I/O or publish telemetry.
 */
struct coo_cmd_response pd_get(const struct coo_cmd_request *cmd);

/**
 * @brief Apply photodiode actions such as dark measurement and reset.
 *
 * Validates the command payload and calls the photodiode domain API. Starting a
 * dark measurement arms work in the sampler thread and returns immediately; it
 * does not wait for the full measurement duration.
 */
struct coo_cmd_response pd_set(const struct coo_cmd_request *cmd);

/**
 * @brief Return app-owned photodiode calibration settings for one channel.
 *
 * Reads app settings and current dark-measurement status. It does not modify
 * hardware or persistent storage.
 */
struct coo_cmd_response pd_settings_get(const struct coo_cmd_request *cmd);

/**
 * @brief Update app-owned photodiode calibration settings for one channel.
 *
 * Validates numeric ranges and updates the app settings cache. Persistence is
 * controlled by the command payload's documented `persistent` field.
 */
struct coo_cmd_response pd_settings_set(const struct coo_cmd_request *cmd);

#endif /* HISPEC_PHOTODIODE_COMMAND_H */
