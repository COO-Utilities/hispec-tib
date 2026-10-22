# Firmware Architecture

## Authority

This page describes runtime ownership in `app`, local libraries and drivers.
`hardware.md` remains authoritative for wiring and physical assumptions.
`commands.md` remains authoritative for intended command/API behavior. When code
and intended docs disagree, the mismatch is listed in
[the owner-review ledger](human_review_required.md).

## System Overview

The firmware is a Zephyr C application with static queues, fixed command
handlers, board-profile selection, and direct domain modules for each hardware
area. The application avoids dynamic allocation and user-programmable runtime
scheduling.

Runtime ownership is:

- `main.c`: boot order, watchdog, serial polling, network/MQTT loop, outbound
  queue draining and optional OTA deadline polling.
- `command.c`: app command queues, static command spec table, support
  predicates, custom request classification callbacks, reboot-prepare hook, and
  app/cross-domain command handlers. It supplies `app_output_emit()` for domain
  warning/telemetry producers, using the common emitter and existing queues.
- `devices.c`: board strap detection, profile setup, shared device objects.
- `mems_switching.c`: MEMS switch state, route matching, timer-driven router thread.
- `attenuator.c`: DAC channel setup/read/write, coefficient application, and
  transmission uncertainty from stored physical-model residual RMS.
- `attenuator_command.c`: command-schema validation for `atten` value and
  coefficient requests.
- `laser_calibration.c`: reference-anchored current acquisition, measured-overlap
  normalization, bounded linear/quadratic fitting, effective noise estimates and
  accepted-core-gated application. Laser scratch uses 1496 bytes within the existing
  3584-byte shared union; its 128 raw windows occupy 3072 bytes. The host can review
  the exact 16-byte support mask in 432-byte metadata before explicitly applying.
- `calibration_fit.c`: the allocation-free two-parameter double solver shared
  by attenuator and laser fits. `calibration_storage.c` owns their shared static
  dataset, scratch, data mutex and best-effort telemetry buffer.
- `maiman.c`: raw/scaled Modbus register transactions.
- `lasers.c`: laser-bank power sequencing, driver verification, laser settings
  validation, output estimates, auto-off delayable work, and higher-level Maiman
  helper APIs.
- `laser_command.c`: command-schema validation and response shaping for laser
  and laser-bank requests.
- `photodiode.c`: ADC sampling, user/fixed moving windows, dark snapshots, and
  noise warnings and diagnostic windows.
- `photodiode_command.c`: command-schema validation for `pd` and `pd/settings`.
- `throughput_command.c`: validates `measure_throughput`, prepares ownership, applies
  launch/return MEMS routes and resolves route-loss settings before starting.
- `throughput_monitor.c`: measure-throughput streaming, route-loss application,
  and optional autolevel control.
- `housekeeping.c`: slow ambient-temperature sampling delayable work,
  relay-box power state, and relay-output on-time tracking.
- `laserbank_tempcontrol.c`: laser-bank heater policy delayable work and
  related status.
- `sntp_sync.c`: low-priority SNTP sync thread and status.
- `app_settings.c`: direct-NVS app configuration and calibration.
- `app_identity.c`: selected board-profile MQTT device ID.

Project-local wrappers under `lib/coo_commons` are intentionally small:

- `network.c`: Ethernet IPv4 bootstrap and runtime reconfiguration with
  DHCP/static/fallback profile selection.
- `mqtt_client.c`: MQTT 5 broker parsing, broker resolution, connect/process,
  and subscription helpers around Zephyr MQTT.
- `command_dispatch.c`: fixed-buffer MQTT/serial command request, built-in
  help/serialguard/reboot and optional OTA commands, exact-match spec lookup with explicitly enabled suffix families, default
  request classification/execution, serial guard policy, persisted
  lastcommand, topic formatting, response metadata, warning JSON, serial
  payload normalization, and serial response printing helpers. Built-ins are
  executor-owned and run before the app command table. Table entries also own
  query/effect permissions while OTA is active. Serial `help` prints at ingress.
