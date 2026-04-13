/*
 * HiSPEC-TIB SNTP synchronization helpers.
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

void sntp_sync_init(void);
void sntp_sync_schedule_now(void);
void sntp_sync_get_status(struct sntp_sync_status *out);
const char *sntp_sync_source_str(enum sntp_sync_source source);

#endif /* HISPEC_SNTP_SYNC_H */
