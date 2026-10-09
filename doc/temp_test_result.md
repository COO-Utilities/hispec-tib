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

The supplied [Maiman SF manual](datasheets/maiman%20sf8025.pdf), printed page 5
(PDF page 6), specifies a **15–40°C control range**. The application's **17–38°C**
compiled window and **25°C target** are separate settings. The heater now uses
**19–21°C idle probes**. During these archives it used the original 15°C turn-on
and, in the cold-ambient regime, 20°C turn-off. That policy let other diodes fall
below their configured start window.

The controller blocks both cold and hot TEC conditions. A reading slightly above
the configured lower bound does not prove start permission has recovered. The
1510h start succeeded after changing its range to 16–38°C. These samples do not
establish a fixed recovery hysteresis or a universal calibrated minimum.

The source comparison now uses the installed-model sheets and the two supplied
FLC unit reports:

| Source | Relevant evidence |
| --- | --- |
| [FLPD-1028-50-DFB-BTF](datasheets/FLPD-1028-50-DFB-BTF.pdf), p. 2 | R25 9.5–10.5 kΩ, coefficient −4.4 %/°C, operating 10–40°C, diode maximum 250 mA, TEC maximum 1.2 A |
| [BF69016 report](datasheets/FLPD-1028-50-DFB-BTF_%23BF69016_testreport.pdf), p. 1 | R25 10,000 Ω at 25°C; threshold 14.5 mA; bias/maximum 250 mA; test operating temperature 25°C; no coefficient/B measurement |
| [FLPD-2330-03-DFB-BTF](datasheets/FLPD-2330-03-DFB-BTF.pdf), p. 2 | R25 9.5–10.5 kΩ, coefficient printed −4.4 with units 1/°C; operating 10–40°C, diode maximum 120 mA, TEC maximum 1.2 A |
| [D4520009 report](datasheets/FLPD-2330-03-DFB-BTF_%23D4520009_testreport.pdf), p. 1 | R25 10,000 Ω at 25°C; threshold 24.9 mA; bias/maximum 120 mA; test operating temperature 25°C; no coefficient/B measurement |
| [Aero 1270](datasheets/1270-nm-laser-diode-Model-1-Datasheet.pdf), [1430](datasheets/1430-nm-laser-diode-Model-1-Datasheet.pdf), [1510](datasheets/1510-nm-laser-diode-Model-1-Datasheet.pdf), p. 1, rev. 08/23 | R25 9.5–10.5 kΩ; no thermistor slope; operating current typical 60 mA, maximum 70 mA; TEC maximum 1 A; case operating −20 to +60°C |
| Maiman, printed pp. 15–16, 20 (PDF pp. 16–17, 21) | Default/DFB PID examples, oscillation cautions, writable internal thermistor B25/100 and temperature bounds; no independent R25/offset calibration register documented |

The 2330 sheet's −4.4 is interpreted as **%/°C**, consistent with the 1028 sheet
and a 10 kΩ NTC. That interpretation is an inference: the printed 1/°C label
would imply a physically implausible fractional slope if taken literally. Obtain
vendor confirmation before using it as a replacement calibration specification.
FLC's case rating is −5 to +70°C. Case temperature, controlled diode temperature and PCB ambient are distinct;
the Aero case range does not widen the Maiman controlled-temperature range.

#### Coefficient disagreement and implemented units

All **273 successful archived engineering snapshots** (264 in run 1, nine in run
2) read internal B = **3988 K**. These are programmed settings, not measurements
of the six thermistors. At 25°C the constant-B relation is
`alpha_pct = −100 × B / 298.15²`, giving **−4.486271 %/°C**.

| Channel | Prior compiled coefficient metadata | Archived controller, translated | Diode sheet / unit report |
| --- | --- | --- | --- |
| 1028y | −0.044 per °C = −4.4 %/°C | −4.486271 %/°C | Sheet −4.4 %/°C; BF69016 slope absent |
| 2330k | −0.044 per °C = −4.4 %/°C | −4.486271 %/°C | Sheet −4.4 %/°C inferred; D4520009 slope absent |
| 1270j | Unavailable | −4.486271 %/°C | Sheet slope absent; unit report unavailable |
| 1430yj / 1430hk | Unavailable | −4.486271 %/°C | Sheet slope absent; unit reports unavailable |
| 1510h | Unavailable | −4.486271 %/°C | Sheet slope absent; unit report unavailable |

