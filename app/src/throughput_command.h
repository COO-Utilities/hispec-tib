/*
 * Copyright (c) 2026 Caltech Optical Observatories
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef HISPEC_THROUGHPUT_COMMAND_H
#define HISPEC_THROUGHPUT_COMMAND_H

#include "command.h"

/**
 * @file throughput_command.h
 * @brief Command adapter for throughput-monitor requests.
 */

/**
 * @brief Parse and apply the measure_throughput command.
 *
 * Validates the MQTT/serial payload, starts or stops the matching monitor, and
 * returns one command response. This adapter does not publish telemetry; the
 * active monitor thread later enqueues telemetry through outbound_queue.
 */
struct OutMsg measure_throughput_set(const struct Command *cmd);

#endif /* HISPEC_THROUGHPUT_COMMAND_H */