- `ota.c`: optional MCUboot image identity, upload admission, confirmation and
  trial deadlines. MCUboot trailers own durable state; no app NVS record is added.
- `json_utils.c`: constrained keyed JSON extraction and fixed-buffer append
  helpers used by command code.

The application otherwise uses Zephyr GPIO, I2C, ADC, DAC, UART, Modbus,
settings/NVS, watchdog, console, networking, MQTT, SNTP, sensor, and 1-Wire
APIs directly.

## Boot Sequence

The standalone bench build enters the application directly. The explicit OTA
sysbuild runs MCUboot first, with HSI clocks, a PB2 bank-off GPIO hog and a
30-second watchdog. It validates/selects the application before chainload. See
[firmware updates](ota.md) for provisioning, flash layout and recovery.

1. Read/clear reset cause and initialize the watchdog.
2. Load app defaults/NVS; detect exactly one strap; apply the board-change reset policy.
3. Restore UTC from a valid RTC and increment the boot counter.
4. Configure the command runtime and fixed queues (no command execution yet).
5. Check required devices. In OTA builds, initialize image state and select bank
   `override_off` on an unconfirmed TIB trial before any workers or ingress.
6. Build the MEMS router, initialize attenuators and load coefficients.
7. Start the app blocking queue; bind laser auto-off and housekeeping before their consumers.
8. Start TIB PD/throughput/heater work when required devices are ready, then the command executor.
9. Start SNTP and network policy, initialize MQTT and its command subscription.
10. Poll serial and OTA deadlines, feed the watchdog, reconnect MQTT, drain output
    and process socket events in main. Trial expiry schedules delayed reboot and
    withholds watchdog feeds; standalone builds always feed during each main pass.

## Board Profiles

Board identity comes from exactly one active strap:

- `tib`: 8 MEMS switches, TIB routes, six logical attenuator channels, laser
  bank GPIO, housekeeping-owned DS2408 relay outputs, Modbus, ADS1115
  photodiodes.