The old FLC metadata **did not program the controller coefficient**. Its units
differed from engineering status, which exposed raw B under a misleading name.
The metadata/readback mismatch therefore did not itself change the regulation
loop. It cannot establish the cause of previously observed instability.

H replaces that ambiguity with **one public field, `ntc_coeff_pct_per_c`**, in
resistance %/°C at 25°C, for configured settings and actual engineering readback.
Firmware translates internally and rounds to integer B; accepted settings report
the representable coefficient. The PCB owns the configured value and optional
NVS persistence. Defaults retain the archived B=3988 equivalents for every
channel, with source qualifications beside the property rows. The generic FLC
−4.4 value is a comparison, not an automatic replacement setting.

Synchronization occurs through existing profile preparation at the first explicit
start after bank power-up, and through explicit driver-backed settings updates.
It does not add MCU-startup programming. A coefficient update follows the existing
stop/power/program/restore flow; a failed profile write leaves preparation false.
Engineering readback reports actual conversion or unavailable, never a configured
fallback. `persist:true` writes a separate eight-byte per-channel app NVS record,
preserving the existing policy/calibration records. No new controller EEPROM
SAVE is added; existing START/STOP controller persistence remains.

The FLC −4.4 %/°C slope corresponds to B = 3911.31 K, rounded to **3911 K**:
**1.93% below** the archived setting, a slope-magnitude difference of
**0.08627 percentage points per °C**. Assuming identical R25 and a true B=3988
thermistor, using 3911 instead would shift the reported value by about **−0.19°C
at 15°C**, **0 at 25°C**, and **+0.31°C at 40°C**. This is model sensitivity, not
a measured error or a calibration recommendation. A coefficient difference can
affect inferred temperature/small-signal loop gain, but this scale alone does not
explain the multi-degree cold-start excursions.

All supplied sheets allow R25 ±5%, corresponding to roughly **±1.1°C near 25°C**
with the nominal conversion. The two FLC reports record 10,000 Ω, without an
uncertainty or slope. One resistance point cannot verify the coefficient. R25
tolerance and spatial gradients may exceed the modeled coefficient effect;
the idle residuals cannot determine which dominates.

#### Other coded/controller/vendor differences

| Quantity | Compiled / archived | Vendor comparison and disposition |
| --- | --- | --- |
| Temperature window / target | Compiled 17–38°C / 25°C; archived 1510h and 2330k trials include 16–38°C and changed targets | Maiman 15–40°C; FLC operating 10–40°C and reports at 25°C. Retain compiled window/target; start tests must include recovery hysteresis and startup undershoot |
| TEC current limit | FLC 1.2 A; Aero 1 A, agreeing with archived readbacks | Matches supplied diode maxima; no change |
| PID, 1028y / 2330k | 20 / 1000 / 1000 in code and archive | Matches Maiman's DFB example. 2330k already used it; its cold-start issue is not omission of that profile |
| PID, Aero channels | 100 / 1000 / 0 in code and archive | Matches Maiman's default, although these are DFB diodes. The manual recommends trying the DFB profile for problematic DFB behavior; this merits a controlled comparison, not automatic gain substitution |
| Normal diode-current maximum | 1028y 250 mA, 2330k 120 mA, Aero 60 mA, agreeing with archive | FLC maxima 250/120; Aero typical 60, maximum 70. Compiled normal maxima match FLC limits and are conservative for Aero |
| OCP acceptance ceiling (`dne_current_ma`) | 1028y 275; 2330k 137; 1270j 75; other Aero 72 mA | Higher than the sheet maxima 250/120/70. This is the firmware acceptance ceiling for a physical protection potentiometer, not the commanded current maximum; remains unchanged pending owner review |

The observed physical OCP thresholds were:

| Channel | Archived range (mA) | Sheet current maximum (mA) |
| --- | ---: | ---: |
| 1028y | 261.5–265.5 | 250 |
| 1270j | 66.3–68.6 | 70 |
| 1430yj | 66.1–68.3 | 70 |
| 1430hk | 65.0–68.2 | 70 |
| 1510h | 67.8–70.1 | 70 |
| 2330k | 131.3–134.3 | 120 |

FLC's observed protection trips exceed the stated diode maxima even though normal
commanded maxima agree. The pot setting needs an independent protection review;
do not lower or retune it from this archive alone. PID, coefficient, startup step,
thermal conditions and protection are separate hypotheses. Maiman also cautions
that high derivative gain can amplify noise and that integral settings can cause
oscillation (printed p. 16); the DFB example is not a universal optimum. Retain
all current/PID/temperature limits in this implementation. A later authorized
trial should change one quantity at a time, retaining engineering readbacks,
settling traces and autonomous-stop warnings. No automatic ramp is added.

