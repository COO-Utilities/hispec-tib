/*
 * Copyright (c) 2026 Caltech Optical Observatories
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef HISPEC_MEMS_COMMAND_H
#define HISPEC_MEMS_COMMAND_H

#include "command.h"

/**
 * @file mems_command.h
 * @brief Command adapters for MEMS switch, route, route-loss, and split control.
 *
 * The adapters parse MQTT/serial payloads and format command responses. MEMS
 * pulse scheduling, static route application, and splitter tick math are owned
 * by mems_switching.c.
 */

/** Query active MEMS routes or route-loss settings. */
struct coo_cmd_response memsroute_get(const struct coo_cmd_request *cmd);

/** Apply one MEMS route or route-loss setting. */
struct coo_cmd_response memsroute_set(const struct coo_cmd_request *cmd);

/** Query all MEMS switches or one switch. */
struct coo_cmd_response mems_get(const struct coo_cmd_request *cmd);

/** Apply one MEMS switch state or toggle profile. */
struct coo_cmd_response mems_set(const struct coo_cmd_request *cmd);

/** Query one AS splitter channel, usually with command key split/yj or split/hk. */
struct coo_cmd_response splitting_get(const struct coo_cmd_request *cmd);

/** Apply one AS-PCB splitter channel using channel, ratio1, and ratio2. */
struct coo_cmd_response splitting_set(const struct coo_cmd_request *cmd);

#endif /* HISPEC_MEMS_COMMAND_H */
