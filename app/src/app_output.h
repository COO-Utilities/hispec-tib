/*
 * Copyright (c) 2026 Caltech Optical Observatories
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef HISPEC_APP_OUTPUT_H
#define HISPEC_APP_OUTPUT_H

#include <coo_commons/command_dispatch.h>

/**
 * @file app_output.h
 * @brief Queue app warnings and telemetry without exposing app runtime wiring.
 */

/**
 * Log/queue through the app's configured output path after command_runtime_init().
 * Uses the existing common emitter's payload, delivery, and return contracts.
 * Queue insertion never waits; required delivery retries only after successful
 * enqueue. Warning scratch contention keeps the local log and returns -EAGAIN.
 * Does not publish MQTT or perform hardware I/O. Main owns the output drain.
 */
int app_output_emit(const struct coo_cmd_runtime_emit_args *args);

#endif /* HISPEC_APP_OUTPUT_H */
