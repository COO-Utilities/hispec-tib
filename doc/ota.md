# OTA operation and validation

OTA updates the application through an explicit maintenance window on the
trusted observatory network. MCUboot checks SHA256 integrity; images are not
authenticated with a signing key. The bootloader itself is installed through
ST-Link and is not updated over the network. One application serves all board
profiles. MQTT/serial owns maintenance and confirmation; MCUmgr image commands
use direct IPv4 UDP port 1337 during an open window.

## Build and initial provisioning

From the workspace root:

```bash
./.venv/bin/west update
./.venv/bin/python -m pip install -r hispec-tib/tools/requirements.txt -r bootloader/mcuboot/scripts/requirements.txt
./.venv/bin/west build --sysbuild -b nucleo_h563zi/stm32h563xx -d hispec-tib/app/build-ota hispec-tib/app
```

The Zephyr manifest pins MCUboot and ZCBOR. `app/sysbuild.conf` selects hash-only
images and swap using offset. MCUboot's overlay explicitly selects the boot
partition, HSI clocks (the PCB does not use HSE), and the same PB2 bank-off GPIO
hog as the application. Partition addresses are unchanged:

| Area | Address | Size |
| --- | --- | --- |
| MCUboot | `0x08000000` | 64 KiB |
| Primary application | `0x08010000` | 960 KiB |
| Secondary application | `0x08100000` | 960 KiB |
| Settings/NVS | `0x081f0000` | 64 KiB |

Initial provisioning is a bench operation with experiments stopped and bank
power off. Using STM32CubeProgrammer/ST-Link, connect under reset; erase only
the boot and image areas (`0x08000000` through `0x081effff`), leaving the NVS
area untouched. This also clears any stale secondary image/trailer. Program
both of these artifacts before releasing reset:

- `app/build-ota/mcuboot/zephyr/zephyr.hex`
- `app/build-ota/app/zephyr/zephyr.signed.confirmed.hex`

The confirmed artifact is essential for the initial baseline. Do not use a
mass erase if calibration/settings must survive. After boot, query `ota` and
verify `confirmed:true`, `enabled:false`, and no pending image. Retain a known
working confirmed baseline and its hash for recovery. Sysbuild's application
flash runner selects the confirmed artifact automatically, but initial
secondary-slot clearing and preservation of NVS still require attention.

## Normal update

