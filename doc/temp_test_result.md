# Laser-bank freezer test, 7–9 October 2026

Analysis date: 9 October 2026. All times below are UTC. This report uses the
archived notebook captures only. No hardware was contacted during analysis.

## Results

The enclosure and heater demonstrated a substantial temperature rise above the
PCB ambient sensor during the freezer test. The original heater policy allowed
idle diodes to fall below their configured start window. The approved replacement
policy turns the heater on when **any fresh idle TEC reads below 19°C** and off
when **all fresh idle TECs read above 21°C**. The archives predate this change.

| Quantity | Result | Interpretation |
| --- | --- | --- |
| Cold YJ photodiode dark | **−2.51875 mV**; between-measurement SD 0.03014 mV, six 2 s averages | Approximately 1.500 mV below the cached laboratory dark |
| New-band idle heater duty at −10°C | **69–71% estimated** | Constant-ambient thermal-model sensitivity, awaiting measurement |
| New-band idle heater duty at 0°C | **32–38% estimated** | The short measured zero-duty interval at 0°C was a warming transient |
| New-band idle heater-free ambient | **Approximately +6 to +10°C estimated** | Extrapolation; no steady idle sweep establishes a threshold |
| Loaded operation | Zero heater duty observed from approximately −4.4 to +10.4°C during qualified five-TEC intervals | Applies to those loads and thermal history; no universal ambient cutoff |
| Initial low-load enclosure rise | Mean idle probes **27.9–35.1°C above PCB ambient** in run 1 | Observed temperature difference, not a steady maximum capacity rating |
| Ambient load divot | Raw drops **0.38 and 0.50°C**; detrended deficits **0.73 and 0.79°C** | Load-associated sensor response; component attribution remains unproven |

At −15°C the new-band model predicts roughly 88–90% duty and minimum idle
temperature **17.5–17.8°C** after turn-on. Heater lag therefore remains important:
a 19°C switching threshold does not guarantee an 18°C minimum or immediate
startability. This needs a cold-soak validation before telescope use.

## Captures and analysis boundaries

| Archive in `tools/temp_data/` | Actual sample interval | Samples | Recorded ambient |
| --- | --- | ---: | --- |
| `temp_20261007T190615_212906Z.pkl` | Oct 7 19:06:18.277 to Oct 8 00:58:05.419 | 2,101 | −16.06 to −9.38°C |
| `temp_20261008T172437_042289Z.pkl` | Oct 8 17:24:40.145 to Oct 9 00:50:36.742 | 2,307 | −10.31 to +10.44°C |

Both captures report firmware `5dfd966e74a9-dirty`, boot count 115. The exact
dirty source state is not reconstructible from that identifier alone. The saved
notebook supplies operator intent; command responses and engineering records
provide evidence of what actually happened.

Run 1 has no sample errors or intervals above 20 s. Run 2 has 12 sample errors
and 14 intervals above 20 s, totaling 60.58 minutes of excluded intervals.
Host suspension and recording interruptions make wall time and notebook elapsed
time disagree. Calculations use sample issue UTC, exclude long intervals, and
break plotted lines across gaps. The run-1 outcome stop timestamp was written
later than its last sample; it is not the duration of the temperature record.

Command acknowledgements establish requested load, not proof of optical emission.
Load classification uses successful commands rather than attempted starts.
The five-TEC class additionally requires the heater status to report one idle
TEC. It does not establish maximum electrical load or stable emission by every
laser. Short photodiode measurements occur during otherwise low-load periods.

![Run 1 temperature and load history](temp_test_figures/run1_overview.png)

![Run 2 temperature and load history](temp_test_figures/run2_overview.png)

The shaded all-probe range includes actively regulated TECs. Those temperatures
are not enclosure thermometers. The fresh idle-probe average can jump when the
membership changes; such a jump is not an instantaneous enclosure temperature
change. The 1430hk probe remained an idle reference during the five-TEC load.
Unavailable or stale aggregates should not be interpreted as valid bank readings.

## Heater duty and temperature rise

### Measurements under the original policy

Duty is time weighted: hold each sampled heater state until the next sample and
divide included on-time by included duration. Include intervals of at most 20 s
with auto heater mode, bank powered, no sample error, and poll age at most 20 s.
Ambient bins are the stated center ±1°C. Initial low load means zero positively
commanded lasers and zero energized FVOAs. Five-TEC intervals require five
positively commanded lasers and one reported idle TEC. These bins mix thermal
transients and are not equilibrium duty measurements.

