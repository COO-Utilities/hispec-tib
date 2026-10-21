# Throughput measurement and autolevel

`measure_throughput` combines the continuously sampled detector with confirmed
laser and attenuator settings. It can stream passive light, measure a known
source, or own a laser and adjust its brightness. See the
[command specification](commands.md#measure_throughput) for payloads and fields,
[Python helpers](python_helpers.md) for collection, and
[hardware paths](hardware_profiles.md) for launch/return connections.

## Start, acquire, publish, adjust

```{mermaid}
flowchart TD
  Request[measure_throughput] --> Validate[Validate board, source, dark and exclusions]
  Validate --> Prepare[Quiesce previous operation and retain shutdown obligations]
  Prepare --> Route[Command applies launch and return routes; resolves losses]
  Route --> Start[Latch losses; enable PD; optionally start owned source]
  ADC[PD owner: fresh YJ and HK conversions] --> Snapshot[Voltage, error and acquisition timestamps]
  Start --> Wake[Monitor wakes on sampler semaphore]
  Snapshot --> Wake
  Wake --> Health{Fresh reading and healthy owners?}
  Health -->|yes| Context[Associate previous or current confirmed source context]
  Context --> Compute[Calculate detector power, source power and throughput]
  Compute --> Queue[Enqueue best-effort telemetry]
  Queue --> Auto{Autolevel enabled and reading eligible?}
  Auto -->|yes| Adjust[Bounded laser or attenuator move]
  Auto -->|no| Wake
  Adjust --> Wake
  Health -->|fault or expiry| Stop[Stop stream; attempt owned laser shutdown]
```

`throughput_command.c` owns route selection and request validation;
`throughput_monitor.c` owns the running measurement and feedback. The ADC owner
knows neither the source nor its wavelength. No timing thread publishes MQTT:
main drains the bounded outgoing queue. A command acknowledgment means execution
completed, not that optics settled or a telemetry record arrived.

There are two independent stream slots, YJ and HK, but only one autolevel owner.
Dark capture and calibration exclude ordinary monitoring. A binary semaphore
coalesces wakes: a delayed consumer uses the newest conversion once, without
replaying missed samples. `sample_ms` is the monotonic acquisition start and
`t_ms` the estimated UTC midpoint. Throughput uses the previous source context
for a conversion begun at or before the last input-change completion. It keeps
only previous/current contexts; this does not reconstruct arbitrary rapid manual
changes or correct physical filter transients. No transition reading is blanked.

## Throughput equations

Let $P_L$ be modeled laser power in mW, $T_A$ the paired FVOA transmission,
$T_O$ the launch-route transmission, and $T_R$ the return-route transmission.
Let $P_D$ and $\sigma_D$ be nominal detector power/error in nW and $c_\lambda$
the known-source wavelength correction (the current table is unity).

```{math}
P_{\rm delivered}=10^6 P_L T_A T_O,\qquad
\sigma_{\rm delivered}=10^6 T_O
  \sqrt{(T_A\sigma_L)^2+(P_L\sigma_{T_A})^2},
```

```{math}
P_{\rm return}=\frac{c_\lambda P_D}{T_R},\qquad
\sigma_{\rm return}=\frac{c_\lambda\sigma_D}{T_R},\qquad
\tau=\frac{P_{\rm return}}{P_{\rm delivered}},
```

```{math}
\texttt{tp\_pd\_err}=\frac{\sigma_{\rm return}}{P_{\rm delivered}},\qquad
\texttt{tp\_err}=\sqrt{
 \left(\frac{\sigma_{\rm return}}{P_{\rm delivered}}\right)^2+
 \left(\frac{\tau\sigma_{\rm delivered}}{P_{\rm delivered}}\right)^2}.
```

The derivative form remains defined at zero detected power, provided delivered
power is finite and positive. For example, 1 mW through 40 dB of FVOA loss and
0.5 launch transmission delivers 50 nW. A detector reading of 4 nW with return
transmission 0.8 gives corrected return 5 nW and throughput 0.1.

Passive illumination has no known emission or wavelength: corrected nominal PD
power/error and detector S/N remain meaningful, but source and throughput fields
are unavailable. Zero laser power likewise gives undefined throughput. Raw ADC
input at or above 2000 mV marks overrange; numerical detector/throughput values
are retained only as lower bounds and their errors/S/N are unavailable.
[Photodiode uncertainty](photodiode_notes.md) and the
[shared error guide](mathematics.md) explain the omitted and correlated terms.

Route lookup selects an exact saved override, then compiled named-source loss,
then compiled generic loss, then unity. A saved generic override is used for a
generic measurement; it is not inherited by a named source. The command latches
losses at start. Restart the measurement after editing a route loss.

## Autolevel control

Autolevel uses each fresh reading, not a rolling mean. Its useful band is
20–80% of the 2000 mV detector range: **400–1600 mV net**. Raw overrange takes
precedence over a small or negative dark-subtracted reading. The requested
source-flux multiplier is

```{math}
k=\begin{cases}
1/3,& V_{\rm raw}\ge2000\ {\rm mV}\ \text{or}\ V_{\rm net}>1600\ {\rm mV},\\
3,& V_{\rm net}<400\ {\rm mV},\\
1,& \text{otherwise}.
\end{cases}
```

For attenuator adjustment alone, $T_A'=kT_A$, or
$A'=A-10\log_{10}k$: a factor of three is about 4.7712 dB. For a fixed attenuator,
laser power is proportional to current overdrive, so
$I'=I_{\rm threshold}+k(I-I_{\rm threshold})$, subject to the allowed current
and 0.1 mA register grid. The controller recomputes from confirmed owner state;
requested settings are never treated as acknowledged writes.

A conversion begun before the last move completed can be published but cannot
request another move. Brightening additionally requires its acquisition to begin
at least 100 ms after that change. Dimming needs only the first fresh conversion.
This guard is not a measured settling bound. Low readings try attenuation first;
high readings use the compiled `TP_LASER_FIRST` policy (the alternative is
`TP_ATTEN_FIRST`). Each actuator is attempted at most once for a reading; a
bounded/no-op actuator yields to the other.

Startup selects maximum calibrated attenuation and `initial_level` (default
0.5 of nominal modeled power). Automatic FVOA allocation is limited by each
model's supported range, the 55 dB per-device ceiling, and reachable DAC drive.
Laser current is bounded by `min_autolevel_current_ma` and nominal current.
`max_flux_ph_s` limits modeled flux after FVOAs, before static launch loss; it is
not a measured transient peak guarantee. See [attenuator allocation](attenuator.md)
for why the pair is balanced without reversing ordinary moves.

## Lifetime, manual changes and failure

A successful manual laser-level or attenuation change disables autolevel while
retaining collection and any existing owned-laser shutdown obligation. Passive
streams never acquire ownership of an unrelated manually started source.
Stop, expiry and owner faults stop an owned laser; a failed shutdown retains its
identity for a subsequent stop retry. Bank power, TECs and other sources are
unchanged. Explicit `laser stop:true` and laser auto-off separately consult
`disable_tec_at_autooff`; throughput STOP passes `stop_tec=false`.

Tuning and successful settings updates currently relinquish a matching stream.
Some policy-only changes can do this without stopping emission. Whether these
paths must retain the measurement's shutdown obligation is an explicit
[owner-review item](human_review_required.md#commandapi-mismatches). Do not infer
that ending collection always implies an optical STOP.

Source communication faults and confirmed controller faults stop dependent
acquisition. Recovery reports availability but never silently restarts a run.
PD auto-off inhibition is held during route preparation and acquisition, and
released on completion/failure. Release resumes the existing deadline, even if
it has already expired. Calibration uses the same throughput thread, so its
synchronous settling/I/O can delay another channel's processing while the ADC
continues sampling. See [runtime ownership](architecture.md).
