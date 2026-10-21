# Laser operation and wavelength tuning

`lasers.c` owns physical sequencing, confirmed setpoints, health and shutdown;
`maiman.c` owns the blocking Modbus register transport. Command handlers validate
wire fields and call these owners. This page explains estimates and behavior;
[commands](commands.md#laser) defines payloads, [calibration](laser_calibration.md)
defines inference from measurements, and [C reference](api/maiman_laser.md)
provides callable contracts.

## Power and photon estimates

For current $I$ in mA, threshold $I_t$ and efficiency $\eta$ in mW/mA:

```{math}
P[\mathrm{mW}]=\max[0,\eta(I-I_t)],\qquad
\sigma_P=\sqrt{(fP)^2+C^2},\qquad
\Phi[\mathrm{s}^{-1}]=\frac{10^{-3}P\lambda}{hc}.
```

Here $f$ is `fractional_noise`, $C$ is `constant_noise_mw`, and $\lambda$ is in
metres. Photon error uses the same power-to-flux scale. For example, 1 mW at
1000 nm is approximately $5.0341\times10^{15}$ photons/s. These are cached,
current-based estimates, not measured emitted power. Operational health is checked
separately; computing a finite estimate does not prove a laser is emitting.

## Wavelength tuning

The local linear model is

```{math}
\lambda(I,T)=\lambda_0+k_T(T-T_0)+k_I(I-I_{\rm nom}),
```

where wavelength is nm, $k_T$ is nm/K, $k_I$ is nm/mA, and $T_0$, $I_{\rm nom}$
are the configured operating temperature and nominal current. At zero current
the reported nominal wavelength is a convention; no optical line is implied.

For requested brightness $b=\texttt{desired\_power\_percent}/100$, the tuner
first chooses $I_d=I_t+b(I_{\rm nom}-I_t)$, bounded to the allowed current.
At $T_0$, compute $\Delta\lambda=\lambda_{\rm requested}-\lambda(I_d,T_0)$.
Temperature is used first when enabled:

```{math}
T_* = \operatorname{clip}\left(T_0+\frac{\Delta\lambda}{k_T},T_{\min},T_{\max}\right).
```

If temperature tuning is disabled, $T_*=T_0$. Remaining wavelength error can
be corrected by current when enabled:

```{math}
\Delta I=\operatorname{clip}\left(
 \frac{\Delta\lambda-k_T(T_*-T_0)}{k_I},-\delta I_{\max},\delta I_{\max}\right),
\qquad
\delta I_{\max}=\frac{\texttt{maximum\_power\_shift\_percent}}{100}
 (I_{\rm nom}-I_t).
```

The final current is bounded, rounded to the 0.1 mA register grid, and used for
both application and reporting. Residuals are requested minus estimated
wavelength and $P(I_d)-P(I_*)$; clamping flags explain unreachable requests.
There is no wavemeter feedback or claim of thermal/optical settling.

The `laser/tune` command stores a relative wavelength request; the next positive
manual laser-level operation applies it. Throughput deliberately skips stored
tuning and retains the live TEC target. Tuning/settings interactions with stream
shutdown ownership remain in the [review ledger](human_review_required.md#commandapi-mismatches).

## Emission updates and failures

A started laser accepts current changes, including zero, with one Modbus write.
Current, percent, power, and tuning paths all round to the 0.1 mA register grid
within their current bounds. Cached estimates and tune results use the applied
value. Healthy repeated register values skip the write without refreshing
communication health; an explicit auto-off request still rearms its deadline.
Startup/recovery and TEC-only tuning changes still perform the required I/O.
Requests that round to zero have the same accounting as explicit zero.
Zero current pauses emission-time accounting but preserves started state, tuning,
and the existing auto-off deadline. Explicit `laser stop=true`, auto-off, and
measurement-owned expiry use zero-current plus STOP. Throughput leaves TECs on;
explicit laser STOP and laser auto-off use `disable_tec_at_autooff`.

Identity is verified once per driver per bank-power interval. Configuration is
applied once and retained across STOP and communication errors; explicit settings
changes/reset and bank power cycling invalidate affected configuration. Operational
faults remain separate from known identity/configuration. An unsuccessful current
or STOP write is still reported; acknowledged zero current is retained even if STOP
fails. A confirmed repeated stop avoids redundant writes.

`hispec_laser_set_output_percent_autooff(..., apply_tune)` makes stored tuning
explicit: manual levels and calibration apply it; throughput skips it and keeps
the live TEC target. Preparation still installs the configured default target.
Profile programming uses writable TEC bounds 0x0072 (minimum) and 0x0071
(maximum), signed values scaled by 100. It expands the existing envelope before
moving the default target and narrowing bounds. Absolute limits are read-only.
A programming failure leaves preparation false and prevents a subsequent START
until preparation succeeds. Range-only settings changes stop emission and defer
programming until next preparation; combined driver-backed edits program now.

`ntc_coeff_pct_per_c` is PCB-owned resistance change in %/°C at 25°C. For the
thermistor approximation $R(T)=R_{25}\exp[B(1/T-1/T_{25})]$ with kelvin temperatures,

```{math}
\alpha_{25}=\frac{100}{R}\frac{dR}{dT}\bigg|_{T_{25}}
=-\frac{100B}{298.15^2}\quad[\%/{}^\circ\mathrm C],\qquad
B=\operatorname{round}\left(-\frac{\alpha_{25}298.15^2}{100}\right)\quad[\mathrm K].
```

For example, $B=3900$ K gives about −4.3873 %/°C. This conversion expresses the
configured coefficient; it is not a physical temperature calibration. Profile
preparation translates it to rounded integer Maiman B and writes the internal LD
NTC register 0x007F after the TEC current limit, before target/bounds/PID programming.
External NTC register 0x008A is unused in this system. Failed writes
leave preparation false. Explicit coefficient edits use the existing settings
stop/power/program flow. Engineering status reads controller configuration before
or after preparation; bank-off readbacks are null. Reads translate controller B
back to the same units. Compiled defaults use vendor values: −4.4 %/°C for FLC
and B=3900 K for Aero. MCU boot does not program this coefficient or save controller EEPROM.
App NVS owns optional persistence; see
[settings](settings.md) and [the thermal comparison](temp_test_result.md).

Raw service names `NTC_COEFFICIENT` and `NTC_B25_100_COEFFICIENT` select external
0x008A; `INTERNAL_LD_NTC_SENSOR` selects internal 0x007F. They retain their
literal register meanings. LD `current_cal_pct` uses 0x0088. TEC calibration
0x007E remains an engineering getter; the [thermal investigation](temp_test_result.md) supports retaining the
factory 100% value.

At 115200 baud, one current write requires about 1.65 ms wire time plus
1.00 ms configured RTU receive framing. These are transport estimates,
not measured end-to-end latency: controller turnaround, thread scheduling, I/O
contention, and optical response are additional. A cold bank also has its boot
wait. Ordinary current changes add no settling delay or optical polling. LD START/STOP and
explicit EEPROM SAVE/RESET wait 350 ms after the transaction attempt, including
a failed acknowledgement, before allowing another transaction. This implements the
approximately 300 ms busy interval described on page 22 of the repository SF8025
manual, with 50 ms margin. The response timeout remains 75 ms.

Relay and temperature 1-Wire transfers use separate UART peripherals without
masking interrupts for the waveform duration. The STM32 driver briefly locks
interrupts around transmit register access. Maiman does not acquire their bus
locks; its owner retains the existing I/O serialization through the response
and 350 ms busy interval.
USART2 uses the STM32 driver's native hardware FIFO and the stock interrupt-driven
Modbus implementation. Zephyr requires no local patches or build hooks.

Maiman owns one RTU client configuration (115200 baud, 8N1, 75 ms response wait),
initialized during device setup. Modbus parsing uses the system workqueue;
the dedicated Modbus workqueue is disabled.
After a Modbus read/write returns `-ETIMEDOUT`, it calls `modbus_disable()` before
releasing the existing laser I/O mutex and before any controller busy wait. The
public API disables RX/TX, stops the framing timer and synchronizes cancellation
of the single shared parser work item. Maiman returns failure and preserves
`-ETIMEDOUT` in the driver's `last_error`; it never automatically replays the
transaction. CRC, short-frame, other errno and positive Modbus exception results
retain the client without a lifecycle reset.

After successful modbus disable, the client stays disabled until the next requested
register operation, including a background poll, calls `modbus_init_client()`.
This configures the local UART, GPIO and transaction state; it sends nothing
and does not establish controller availability. Client RX remains disabled until
transmission completes, when the stock interrupt-driven path drains idle FIFO
bytes before enabling reception.
Client reinitialization does not power-cycle the bank, clear faults or write
controller properties. An init
failure sends no request, incurs no controller busy wait, and can be retried by
a later request. An unexpected disable failure is logged separately, preserves
the transaction timeout, and blocks further bus use until device initialization
at reboot. Initialization never updates successful-response timestamps or clears
the driver's sticky error state. Successful transactions advance response
timestamps; failed operations and elapsed time can worsen communication health.

Late on-wire replies have no transaction ID; timeout cleanup cancels software
work and does not resolve that protocol ambiguity. See
[hardware control ownership](architecture.md#hardware-control) and the
[pending hardware checks](human_review_required.md) for transport validation.

The laser owner keeps preparation and confirmed setpoints separately from
operational communication health. Failed control operations or confirmed
controller faults invalidate internal operational health without erasing confirmed configuration.
Public status readiness separately describes known blockers to the explicit start
endpoint; stopped TECs and recoverable fault history are not public blockers. Diagnostic failures warn and invalidate
only that observation; five seconds without a response while started (including
zero current) faults operation. Successful communication restores availability, but measurements
remain stopped and a control fault requires a successful control operation.
When both the cached command state and observed controller state are stopped,
the physical-interlock bit alone does not invalidate the acknowledged zero/STOP
state. Status still reports the raw interlock, readiness and blocked reason.
Identity mismatches, hard controller faults, and unexpected loss of operation or
TEC while started retain their fault behavior. A diagnostic read never clears
an existing control fault.
Failed shutdown preserves its emission/shutdown obligation. See
[communication flow](#communication-and-power-lifetime).

## Bench transaction timing diagnostics

Normal firmware uses Maiman and laser INFO levels, Modbus WARNING, and a shared
2048-byte deferred-log buffer. Transaction/quiet and preparation details use DEBUG.
These application logs work with unmodified Zephyr and use standard module log
levels, with no per-operation verbosity argument or separate trace switch.
Communication-health timestamps and busy waits are maintained even when logging is off.

For a diagnostic capture, build separately with these Kconfig overrides:

```sh
./.venv/bin/west build --board=nucleo_h563zi/stm32h563xx \
  --build-dir ./hispec-tib/app/build-maiman-log ./hispec-tib/app -- \
  -DCONFIG_MAIMAN_LOG_LEVEL_DBG=y -DCONFIG_LASERS_LOG_LEVEL_DBG=y
```

Optionally add `-DCONFIG_MODBUS_LOG_LEVEL_DBG=y` for Zephyr's stock Modbus debug
messages and packet dumps. This increases log volume and does not provide separate
frame-arrival or RX-worker timing probes. The separate `debug.conf` profiling
fragment overrides the buffer to 16 KiB and is not needed for this capture. The
2 KiB buffer is allocated once across modules; it does not guarantee lossless diagnostic bursts.

Capture the serial console continuously from before the first command through at
least one second after the final response. Use firmware monotonic timestamps for
analysis; terminal wall-clock timestamps include buffering. Also retain any dropped
log-message warnings: missing records leave gaps in the transaction history.

| Record | Interpretation |
|---|---|
| `MB seq=... node=... op=... reg=... value=... start_ms=... elapsed_ms=... gap_ms=... rc=...` | One read/write, including failures. Elapsed time ends when the Modbus API returns, before timeout cleanup or the quiet wait. Gap is since the preceding API completion; first gap is -1. |
| `MB quiet ... start_ms=... wait_ms=350` / `release_ms=...` | Busy operation and enforced release time. The next request must start at or after release. |
| `Laser ... prepare configuration_needed=...` / `applying configuration` | Separates ordinary restart from actual configuration writes. |

Use `start_ms + elapsed_ms` to locate API completion, including timeouts, and the
quiet-wait records to check the busy guard. Application elapsed times cannot
distinguish late device responses from delayed RX processing; missing transaction
records cannot establish that no frame arrived. Separating those causes requires
additional evidence, such as a UART capture or existing Zephyr tracing/debugger
facilities. RTU has no transaction ID: transaction `seq` is firmware bookkeeping,
not an on-wire identifier, and ambiguous late frames cannot be assigned to a
request with certainty.

Successful response timestamps remain the actual API completion time, never the end
of the quiet interval. Neither elapsed time nor the guard turns a timeout into a
successful acknowledgement. Use these records to assess the 75 ms ACK deadline
within the measurement limits above.

## Laser command failure evidence

Laser operation helpers accept optional bounded failure text (`error`, `error_len`);
pass NULL/0 to omit returned text. The laser owner formats existing controller
evidence; command handlers include it in MQTT errors. Nested failures retain their
detail through power restoration. Existing one-shot warnings remain best-effort.
Controller observations stay private. Error formatting uses existing observations; it adds no register reads.

Compact `output_state` combines acknowledged output operations and positive LD
observations. Its four values are documented in commands.md. It does not clear
the existing control-fault latch or gate throughput/dark acquisition. Internal
stop intent distinguishes an observed stop after a failed intentional shutdown
from an autonomous stop after established nonzero operation.

The one-second auto-off worker reads LD operation state as well as
TEC state during its eligible nonblocking checks. Foreground status, temperature
polls and background observations share confirmed-stop accounting. Each positive
component stop freezes its own accumulated on-time; failed component reads leave
that component's counter unchanged. This does not persist/reset the current runtime,
erase setpoints, release deadlines, restart output or add a post-START wait.

## Communication and power lifetime

Throughput fault stops also use the console/MQTT warning path, including when
the measurement detects an expired response deadline before background work runs.

Laser control state distinguishes unconfirmed bank startup, confirmed control,
and a control/controller fault. Bank power-on invalidates preparation and leaves
control unconfirmed, which permits passive throughput capture with no confirmed
emission. Preparation alone does not start the response-timeout check. Successful
laser startup publishes confirmed state and its response deadline together;
failed control enters the fault path. A successful STOP confirms stopped state,
while a zero-current write preserves unconfirmed or faulted state while powered.
Autolevel and calibration still require healthy, confirmed emission.

Laser auto-off work reads checked TEC and LD operation-state registers
of established started channels (including zero current) once per second, including
channels whose shutdown failed. A nonblocking bus attempt and a second eligibility
check prevent reads of inactive or concurrently stopped channels. A positive
unexpected stop latches the existing fault and emits its one-shot warning; no restart
is attempted. Foreground status and heater temperature observations use the same
component-stop accounting: only confirmed stopped components end their counters,
while failed reads retain accounting, setpoints, deadlines and shutdown ownership. It does not
depend on heater mode, setpoint changes, or the 20 Hz measurement loop. Heater
control retains its ten-second cadence. Housekeeping's existing ambient work
reads the DS2408 port once per second and publishes all three logical outputs.
These intervals include work execution/scheduling overhead and are not hard
real-time deadlines.

A successful driver response refreshes the owner's five-second communication
deadline. Local calculations, requested settings, and a busy bus do not count as
responses. Maiman retains the last response timestamp and last error within each
operation, so a partial read can report its error without hiding successful
responses. Numerical laser estimates use confirmed setpoints, initially zero
current with the configured default temperature before any command; they return
`-EINVAL` for invalid/uninitialized use, never an operational I/O error.

```{mermaid}
flowchart LR
  Ready[Responsive owner] -->|failed read| Transient[Warn; retain confirmed state]
  Transient -->|successful response| Ready
  Transient -->|five seconds without response| Fault[Owner communication fault]
  Ready -->|failed laser control or confirmed controller fault| Fault
  Fault --> Stop[Stop dependent acquisition; attempt owned laser shutdown]
  Fault -->|communication restored| Recovery[Report recovery; acquisition remains stopped]
```

Source faults stop consumers of that source; shared relay loss stops affected PD
measurements and calibration. Other independent measurements continue. An
unsuccessful laser shutdown preserves the identity for an explicit stop retry.
Recovery does not resume acquisition automatically. A successful DS2408 response
is accepted as evidence that power is available to its loads; logical relay
states determine which loads are enabled. Explicit power-off is always effective.

Relay I/O and state copies use separate mutexes, ordered I/O then state. The
throughput loop reads only confirmed state/health. Auto-off inhibition takes the
I/O lock, serializing against a queued worker's final deadline check and write.
Throughput owns inhibition while preparing routes and while running; calibration
owns it throughout acquisition. Completion, cancellation, and failed startup
release ownership. Releasing inhibition resumes the existing deadline, including
an already expired deadline. No ownership counter is needed.

Owner communication warnings use the existing console/MQTT warning emitter.
The first failure is immediate; repeated failures are limited to one per device
per five seconds. Fault and recovery transitions are immediate. They remain
available with verbose Modbus/GPIO logging disabled; console warning filtering
and best-effort MQTT queue capacity still apply. Raw `maiman` errors remain
console diagnostics, separate from owner messages and measurement `/dt/` traffic.

### Laser zero level versus shutdown

A manual `laser value=0` is a zero-current update; it preserves driver readiness and
any existing auto-off deadline. `laser stop=true` is explicit shutdown. Captures and
notebook cleanup must use explicit stop when they intend shutdown. Laser auto-off
and measurement-owned expiry retain shutdown ownership at zero current. Temporary
zero levels keep PD collection alive, with undefined throughput at zero source power.
The [transaction timing notes](#bench-transaction-timing-diagnostics)
describe application transaction and quiet-interval logs, and their measurement
limits, for the next bench capture.