### Replacement-diode procedure

1. Record the replacement model/serial, R25 and coefficient specification, diode
   and TEC current limits, permitted temperatures and controller revision. Match
   the controller serial to the physical diode association. Save configured
   settings, actual engineering readback and this idle-probe baseline.
2. First perform the simple relative check: emission and TEC regulation off,
   bank powered for reads, same heater/load conditions as this baseline. Compare
   each probe minus the simultaneous idle-bank mean after comparable settling.
   An outlier merits reference testing; do not negate these residuals into offsets.
3. For an absolute check, place an independently calibrated contact reference
   near the diode mount and establish uniform passive equilibrium with emission
   and active TEC regulation off. Check driver self-heating and gradients. A
   proposed initial criterion is drift below 0.02°C/min for 10 minutes, accompanied
   by spatial checks; it is not a vendor acceptance specification.
4. Repeat at safe points such as 18, 25 and 35°C when the replacement permits.
   Record reference uncertainty, mount/reference position, current coefficient,
   reported temperatures and raw resistance if obtainable by an approved service
   measurement. Repeat placement or swap reference positions to check gradients.
5. If raw resistance is available, fit `ln(R)` against `1/T_reference_K` to
   estimate internal B and R25 separately. Convert the fitted slope to the single
   public coefficient in %/°C at 25°C. With temperature readback alone, resistance
   can only be inferred using the known controller conversion and assumed R25;
   document that assumption rather than claiming an independent resistance test.
   Two points constrain a simple model and a third checks it. One point at 25°C
   cannot identify slope, and coefficient-only control cannot fix R25 bias there.
6. If a slope correction is independently supported, review it with the operator,
   apply `pcb.laser_settings(name, ntc_coeff_pct_per_c=value, persist=False)`,
   and query both configured and actual coefficients. This stops emission and
   programs the profile through the normal flow. Recheck the reference points
   after integer-B rounding. Persist only after acceptance. An R25 error requires
   vendor/controller calibration or a suitable thermistor; do not absorb it into
   the coefficient or protection bounds. These are future live actions; none
   were performed during this implementation.
7. Verify cold/hot start and restart, actual current, TEC settling and absence of
   autonomous stops. Repeat the relative idle baseline under matched conditions,
   preserving new raw records and serial history.

| Avenue | Benefit | Limitation / prerequisite |
| --- | --- | --- |
| Relative idle baseline | Fast, no writes; flags a replacement anomaly | Cannot separate gradients and sensor bias or fix controller interlocks |
| Multi-temperature reference plus coefficient setting | Can correct a demonstrated slope error through the normal API | Needs uniform equilibrium and known conversion; cannot independently correct R25 at 25°C |
| Reference-derived display correction | Can improve displayed temperature when repeatable | Would need a separately approved API/interpretation change; hardware interlocks still use controller conversion |
| Vendor calibration / matched thermistor | Can address R25 and conversion at the source | No independent R25/offset register documented here; needs vendor support or replacement |
| Wavelength thermometry | Potential independent diode-temperature indicator | Needs unit-specific wavelength versus temperature/current calibration; nominal wavelength is insufficient |

Absolute calibration is **not possible from these archives alone**. Full
in-controller calibration is not established by the documented coefficient-only
interface. If an independently referenced R25 bias remains, record the residual
uncertainty and obtain vendor guidance. The manual's serial-register notation
differs from the Modbus map: current firmware uses 0x008A for B. Do not copy
printed service addresses into the driver blindly.

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
thermal excursion was not sampled at its trigger. The approved heater policy and all
A–H changes in [temp_test_change_proposals.md](temp_test_change_proposals.md)
are implemented. Failure replies retain available controller evidence; compact
status adds output_state while readiness describes public start blockers;
eligible background checks detect LD-only stops and freeze confirmed stopped
component counters; heater overrides continue observation; temps() uses idle
probes; notebook low-power transitions stop channels without bank cycling; the
PCB power helper uses measured output currents; H synchronizes the thermistor
coefficient in datasheet units. Temporary native/fake-client/archive checks and
the Nucleo build passed. No hardware, flashing or live notebook execution was
used. Target timing, physical synchronization and thermal/start behavior remain
in [human_review_required.md](human_review_required.md). The refreshed editable
presentation is intentionally uncommitted.