| Capture / load | Ambient bin | Included minutes | Measured duty |
| --- | ---: | ---: | ---: |
| Run 1, low load | −15°C | 61.57 | 50.7% |
| Run 1, low load | −10°C | 4.03 | 100% |
| Run 1, five TECs | −15°C | 17.43 | 69.3% |
| Run 1, five TECs | −10°C | 22.96 | 11.7% |
| Run 2, low load | −10°C | 19.67 | 73.5% |
| Run 2, low load | 0°C | 5.36 | 0% |
| Run 2, five TECs | 0°C | 41.14 | 0% |
| Run 2, five TECs | +5°C | 50.72 | 0% |
| Run 2, five TECs | +10°C | 35.96 | 0% |

One complete low-load cycle, Oct 7 19:16:50.915 to 20:42:56.994, lasted
86.10 minutes with 72.48% heater duty. Time-weighted PCB ambient was −13.84°C
(range −16.06 to −10.88°C). The coldest probe ranged from 14.49 to 17.68°C,
while the six-probe mean ranged from 15.01 to 19.02°C. This directly shows why a
15°C turn-on threshold was inadequate for a compiled 17°C minimum, particularly
with controller start blocking and thermal lag. After turn-on near 15°C, the
coldest readings continued down to about 14.4–14.8°C before recovering.

During the initial all-idle periods, run 1 sustained an observed mean-probe rise
of 27.93–35.05°C and coldest-probe rise of 26.80–33.69°C above PCB ambient.
Run 2 ranges were 20.98–26.99°C and 20.42–26.07°C respectively. These include
warming/cooling history and bank self-heating. They demonstrate enclosure
performance but do not establish the maximum sustainable temperature difference
under constant ambient. A heater input-power measurement and steady soak would
be needed for a thermal conductance or wattage claim.

### Estimates for the approved 19–21°C policy

The comparison uses a small empirical model of the initial idle 1510h reading,
usually the coldest probe, driven by measured PCB ambient and heater state:

```text
dh/dt = (u − h) / τ
dT/dt = a h − b (T − A) + c

T: 1510h reported temperature, °C
A: PCB ambient, °C
u: observed heater relay state, 0 or 1
h: delayed heater influence, dimensionless
t: minutes
```

Fit three-minute temperature slopes against the mean delayed heater influence
and temperature difference. Use τ of 12 and 16 minutes, fitting run 1 alone and
both runs pooled, giving four sensitivity cases. Discard the first 2τ of each
continuous fitting segment and reset at gaps over 24 s. Fit only the initial
all-idle periods, ending at Oct 7 20:50 and Oct 8 19:26. The parameter `c`
absorbs idle heating and sensor/reference bias; it is not a measured heat input.
Coefficients and sample counts are in [results.json](temp_test_figures/results.json).

Run-1-only models replay the withheld Oct 8 18:46:57.660–19:26:03.538 segment
with temperature RMSE 0.22–0.24°C, initializing at the observed temperature and
assuming zero delayed heater influence. Pooled-model replay is not independent
validation. Simulations use constant ambient, 0.1-minute steps for 72 hours, and
summarize the final 48 hours. Model ranges below show sensitivity to these four
fits, **not statistical confidence intervals**. Spatial gradients, sparse cycles,
changing ambient bias, and using one probe as a proxy limit extrapolation.

| Ambient | Off threshold; on below 19°C | Duty estimate | Full cycles/hour | Predicted minimum |
| ---: | ---: | ---: | ---: | ---: |
| −10°C | **21°C, selected** | **69.4–70.8%** | 0.75–0.77 | 18.15–18.29°C |
| −10°C | 22°C | 71.7–72.8% | 0.58–0.60 | 18.02–18.20°C |
| −10°C | 23°C | 72.9–74.2% | 0.48–0.50 | 17.91–18.16°C |
| 0°C | **21°C, selected** | **32.1–38.3%** | 0.75–0.83 | 18.80–18.86°C |
| 0°C | 22°C | 33.3–40.2% | 0.65–0.71 | 18.79–18.86°C |
| 0°C | 23°C | 35.1–41.5% | 0.56–0.60 | 18.78–18.85°C |
| −15°C | **21°C, selected** | **87.6–89.7%** | 0.35–0.42 | 17.48–17.80°C |

