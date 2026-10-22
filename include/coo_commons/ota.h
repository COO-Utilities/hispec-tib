/*
 * Copyright (c) 2026 Caltech Optical Observatories
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef COO_COMMONS_OTA_H
#define COO_COMMONS_OTA_H

#include <stdbool.h>
#include <stdint.h>

/**
 * @file ota.h
 * @brief MCUboot single-image trials and explicit MCUmgr UDP maintenance windows.
 *
 * One static instance owns OTA state; MCUboot trailers own durable image state.
 * Initialize before workers/command ingress. Serialize window/confirmation calls
 * with guarded commands (COO dispatch does this on its executor). Main polls
 * deadlines; SMP callbacks run on Zephyr's processing workqueue. API calls may
 * sleep on the OTA mutex. No function publishes, feeds a watchdog, or reboots.
 */

#define COO_OTA_HASH_SIZE 32U
#define COO_OTA_DEFAULT_WINDOW_S 600U
#define COO_OTA_MAX_WINDOW_S 1800U

struct coo_ota_status {
	bool enabled;
	bool active;
	bool confirmed;
	bool pending;
	uint32_t remaining_s;
	uint32_t trial_remaining_s;
	uint32_t max_image_size;
	uint8_t image_hash[COO_OTA_HASH_SIZE];
};

enum coo_ota_poll_result {
	COO_OTA_CONTINUE,
	COO_OTA_REBOOT_REQUESTED,
	COO_OTA_AWAITING_REBOOT,
};

/**
 * Return 0 when hardware is already ready for OTA, or a negative errno.
 * Called under the OTA mutex before opening/renewing a window. Inspect existing
 * hardware state only; do not shut hardware down or call back into OTA.
 */
typedef int (*coo_ota_ready_fn)(void *user_data);

/**
 * Call once after device setup, before workers/ingress. Reads the running image
 * hash/confirmation state and registers SMP admission; UDP remains closed.
 * A NULL readiness callback means the application has no entry prerequisite.
 * Then inspect status.confirmed and prepare trial hardware before starting work.
 * The five-minute trial deadline is measured from application boot uptime.
 */
int coo_ota_init(coo_ota_ready_fn ready, void *user_data);

/** Snapshot OTA state; closes an expired window. Does not perform hardware I/O. */
void coo_ota_get_status(struct coo_ota_status *out);

/** Admission snapshot including open/in-flight/pending/trial/reverting states. */
bool coo_ota_active(void);

/**
 * Open/renew for 1..1800 seconds, or close (duration ignored). May open/close a
 * socket and invoke the readiness check. -EBUSY means an incompatible OTA state;
 * -EINVAL means an invalid duration; other errors come from readiness/transport.
 * Closing never cancels a pending image or an already-admitted SMP operation.
 */
int coo_ota_set_window(bool enable, uint32_t duration_s);

/**
 * Confirm this exact running hash by writing MCUboot's image-ok flag. May block
 * on flash. -EINVAL means hash mismatch; -ETIMEDOUT means rollback/deadline.
 * An already-confirmed matching image succeeds. Never restores hardware state.
 */
int coo_ota_confirm(const uint8_t image_hash[COO_OTA_HASH_SIZE]);

/**
 * Main-loop maintenance; may close UDP. REBOOT_REQUESTED is returned once on
 * trial expiry: request the application's delayed reboot. Stop feeding the
 * watchdog for both REBOOT_REQUESTED and AWAITING_REBOOT, including reboot errors.
 */
enum coo_ota_poll_result coo_ota_poll(void);

#endif /* COO_COMMONS_OTA_H */