- `cal_yj`: 7 MEMS switches, CAL routes, one logical CAL attenuator channel (TIB's H channel).
- `cal_hk`: same firmware profile shape as CAL YJ.
- `as`: 6 MEMS switches, AS routes, no attenuators.
- `unknown`: no board-specific hardware setup is allowed.

## Command Model

MQTT subscribes to `cmd/<device>/req/#`. The suffix after that prefix is the
command key. The shared command-dispatch helper copies the MQTT request into a
fixed request object. An MQTT response topic property is used when present;
otherwise the default response topic is `cmd/<device>/resp/<key>`.
The `<device>` namespace is selected from the detected board strap: `tib` maps
to `hsfib-tib`, `cal_hk` to `hsfib-rcal`, `cal_yj` to `hsfib-bcal`, and `as` to
`hsfib-as`. The same name is used for telemetry under `dt/<device>/...` and as
the MQTT client ID.

Requests are classified by schema and topic shape, not by user-visible method
verbs. Command dispatch owns the default query/effect rules and uses the app
command spec table for special cases such as always-query commands,
suffix-triggered actions, and custom payload classifiers.

Empty or no-payload requests are queries except for dispatcher built-ins such as
`reboot`, app actions such as `laser/clearfaults`, and laser-bank topic suffixes such as
`laser/bankpower/override_on`. Non-empty payloads normally mean an effect
request, but documented query payloads remain queries: `status`, laser status
endpoints, laser name-only queries, laser tune/settings readbacks, and
`mems/route/loss` payloads that contain only `route`.

Serial commands use the same classification after line normalization by the
shared command-dispatch helper:
`<key>` becomes an empty JSON payload, raw JSON is copied, `key=value` tokens
are wrapped as JSON fields, and command-table-selected human shorthands are
translated into the same payload shapes as MQTT. The old payload `msg_type`
convention is not part of current ingress classification.

The command executor runs exactly one request at a time from `inbound_queue`.
Command dispatch handles library built-ins first, then the static spec-table
executor for app commands. OTA admission is checked after dequeue, before the
handler or lastcommand write, so it also covers requests queued before entry.
Handlers may block on I/O, sleep, enqueue warnings, update settings, and return
one response. Pure queries are not recorded as `lastcommand`; supported
effect-capable requests are recorded before handler execution in
command-dispatch-owned runtime state and NVS storage.
App support predicates reject unsupported command families before they reach
laser, photodiode, throughput, or laser-bank command handlers on other board
profiles. Serial help marks those entries as unsupported instead of encoding
board names in the common command-dispatch library.

`reboot` is always a dispatcher built-in when COO MQTT is enabled.
After the response window, command dispatch calls the app reboot-prepare hook
and then `sys_reboot(SYS_REBOOT_COLD)`. The delayed reboot runs on the existing
app blocking workqueue, so laser STOP can wait for Modbus completion on the system
workqueue. System priority is 2, command 6 and app blocking 7.

Command dispatch can persist one lastcommand record through Zephyr NVS when the
app supplies a mounted `struct nvs_fs *` and numeric NVS ID. The record stores a
fixed header plus the full `struct coo_cmd_request`, requiring
`sizeof(struct coo_cmd_request) + COO_CMD_LASTCOMMAND_NVS_OVERHEAD` bytes. The
payload portion is sized by `CONFIG_COO_CMD_PAYLOAD_SIZE`, which is constrained
to fit within `CONFIG_COO_MQTT_PAYLOAD_SIZE` but can be tuned independently
because command queues store complete request and response objects.

## Serial polling

Main consumes at most 64 console characters per pass without waiting. The
128-byte UART receive buffer and 127-character command line are bounded;
main assembles CR/LF-delimited lines and handles backspace. Serial `help` prints
directly; other requests enter the shared executor. Serial polling, MQTT ingress
and output drain share main-owned scratch buffers.

Network waits can delay parsing and serial-guard activation. UART interrupts
still receive bytes, but excess input is dropped if the receive buffer fills;
line-overflow handling cannot detect characters already lost there. There is
no separate serial thread. Avoid pasting long commands during network stalls.

## Network And MQTT Runtime

Network capability presence follows Zephyr Kconfig: DHCP uses
`CONFIG_NET_DHCPV4`, DNS uses `CONFIG_DNS_RESOLVER`, and SNTP uses
`CONFIG_SNTP`. App code should not add duplicate capability flags.
Zephyr `NET_CONFIG_AUTO_INIT` is disabled; `network.c` owns the app-level
DHCP/static/fallback decision from `main()` so network setup does not delay
hardware boot-state work before application code starts. Zephyr connection
manager remains enabled for interface bring-up, reconnect attempts, and L4
connected/disconnected events after `main()` starts.

The MQTT wrapper accepts one publish callback plus caller-owned user data. This
app registers the command runtime with `coo_cmd_runtime_mqtt_callback()` so MQTT
ingress can enter command dispatch without an app-specific forwarding shim.
The MQTT ingress payload buffer is static in the MQTT wrapper, and command
runtime request/response buffers are owned by the runtime object rather than the
main or executor thread stacks.

IPv4 configuration precedence is:

1. Runtime settings from the `ip` command.
2. Compile-time static IPv4 defaults from `CONFIG_NET_CONFIG_MY_IPV4_*`.
3. The same compile-time static defaults as the last-resort service fallback.

At apply time the helper starts DHCP-first policy when configured, returns
without blocking boot, and schedules a bounded static fallback only after
Zephyr reports the interface operationally up. That fallback uses Zephyr's
`NET_ADDR_OVERRIDABLE` address type so a later DHCP lease can replace the
service address in the single IPv4 slot. Static-first mode uses
`NET_ADDR_MANUAL` and stops DHCP. The `ip` command applies network-affecting
changes at runtime through `network_reconfigure()`; it no longer requires reboot
for ordinary IPv4 profile changes.

DNS and NTP addresses are profile/settings data. Unsupported DNS/NTP fields are
reported by command code. Manual DNS is applied to Zephyr's resolver when DNS is
compiled in and a nonzero DNS server is configured; DHCP DNS is used when DHCP
provides it and `prefer_dhcpdns` is true.

MQTT broker hostnames are accepted only when they resolve before settings are
updated. Numeric IPv4 brokers do not require DNS. After a broker setting change,
the main loop disconnects and tries the new broker once; if that connection
fails, firmware restores the prior broker setting and emits a best-effort
`mqtt_broker_revert` warning.

Native STM32 LPTIM4, clocked by 32768 Hz LSE, supplies kernel uptime at 4096 ticks/s
(244 microseconds/tick). The timer runs periodically (`CONFIG_TICKLESS_KERNEL=n`)
with a fixed eight-LSE-cycle period. This avoids the workload-dependent uptime
gain measured with tickless reloads: idle tests gained 37–42 ms/minute, while
periodic tests gained about 4.4 ms/minute under idle/streaming and command loads.
The exact faulty tickless transition remains unproven. The measured cost of
periodic mode is about 0.9 percentage points of additional timer-ISR CPU time
(excluding exception entry/exit), at 4096 interrupts/s. The RTC uses LSE too.
PM is enabled solely for the native driver dependency; CPU power states are removed from the application tree so
there is no automatic STOP. LSI remains enabled for the independent watchdog.
PD timing diagnostics use native 64-bit kernel uptime ticks, with 244 microsecond
granularity. The LPTIM driver's 32-bit cycle API scales its counter to the CPU
frequency and has a discontinuity when its underlying accumulated count wraps;
using kernel ticks avoids that diagnostic artifact without another counter. Millisecond timer rounding is below one
tick, apart from Zephyr's ordinary minimum timeout alignment.

A valid RTC calendar restores boot UTC. One serialized setter handles both manual
UTC and full-fraction SNTP. It reads the RTC and writes whole calendar seconds only
when the RTC is unreadable or differs from the requested UTC by more than 1.25 s
in either direction. This guard accommodates the setter's <1 s quantization plus
250 ms margin and avoids stopping a healthy calendar under the RTC driver's
spinlock on every sync. RTC write failure remains a warning; running UTC still
updates. The RTC is a warm-boot seed, not the source of running UTC. SNTP adds Zephyr's
estimated one-way response delay to the server transmit timestamp before setting
UTC. No fractional retention or NVS last-time fallback is used; the old NVS ID
remains reserved. Success syncs
repeat at 60 s; failures retry at 30 s, with an initial one-second attempt and
immediate attempts when network changes wake the SNTP thread.

SNTP is independent of manual `time` commands. Manual time setting updates
Zephyr's realtime clock; it does not mark SNTP state as manual. If SNTP is configured
and later succeeds, it will update the clock again, and failures remain visible
through `time`, `ip`, and status paths that report SNTP state.
SNTP network waits run in a low-priority SNTP thread, not on the system
workqueue and not in the command, MQTT, MEMS, or ADC timing paths.

Output uses eight best-effort slots and four reserved reply slots. Producers use
`K_NO_WAIT`; dropped telemetry/replies and publish failures have separate runtime
atomic counters. Main drains replies first with a bounded budget. A native
nonblocking eventfd wakes the existing MQTT socket poll when output is enqueued;
queued work uses a zero poll timeout. There is no extra network thread. Keepalive
is checked even with continuous socket/output events, and a failed reply publish
aborts the connection before the normal reconnect path rather than spinning main.
Command completion centrally adds MQTT5 `ack_utc_ms` before enqueueing; at present
epoch lengths this property costs 28 bytes (35 for a 20-digit uint64), plus at
most one property-length varint byte. It does not alter response JSON.

Command payload capacity is 1152 bytes, MQTT buffers 1280. Throughput binary is
227 bytes; even a pessimistic JSON frame with 24 worst-length double strings and
uint64 timestamps is below 1152 bytes. Eventfd capacity 2 and fd capacity 10 allow
the existing socket service plus output wakeup, MQTT/DNS/SNTP and OTA SMP resources.
The lastcommand record includes the request buffer, so its storage size depends
on the configured payload size. App settings own calibration/dark schema reset.

## Hardware Control

Commands do not directly publish. For example, MEMS commands update
router-owned switch state that is applied by the timer-driven MEMS router
thread.
Photodiode sampling does not publish directly. Throughput monitoring owns
photodiode stream publication through the best-effort output queue. Warning publication is
non-blocking and best-effort. Runtime warnings use one shared scratch response;
if that buffer or the outbound queue is busy, the warning is logged locally and
dropped rather than blocking a timing-sensitive caller.

Maiman owns client initialization and blocking Modbus RTU transactions. USART2
uses its native hardware FIFO with the interrupt-driven driver. Modbus parsing
runs on the system workqueue; the dedicated Modbus workqueue is disabled.
Device setup initializes the client. Only a transaction timeout disables it
through the public Modbus API, synchronizing parser cancellation before releasing
the laser I/O mutex. After successful disable, reinitialization is deferred until
the next register request; neither initialization nor timeout cleanup sends a
probe or replays a command. The Maiman write path
holds the owner's I/O serialization through a yielding 350 ms quiet interval after
LD START/STOP and EEPROM SAVE/RESET attempts, including acknowledgement failures.

Preemptible application priorities (smaller numbers run first):

| Priority | Owner | Purpose |
|---|---|---|
| 0 | MEMS router / ADS1x1x driver acquisition | Splitter pulse cleanup / ADC conversion service |
| 1 | Photodiode | Independent 50 ms acquisition cadence |
| 2 | System workqueue | Native Modbus RX parsing/completion, short system work |
| 3 | Throughput | Streaming, autolevel, calibration acquisition |
| 4 | Main/MQTT | Serial polling, network service and queued outbound publication |
| 6 | Command executor | Serialized MQTT and serial commands |
| 7 | App blocking workqueue | Housekeeping, heater, auto-off and reboot preparation |
| 8 | MCUmgr SMP (OTA build only) | Progressive flash upload and image state/test requests |
| 13 | Logging | Deferred log output |
| 14 | Calibration fitting / SNTP | Numerical work / blocking time-server requests |

Networking's cooperative priorities and system-workqueue yielding are unchanged.
The Modbus caller sleeps while awaiting its 75 ms reply; that deadline and UART
cleanup policy are unchanged. PD never takes the laser I/O mutex. A synchronous
laser operation in the shared throughput thread can delay both streams while PD
continues sampling; the semaphore coalesces those acquisitions, without replay.
The 4096 Hz LPTIM timebase does not change the 240 MHz core clock.

Relay and temperature 1-Wire waveforms use UART12 and UART9, respectively,
through Zephyr's stock serial 1-Wire driver. UART polling does not mask interrupts
for the waveform duration, although the STM32 driver briefly locks interrupts
around transmit register access. Maiman does not acquire either 1-Wire bus lock.
Housekeeping is the sole DS18B20 caller; runtime relay calls are serialized by
housekeeping's I/O lock and the DS2408 driver's initialized mutex. DS18B20
conversion sleeps on the blocking queue for up to 750 ms, outside the bus lock.

The unmodified Zephyr revision pinned in `west.yml` leaves the serial 1-Wire
driver's native bus mutex zero-initialized without initializing its wait queue.
The current ownership avoids contention on that mutex: each bus has one configured
slave, no other raw application bus caller, and no shell access. The pinned
kernel's uncontended lock/unlock path tolerates this state, but it is not a
properly initialized mutex and must not be relied on for contended access.
Revisit this limitation before changing bus ownership or the Zephyr revision.
Stock reset timing and the accepted DS2408 timing exception at 3.3 V are documented
in [hardware.md](hardware.md#laser-control-and-switched-power).

Numerical attenuator fitting reuses the throughput thread after stopping the
calibration-owned laser and releasing PD auto-off inhibition. It releases the
calibration mutex and temporarily uses `K_LOWEST_APPLICATION_THREAD_PRIO` (14 in
this build), below Modbus RX (2), commands (6), housekeeping (7), and logging (13).
It restores priority 3 with no mutex held
before finalization. Other throughput work still waits for fitting to finish.
Start/stop commands set a cancellation flag and wait on a completion semaphore
without holding the calibration mutex. Numerical loops check cancellation;
the command cannot clear/reuse the single static dataset until fitting returns.
Only an accepted new start clears records, references, bridges, and fit results.
Stop and error cleanup preserve them. No second record buffer is allocated.
Either an attenuator or laser start replaces the shared raw dataset, after
cancel/join and confirmed shutdown of the previous owner. Applied settings survive.
Laser fitting uses the same priority/cancellation discipline; it retains PD
auto-off inhibition through optional lower-boundary refinement and the emission-off
recabling pause, releasing it at completion, error or stop. The laser status uses an
explicit `fitting` state. Data and scratch unions stay at 7680 and 3584 bytes;
128 laser records use 3072 bytes, with bounded metadata and <=1 KiB numerical
scratch. The throughput stack remains 4096 bytes. Details are in
[laser_calibration.md](laser_calibration.md).

The Cortex-M33 single-precision FPU is enabled with Zephyr shared FP context.
PD window storage/mean/RMS and runtime FVOA dB/slope/local uncertainty use float.
FVOA inversion prepares open normalization, leakage floor and the calibrated
correction endpoint once, then makes 24 value-only voltage bisections. Its erfc
form preserves the shutter tail in float. Public dB results promote to double
before summation and linear transmission/error conversion. Throughput, single-PD
dark subtraction and final uncertainty propagation remain double. Calibration
keeps double coefficients, Jacobians, weights, solves and residual metrics; only
its final grid, retained-point and continuation-join validation uses the deployed
float evaluator. No persistent calibration or telemetry layout changes.

Offline Cortex-M33 instruction replay (SDK 14.3, 2026-10-02) gives 7,023,042
to 26,731 instructions for a corrected 30 dB inverse, and 280,255 to 4,470
for the full-model versus runtime dB/slope evaluation at 2600 mV. Complete PD
window recomputations, including double uncertainty/power arithmetic, fall from
15,375 to 7,409 instructions (10 samples) and 28,909 to 8,621 (40 samples).
These are instruction counts, not measured target cycles or wall times. Merely
enabling the FPU changes the retained double forward evaluation by only 0.23%.
Shared FP context adds 64 bytes of saved S16–S31 registers per thread object,
plus a 72-byte hardware frame on an active FP thread's stack. Stack sizes are
unchanged; DWT timings and runtime margins require the flashed board.

Attenuator calibration publishes a coherent command-status snapshot under a separate short
mutex after start/stop/tick updates and before fitting. Status and active checks
read that snapshot without waiting for acquisition I/O. The snapshot stays
`running` throughout fitting and exposes each completed physical fit coherently.
Record downloads take the calibration mutex only for their bounded copies and
remain available during fitting. Canceled work cannot install coefficients;
cancellation and installation serialize at finalization.
Laser identity and applied configuration are retained for the bank-power interval;
configuration, driver-started state, and nonzero-current accounting are separate. Laser-bank power
commands can sleep while waiting for the Maiman modules to boot or for a
fault-clear power-cycle interval. Background laser-bank temperature control,
laser auto-off, and ambient-temperature refresh run on the app blocking
workqueue rather than Zephyr's system workqueue because Zephyr Modbus client RX
completion uses the system workqueue.
`main()` starts the static app queue before binding its consumers. Housekeeping
and laser auto-off are bound before command execution and throughput monitoring
start; their startup functions assert the non-null queue precondition. Heater
work is bound only when required TIB devices are ready.

Laser thermistor intent is an ordinary PCB-owned channel setting in %/°C at
25°C. `lasers.c` converts it to/from internal Maiman B, normalizes integer-B
rounding and programs it during existing profile preparation or explicit
driver-backed settings updates. Status reads do not program profiles; MCU startup
does not add controller writes. `app_settings.c` persists the coefficient in a
separate per-channel NVS record without changing existing calibration records.
Coefficient I/O uses internal LD NTC register 0x007F. External NTC 0x008A is
unused. Compiled defaults use vendor values: −4.4 %/°C for FLC and B=3900 K
for Aero. Accepted app NVS overrides retain their existing precedence.

## Design Constraints

- Static memory and bounded queues are preferred.
- Domain modules own hardware sequencing; command handlers own command schema.
- Timing-sensitive paths should not perform MQTT publish calls.
- Warnings and periodic telemetry are best-effort. Watchdog boot telemetry is
  queued as non-best-effort so it is retried until MQTT is available.
- Broad schedulers, plugin systems, and dynamic command registries are out of
  scope for current firmware.

Laser, attenuator, and relay hardware operations serialize with module I/O mutexes.
Their state mutexes protect only short copies/confirmed updates: no I/O,
sleep, persistence, or telemetry runs under a state mutex. Readers see the last
confirmed state during a pending operation. Numerical laser estimates and
operational health are separate; transient read errors do not invalidate setpoints.
Lock order is I/O then state; state readers never acquire the I/O mutex.
Attenuator estimates use confirmed DAC voltages; explicit `attenuator_get`
queries still read both registers. Settings/status copies own their mutable
properties rather than borrowing pointers into another thread's state.

## Feature ownership

Acquisition timing and equations belong with their consumers:

- [Photodiode](photodiode_notes.md): ADC conversion, dark windows, RMS and warning eligibility.
- [Throughput](throughput.md): source alignment, streaming, autolevel and shutdown ownership.
- [MEMS](mems_splitting.md): latching pulses, route intent and split quantization.
- [Attenuator](attenuator.md): confirmed DAC state, allocation and physical model.
- [Calibration](attenuator_calibration.md) and [laser calibration](laser_calibration.md): acquisition, retained data, acceptance and persistence.
- [Laser operation](laser_operation.md): Modbus behavior, emission/TEC policy and health.

## Queue and delayed-work contracts

```{mermaid}
flowchart LR
  MQTT[MQTT ingress in main] -->|K_NO_WAIT| In[Inbound queue: 2 requests]
  Serial[Serial polling in main] -->|K_NO_WAIT| In
  In --> Exec[Single executor]
  Exec --> Replies[Reserved reply queue: 4 responses]
  Workers[PD / throughput / owners] --> Best[Best-effort queue: 8 responses]
  Replies --> Drain[Main: bounded reply-first drain]
  Best --> Drain
  Drain --> Pub[MQTT publish or serial print]
  Replies -. eventfd wake .-> Drain
  Best -. eventfd wake .-> Drain
```

The queue elements are `struct coo_cmd_request` and `struct coo_cmd_response`.
Ingress never waits for space; full queues return busy/error paths. Producers
use `K_NO_WAIT`; reserved replies reduce competition with telemetry but do not
guarantee delivery. Required MQTT output is requeued when offline/failed and can
still be lost if requeueing fails. Best-effort output is discarded instead.
Watchdog boot telemetry uses required output. Drop/publish counters distinguish
these cases. Serial `help` prints directly, bypassing both queues.

The MEMS and PD timer callbacks only give semaphores; hardware I/O runs in their
threads. Dispatcher serial-guard expiration is short system work. Dispatcher
reboot work runs on the app blocking queue, invokes the app prepare hook after
the response window, then cold-reboots. Later app commands are rejected while
reboot is pending. Auto-off, ambient/relay checks and heater work share that queue;
a slow DS18B20 conversion can delay other work on it. Never put a blocking
Modbus caller on the system queue that must parse its response.

DHCP fallback is system work; main owns MQTT reconnect and SNTP owns its separate
blocking thread. OTA uses Zephyr's SMP processing workqueue, not an application
upload thread. Receive/done hooks retain admission restrictions through a closing
in-flight operation and refresh pending image state before releasing them.
The [OTA C contract](api/ota.md) defines lifecycle locking and main-loop duties.