First stop calibration/monitoring and any other experiment. On TIB, explicitly
select `laser/bankpower mode=override_off` and verify `powered:false`. OTA entry
rejects a powered bank or another mode; it does not perform that shutdown.
The existing off mode prevents heater/control/settings paths from powering
the bank. COO dispatch enforces the [command allowlist](commands.md#commands-while-ota-is-active)
while OTA is active, before handlers run. All bank-mode writes are blocked,
including a repeated `override_off`; queries remain available. No Modbus
response is expected from unpowered laser drivers. The reusable lifecycle and
application integration are described in [the COO OTA API](api/ota.md).

Validate locally without connecting:

```bash
./.venv/bin/python hispec-tib/tools/hispec_ota.py update hispec-tib/app/build-ota/app/zephyr/zephyr.signed.bin --dry-run
```

Run an update, replacing the broker address as appropriate:

```bash
./.venv/bin/python hispec-tib/tools/hispec_ota.py update hispec-tib/app/build-ota/app/zephyr/zephyr.signed.bin --broker 192.168.1.5 --device hsfib-tib
```

The helper validates the unpadded binary with the pinned imgtool, checks fresh
MQTT identity/readiness/bank state and the generated upload limit, derives the
device IPv4 address, and opens a 600-second window. `--ip` optionally checks
that address against an expected value. CAL/AS device namespaces work the same
way, without TIB bank checks. Use `--duration-s` (30..1800) to change the host
upload budget; this does not change the trial deadline.

It then compares SMP's running hash with MQTT's running hash, uploads image 0's
secondary slot, verifies the reported secondary hash, marks it for a **test**
boot, closes the window, and requests the ordinary delayed reboot. Upload uses
at most three transport attempts and resumes using the transfer SHA/offset.
After reboot, fresh correlated MQTT replies must report a new boot count, the
exact expected image hash, a ready board, and bank off on TIB before the helper
confirms. A lost confirmation reply is recovered by querying current state.
The helper does not use firmware-version strings to identify an image.

Image-state responses report the stored hash TLV; they do not recompute the
flash contents. The host verifies its file before upload, and MCUboot performs
the full candidate-body hash check before running it. An interrupted/corrupted
upload cannot be confirmed by the normal workflow. The SMP client's optional
OS-parameters request receives an unsupported response because only the image
group is enabled; its fixed 1024-byte MTU remains in use.

On successful confirmation, **the operator must explicitly set bank mode back
to `auto`**. Confirmation and window close both leave `override_off` unchanged.
Ordinary later boots of confirmed firmware retain the existing `auto` default;
bank mode is not persisted by OTA. A rollback returns to already-confirmed
firmware and therefore follows that ordinary boot behavior.

The Python notebook API also provides `pcb.ota()`,
`pcb.set_ota_window(True, duration_s=600)`, `pcb.set_ota_window(False)`, and
`pcb.confirm_image(image_hash)`. See [the command contract](commands.md#ota).

## Failure and rollback

An upload window expires independently of the host. Closing it can leave an
already-admitted request finishing; command restrictions remain until it finishes,
and remains longer if a test image became pending. Pending images cannot be
overwritten or erased through SMP. A close does not cancel a pending test boot.
If a host fails after marking the candidate pending, inspect `ota` and issue
the ordinary `reboot` to complete the trial workflow.

A trial boots with UDP closed and TIB bank `override_off` before application
workers or commands start. It has **five minutes of application boot uptime**
to be confirmed through MQTT or serial with its exact running image hash. No
network connection, host timeout, or maintenance-window renewal extends this
deadline. At expiry, the main loop schedules the existing three-second delayed
reboot and stops feeding IWDG. Thus a stuck reboot work item also leads to reset.
The deadline and confirmation flag write share a mutex so they cannot race.
MCUboot reverts an unconfirmed test on reset. The old secondary image remains
protected throughout the trial.

`--skip-confirm` exercises this path: the helper waits for the exact trial but
deliberately does not confirm. It reports the remaining trial time and exits;
observe the later reset and return of the baseline hash separately. The default
host boot wait is 180 seconds (`--boot-timeout-s` changes it, up to 300). A host
timeout is a failure to verify success, not a claim that rollback already ran.

## Watchdog phases

| Phase | Nominal timeout | Feed owner |
| --- | --- | --- |
| MCUboot validation, swap, revert | 30 seconds | MCUboot watchdog checkpoints |
| Chainload and application initialization before `main()` | Remaining bootloader interval | Inherited running IWDG |
| Application startup, upload, ordinary operation, trial | 15 seconds | Application main loop |
| Expired trial | No further feeds | Delayed reboot, with IWDG as fallback |

These are intervals **between feeds**, not total upload/swap time limits.
MCUboot installs a valid timeout and starts IWDG; it feeds during long boot
operations. It jumps into the application without resetting the peripheral.
The application's fresh STM32 driver state permits `wdt_install_timeout()`;
`wdt_setup()` rewrites the running prescaler/reload to 15 seconds and reloads
the counter. No attempt is made to disable IWDG. The upstream 300-second
MCUboot default exceeds this MCU's IWDG range, so it is explicitly overridden.

Both 15 and 30 seconds use the pinned driver's nominal 32 kHz LSI calculation.
ST specifies LSI up to 33.6 kHz, giving approximately 14.29 and 28.57 seconds
at that extreme. Leave measured feed-gap margin below those bounds. See the
[STM32H563 datasheet, LSI characteristics](https://www.st.com/resource/en/datasheet/stm32h563zi.pdf).
Pre-main protection starts once MCUboot has enabled IWDG; it does not cover
earlier cold-boot clock initialization. A debugger halt is also not a valid
watchdog test: the application requests pause while halted. Inject a running
spin/hang or use temporary lab instrumentation instead.

SMP flash work runs at priority 8 instead of the upstream default 3, below main
(4), system work (5), commands (6), and app blocking work (7). Progressive erase
keeps uploads from erasing the entire slot in one operation. Thread ordering
does not eliminate internal-flash stalls; actual feed gaps require measurement.
See [MCUboot's swap and image format](https://docs.mcuboot.com/design.html).

## Maximum-size and negative fixtures

```bash
./.venv/bin/python hispec-tib/tools/hispec_ota.py fixtures
```

This reads the generated DTS and application/MCUboot configurations. The
current 983,040-byte slot reserves 16,384 bytes for offset swapping and trailer
space, allowing a **966,656-byte** unpadded image. The tool appends deterministic
non-`0xff` bytes to `zephyr.bin` **before** imgtool builds the header/hash. The
declared image body includes the extension, so every appended byte participates
in upload, flash, validation, swap, and rollback. Linked code, entry point,
initialized data and RAM footprint stay the same. `imgtool --pad` alone would
not create this test.

`app/build-ota/test-images/` contains:

- `maximum.bin`: valid, exactly the generated limit, runnable with unchanged code.
- `oversized.bin`: valid hash/container, one flash-write unit (16 bytes) over
  the generated upload limit; the host rejects it before connecting.
- `corrupt-tail.bin`: maximum-size image with its last body byte changed after
  hashing; local verification must fail.
- `manifest.json`: sizes and MCUboot hashes of the valid fixtures.

Run `maximum.bin` through normal update and `--skip-confirm` workflows. Start
each case from a different confirmed baseline; uploading an already-running
hash is deliberately a no-op, so restore the baseline between these cases. For
device-side negative tests, bypass only the normal host validator using the
installed typed SMP client in a temporary lab script. With a manually opened
window, send `ImageUploadWrite(off=0, image=0, len=len(oversized),
data=oversized[:128])`; require an image-too-large error before an upload is
accepted. To test bootloader validation, upload `corrupt-tail.bin` using
`SMPClient.upload`, request `ImageStatesWrite(hash=<maximum image hash>,
confirm=False)`, close, and reboot. MCUboot must retain/recover the baseline;
never confirm the damaged candidate. Close the window after each negative test.

## Bench acceptance

Build and offline checks do not establish target flash timing or power-failure
behavior. Before relying on OTA:

1. Provision a confirmed baseline, record its hash and selected calibration/IP
   settings, and verify UDP is closed at startup on each board profile.
2. Complete normal and maximum-size updates with exact-hash confirmation.
   Observe PB2/bank power through boot, swap, trial and confirmation. Exercise
   the command allowlist over both MQTT and serial, including requests queued
   before OTA entry. Bank queries succeed; every payload/suffix mode write is
   rejected, including off. After confirmation, require operator `auto`.
3. Repeat maximum-size trials without confirmation and with broker/network
   loss; verify the deadline, reset, baseline hash and NVS retention. Reject a
   wrong confirmation hash and an attempt at/after expiry.
4. Measure feed gaps during progressive erase, image validation, both swap
   directions, application startup, and full upload. Verify IWDG registers
   change from 30 to 15 seconds at handoff and test running hangs before main,
   in the main loop, and at bootloader checkpoints without debugger freeze.
5. Interrupt uploads and cut power at several erase/write/trailer stages of
   maximum-size swap and revert. After recovery, require either the exact trial
   (still off/unconfirmed) or the known confirmed baseline, never a partial image.
6. Perform the oversized and corrupt-body tests separately at host and device
   layers. Verify a pending/trial image cannot be overwritten or made permanent
   by SMP, and preserve calibration, IP, and other app settings throughout.

No hardware flash, watchdog timing run, or power-cut test is implied by a
successful software build. Record actual bench results in the owner ledger.
