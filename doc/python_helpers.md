# Python command and lab helpers

`tools/hispec_fibpcb.py` is the repository's convenience tool for engineering
bring-up, notebook experiments and firmware testing. It wraps MQTT commands and
telemetry and includes collectors, typed records and analysis helpers. **It is not
the official Python API**, which is maintained outside this repository. Firmware
field definitions remain in the [control and telemetry specification](commands.md).

**The PCB owns command validation.** The helper sends encodable arguments to
firmware, including invalid values supplied deliberately to test rejection. It
does not maintain another set of device limits, allowed names or argument rules.

## Setup and connection

Use Python 3.11 or newer. From the repository directory:

```sh
python3 -m venv .venv
source .venv/bin/activate
python -m pip install -r tools/requirements.txt
cd tools
python -m notebook
```

The parent Zephyr workspace's existing `.venv` can also install these requirements;
the clone-local environment above does not install the firmware toolchain.
`numpy` and `paho-mqtt` support the client; the requirements also include SciPy,
pandas, Matplotlib, notebook and widget support for the included lab tools.
Importing the module does not connect or operate hardware.
Notebook setup cells locate the helper from the workspace, repository, `tools/`
or archived notebook directory. Restart the kernel after updating the helper so
imports and previously defined notebook functions use the same revision. The
scope notebooks additionally need `SCPI-Instrument-Control==7.3.0`.

```python
import hispec_fibpcb as hspcb

with hspcb.HispecFibPcb("broker.example", device="hsfib-tib") as pcb:
    print(pcb.help())
    print(pcb.help_options())
    print(pcb.status())
```

Use the actual broker hostname and formal device name (`hsfib-tib`, `hsfib-bcal`,
`hsfib-rcal`, or `hsfib-as`). Construction normally connects lazily; `connect=True`
or the context manager connects immediately. Commands block awaiting responses
with the instance `timeout_s` (default five seconds). Paho reconnects and restores
subscriptions; lost in-flight commands are not replayed. A timeout does not prove
that a hardware operation failed to execute.
Default MQTT client IDs and 16-byte request correlation values are unique across
notebooks. Replies must match both correlation data and the expected topic.

`close()` stops registered collectors' measurements, then disconnects even when
shutdown errors occur; such failures are raised as an `ExceptionGroup`.
Use `close(stop_measurements=False)` for a passive listener: it drains and
detaches local collectors and disconnects without sending any board commands.
Context-manager exit retains the default measurement-stopping behavior.
Uncollected/manual laser activity is not a blanket responsibility of connection
cleanup. Use explicit `laser(..., stop=True)` when the experiment owns a manual
source. `laser(..., value=0)` or `laser(..., value_ma=0)` only requests zero current and preserves readiness
and any existing deadline.

## Predictable command convention

Selectors alone read; supplying a setting writes. `None` means omitted, and
zero/`False` are real supplied values within the field's domain. Action endpoints
such as `reboot()` and `laser_clearfaults()` are explicit exceptions. `status()`
flags select extra query content. Methods return dataclasses/tuples matching wire
semantics, or `CommandOk` for data-less success; updates do not automatically
query again. Read a method's return annotation when an endpoint has distinct
query/update responses (`ip()` can also return `PartialSupport`).

Laser `value` and attenuator `value` are **fractions**: `0.25` is 25%, not 0.25%.
Laser `value_ma` instead requests engineering current from zero through the
configured `max_current_ma`; it bypasses the optical inverse, automatic current
bounds and stored tune request. Supply either `value` or `value_ma`; firmware
rejects a request containing both.
Attenuator `_db` is positive power loss, `_mv` is pre-amplifier DAC drive.
`mems_route_loss(..., loss=0.5)` is a lost fraction (transmission 0.5);
`loss="3.0103 dB"` requests the same loss. These conventions come from the wire
specification, not a separate host-unit system.

```python
pcb.laser("1028y")                       # Query
pcb.laser("1028y", value=0.25)           # Set 25% modeled power
pcb.laser("1028y", value_ma=100.0)       # Engineering current in mA
pcb.laser("1028y", value=0)              # Zero current, not STOP
pcb.laser("1028y", stop=True)            # Explicit shutdown
pcb.atten("1028y")                       # Query
pcb.atten("1028y", value_db=40)          # Set total attenuation
pcb.ip(try_dhcp_first=False)              # False is an update
pcb.laser_tune("1028y", tune_nm=0)       # Zero resets stored tune request
```

