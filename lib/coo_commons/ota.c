/*
 * Copyright (c) 2026 Caltech Optical Observatories
 * SPDX-License-Identifier: Apache-2.0
 */

#include <coo_commons/ota.h>

#include <errno.h>
#include <string.h>
#include <zephyr/kernel.h>
#include <zephyr/dfu/mcuboot.h>
#include <zephyr/mgmt/mcumgr/grp/img_mgmt/img_mgmt.h>
#include <zephyr/mgmt/mcumgr/mgmt/callbacks.h>
#include <zephyr/mgmt/mcumgr/mgmt/mgmt.h>
#include <zephyr/mgmt/mcumgr/transport/smp_udp.h>
#include <zephyr/storage/flash_map.h>
#include <zephyr/sys/util.h>

#define OTA_TRIAL_DEADLINE_MS (5 * 60 * 1000)

BUILD_ASSERT(COO_OTA_HASH_SIZE == IMAGE_HASH_LEN);

/* All OTA transitions are serialized here. SMP has one processing workqueue;
 * its admission/done callbacks bracket an operation even if the UDP listener
 * closes meanwhile. Keep command restrictions until that operation ends.
 * MCUboot flash flags own durable state; pending is refreshed after each SMP
 * operation. Window/trial bookkeeping never goes into application NVS.
 */
K_MUTEX_DEFINE(ota_lock);
static bool ota_enabled;
static bool ota_inflight;
static bool ota_pending;
static bool ota_trial;
static bool ota_reverting;
static int64_t ota_window_deadline_ms;
static uint8_t ota_image_hash[COO_OTA_HASH_SIZE];
static coo_ota_ready_fn ota_ready;
static void *ota_ready_data;

/* Caller holds ota_lock. Error/unknown swap state also retains command restrictions. */
static void ota_read_pending(void)
{
    int type = mcuboot_swap_type();

    ota_pending = type != BOOT_SWAP_TYPE_NONE && type != BOOT_SWAP_TYPE_REVERT;
}

/* Caller holds ota_lock; queued SMP requests still pass admission below. */
static void ota_close(void)
{
    if (ota_enabled) {
        ota_enabled = false;
        (void)smp_udp_close();
    }
}

/* SMP processing workqueue only; entry readiness was checked before opening. */
static enum mgmt_cb_return ota_smp_event(uint32_t event, enum mgmt_cb_return prev_status,
                                       int32_t *rc, uint16_t *group, bool *abort_more,
                                       void *data, size_t data_size)
{
    const struct mgmt_evt_op_cmd_arg *arg = data;
    enum mgmt_cb_return result = MGMT_CB_OK;

    ARG_UNUSED(group);
    ARG_UNUSED(abort_more);
    ARG_UNUSED(data_size);
    k_mutex_lock(&ota_lock, K_FOREVER);
    if (event == MGMT_EVT_OP_CMD_RECV) {
        bool image_operation = arg->group == MGMT_GROUP_ID_IMAGE &&
            (arg->id == IMG_MGMT_ID_STATE ||
             (arg->id == IMG_MGMT_ID_UPLOAD && arg->op == MGMT_OP_WRITE));

        if (prev_status != MGMT_CB_OK || !image_operation || !ota_enabled ||
            k_uptime_get() >= ota_window_deadline_ms || ota_trial || ota_reverting ||
            (ota_pending && arg->id == IMG_MGMT_ID_UPLOAD)) {
            *rc = MGMT_ERR_EBADSTATE;
            result = MGMT_CB_ERROR_RC;
        } else {
            ota_inflight = true;
        }
    } else if (event == MGMT_EVT_OP_CMD_DONE && ota_inflight) {
        /* A test request can persist its pending flag after the window closes.
         * Refresh it before releasing the in-flight guard.
         */
        ota_read_pending();
        ota_inflight = false;
    }
    k_mutex_unlock(&ota_lock);
    return result;
}

static struct mgmt_callback ota_smp_callback = {
    .callback = ota_smp_event,
    .event_id = MGMT_EVT_OP_CMD_RECV | MGMT_EVT_OP_CMD_DONE,
};

