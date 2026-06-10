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
int memsroute_get(const struct coo_cmd_request *cmd, struct coo_cmd_response *out);

/** Apply one MEMS route or route-loss setting. */
int memsroute_set(const struct coo_cmd_request *cmd, struct coo_cmd_response *out);

/** Query all MEMS switches or one switch. */
int mems_get(const struct coo_cmd_request *cmd, struct coo_cmd_response *out);

/** Apply one MEMS switch state or toggle profile. */
int mems_set(const struct coo_cmd_request *cmd, struct coo_cmd_response *out);

/** Query one AS splitter channel, usually with command key split/yj or split/hk. */
int splitting_get(const struct coo_cmd_request *cmd, struct coo_cmd_response *out);

/** Apply one AS-PCB splitter channel using channel, ratio1, and ratio2. */
int splitting_set(const struct coo_cmd_request *cmd, struct coo_cmd_response *out);

#endif /* HISPEC_MEMS_COMMAND_H */
