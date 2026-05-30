/**
 * @file sntp_sync.h
 * @brief SNTP scheduling and clock-status helpers.
 *
 * SNTP state is cached for command responses. Synchronization runs in a
 * low-priority SNTP thread and can block while waiting for an SNTP reply.
 *
 * Copyright (c) 2026 Caltech Optical Observatories
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef HISPEC_SNTP_SYNC_H
#define HISPEC_SNTP_SYNC_H

#include <stdbool.h>
#include <stdint.h>
#include <zephyr/net/net_ip.h>

enum sntp_sync_source {
	SNTP_SYNC_SOURCE_NONE = 0,
	SNTP_SYNC_SOURCE_MANUAL = 1,
	SNTP_SYNC_SOURCE_DHCP = 2,
};

struct sntp_sync_status {
	bool enabled;
	bool synced;
	enum sntp_sync_source source;
	char server[NET_IPV4_ADDR_LEN];
	int last_error;
	uint64_t last_sync_utc_ms;
	int64_t last_sync_uptime_ms;
};

/** @brief Initialize SNTP status and start first sync when enabled. */
void sntp_sync_init(void);

/** @brief Wake the SNTP thread for immediate execution. */
void sntp_sync_schedule_now(void);

/** @brief Copy the most recent SNTP status under the module mutex. */
void sntp_sync_get_status(struct sntp_sync_status *out);

/** @brief Convert SNTP source enum to command/status text. */
const char *sntp_sync_source_str(enum sntp_sync_source source);

#endif /* HISPEC_SNTP_SYNC_H */