![Sensitivity to the heater off threshold](temp_test_figures/heater_policy_estimates.png)

The selected 21°C upper threshold uses less heater energy and avoids the long
recovery cycles of higher thresholds. The predicted −15°C minima still need
attention. Some models cannot reach 22–23°C at −15°C even with continuous heat.
The modeled full-heater equilibrium rise of roughly 36.8–38.1°C is an
extrapolation, not an enclosure rating.

The idle heater-free ambient estimate is `19 − c/b`, approximately +6 to +10°C.
Internal active-load heat can remove heater demand at much lower ambient:
qualified loaded intervals had zero duty from about −4.4 through +10.4°C.
The low-load 0°C bin lasted only 5.36 minutes after prior heating, so it does not
contradict the estimated steady idle duty at 0°C.

## Cold YJ dark

| UTC, Oct 7 | Dark mean (mV) | Reported SEM (mV) | PCB ambient (°C) | Nearest sampled PD on-time (s) |
| --- | ---: | ---: | ---: | ---: |
| 19:12:41.536 | −2.478125 | 0.011386 | −14.00 | 0* |
| 19:13:24.213 | −2.507812 | 0.012340 | −14.00 | 38 |
| 20:19:14.919 | −2.501563 | 0.011156 | −16.00 | 0* |
| 20:19:57.553 | −2.553125 | 0.012412 | −16.00 | 40 |
| 20:43:41.251 | −2.517188 | 0.010362 | −14.38 | 1231 |
| 20:44:13.905 | −2.554688 | 0.013293 | −14.25 | 1271 |

Each average lasts 2 s. *Zero is a nearby pre-command state, not a dark sampled
with the PD off or at zero settling time. The dark command enforces the existing
10 s PD power-on settling interval. The latter pair followed about 20 minutes
of powered operation.

![Cold YJ dark measurements with reported SEM](temp_test_figures/yj_dark.png)

The mean is −2.518750 mV with between-average SD 0.030136 mV and range
−2.554688 to −2.478125 mV. Within-window RMS is 0.0655–0.0841 mV. The archive
contains a cached laboratory reference of −1.018750 mV, giving a cold shift of
−1.500000 mV. The original reference acquisition time and temperature are not
preserved in this capture, so this is a comparison to that saved reference,
not a fitted temperature coefficient.

There is no large warm-up effect comparable to the 1.5 mV cold shift. Differences
of roughly 0.03–0.05 mV within successive pairs remain visible. Six averages
cannot establish exact independence from PD on-time. Repeating matched settling
times at controlled temperatures would distinguish time and temperature effects.

## Idle TEC differences and calibration limits

Only the initial all-six-idle, fresh, powered-bank intervals are used. Subtract
the simultaneous six-probe mean from each reported temperature. The SD below
describes variation over the interval, not uncertainty on a calibration constant.

| Probe | Run 1 residual, mean ± SD (°C) | Run 2 residual, mean ± SD (°C) |
| --- | ---: | ---: |
| 1028y | +0.403 ± 0.202 | +0.419 ± 0.159 |
| 1270j | −0.504 ± 0.219 | −0.430 ± 0.181 |
| 1430yj | +0.607 ± 0.352 | +0.470 ± 0.255 |
| 1430hk | +0.221 ± 0.055 | +0.155 ± 0.027 |
| 1510h | −0.753 ± 0.537 | −0.556 ± 0.385 |
| 2330k | +0.027 ± 0.279 | −0.058 ± 0.228 |

![Probe differences during the initial idle periods](temp_test_figures/idle_probe_differences.png)

The median instantaneous spread was 1.58°C in run 1 and 0.92°C in run 2, with
maxima 2.38 and 2.40°C. Residuals change through the heater cycle and between
runs. This is useful replacement-diode baseline information, but sensor bias and
real thermal gradients cannot be separated. Applying negative residuals as
calibration corrections would erase some real physical differences.

### Datasheets, controller limits, and settings

The local [Maiman SF manual](../../refrence_docs_examples/maiman%20sf8025.pdf)
specifies a **15–40°C temperature control range** on printed page 5 (PDF page 6).
That is a driver capability specification. The application's **17–38°C**
compiled operating window is a separate, narrower configuration, with a default
25°C target. Neither number is the bank heater floor. At test time the heater
turned on below 15°C and, in this cold-ambient regime, off when an idle probe
exceeded 20°C. This mismatch left other diodes too cold to start.

