# COO OTA integration

`coo_commons/ota.h` provides the MCUboot lifecycle for one application and one
primary/secondary image pair. It owns image identity, explicit SMP UDP windows,
pending-image protection, exact-hash confirmation, and a five-minute trial
deadline measured from application boot uptime. MCUboot trailers own durable
state; there is no OTA settings record or new application thread.

The module calls Zephyr directly and does not include command-dispatch or HISPEC
headers. COO dispatch uses its C API to provide the shared `ota` MQTT/serial
command. Applications supply hardware preparation and service deadlines from
their existing main loop.

## Configuration

Enable `CONFIG_COO_OTA=y` together with the existing MCUboot/MCUmgr facilities:

```ini
CONFIG_BOOTLOADER_MCUBOOT=y
CONFIG_MCUMGR=y
CONFIG_MCUMGR_GRP_IMG=y
CONFIG_MCUBOOT_IMG_MANAGER=y
CONFIG_MCUMGR_TRANSPORT_UDP=y
CONFIG_MCUMGR_TRANSPORT_UDP_IPV4=y
CONFIG_MCUMGR_TRANSPORT_UDP_AUTOMATIC_INIT=n
CONFIG_MCUMGR_GRP_IMG_TOO_LARGE_SYSBUILD=y
CONFIG_MCUMGR_GRP_IMG_DIRECT_UPLOAD=n
CONFIG_MCUMGR_GRP_IMG_ALLOW_CONFIRM_NON_ACTIVE_IMAGE_SECONDARY=n
CONFIG_MCUMGR_GRP_IMG_ALLOW_CONFIRM_NON_ACTIVE_IMAGE_ANY=n
CONFIG_MCUMGR_GRP_IMG_ALLOW_CONFIRM_NON_ACTIVE_SLOT=n
CONFIG_MCUMGR_GRP_IMG_ALLOW_ERASE_PENDING=n
CONFIG_COO_OTA=y
```

COO OTA selects the MCUmgr receive/done notification hooks. Sysbuild must provide
the MCUboot footer reservation used to calculate the secondary-slot upload
limit. Image management must use one updatable image and secondary-slot uploads.
Keep partition geometry, signing policy, bootloader GPIOs, watchdog
configuration, buffers, and thread priorities in the consuming application.
The [HISPEC configuration and validation](../ota.md) describe this board's
30-second bootloader and 15-second application watchdog budgets.

With COO MQTT/dispatch enabled, `ota` is a builtin command. An application must
not also register its own `ota` row. Without `CONFIG_COO_OTA`, that builtin and
the lifecycle code are omitted; ordinary dispatch/reboot remain available.

## Application responsibilities

Initialize once after device setup and before starting workers or command
ingress. Pass a readiness callback returning zero if hardware is **already**
ready, or a negative errno otherwise. It runs under the OTA mutex on each
window open/renewal. It may inspect synchronized state, but must not shut down
hardware or reenter the OTA API. Pass `NULL, NULL` if there is no prerequisite.

HISPEC's callback requires TIB bank mode `override_off` and unpowered state;
other profiles have no bank prerequisite. `-EPERM` produces the shared command
error `OTA entry requirements not met`; the callback can log the hardware reason.

The following integration outline uses application-owned hardware helpers:

```c
int rc = coo_ota_init(check_hardware_ready, NULL);
if (rc != 0) {
    return rc;
}
struct coo_ota_status boot;
coo_ota_get_status(&boot);
if (!boot.confirmed) {
    rc = prepare_trial_hardware();
    if (rc != 0) {
        return rc;
    }
}
/* Only now start workers and command ingress. */
```

HISPEC trial preparation sets the existing bank mode to `override_off`. The
application must retain that hardware restriction when confirmation succeeds;
the operator restores operation explicitly.

In the existing main loop, request the existing delayed reboot exactly once
on expiry and stop watchdog feeding while waiting for reset:

```c
enum coo_ota_poll_result result = coo_ota_poll();
if (result == COO_OTA_REBOOT_REQUESTED) {
    int rc = coo_cmd_runtime_schedule_reboot(cmd_runtime, false);
    if (rc != 0 && rc != -EALREADY) {
        LOG_ERR("Rollback reboot scheduling failed (%d); watchdog will reset", rc);
    }
}
if (result == COO_OTA_CONTINUE) {
    wdt_feed(wdt, wdt_channel);
}
```

`COO_OTA_AWAITING_REBOOT` keeps feeds withheld on later passes. The OTA module
never feeds/configures the watchdog or performs a reboot itself.

## Command admission

The existing command table owns permissions. For example, a bank endpoint can
allow inspection during OTA while rejecting every mode write:

```c
{
    .key = "laser/bankpower",
    .query_handler = laserbank_power,
    .effect_handler = laserbank_power,
    .class_policy = COO_CMD_CLASS_SUFFIX_OR_PAYLOAD_EFFECT,
    .key_prefix_match = true,
    .allowed_payload_keys = "mode",
    .ota_query_allowed = true,
    .ota_effect_allowed = false,
},
```

Both fields default to false. Builtin commands carry the same metadata. COO
dispatch checks the matched query/effect permission after dequeue, before the
handler or last-command persistence. Serial guard and board support remain
independent checks. See the [HISPEC allowlist](../commands.md#commands-while-ota-is-active).

Window and confirmation requests must be serialized with guarded commands;
COO's single executor provides this ordering. Main polls deadlines and SMP's
processing workqueue handles transfer callbacks. The mutex protects those
transitions, and admission releases it before invoking hardware handlers.
Closing/expiry can leave an admitted SMP operation finishing; restrictions
remain until it finishes and throughout any pending image or unconfirmed trial.

The protocol remains a 600-second default window (1..1800 seconds), image-group
SMP operations only, and confirmation of the exact running image hash. Firmware
confirmation does not require powered hardware or a Modbus response.

```{eval-rst}
.. doxygenfile:: include/coo_commons/ota.h
   :project: hispec_tib
```
