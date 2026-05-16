/**
 * @file app_warning.h
 * @brief HISPEC warning topic wrapper.
 *
 * The reusable command dispatch layer owns warning JSON construction and
 * non-blocking outbound queueing. This app-local wrapper supplies the HISPEC
 * warning topic so handlers do not repeat topic formatting.
 *
 * Copyright (c) 2026 Caltech Optical Observatories
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef HISPEC_APP_WARNING_H
#define HISPEC_APP_WARNING_H

/**
 * @brief Emit a lightweight warning to local logs and best-effort MQTT.
 *
 * Warnings are for suspicious or degraded conditions that should be visible but
 * should not make a command fail by themselves. Emission may enqueue an outbound
 * MQTT warning and never publishes directly from the caller's context.
 *
 * @param code Short stable warning code, for example "serial_guard_active".
 * @param msg Human-readable warning text.
 * @param context Optional short context string, such as a command key.
 */
void app_warning_emit(const char *code, const char *msg, const char *context);

#endif /* HISPEC_APP_WARNING_H */