Firmware checks command types, ranges, names, required fields and combinations.
For example, an incomplete laser curve or a negative PD gain reaches the PCB;
the helper reports its rejection as `HispecFibPCBError`, preserving `.topic`
and `.response`. It does not turn strings into numbers, truncate durations or
convert arbitrary values to booleans to make a request pass validation.

Python checks only its own requirements: JSON encoding, positional argument
shapes needed to assign field names, local calculation inputs, collector setup
and response decoding. NumPy numbers and arrays encode as their JSON equivalents;
nonfinite numbers and objects without a JSON representation fail locally.
`HispecFibError` covers these local failures and transport failures.
A reported partial board error can leave successful hardware writes applied;
query confirmed state before deciding the next action.

`atten_coeff()` accepts an `AttenuatorPhysicalCoeff`, a mapping with wire field
names, or a flat sequence for each physical drive. The sequence has **five or
eleven values**, in this order:

```text
fvoa_50pct_mv, slope_inv_fvoa_mv, max_atten_db, gain, max_calibrated_db
[, c0, c1, c2, c3, c4, c5]
```

The six optional terms form `correction_coeff`. Use a mapping or dataclass to
include `rms_db`; it has no positional slot. Named fields are easier to review
than a long sequence. The helper checks sequence length to assign those names;
the PCB checks the coefficient values, completeness and model validity.

## Included tools and their side effects

| Tool | Purpose | Hardware / lifecycle |
|---|---|---|
| `HispecFibPcb` command methods | Predictable request/typed response wrapper | Queries may perform I/O; mutations and `persist=True` follow the wire contract |
| `ThroughputMonitor` | Decode telemetry, retain samples and command events, live plot | `start_throughput_monitor()` attaches collection only; `.stop()` sends measurement STOP |
| `AttenuatorCalibrationDataset` | Raw records, bridge roles, derived values and plots | Offline after download; never installs coefficients |
| `LaserCalibrationDataset` | Laser records and run metadata | Offline result of a multi-request download |
| `AttenuatorGridDataset` | Grid/slice fitting, NPZ save/load and plots | Offline; grid shape terms are not a complete firmware model |
| `AttenuatorGridProbe` | Background handle for a manual DAC grid acquisition | Changes laser and DACs; `wait()` joins/rethrows; it is not an automatic shutdown guard |
| `get_some_signal()` | Bounded manual search for a useful PD level | Changes output/routes as documented below; blocks until completion |
| `hispec_ota.py` | Validate, upload and test-boot firmware; manual confirmation by default | Changes flash and reboots; requires an explicitly provisioned OTA board and prepared hardware |
| `parameter_table()` | Display nested settings with API field names | Pure formatting, no I/O |
| `read_route_losses()` | Tabulate transmission and dB | Queries each requested route, no writes |
| `attenuator_noise()`, `throughput_noise()`, `photodiode_shot_noise_mv()` | Explicit model/noise scenarios | Pure numerical calculations, no settings changes |

## Firmware maintenance

`ota()`, `set_ota_window()` and `confirm_image()` wrap individual MQTT operations
and return `OtaStatus`. They use the MCUboot firmware; they do not
perform an image upload or turn bank power off. `tools/hispec_ota.py` combines
those commands with the Python SMP client to perform a verified network update.
Omitting `duration_s` from `set_ota_window(True)` uses the firmware's 600-second
default. Explicit fields are sent even when firmware will reject their combination,
such as a duration supplied with `set_ota_window(False, duration_s=10)`.

Follow [firmware updates and OTA](ota.md) for workspace/tool installation, USB
provisioning, artifact selection, the complete helper workflow and recovery.
Unlike ordinary notebook use, that tool also needs the pinned MCUboot checkout
and generated sysbuild configuration. `--dry-run` validates locally without
connecting; a real update writes firmware and reboots the target. The default
leaves the trial unconfirmed and prints `pcb.confirm_image(hash)`, the MQTT
equivalent and remaining time. `--auto-confirm` explicitly opts into confirmation
and prints the checks that justify it before accepting the image.

## Throughput collection and plotting

The following starts a passive return measurement, which selects the SM return
but does not start a laser. PD power and routing still change:

