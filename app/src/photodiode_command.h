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
int pd_get(const struct coo_cmd_request *cmd, struct coo_cmd_response *out);

/** @brief Return the active and lowest dark windows for one channel. */
int pd_dark_get(const struct coo_cmd_request *cmd, struct coo_cmd_response *out);

/** @brief Measure, force, or reset photodiode dark state for one channel. */
int pd_dark_set(const struct coo_cmd_request *cmd, struct coo_cmd_response *out);

/**
 * @brief Return app-owned photodiode calibration settings for one channel.
 *
 * Reads app settings and current dark-measurement status. It does not modify
 * hardware or persistent storage.
 */
int pd_settings_get(const struct coo_cmd_request *cmd, struct coo_cmd_response *out);

/**
 * @brief Update app-owned photodiode calibration settings for one channel.
 *
 * Validates numeric ranges and updates the app settings cache. Persistence is
 * controlled by the command payload's documented `persist` field.
 */
int pd_settings_set(const struct coo_cmd_request *cmd, struct coo_cmd_response *out);

#endif /* HISPEC_PHOTODIODE_COMMAND_H */
