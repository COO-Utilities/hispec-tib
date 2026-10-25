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

`close()` stops registered collectors' measurements, then disconnects even when
shutdown errors occur; such failures are raised as an `ExceptionGroup`.
Uncollected/manual laser activity is not a blanket responsibility of connection
cleanup. Use explicit `laser(..., stop=True)` when the experiment owns a manual
source. `laser(..., value=0)` only requests zero current and preserves readiness
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
Attenuator `_db` is positive power loss, `_mv` is pre-amplifier DAC drive.
`mems_route_loss(..., loss=0.5)` is a lost fraction (transmission 0.5);
`loss="3.0103 dB"` requests the same loss. These conventions come from the wire
specification, not a separate host-unit system.

```python
pcb.laser("1028y")                       # Query
pcb.laser("1028y", value=0.25)           # Set 25% modeled power
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
| `hispec_ota.py` | Validate, upload, test-boot and confirm firmware | Changes flash and reboots; requires an explicitly provisioned OTA board and prepared hardware |
| `parameter_table()` | Display nested settings with API field names | Pure formatting, no I/O |
| `read_route_losses()` | Tabulate transmission and dB | Queries each requested route, no writes |
| `attenuator_noise()`, `throughput_noise()` | Explicit model/noise scenarios | Pure numerical calculations, no settings changes |

## Firmware maintenance

`ota()`, `set_ota_window()` and `confirm_image()` wrap individual MQTT operations
and return `OtaStatus`. They require an OTA-enabled firmware build; they do not
perform an image upload or turn bank power off. `tools/hispec_ota.py` combines
those commands with the Python SMP client to perform a verified network update.
Omitting `duration_s` from `set_ota_window(True)` uses the firmware's 600-second
default. Explicit fields are sent even when firmware will reject their combination,
such as a duration supplied with `set_ota_window(False, duration_s=10)`.

Follow [firmware updates and OTA](ota.md) for workspace/tool installation, USB
provisioning, artifact selection, the complete helper workflow and recovery.
Unlike ordinary notebook use, that tool also needs the pinned MCUboot checkout
and generated sysbuild configuration. `--dry-run` validates locally without
connecting; a real update writes firmware and reboots the target.

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
Firmware defaults to a 300-second
measurement lifetime; zero disables expiry. Manual laser/attenuator changes
disable adjustment but retain streaming and owned-laser shutdown. See
[throughput lifetime](throughput.md#lifetime-manual-changes-and-failure).

The collector decodes outside Paho's callback, retains a finite sample deque
(default 20,000), and counts all decoded records in `samples_received`, including
those displaced from the deque. `clear()` clears local samples/events, not the
measurement. `arrays("field")` or `arrays("field1", "field2")` returns snapshots.
Command events retain host issue/receipt times and board `ack_utc_ms`; an ACK is
command completion, not optical settling. Missing replies have no board ACK.

In Jupyter, use `%matplotlib widget`, then keep the result of
`monitor.plot_live()` referenced. Its panels show linear throughput, a separate
dB trace, detector levels/SNR and confirmed source/attenuation context. Pausing
or closing a plot changes display only. Save sample/event DataFrames and
experiment context directly (for example with `pandas.to_pickle`); there is no
separate commissioning archive codec. Unit definitions and uncertainty meanings
are in [the math guide](mathematics.md).

## Calibration datasets and manual grids

`atten_calibrate()` and `laser_calibrate()` with no start arguments query status.
Start arguments run the physical procedures described in
[attenuator calibration](attenuator_calibration.md) and
[laser calibration](laser_calibration.md); `persist=False` still permits accepted
results to change RAM settings. `*_stop()` cancels and retains acquired data.
Download before starting either calibration again: firmware has one shared raw
dataset, replaced by an accepted new run and cleared by reboot.

`atten_calibration_data()` downloads metadata and record chunks into an
`AttenuatorCalibrationDataset`. Use `.physical("dac1")`, `.derived()`,
`.bridge_table()`, `.plot_physical()` and `.plot_surface()` to inspect measured
records separately from derived normalization and model predictions.
`laser_calibration_data()` returns `LaserCalibrationDataset` with `.records` and
`.meta`. A replacement run during a multi-request download invalidates that
snapshot. The dataset does not automatically reapply a saved calibration.
Install a reviewed saved curve with `laser_settings`, supplying `threshold_current_ma`,
`efficiency_mw_per_ma`, `efficiency_mw_per_ma2` and `efficiency_ref_ma` together.
Optional noise uses that same settings call; outbound loss uses `mems_route_loss`.
No retained acquisition or notebook identifier is required by firmware.

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

The default dwell is 0.5 s followed by three fresh records after board ACK with
matching confirmed current/DAC settings. At most 80 moves are tried. Success is
a three-record mean inside the inclusive requested band with no raw overrange.
The result records achieved settings and a reason if bounds prevent success.
Successful manual settings remain; failure/exception stops the selected laser.
Only a temporary monitor is stopped on exit; an existing monitor retains its
usual ownership. In a notebook, use `await asyncio.to_thread(...)` so widgets
can refresh while this blocking search runs.

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
The [thermal report](temp_test_result.md) retains evidence relevant to present
laser settings and fault interpretation.