```python
monitor = pcb.measure_throughput(
    "none", channel="hk", fiber="S", autolevel=False, collect=True,
)
try:
    # Collect while the experiment runs; these calls take local snapshots.
    records = monitor.to_recarray()
    frame = monitor.to_dataframe()
    events = monitor.command_events
finally:
    monitor.stop()
```

For a named source, supply `output` such as `yj_ao`; `autolevel=True` starts and
controls that source. With `collect=True`, the helper also queries laser policy
and attenuator coefficients for display limits and needs a channel for the local
collector; supply `channel` if it cannot infer one from the laser name. With
`collect=False`, the command goes to firmware without a local channel lookup.
The Python helper defaults `off_in_s` to 300 seconds. Autolevel and continuations
retaining its laser shutdown require a positive duration; raw firmware commands
must supply one. Zero allows an unlimited purely passive stream, while manual
laser starts still have their own finite timer. Manual laser/attenuator changes,
tuning and settings operations disable adjustment but retain streaming, its
deadline and owned-laser shutdown. Automatic control uses
`min_autolevel_current_ma` / `max_autolevel_current_ma`; engineering commands keep
their full ranges. See
[throughput lifetime](throughput.md#lifetime-manual-changes-and-failure).

Use `autolevel_response_hz=0.1` to request quiet re-ranging. After one initial
positioning episode, useful signal with adequate saturation headroom causes no
actuator writes, even far from 1000 mV. The response filters the need to change
range; it neither continuously centers the detector nor smooths streamed samples.
Omitting the option or supplying zero retains band control. See the exact
[re-ranging rules](throughput.md#quiet-re-ranging).

The collector decodes outside Paho's callback, retains a finite sample deque
(default 20,000), and counts all decoded records in `samples_received`, including
those displaced from the deque. `clear()` clears local samples/events, not the
measurement. `arrays("field")` or `arrays("field1", "field2")` returns snapshots.

`command_events` is a DataFrame snapshot with these columns:

| Columns | Meaning |
|---|---|
| `command`, `arguments` | Wire command key and encoded argument dictionary |
| `issue_utc_ms`, `receipt_utc_ms` | Host UTC milliseconds at issue and reply receipt; receipt is absent if no reply arrives |
| `ack_utc_ms`, `ack_clock` | Board dispatch-completion UTC milliseconds; `ack_clock` is `board_utc` |
| `ok`, `error` | Request completion status and failure text |

An ACK marks command completion, not optical settling. Missing replies have no
board ACK. Host and board timestamps belong to different clocks. Their difference includes
clock offset as well as transport/dispatch time; it is not by itself a latency
measurement. The [command callback](#command-callbacks-and-notebooks) receives the
same dictionary without needing a collector.

In Jupyter, run `%matplotlib widget` in a setup cell before acquisition, then keep the result of
`monitor.plot_live()` referenced. Its panels show linear throughput, a separate
dB trace, detector levels/SNR and confirmed source/attenuation context. Pausing
or closing a plot changes display only. Explicit current/attenuation limits or
measurement settings are honored; an attached listener without those limits
autoscales. The title shows local sample age and marks samples stale after two
seconds without a decoded sample for that channel. Disconnection and stopped
collection are also shown; old samples are never replaced with zeros.

Display and draw an attached collector in a preceding cell when observing
power-up, then start the measurement in the next cell. For example, commissioning's
dark display is already listening before its acquisition cell enables the PD.
Ordinary board commands remain synchronous; collection continues independently,
although a busy cell can delay widget redraws. Concurrent acquisition guards and
display refresh belong in their focused helpers. Do not move a multi-command
hardware search into `to_thread` merely to animate a widget: cancelling its await
does not stop the worker or prevent it issuing commands during cleanup. Close the live
figure before switching back to inline review plots. Save sample/event DataFrames and
experiment context directly (for example with `pandas.to_pickle`); there is no
separate commissioning archive codec. Unit definitions and uncertainty meanings
are in [the math guide](mathematics.md).

### Passive monitoring notebook

`tools/monitoring.ipynb` runs beside an experiment notebook. Run setup and connection,
then either or both channel dashboard cells. Run All starts both dashboards and
leaves them running. The connection only subscribes and attaches
`start_throughput_monitor("all")`; it does not call `measure_throughput()`.

The shared message pane retains the latest 500 decoded info/warning/error,
calibration and boot messages, refreshing at 0.5 seconds. Each dashboard shows
the latest 600 samples; the shared collector retains at most 40,000 samples.
There are no automatic status queries or file writes. The explicit optional
status cell sends only `status()`. It does not use `pd()`, whose query enables
and rearms detector power.

Dashboard reruns replace only that channel's display. Connection reruns, failure
cleanup and the explicit shutdown cell use `close(stop_measurements=False)` and
leave other notebooks' measurements running. Do not substitute `monitor.stop()`
or default `pcb.close()` for that local shutdown.

This notebook shows existing broadcasts. Optical calibration stops ordinary
throughput streams, so those plots become stale while calibration messages continue.
It neither starts a replacement stream nor polls calibration records. Continuous
observation telemetry across operating modes is deferred. Boot messages appear
only when firmware publishes them; their absence is not proof of no restart.

## Calibration datasets and manual grids

`atten_calibrate()` and `laser_calibrate()` with no start arguments query status.
Start arguments run the physical procedures described in
[attenuator calibration](attenuator_calibration.md) and
[laser calibration](laser_calibration.md); `persist=False` still permits accepted
results to change RAM settings. Attenuator calibration installs an accepted pair
and its open voltages in RAM; laser calibration can suppress application with
`apply=False`. A valid laser candidate that needs review automatically writes no
curve, noise or route settings. Its acquisition, candidate and identifiable
within-window noise estimates remain available. Observed repeat variability is
reported separately and does not by itself reject the candidate.
`*_stop()` cancels and retains acquired data.
Download before starting either calibration again: firmware has one shared raw
dataset, replaced by an accepted new run and cleared by reboot.

`atten_calibration_data()` downloads metadata and record chunks into an
`AttenuatorCalibrationDataset`. Use `.physical("dac1")`, `.derived()`,
`.bridge_table()`, `.plot_physical()` and `.plot_surface()` to inspect measured
records separately from derived normalization and model predictions. Reverse
attenuator acquisition holds laser output fixed, sweeps 3300→0 mV, and uses
source-off backgrounds plus measured companion-closing bridges. Metadata records
the selected reference, operational open point and exact fitting support.
`laser_calibration_data()` returns `LaserCalibrationDataset` with `.records` and
`.meta`. A replacement run during a multi-request download invalidates that
snapshot. The dataset does not automatically reapply a saved calibration.
Current metadata carries the three observed dark-knot times/values, exact support
and fitted gain for each exact DAC pair. `fit.top_supported` distinguishes a
maximum current supported by a same-DAC shape series from an isolated measurement.
These gains normalize the laser fit;
they are not installed attenuator coefficients. `fit.valid` describes numerical
availability; `fit.accepted` describes automated acceptance. A valid candidate
can have noise estimates even when acceptance requires review. Within-window
constant/fractional noise and observed repeat variability describe different
timescales; neither identifies the laser alone as the cause of variation.
Install a reviewed saved curve with `laser_settings`, supplying `threshold_current_ma`,
`efficiency_mw_per_ma`, `efficiency_mw_per_ma2` and `efficiency_ref_ma` together.
Supply the reviewed operating range as `min_autolevel_current_ma` and
`max_autolevel_current_ma`; preserve the separate nominal and engineering maximum.
Optional noise uses that same settings call; outbound loss uses `mems_route_loss`.
No retained acquisition or notebook identifier is required by firmware.

`dataset.status.repeat` gives the number of matched and illuminated comparisons,
the number inside the proposed range, and the largest illuminated comparison's
current, exact DAC codes, two corrected PD signals and elapsed time. `matched`
requires eligible paired late windows at both same-current/exact-DAC conditions;
it does not count every raw revisit. Zero can therefore reflect ineligible windows
rather than absent observations. A missing
comparison is `None`, not zero variability. Usable comparisons must be above the
independently selected lasing floor as well as above the local signal criterion.
The largest difference remains visible
when it is outside the proposed range; background-level records remain available
without a misleading percentage. This samples the assembled path over the run's
timescales, not a Gaussian sigma or a bound on future variation.

The fit's `settings()` method only translates its six candidate fields to the
existing settings API; it performs no I/O and does not imply acceptance:

```python
settings = dataset.status.fit.settings()
payload = {"name": dataset.status.laser, "settings": settings, "persist": False}
print(json.dumps(payload, indent=2))  # MQTT: cmd/<device>/req/laser/settings
# After reviewing the candidate:
pcb.laser_settings(dataset.status.laser, **settings, persist=False)
pcb.laser_settings(dataset.status.laser)  # Read back the RAM policy.
```

Noise `fractional=0.044` means 4.4% within-window RMS. It is an effective
illuminated-path estimate after measured dark-variance subtraction, not intrinsic
diode noise or the time-separated repeat variability. `noise.error` explains an
attempted noise fit that could not provide estimates; if fitting never ran,
inspect acquisition and curve-fit state instead. An incomplete or invalid-model
capture need not identify an absolute source-referred constant-noise term, even
though its raw PD means and RMS remain useful. A fitted zero is the nonnegative
model boundary, distinct from an unavailable estimate.
`photodiode_shot_noise_mv(net_mv, enbw_hz=20,
transimpedance_v_per_a=...)` supplies an ideal photoelectron shot-RMS comparison
for an **explicit 20 Hz ENBW scenario**. Use the effective PD gain including its
divider. ENBW is not determined by the streaming sample rate; do not mistake this
scenario for a measured detector/acquisition bandwidth.

`attenuator_grid_probe()` sets the laser, steps explicit DAC coordinates, sleeps
for `dwell_s` and reads the PD window. It assumes routing, PD power and dark were
prepared by the experiment. It leaves the last laser/DAC settings in place and
has no exception cleanup STOP. Use an experiment-owned `try/finally` shutdown.
The asynchronous form returns `AttenuatorGridProbe`; `.done` and `.error` inspect
completion, `.wait(timeout_s=...)` returns the dataset or rethrows failure, and
`.save()` waits then writes the NPZ. A wait timeout does not cancel acquisition.
Grid `.fit()`/`.fit_slice()` are exploratory host fits, not the firmware's bridge
calibration or proof of absolute optical accuracy. Saturated/censored points are
handled separately; inspect fit acceptance before an explicit `atten_coeff()`.

The manual grid fitter uses a simpler erf surface than the firmware's normalized,
finite-floor, corrected model. `fit.coeff_args()` returns only shape/gain terms;
`atten_coeff()` additionally requires a leakage floor and calibrated limit for
each drive. Treat the grid result as a diagnostic, not a directly installable
replacement for firmware calibration.

The commissioning notebook keeps YJ/HK attenuator sections and all six laser
run/review cells independent of its live experiment `CHANNEL`. Each run retains
its archive path; an ordered replay list can include several runs of one laser.
Use the isolated `%matplotlib inline` cells for historical reviews and the widget
setup cells before live acquisition. Static review displays and closes each figure.

PCB command methods are synchronous. MQTT reception and throughput collection
already run independently, so ordinary notebook commands do not need
`await asyncio.to_thread(...)`. A busy cell can delay widget redraws. Commissioning
retains asynchronous holds and read-only metadata queries where the fast sample
guard must run concurrently. Command searches execute on the caller thread so an
interrupt cannot leave a background search issuing commands during cleanup.

## Terminal queries

From `tools/`, `python hispec_fibpcb.py --help` lists the small CLI. For example,
`python hispec_fibpcb.py --host broker.example --device hsfib-tib status` connects,
queries and closes. `warnings` keeps listening until interrupted. The `atten-db`
CLI entry queries the attenuator pair and displays its state, including dB loss.
Selector arguments are sent to the PCB for validation, just as in notebook calls.

## Manual signal search

`get_some_signal(laser, min_mv=800, max_mv=1200)` uses an existing collector for
that laser or starts a temporary passive run on the supplied AO/FEI and M/S route.
It opens selected FVOAs from maximum drive, then can vary laser level.
`use_atten_db=True` uses calibrated dB; `attenuators=(1,)`, `(2,)` or `()` selects
which devices may move; `vary_laser=False` holds the chosen `initial_level`.

The local search band must be finite and satisfy
$0<\mathtt{min\_mv}<\mathtt{max\_mv}\le2000$ mV. These are bounds on the
search's dark-subtracted PD result; raw overrange is checked separately. A weak
signal can use `min_mv=100, max_mv=200` rather than the default 800–1200 mV.

The default dwell is 0.5 s followed by three fresh records after board ACK with
matching confirmed current/DAC settings. At most 80 moves are tried. Success is
a three-record mean inside the inclusive requested band with no raw overrange.
When attenuation reaches its selected limit, laser emission fractions double or
halve within their allowed manual range. An unchanged bounded fraction ends the
search; the overall move limit also bounds repeated quantized no-ops.
The result records achieved settings and a reason if bounds prevent success.
Successful manual settings remain; failure/exception stops the selected laser.
Only a temporary monitor is stopped on exit; an existing monitor retains its
usual ownership. Run this command-producing search synchronously, so an interrupt
can enter its shutdown path without leaving a background worker issuing commands.
Collection continues while widget redraws may wait for the cell to return.

## Noise calculations

`attenuator_noise(coeff, mean_fvoa_mv=(...), noise_rms_mv=(...))` takes two
**post-amplifier** scope means/RMS values in mV and returns a record array with
`dac1`, `dac2`, `pair` rows. Model sigma, electrical RMS and their quadrature total
remain separate. `_tx` errors are absolute transmission fractions; electrical
percent is relative to nominal transmission. The derivative uses a 0.1 mV
FVOA-side difference and the calibrated curve/continuation.

`throughput_noise(records, enbw_hz=..., transimpedance_v_per_a=...,
dark_rms_mv=...)` predicts single-reading PD temporal RMS. With net voltage $V$
in volts, effective gain $G$ in V/A and electron charge $q_e$, shot RMS is
$\sqrt{2q_eVG\,\mathrm{ENBW}}$. A drive's electrical RMS is
$|V|(\ln10/10)|dA/du|\sigma_u$ in matching drive units. Independent temporal
variances add. Static calibration, dark-mean and model residual errors are
excluded; measured dark already contains quantization. A forced dark is not
measured detector noise. The default 10 mV drive RMS is an explicit assumption,
not a scope result. Returned DataFrame attributes retain units/bandwidth/provenance.

Use ENBW for the [full detector/acquisition response](photodiode_notes.md#sensitivity-and-noise-bandwidth),
and supply drive RMS over the applicable bandwidth. The helper assumes independent
drives; [correlation and filtering](attenuator.md#transmission-uncertainty) require
more information than two scalar RMS values.

For saved records lacking individual slope fields,
`historical_model_rms_db=(r1, r2)` enables the existing aggregate calculation.
It subtracts recorded laser fractional variance and the supplied static fit-RMS
variance from delivered-power fractional variance, clips a negative remainder
to zero, and rescales from the recorded 10 mV drive-noise assumption. This needs
the fit RMS values used for that capture and **equal requested RMS for both
drives**. Individual drive terms remain NaN because the aggregate cannot separate
them. Without slopes or this explicit input, electrical and combined RMS are
unavailable. Current records with slopes use those slopes directly.

## Electrical output-power estimate

`pcb.laser_bank_power_mw(names=None)` reads engineering status sequentially for
all six lasers, or an explicit unique selection, and sums

```{math}
P_{\rm outputs}[\mathrm{mW}]=\sum_i
 \left(V_{{\rm diode},i}[\mathrm V]I_{{\rm measured},i}[\mathrm{mA}]
 +\left|V_{{\rm TEC},i}[\mathrm V]I_{{\rm TEC},i}[\mathrm{mA}]\right|\right).
```

It uses measured diode current, not the commanded `i_mA`. A confirmed unpowered
driver contributes zero; an empty selection returns zero. Missing/nonfinite
readings or nonzero `read_rc` for a powered driver make the whole result NaN.
Request/transport exceptions propagate. It performs no writes and is not a
simultaneous snapshot. Driver losses, heater power and other bank input
consumption are excluded.

## Command callbacks and notebooks

Set `HispecFibPcb(..., on_command=callback)` or `pcb.on_command` for the same event
dictionary used by collectors without starting a measurement. It runs on the
requesting thread after completion, including errors/timeouts; concurrent requests
can invoke it concurrently. Return promptly, treat the event as read-only and do
not issue commands from the callback. Exceptions are logged without replacing the
request result. Assign `None` to detach; the client keeps no separate history.

`tools/tib_pcb_commissioning.ipynb`, `tools/throuput_monitor_lab.ipynb`,
`tools/attenuator_calibration_lab.ipynb` and `tools/temp_test.ipynb` are experiments
built on this tool. Run their cells deliberately: saved outputs are observations,
and a notebook may change routing, power, calibration or persistent settings.
The [thermal report](temp_test_result.md) retains measurements relevant to present
laser settings and fault interpretation.