int coo_ota_init(coo_ota_ready_fn ready, void *user_data)
{
    int rc;

    ota_trial = !boot_is_img_confirmed();
    rc = img_mgmt_read_info(0, NULL, ota_image_hash, NULL);
    if (rc != 0) {
        return -EIO;
    }
    ota_ready = ready;
    ota_ready_data = user_data;
    ota_read_pending();
    mgmt_callback_register(&ota_smp_callback);
    return 0;
}

/* Caller holds ota_lock. One predicate covers dispatch and reported status. */
static bool ota_active_locked(void)
{
    return ota_enabled || ota_inflight || ota_pending || ota_trial || ota_reverting;
}

bool coo_ota_active(void)
{
    bool active;

    k_mutex_lock(&ota_lock, K_FOREVER);
    active = ota_active_locked();
    k_mutex_unlock(&ota_lock);
    return active;
}

void coo_ota_get_status(struct coo_ota_status *out)
{
    int64_t now;

    k_mutex_lock(&ota_lock, K_FOREVER);
    now = k_uptime_get();
    if (ota_enabled && now >= ota_window_deadline_ms) {
        ota_close();
    }
    *out = (struct coo_ota_status){
        .enabled = ota_enabled,
        .active = ota_active_locked(),
        .confirmed = !ota_trial,
        .pending = ota_pending,
        .remaining_s = ota_enabled ?
            (uint32_t)MAX(0, (ota_window_deadline_ms - now + 999) / 1000) : 0U,
        .trial_remaining_s = ota_trial ?
            (uint32_t)MAX(0, (OTA_TRIAL_DEADLINE_MS - now + 999) / 1000) : 0U,
        .max_image_size = PARTITION_SIZE(slot1_partition) - CONFIG_MCUBOOT_UPDATE_FOOTER_SIZE,
    };
    memcpy(out->image_hash, ota_image_hash, sizeof(out->image_hash));
    k_mutex_unlock(&ota_lock);
}

int coo_ota_set_window(bool enable, uint32_t duration_s)
{
    int rc = 0;
    int64_t now;

    if (enable && (duration_s < 1U || duration_s > COO_OTA_MAX_WINDOW_S)) {
        return -EINVAL;
    }
    k_mutex_lock(&ota_lock, K_FOREVER);
    now = k_uptime_get();
    if (ota_enabled && now >= ota_window_deadline_ms) {
        ota_close();
    }
    if (!enable) {
        ota_close();
    } else if (ota_trial || ota_reverting || ota_pending || (!ota_enabled && ota_inflight)) {
        rc = -EBUSY;
    } else if (ota_ready == NULL || (rc = ota_ready(ota_ready_data)) == 0) {
        if (!ota_enabled) {
            rc = smp_udp_open();
        }
        if (rc == 0) {
            ota_enabled = true;
            ota_window_deadline_ms = now + (int64_t)duration_s * 1000;
        }
    }
    k_mutex_unlock(&ota_lock);
    return rc;
}

int coo_ota_confirm(const uint8_t image_hash[COO_OTA_HASH_SIZE])
{
    int rc = 0;

    /* Hold the lock through image-ok: expiry cannot schedule competing rollback.
     * Confirmation changes MCUboot state only; application hardware stays put.
     */
    k_mutex_lock(&ota_lock, K_FOREVER);
    if (memcmp(image_hash, ota_image_hash, sizeof(ota_image_hash)) != 0) {
        rc = -EINVAL;
    } else if (ota_reverting || (ota_trial && k_uptime_get() >= OTA_TRIAL_DEADLINE_MS)) {
        rc = -ETIMEDOUT;
    } else if (ota_trial) {
        rc = boot_write_img_confirmed();
        if (rc == 0) {
            ota_trial = false;
        }
    }
    k_mutex_unlock(&ota_lock);
    return rc;
}

enum coo_ota_poll_result coo_ota_poll(void)
{
    enum coo_ota_poll_result result = COO_OTA_CONTINUE;

    k_mutex_lock(&ota_lock, K_FOREVER);
    if (ota_enabled && k_uptime_get() >= ota_window_deadline_ms) {
        ota_close();
    }
    if (ota_reverting) {
        result = COO_OTA_AWAITING_REBOOT;
    } else if (ota_trial && k_uptime_get() >= OTA_TRIAL_DEADLINE_MS) {
        ota_reverting = true;
        ota_close();
        result = COO_OTA_REBOOT_REQUESTED;
    }
    k_mutex_unlock(&ota_lock);
    return result;
}
