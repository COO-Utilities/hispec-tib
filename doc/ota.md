# Firmware updates and OTA

Firmware can be programmed through USB/ST-Link at the bench or updated over
Ethernet using MCUboot. MCUboot is the small program that runs before the
application: it checks image integrity, starts a trial image, and returns to the
previous confirmed image if the trial is not confirmed. The bootloader itself
is installed through ST-Link and is not updated over the network.

**The existing standalone USB/CLion build remains the default during validation.**
An explicit `--sysbuild` build enables MCUboot and OTA. These are temporary build
choices: after USB and network workflows pass [bench acceptance](#bench-acceptance-and-retiring-the-standalone-build),
MCUboot will become the single build flow. USB and network transfer will both
remain useful ways to install that firmware.

## Choose the operation

| Task | Build / artifact | Connection |
|---|---|---|
| Continue ordinary bench development | Standalone application in `app/build` | Existing USB/ST-Link and CLion workflow |
| Bring up a fresh board for MCUboot/OTA | MCUboot plus a confirmed application from `app/build-ota` | USB/ST-Link; one-time full erase and provisioning |
| Edit and reflash a provisioned board locally | Confirmed application from `app/build-ota` | USB/ST-Link; no trial or rollback test required |
| Update a provisioned board remotely | Unpadded `zephyr.signed.bin` from `app/build-ota` | MQTT control plus direct IPv4 UDP upload |

The standalone application starts at `0x08000000`; MCUboot occupies that address
on a provisioned board. Flashing a standalone image there replaces MCUboot.
Returning that board to network updates requires provisioning again. Keep the
build directories distinct and select the artifact for the operation being done.

This firmware uses **SHA256 integrity checks without signature authentication**,
for use on the trusted instrument network. The generated filename `signed` is
Zephyr/imgtool terminology; it does not mean these images have a signing-key
signature. The helper uses the MCUboot image hash, not the Git version string,
to identify exactly which firmware was uploaded and booted.

## Install and check the tools

Commands below run from the **parent Zephyr workspace**, containing `hispec-tib/`,
`zephyr/`, `bootloader/`, and `.venv/`. Use the existing workspace environment.
The OTA helper needs this workspace and a matching sysbuild directory; installing
only the notebook requirements in an isolated tools checkout is insufficient.

| Tool | Used for | Install / source |
|---|---|---|
| Python 3.12 or newer | west, host helper and image validation; this workspace was checked with Python 3.13 | [Python downloads](https://www.python.org/downloads/); create/use the workspace `.venv` |
| Git, CMake, Ninja and Zephyr host dependencies | Fetch pinned modules and build firmware | [Zephyr setup by operating system](https://docs.zephyrproject.org/latest/develop/getting_started/index.html) |
| west | Workspace modules, build and runner selection | Install with `python -m pip install west` in `.venv`; [west installation](https://docs.zephyrproject.org/latest/develop/west/install.html) |
| Zephyr SDK | ARM compiler and GDB; builds here use SDK 1.0.1 | [SDK installation](https://docs.zephyrproject.org/latest/develop/toolchains/zephyr_sdk.html), [SDK releases](https://github.com/zephyrproject-rtos/sdk-ng/releases) |
| STM32CubeProgrammer | Initial provisioning and later USB programming; includes `STM32_Programmer_CLI` | [ST download](https://www.st.com/en/development-tools/stm32cubeprog.html), [command-line reference](https://dev.st.com/stm32cube-docs/prog/2.23.0/en/docs/markup/CubeProg_Command_Lines.html) |
| STM32CubeCLT | ST-Link GDB server used by the existing CLion debug configuration; also includes the programmer CLI | [ST CubeCLT download](https://www.st.com/en/development-tools/stm32cubeclt.html) |
| MCUboot and imgtool | Bootloader, image packaging and offline hash verification | Fetched at the revision selected by `west.yml`; [MCUboot with Zephyr](https://docs.mcuboot.com/readme-zephyr.html) |
| Python `smpclient==7.3.0` | UDP image transfer inside the repository helper | Installed by `tools/requirements.txt`; [package](https://pypi.org/project/smpclient/), [client documentation](https://intercreate.github.io/smpclient/latest/) |

On macOS, download and install STM32CubeProgrammer from the ST link above,
choosing the package for the Mac's processor. For the default installation,
paste this into the terminal used for the commands below:

```bash
ota_bin="/Applications/STMicroelectronics/STM32Cube"
ota_bin="$ota_bin/STM32CubeProgrammer"
ota_bin="$ota_bin/STM32CubeProgrammer.app"
ota_bin="$ota_bin/Contents/Resources/bin"
export PATH="$ota_bin:$PATH"
```

The pinned `stlink_gdbserver` runner discovers CubeCLT installations under `/opt/ST/` on
macOS, `/opt/st/` on Linux, and `C:\ST\` on Windows. Installing only the
standalone programmer does not supply this GDB server.

For an initialized workspace:

```bash
./.venv/bin/west update
./.venv/bin/python -m pip install \
  -r hispec-tib/tools/requirements.txt \
  -r bootloader/mcuboot/scripts/requirements.txt
```

`west update` fetches the pinned MCUboot and ZCBOR modules. The helper imports
imgtool directly from that MCUboot checkout, so do not substitute an unrelated
PyPI imgtool version. Python `smpclient` supplies the transport; **no separate
Go installation or MCUmgr command-line client is needed**.

Check host setup without connecting to a board:

```bash
./.venv/bin/python --version
./.venv/bin/west --version
./.venv/bin/python hispec-tib/tools/hispec_ota.py --help
STM32_Programmer_CLI --help
```

For a new workspace, follow the repository README's setup and the linked Zephyr
host/SDK instructions first. A tool that fails to start must be repaired before
using it for programming; its installation alone does not verify USB access.

## Build and select the image

### Current standalone bench build

The existing command and CLion profiles remain valid:

```bash
./.venv/bin/west build \
  --board=nucleo_h563zi/stm32h563xx \
  --build-dir hispec-tib/app/build hispec-tib/app
```

Use the existing standalone USB workflow with this build. The timing profile
continues to add `-- -DEXTRA_CONF_FILE=debug.conf` in its separate
`app/build-timing` directory. These builds omit the `ota` command and MCUboot.

(ota-build)=
### Explicit MCUboot/OTA build

```bash
./.venv/bin/west build --sysbuild \
  --board=nucleo_h563zi/stm32h563xx \
  --build-dir hispec-tib/app/build-ota hispec-tib/app
```

Sysbuild loads `app/sysbuild.conf`, the app-only `app/sysbuild/app.conf`, and the
MCUboot configuration/overlay. It packages the app for offset swapping and reserves
the required image/trailer space. For a stale directory, repeat with `-p always`;
never reuse a standalone directory for sysbuild. Avoid a workspace-wide
`west config build.sysbuild True` during the transition, because it changes the
meaning of ordinary bench builds.

One application serves TIB, CAL YJ, CAL HK and AS; the board straps select its
profile. The generated artifacts have different purposes:

| Relative to `hispec-tib/app/build-ota/` | Purpose |
|---|---|
| `mcuboot/zephyr/zephyr.hex` | Bootloader at `0x08000000`; initial provisioning or deliberate recovery |
| `app/zephyr/zephyr.signed.confirmed.hex` | Application container with confirmation trailer; USB provisioning/reflash |
| `app/zephyr/zephyr.signed.bin` | Unpadded application container; network OTA candidate |
| `app/zephyr/zephyr.elf` | Application symbols for debugging the exact matching build |
| `app/zephyr/zephyr.bin` | Raw build output used internally by packaging/fixtures; not an OTA candidate |

The helper rejects padded/preconfirmed images, malformed containers, bad hashes,
and files above the generated upload limit. Keep the confirmed baseline image
and its matching ELF before testing another image.

## USB provisioning and local development

### First-time provisioning on macOS

Use this once for fresh bring-up. **It erases all existing firmware and saved
configuration**, installs MCUboot and a confirmed application, then boots with
the defaults compiled into that application.

1. Connect one Nucleo board to the Mac with a USB data cable at **CN1, the
   ST-LINK USB-C connector**. Stop any CLion debug session and close the
   STM32CubeProgrammer GUI so the command-line tool can use ST-Link.
2. Complete the [tool setup](#install-and-check-the-tools), then run the
   [MCUboot/OTA build command](#ota-build). Wait for the build
   to succeed. Keep the terminal in the parent Zephyr workspace.
3. Paste this entire block into that terminal:

   ```bash
   ota_build="hispec-tib/app/build-ota"
   ota_boot="$ota_build/mcuboot/zephyr/zephyr.hex"
   ota_app="$ota_build/app/zephyr/zephyr.signed.confirmed.hex"

   if test -f "$ota_boot" && test -f "$ota_app"; then
     STM32_Programmer_CLI -c port=SWD mode=UR -e all &&
     STM32_Programmer_CLI -c port=SWD mode=UR \
       -w "$ota_boot" -v &&
     STM32_Programmer_CLI -c port=SWD mode=UR \
       -w "$ota_app" -v &&
     STM32_Programmer_CLI -c port=SWD mode=UR -rst
   else
     echo "Build the MCUboot/OTA images before provisioning."
   fi
   ```

   The block checks that both files exist before erasing. It connects under
   reset, erases the device, writes and verifies each image, then resets to run
   the application. Each command runs only if the preceding command succeeded.
   Both downloads must report successful verification. If a command fails,
   fix the reported error and rerun the block.
4. Find the board's serial console:

   ```bash
   ls /dev/cu.usbmodem*
   ```

   Open it with macOS's built-in `screen`, replacing the example port below
   with the path printed by `ls`:

   ```bash
   screen -fn /dev/cu.usbmodemXXXX 115200
   ```

   This uses 115200 baud with flow control off. Press Return, then type:

   ```text
   ota
   ```

5. Check the reply for `confirmed:true`, `enabled:false`, `active:false`, and
   `pending:false`. These mean the installed application is confirmed and no
   update is in progress. Record its `image_hash` with the build artifacts.
   To leave `screen`, press **Ctrl-A**, then **K**, then **Y**.

After bring-up, use the USB reflash or network update procedure below. Both
preserve saved settings and calibration. A deliberate settings-schema change
resets configuration to compiled defaults and must be documented with that
firmware release; reconfiguration is then the user's responsibility. See
[settings persistence](settings.md).

### Reflash a provisioned board through USB

Once MCUboot and a clean confirmed baseline are installed, normal local edits
can be built and flashed directly as a confirmed application. Stop experiments
first. Start with no pending test image; allow an unfinished OTA trial to confirm
or revert before reflashing.

```bash
./.venv/bin/west flash \
  --build-dir hispec-tib/app/build-ota \
  --domain app --runner stm32cubeprogrammer
```

The generated app runner selects `zephyr.signed.confirmed.hex`. `--domain app`
leaves the bootloader alone. Do not add `--erase`, which requests mass erase.
USB installation goes directly to confirmed firmware; it does not need to
exercise the network trial or rollback path. A later edit/reflash repairs a
broken local application.

For debugging, flash the packaged image first and attach with its matching ELF:

```bash
./.venv/bin/west attach --no-rebuild \
  --build-dir hispec-tib/app/build-ota \
  --domain app --runner stlink_gdbserver --port-number 3333
```

`--no-rebuild` keeps the symbols matched to the image just programmed. In a
separate CLion OTA configuration, use the app domain's ELF and attach without
an automatic raw-ELF download; the existing standalone configurations need no
change. A raw ELF download does not create MCUboot's header/hash/trailer and can
invalidate an otherwise packaged image. Confirm this flash/attach/reset workflow
on the bench before replacing the default CLion flow.

### Flash layout

The Nucleo STM32H563 layout is shared by the application and MCUboot. This is
implementation reference; the programming commands select the image addresses.

| Area | Inclusive addresses | Size |
|---|---|---|
| MCUboot | `0x08000000`–`0x0800ffff` | 64 KiB |
| Primary application | `0x08010000`–`0x080fffff` | 960 KiB |
| Secondary application | `0x08100000`–`0x081effff` | 960 KiB |
| Settings/NVS | `0x081f0000`–`0x081fffff` | 64 KiB |

```{mermaid}
flowchart TD
  Reset[Reset] --> Boot[MCUboot: 64 KiB at 0x08000000]
  Boot --> Primary[Primary application: 960 KiB at 0x08010000]
  Secondary[Secondary candidate or rollback image: 960 KiB at 0x08100000] <-->|offset swap / revert| Primary
  Primary --> NVS[Settings and calibration: 64 KiB at 0x081f0000]
```

MCUboot's overlay uses HSI clocks and the same PB2 bank-off GPIO hog as the app.
Its 30-second watchdog stays running across chainload; the application takes
it over with a 15-second timeout. Detailed ownership is in
[architecture](architecture.md#boot-sequence) and [the C integration contract](api/ota.md).

## Network update with the helper

### Prepare the target

The host needs access to the MQTT broker (normally TCP 1883) **and direct IPv4
UDP port 1337 on the board**. The upload does not travel through the broker.
UDP listens only during an explicit maintenance window; no DHCP lease or open
MQTT connection opens that window by itself.

Stop calibration, throughput monitoring and any other experiment before entry.
On TIB, select `laser/bankpower mode=override_off` and query it until
`mode:override_off` and `powered:false` are reported. For example, from serial:

```text
laser/bankpower mode=override_off
laser/bankpower
ota
```

The helper checks this prerequisite but never changes bank mode for you. CAL/AS
have no TIB bank prerequisite; their experiments still need to be stopped.
Serial activity can activate serial guard, which rejects MQTT effects including
OTA entry/confirmation. Let it expire or use `serialguard off` before starting
the MQTT-driven update. See the [command permissions](commands.md#commands-while-ota-is-active).

### Validate, upload and confirm

Validate the candidate locally first; this opens no network connection:

```bash
./.venv/bin/python hispec-tib/tools/hispec_ota.py update \
  hispec-tib/app/build-ota/app/zephyr/zephyr.signed.bin --dry-run
```

Then update, replacing `broker.example` and the device namespace:

```bash
./.venv/bin/python hispec-tib/tools/hispec_ota.py update \
  hispec-tib/app/build-ota/app/zephyr/zephyr.signed.bin \
  --broker broker.example --device hsfib-tib
```

Use `hsfib-bcal` for CAL YJ, `hsfib-rcal` for CAL HK, and `hsfib-as` for AS.
The helper obtains the board's IPv4 address from a fresh MQTT reply. Optional
`--ip` checks that address against your expectation; it does not override it.

```{mermaid}
sequenceDiagram
  participant H as Host helper
  participant A as Running application
  participant B as MCUboot
  H->>H: Check image<br/>and size limit
  H->>A: MQTT identity, readiness,<br/>image and bank queries
  H->>A: MQTT open window
  H->>A: UDP verify running hash,<br/>upload secondary image
  H->>A: UDP verify candidate hash,<br/>request test boot
  H->>A: MQTT close window,<br/>request delayed reboot
  A->>B: Reset
  B->>B: Validate body hash,<br/>swap candidate into primary
  B->>A: Start unconfirmed trial<br/>UDP closed, TIB bank off
  H->>A: Fresh MQTT boot count,<br/>exact hash and readiness
  alt Candidate verified before deadline
    H->>A: MQTT confirm exact hash
    Note over H,A: Operator explicitly<br/>restores TIB bank mode
  else Candidate remains unconfirmed
    A->>B: Trial deadline: stop feeds,<br/>schedule reset
    B->>A: Revert to confirmed baseline
  end
```

The helper requires a confirmed, inactive baseline and matching host/device
upload limits. It compares the SMP running hash against MQTT identity before
uploading, verifies the secondary image's reported hash, and requests a **test**
boot rather than permanent installation. Upload transport failures have at most
three attempts, using transfer SHA/offset recovery. After reboot, it requires a
new boot count, the exact candidate hash, board readiness, and TIB bank off before
confirming. A lost confirmation acknowledgment is recovered by querying state.

Progress reports image size/hash, upload percentages, and finally
`Confirmed <hash>`. If that exact image is already running and confirmed, it
reports a no-op. These checks establish identity and reported readiness, not an
optical or electrical acceptance test of the new application.

**After successful network confirmation, explicitly restore TIB bank mode to
`auto` when ready to resume work.** Closing the window and confirming the image
leave `override_off` unchanged. Later ordinary boots of confirmed firmware,
including a rollback to the confirmed baseline, use the ordinary `auto` default.
OTA does not persist bank mode.

### Helper options and notebook access

| Option | Meaning |
|---|---|
| `--build-dir PATH` | Matching sysbuild configuration; defaults to `hispec-tib/app/build-ota` in the workspace |
| `--broker`, `--broker-port` | MQTT broker and port; port defaults to 1883, broker required for a real update |
| `--device` | MQTT device identity; defaults to `hsfib-tib` |
| `--ip` | Optional expected board IPv4 address |
| `--duration-s` | Upload-window budget, 30–1800 s, default 600; does not extend a trial |
| `--boot-timeout-s` | Host wait for the new boot/confirmation, 1–300 s, default 180 |
| `--dry-run` | Validate the file against the build locally, then exit without a connection |
| `--skip-confirm` | Lab trial: verify the candidate boot, deliberately leave it unconfirmed, then exit |

`update` exits with status 1 and `OTA failed: ...` on the handled validation,
transport or verification failures. Status 0 also covers a dry-run, an
already-running image, or a deliberately unconfirmed `--skip-confirm` trial;
read the reported outcome. `fixtures` is the separate offline image-generation
subcommand described below.

The repository's [Python convenience helper](python_helpers.md) exposes
`pcb.ota()`, `pcb.set_ota_window(True, duration_s=600)`,
`pcb.set_ota_window(False)`, and `pcb.confirm_image(image_hash)`.
These wrap individual MQTT commands; `hispec_ota.py` owns the complete update
sequence. Neither is the official external Python API. The
[control specification](commands.md#ota) owns field definitions and serial syntax.

## Failure, deadlines and recovery

The upload window and trial have separate clocks. Closing/expiry stops new UDP
admission but does not cancel an admitted operation or an already pending test
boot. Command restrictions remain while an operation finishes, a candidate is
pending, or a trial is unconfirmed. A pending/trial image cannot be overwritten
or made permanent through SMP.

A trial has **five minutes of application boot uptime** to be confirmed with its
exact running hash. Network loss, host timeout and maintenance-window renewal
cannot extend it. At expiry, main requests the existing three-second delayed
reboot and stops feeding IWDG. MCUboot reverts an unconfirmed trial on reset.
The confirmation flag write and expiry decision share the OTA mutex.

| Observation | Interpretation and next action |
|---|---|
| `ota` is unavailable | Check that this is the explicit OTA build and that MCUboot was provisioned |
| Entry rejected | Query bank state, pending/trial state and serial guard; do not assume the helper powered anything off |
| UDP upload fails while MQTT works | Check direct host-to-board routing/firewall access on UDP 1337 and window state |
| Close succeeded but `active` remains true | An admitted operation, pending candidate or unconfirmed trial still owns restrictions |
| Helper failed after requesting a test boot | Query `ota`; a pending candidate remains pending. Ordinary `reboot` starts its trial |
| Host times out after reboot | Success was not verified. Inspect the running hash/confirmation state; timeout does not prove rollback has already completed |
| Wrong image hash or board failure after reboot | Helper refuses confirmation. Leave the trial unconfirmed and inspect the later baseline boot |
| Board cannot recover over the network | Use ST-Link for a [USB application reflash](#reflash-a-provisioned-board-through-usb). If full reinitialization is needed, [first-time provisioning](#first-time-provisioning-on-macos) erases configuration and restores compiled defaults |

Image-state replies report the stored hash TLV, not a fresh hash of flash.
The helper verifies the local file and MCUboot hashes the candidate body before
executing it. The upload SHA used for transfer recovery is also distinct from
MCUboot's header/body hash. Only image-group SMP operations are enabled; an
unsupported optional OS-parameters request from the client is expected, and its
fixed 1024-byte transport MTU remains in use.

### Watchdog ownership

| Phase | Nominal interval between feeds | Owner |
|---|---|---|
| MCUboot validation, swap, revert | 30 s | MCUboot checkpoints |
| Chainload and initialization before `main()` | Remaining bootloader interval | Inherited running IWDG |
| Application startup, upload, ordinary operation and trial | 15 s | Main loop |
| Expired trial | No further feeds | Delayed reboot, with IWDG as fallback |

These are not total upload/swap limits. The application reprograms the inherited
running watchdog; it does not disable it. The pinned driver uses nominal 32 kHz
LSI. At ST's specified 33.6 kHz upper frequency, the nominal intervals become
approximately 28.57 and 14.29 seconds; measured feed gaps need margin below them.
See the [STM32H563 LSI specifications](https://www.st.com/resource/en/datasheet/stm32h563zi.pdf).

SMP processing runs at priority 8, below system/Modbus RX (2), main (4), commands
(6), and app blocking work (7). Progressive erase bounds individual erase work,
but internal-flash stalls still need measurement. A debugger halt is not a
watchdog validation: the application requests watchdog pause while halted.
Pre-main protection begins when MCUboot starts IWDG, after its early clock setup.

## Bench acceptance and retiring the standalone build

Software builds and offline checks do not establish flash timing, bank-power
behavior or power-failure recovery. Record actual results in the
[owner-review ledger](human_review_required.md), using a baseline and candidate
with compatible settings schemas:

1. Provision and cold-boot a confirmed baseline, record its hash and settings,
   and verify UDP is closed on each board profile.
2. Repeat USB confirmed-image reflash, matching-ELF attach/debug and cold reset.
   Verify the normal edit/build/reflash loop is usable; USB need not test rollback.
3. Complete normal and maximum-size network updates. Observe PB2 through boot,
   swap and trial. Check MQTT/serial permissions, including requests queued before
   entry, all bank-mode writes, and operator-only restoration after confirmation.
4. Leave trials unconfirmed, including broker/network loss. Verify the deadline,
   reset, baseline hash and settings retention. Reject wrong/late confirmation.
5. Measure watchdog feed gaps through startup, upload, validation, swap and
   revert, including the 30-to-15-second handoff. Exercise running hangs without
   debugger freeze, interrupted uploads, and power cuts at erase/write/trailer stages.
6. Reject oversized and corrupt candidates; verify pending/trial protection.
   Recovery must produce the exact unconfirmed candidate or the confirmed
   baseline, never execute a partial image.

After acceptance is recorded, switch the standard build and CLion workflow to
MCUboot, remove the standalone build path and its instructions, and keep one
canonical artifact table. Continue to support USB confirmed-image installation
and network trial installation from that one build.

### Maximum-size and negative fixtures

```bash
./.venv/bin/python hispec-tib/tools/hispec_ota.py fixtures
```

The helper reads the generated DTS and app/MCUboot configuration. The current
983,040-byte slot reserves 16,384 bytes for offset swapping/trailer space, leaving
**966,656 bytes** for an unpadded image. It extends `zephyr.bin` with deterministic
non-erased bytes *before* constructing the hash. The declared body includes those
bytes, so they participate in upload, validation, swap and rollback without
changing linked code or RAM use. Merely padding a finished image would not test this.

| File under `app/build-ota/test-images/` | Expected result |
|---|---|
| `maximum.bin` | Valid runnable image exactly at the generated limit |
| `oversized.bin` | Valid hash/container, one 16-byte flash-write unit too large; host rejects before connecting |
| `corrupt-tail.bin` | Last body byte changed after hashing; offline verification rejects it |
| `manifest.json` | Fixture sizes and MCUboot hashes |

`fixtures --build-dir PATH --output PATH` selects another matching build/output
directory. Use `maximum.bin` in both normal update and `--skip-confirm` cases,
restoring a different confirmed baseline between them: an already-running hash
is intentionally a no-op. `--skip-confirm` verifies the trial and reports remaining
time; observe the subsequent reset and baseline hash separately.

Device-side rejection needs a temporary lab script that bypasses only the host
validator. With a manually opened window, send
`ImageUploadWrite(off=0, image=0, len=len(oversized), data=oversized[:128])` and
require an image-too-large response. Separately upload `corrupt-tail.bin` with
`SMPClient.upload`, request `ImageStatesWrite(hash=<maximum image hash>, confirm=False)`,
close and reboot. MCUboot must retain/recover the baseline; do not confirm the
damaged candidate. Close the window after each negative test. These are bench
acceptance procedures, not steps in an ordinary update.