The controller blocks both cold and hot TEC conditions. A displayed reading
slightly above a configured lower bound does not prove the controller's start
permission has recovered. The 1510h test succeeded after changing the allowed
range to 16–38°C, but these samples do not measure a guaranteed fixed hysteresis
or justify treating 16°C as a calibrated universal minimum.

The manual's printed page 20 (PDF page 21) distinguishes writable temperature
minimum/maximum from read-only absolute limits and exposes internal LD thermistor
`B25/100`. It does not document an independent thermistor R25 or temperature
offset calibration parameter. Its address notation differs from the application's
register map; service writes require the correct controller/firmware-specific
documentation. Do not copy manual addresses into the current driver blindly.

The exact-model [AeroDIODE 1270 nm](https://www.aerodiode.com/wp-content/uploads/2023/08/1270-nm-laser-diode-Model-1-Datasheet.pdf)
and [1510 nm](https://www.aerodiode.com/wp-content/uploads/2023/08/1510-nm-laser-diode-Model-1-Datasheet.pdf)
datasheets specify thermistor resistance at 25°C of 9.5–10.5 kΩ. That ±5%
resistance tolerance alone corresponds to approximately ±1.1°C near 25°C if a
nominal 10 kΩ, B = 3988 K conversion is assumed. It is therefore comparable to
the observed inter-probe spread, but does not prove its cause. The manufacturer
also supplies a [1430 nm model-1 datasheet](https://www.aerodiode.com/wp-content/uploads/2023/08/1430-nm-laser-diode-Model-1-10-mW-Datasheet-1.pdf).
Confirm the actual installed model, serial number, and datasheet revision for
each of the two 1430 channels before changing its configuration.

Archived engineering status reports B = 3988 K for the drivers. The application's
FLC thermistor coefficient −0.044 per °C is a different quantity from B in kelvin:
`alpha = −B/T²` gives about −0.0449 per K at 25°C for B = 3988 K. These numbers
must not be compared or copied as though they use the same units. Exact installed
1028y and 2330k unit datasheets were not available for verification; repository
properties are configured values, not independent vendor confirmation. A nearby
wavelength model is insufficient evidence for a settings change.

**Settings disposition:** retain the compiled 17–38°C window and 25°C target
pending controlled start tests and exact unit specifications. Do not derive
per-diode offsets from this freezer run. Apply only the approved heater change.
The 2330k evidence favors avoiding a large cold-target startup step and then
testing a controlled target ramp, but any automated ramp requires separate
approval. The existing DFB PID profile is already selected for 1028y/2330k;
the manual's warning about DFB PID startup behavior does not justify blindly
reapplying that profile or changing gains.

### Replacement-diode procedure

1. Record the replacement's model/serial, R25 and B specifications, diode current
   limits, TEC limits, permitted temperature range, and exact controller version.
   Save the existing engineering status and settings before any approved change.
2. With diode emission and active TEC regulation off, obtain uniform passive
   equilibrium with an independently calibrated contact reference near the
   diode mount. Power only what is needed to read the sensor. Monitor driver
   self-heating and reference placement. A practical initial acceptance rule is
   drift below 0.02°C/min for 10 minutes; verify spatial agreement as well.
3. Repeat at several safe temperatures, for example 18, 25, and 35°C when the
   actual unit specifications permit. Measure reference uncertainty and gradients.
   Repeat mounting or swap reference positions to test whether an apparent
   sensor error is actually a spatial gradient. Log raw resistance if accessible
   through an approved service measurement, along with reported temperature.
4. Compare R25 and the slope against the vendor conversion. One point at 25°C
   cannot identify B, and a B-only adjustment cannot correct R25 bias there.
   Use at least two reference temperatures to constrain a simple beta model and
   a third to check it; use the vendor's full conversion if available.
5. Select an approach below. Any settings write or API/flow change remains a
   separate approval. Verify both cold and hot start/restart behavior, measured
   current, TEC settling, and absence of an autonomous stop before accepting.
6. Re-run the simple initial idle-probe comparison against this baseline under
   matched conditions. Preserve the new raw records and serial-number history.
   This quickly detects a replacement anomaly without pretending it is an
   absolute calibration.

| Avenue | Benefit | Limitation / prerequisite |
| --- | --- | --- |
| Relative idle baseline | Fast, no parameter writes; flags an unusual replacement | Cannot separate gradients and sensor bias or fix hardware interlocks |
| Independent reference plus verified B setting | Can correct slope if B is actually wrong | Requires uniform multi-temperature reference; B-only control cannot correct R25 error |
| Reference-derived display correction | Can improve displayed temperature when repeatable | Adds API/interpretation work; hardware interlocks still use the controller's raw conversion |
| Vendor/controller calibration or matched thermistor | Can address R25/conversion at the source | No independent R25/offset register documented here; needs vendor support or a suitable replacement |
| Wavelength thermometry | Potential independent diode-temperature indicator | Needs calibrated wavelength versus temperature and current for that particular diode; nominal wavelength is inadequate |

Absolute calibration is **not possible from these archives alone**. A complete
in-controller correction is also not established by the documented B-only
interface. If reference testing identifies an R25 error that B cannot correct,
record the residual uncertainty and obtain vendor guidance rather than silently
compensating with the heater threshold or thermal protection limits.

## PCB ambient response to load changes

![Ambient sensor response at two returns to low power](temp_test_figures/ambient_load_divots.png)

| Load-off UTC, Oct 8 | Low-power duration | Raw decline | Deficit below extrapolated warming |
| --- | ---: | ---: | ---: |
| 20:39:45.607 | 5.79 min | 0.38°C | 0.73°C |
| 22:59:30.647 | 6.84 min | 0.50°C | 0.79°C |

Fit a straight warming trend from 8 to 1 minutes before each transition. Compare
the last minute before load restoration with that extrapolation. Raw decline
compares that last minute with the final six pre-transition samples in the fit
window. Pre-transition warming slopes were 0.0487 and 0.0391°C/min.

The response is consistent with local load-dependent heating or a measurement
bias. FVOAs 6–12 are a plausible source given the owner's physical-layout
observation. Lasers, FVOAs, and PD loads changed together, however, so this
record cannot attribute the effect to that group or establish an ADC scale
error. A later test should switch the suspected FVOA group alone, then lasers
alone, with a separate ambient reference and comparable dwell times. PCB ambient
should retain a roughly degree-scale load-dependence caveat in thermal estimates.

## Commissioning validation

Replay these archives first. For a later explicitly authorized live run, record
the firmware revision, actual controller settings, independent ambient reference,
load transitions, and fresh per-diode temperatures. Observe cooldown without
changing heater policy mid-phase. At stable −10°C and 0°C, acquire multiple
complete idle heater cycles, minima after turn-on, maxima after turn-off, and
relay cycle rate. Add a −15°C capacity check before asserting that the bank
remains startable over the full intended range.

Take matched YJ darks after the normal settling interval and prolonged power-on.
Only after reviewing the idle record should the operator enable the high-load
phase. Verify actual TEC/LD operation and measured currents, then isolate the
FVOA ambient effect with single-subsystem steps. Stop and save through the
notebook's explicit end-test action, preserving raw records even if analysis
fails. The separate [notebook update prompt](temp_test_update_prompt.txt) describes
the requested archived replay and future commissioning preamble.

## Data provenance

Full numerical results and editable-chart source arrays are in
[results.json](temp_test_figures/results.json). SVG versions sit beside each PNG.
These are analysis products, not new firmware telemetry or host APIs.

| Input | SHA-256 |
| --- | --- |
| Run 1 pickle | `8ef8ebe5c7fbfad042dd5adef3bb69fd3de408af5bf482074baf08f1ea1547cb` |
| Run 1 log | `41d47c20c10bfcff05c6f61eab06222109dec3fd733bb705b699a9d745e21d7a` |
| Run 2 pickle | `37205445a1dad8a099d2b81ba6de8a0691176bab752ae7640c365886b7374fa7` |
| Run 2 log | `59f44057cab69426b219cc886b79c35ca3c1826cf62f6cf4012777698e2b9ca5` |

Software footnote: the archive records a 2330k output-fault warning a few seconds
after an acknowledged start, followed by both TEC and LD stopped. The brief
thermal excursion was not sampled at its trigger. The approved heater policy is
implemented separately; diagnostic, status, polling, default-power, aggregate,
and convenience-helper changes remain proposals in
[temp_test_change_proposals.md](temp_test_change_proposals.md).
