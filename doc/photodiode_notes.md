# Photodiode Sampling and Uncertainty

## Sampling and throughput measurements

Each channel produces one fresh ADS1115 conversion every **50 ms (20 Hz)**.
Successive stream records never reuse ADC conversions. The fixed 500 ms rolling
window remains available for PD diagnostics and the configurable window for
dark capture and attenuator calibration; neither feeds throughput or autolevel.
The ADS1115 remains at **250 SPS**, selected by devicetree, with sequential YJ/HK
conversions. Selecting **64 SPS** requires no algorithm or window changes:
two conversions take about 31.3 ms before I2C and scheduling overhead, compared
with about 8.1 ms at 250 SPS. Calibration asks the PD owner to round its
window to whole samples, resets it after each input change, and waits for that
many conversion attempts. A conversion begun before reset is excluded. There
is no converter-time pad or additional settling window.

The ADC owner stores detector readings, uncertainty, one monotonic acquisition
start (`sample_ms`), and an estimated UTC midpoint (`t_ms`). It owns no laser,
attenuator, wavelength, or expected-source context. The nominal PD power uses
validated channel calibration; the measurement layer applies a known source's
wavelength correction and return-path loss afterward.

One binary semaphore wakes throughput after the two-channel round. A delayed
consumer gets the latest acquisition once; intermediate acquisitions may be
missed, never replayed. Use acquisition gaps and existing ADC timing logs
(`worst_loop_us`, `min_margin_us`, missed intervals, overruns) to assess hardware
margin. The timeout still services expiry and calibration if ADC work stalls.

Throughput reads the owners' confirmed state without hardware I/O. It retains
only the previous and current source contexts and the time a change completed
(or an external change was observed). A conversion begun at/before that time
uses the previous context. This prevents a delayed old reading from receiving
the next input's denominator; it does not deconvolve a physical transition or
reconstruct an arbitrary series of rapid manual changes. The PCB and detector
filters may make a transition reading predominantly reflect the previous input.
No reading is blanked because an input changed.

Throughput publishes the completed reading before selecting the next adjustment.
Normal and startup control use that fresh reading: below 20% useful net input,
request three times the flux; above 80%, request one third. Raw input at/above
2000 mV takes precedence over a low dark-subtracted value. A conversion that
began before completion of the preceding control move can still be published,
but cannot trigger another move. There is no additional settling window or
blanked stream interval. Only one channel may own autolevel; both PDs may stream.

Both PCB input traces have nominal 20 Hz low-pass filters after the 0–10 V to
0–2 V divider. For a single-pole model, the time constant is 7.96 ms and settling
to 0.1% takes about 55 ms. Nonoverlapping conversions remove software-induced
correlation; they do not prove physical statistical independence. Ideal filtered
white noise has correlation `exp(-2*pi*20*.050) ≈ .0019` between instantaneous
50 ms samples, but the actual detector/ADC chain needs measurement. A 20 Hz RC
is not a sharp anti-alias filter for a 20 Hz stream. Lowering converter rate can
change ADC noise filtering; validate timing, noise, aliasing, and actuator response
before changing the default. No inverse filter or physical settling correction
is applied. Source or external optical motion during a conversion remains visible.

## Uncertainty and failure-path audit

| Term | Implemented treatment | Interpretation / limit |
|---|---|---|
| ADC quantization | `q = 0.0625/sqrt(12)` mV | Uniform rounding over one code width has variance LSB²/12, hence RMS LSB/√12. This quantization floor is not the total ADC noise. |
| Measured dark noise | `sigma_read = max(q, dark.rms_mv)` | Empirical reading noise already includes ADC noise; do not add it twice. |
| Measured dark offset | `sigma_dark = sigma_read/sqrt(N_dark)` | `N_dark = duration_ms/50 - failed_samples`, using nearest sample rounding. Assumes independent samples. |
| Forced dark offset | `sigma_dark = supplied rms_mv`; `sigma_read = q` | Supplied offset uncertainty is not an empirical detector-noise measurement. |
| One net reading | `sigma_net = hypot(sigma_read, sigma_dark)` | Offset error is shared between records, not independent noise to average away. |
| Diagnostic window | `hypot(max(window.rms_mv,sigma_read)/sqrt(N_good), sigma_dark)` | Includes optical variation in window RMS; the dark floor does not shrink with that window. |
| PD power | `net_mV * 1e6 / (effective_V_per_A * responsivity_A_per_W)` nW, with wavelength coefficient | Clip negative power to zero, retain signed net voltage, and propagate NaN voltage. Error uses the same scale. Validated effective gain already contains the divider. |
| Laser power | Current-based calibration with `hypot(P*fractional_noise, constant_noise_mw)` | Defaults 3% plus a floor of 1% of nominal maximum power; this is an estimate, not an optical reading. |
| Attenuator | `sigma_T = T*ln(10)/10*hypot(hypot(rms1_db,rms2_db),hypot(electrical1_db,electrical2_db))` | Stored residual RMS estimates curve accuracy; electrical terms use the full local slope times `ATTENUATOR_FVOA_NOISE_RMS_MV / gain` (default 10 mV RMS after the amplifier). Assume independent contributions and devices. First-order symmetric error is approximate for large dB scatter. |
| Delivered power | `L*T*laser_route_tx`; quadrature of laser and attenuator errors | Source calibration terms remain correlated across stream records. |
| Route correction | Divide detected power **and its error** by `pd_route_tx`; multiply known source by `laser_route_tx` | Command applies launch and independent MM/SM return, latching losses at start. Passive light uses generic return defaults. No route-loss uncertainty terms are stored. |
| Unknown illumination | PD voltage, corrected nW/error and detector S/N remain defined; source/throughput fields are NaN/null | No known emitted power or wavelength is inferred from a passive MM/SM return. Nominal responsivity is used; any source-spectrum correction belongs above the PD owner. |
| Throughput | `tp = detected_corrected/delivered`; `tp_pd_err = sigma_detected/delivered`; `tp_err = hypot(tp_pd_err, tp*sigma_delivered/delivered)` | Derivative form works at zero PD power; source must be finite and positive. |
| Overrange | Raw ADC input ≥2000 mV sets `overrange`; retain numerical PD/TP value as nominal lower bound | PD and throughput errors become NaN/null, S/N suppressed. Source calibration error is still reported. The ADC rail remains 2047.9375 mV. |
| Missing ADC conversion | Discard; retain previous latest state without advancing its timestamp | No duplicate stream record or control move. Diagnostic windows count failures; zero-good-sample averages fail. Warnings are rate-limited. |
| Laser owner fault | Check operational health separately from the numerical estimate; stop affected acquisition | One failed read warns. Five seconds without a response while emitting faults; control/controller faults remain immediate. Failed shutdown retains its explicit retry obligation. |
| Actuator failure | Stop monitoring; preserve confirmed owner state after partial writes | Do not normalize subsequent readings using an assumed successful move. |
| Serialization | Binary doubles; JSON 12 significant digits, nonfinite values null | Preserve tiny powers/errors through Python, record arrays, and CSV. Binary and JSON share field order in firmware. |

Responsivity, effective gain, wavelength correction, route calibration, drift,
shot noise at illuminated levels, and dynamic actuator/filter mismatch have no
separately measured uncertainty terms in the present settings. Reported errors
therefore cover the terms above, not a complete absolute calibration budget.
Attenuator electrical variation is a static-model prediction along the curve;
model residuals describe uncertainty in that curve. Their combined uncertainty
is not temporal RMS. Calibration error remains correlated across records;
electrical independence between the two devices does not establish temporal
independence or justify dividing the combined error by sqrt(sample count).
No FVOA frequency-response or averaging correction is inferred from scope RMS.
The wavelength correction table is currently unity. Shot noise is not inferred
from dark data; brighter-light noise needs validation against captures.

Recapture dark after changing ADC cadence or converter rate. Saved dark records
store duration and failures, not their acquisition rate; old captures cannot be
reinterpreted exactly. Measured `dark.rms_mv` remains the reading scatter;
`dark.mean_net_err_mv` and `pd.dark_err_mv` now report the dark-mean uncertainty.
A capture stops throughput and its owned laser first, as attenuator calibration
does. It does not automatically resume monitoring or turn off unrelated manual
lasers. The notebook retains an explicit selected-laser-off cell.

## Historical exploratory noise model

The example below retains its original assumptions for review. Its `/2`
detector gains and HK `500 Hz` noise bandwidth do not describe the present ADC
input chain. The `noise_bandwidth` parameter is used as equivalent noise
bandwidth, which must not be confused with the RC cutoff. Reconcile the full
noise model before using it for predictions.

```python
import astropy.units as u
from astropy import constants as const
# Photodiode responsivity tables (A/W). We'll smooth with PCHIP to continuous curves.
FEMTO_QE_TC = {900.: 0.2*u.A/u.W, 1000.: 0.6*u.A/u.W, 1040.: 0.68*u.A/u.W, 1200.: 0.8*u.A/u.W,
               1270.: 0.85*u.A/u.W, 1430.: 0.93*u.A/u.W, 1500.: 0.95*u.A/u.W, 1600.: 0.93*u.A/u.W,
               1700.: 0.2*u.A/u.W}
FEMTO_QE_TC = tuple(map(u.Quantity, zip(*list(FEMTO_QE_TC.items()))))
THOR_QE_TC = {
    1400.: 0.50199*u.A/u.W, 1410.: 0.51144*u.A/u.W, 1420.: 0.51867*u.A/u.W, 1430.: 0.5264*u.A/u.W,
    1440.: 0.5337*u.A/u.W, 1450.: 0.54358*u.A/u.W, 1460.: 0.55372*u.A/u.W, 1470.: 0.5643*u.A/u.W,
    1480.: 0.5746*u.A/u.W, 1490.: 0.58604*u.A/u.W, 1500.: 0.59885*u.A/u.W, 1510.: 0.60971*u.A/u.W,
    1520.: 0.62102*u.A/u.W, 1530.: 0.63428*u.A/u.W, 1540.: 0.64785*u.A/u.W, 1550.: 0.66118*u.A/u.W,
    1560.: 0.67499*u.A/u.W, 1570.: 0.68843*u.A/u.W, 1580.: 0.70238*u.A/u.W, 1590.: 0.71497*u.A/u.W,
    1600.: 0.7285*u.A/u.W, 1610.: 0.74146*u.A/u.W, 1620.: 0.75481*u.A/u.W, 1630.: 0.76951*u.A/u.W,
    1640.: 0.78517*u.A/u.W, 1650.: 0.79927*u.A/u.W, 1660.: 0.81352*u.A/u.W, 1670.: 0.82736*u.A/u.W,
    1680.: 0.84172*u.A/u.W, 1690.: 0.85701*u.A/u.W, 1700.: 0.87061*u.A/u.W, 1710.: 0.88342*u.A/u.W,
    1720.: 0.89808*u.A/u.W, 1730.: 0.91253*u.A/u.W, 1740.: 0.92943*u.A/u.W, 1750.: 0.94613*u.A/u.W,
    1760.: 0.96487*u.A/u.W, 1770.: 0.98363*u.A/u.W, 1780.: 1.00287*u.A/u.W, 1790.: 1.02496*u.A/u.W,
    1800.: 1.04875*u.A/u.W, 1810.: 1.06712*u.A/u.W, 1820.: 1.08436*u.A/u.W, 1830.: 1.10028*u.A/u.W,
    1840.: 1.11462*u.A/u.W, 1850.: 1.12724*u.A/u.W, 1860.: 1.13955*u.A/u.W, 1870.: 1.14782*u.A/u.W,
    1880.: 1.15491*u.A/u.W, 1890.: 1.16097*u.A/u.W, 1900.: 1.16611*u.A/u.W, 1910.: 1.17542*u.A/u.W,
    1920.: 1.18509*u.A/u.W, 1930.: 1.18763*u.A/u.W, 1940.: 1.19081*u.A/u.W, 1950.: 1.19343*u.A/u.W,
    1960.: 1.19673*u.A/u.W, 1970.: 1.20183*u.A/u.W, 1980.: 1.20729*u.A/u.W, 1990.: 1.21037*u.A/u.W,
    2000.: 1.21345*u.A/u.W, 2010.: 1.21642*u.A/u.W, 2020.: 1.21931*u.A/u.W, 2030.: 1.22209*u.A/u.W,
    2040.: 1.22468*u.A/u.W, 2050.: 1.22843*u.A/u.W, 2060.: 1.23213*u.A/u.W, 2070.: 1.23456*u.A/u.W,
    2080.: 1.23699*u.A/u.W, 2090.: 1.23941*u.A/u.W, 2100.: 1.24186*u.A/u.W, 2110.: 1.24433*u.A/u.W,
    2120.: 1.24566*u.A/u.W, 2130.: 1.24771*u.A/u.W, 2140.: 1.24979*u.A/u.W, 2150.: 1.25045*u.A/u.W,
    2160.: 1.25089*u.A/u.W, 2170.: 1.25113*u.A/u.W, 2180.: 1.25137*u.A/u.W, 2190.: 1.25029*u.A/u.W,
    2200.: 1.24917*u.A/u.W, 2210.: 1.24931*u.A/u.W, 2220.: 1.24955*u.A/u.W, 2230.: 1.24985*u.A/u.W,
    2240.: 1.25024*u.A/u.W, 2250.: 1.25124*u.A/u.W, 2260.: 1.25234*u.A/u.W, 2270.: 1.25041*u.A/u.W,
    2280.: 1.2472*u.A/u.W, 2290.: 1.24611*u.A/u.W, 2300.: 1.24395*u.A/u.W, 2310.: 1.24038*u.A/u.W,
    2320.: 1.23692*u.A/u.W, 2330.: 1.23378*u.A/u.W, 2340.: 1.22827*u.A/u.W, 2350.: 1.22241*u.A/u.W,
}
THOR_QE_TC = tuple(map(u.Quantity, zip(*list(THOR_QE_TC.items()))))


class Detection:
    def __init__(self, levels, signal, noise, saturation, total_noise=False):
        self.levels = levels
        self.signal = signal
        self.noise = noise
        self.saturation = saturation
        self.snr = self.signal/(self.noise if total_noise else np.sqrt(self.signal+self.noise**2))
        self.saturation_mask = self.signal >= self.saturation

    def sn(self, saturated=np.nan, collapse=np.max, axis=0):
        if isinstance(self.snr, float):
            return self.snr if self.signal < self.saturation else np.nan

        snr = self.snr.copy()
        snr[self.saturation_mask] = saturated
        if snr.ndim ==1:
            return snr
        if collapse ==np.sum:
            return np.sqrt(collapse(snr**2, axis=axis))
        else:
            return collapse(snr, axis=axis)

    @property
    def has_saturation(self):
        return self.saturation_mask if isinstance(self.saturation_mask, bool) else self.saturation_mask.any()

class Detector(Component):
    def __repr__(self):
        return f"<{self.__class__.__name__} {self.name}>"

class Photodiode(Detector):

    def __init__(self, name: str,
                 noise = 7.5 * u.femtowatt / u.Hz ** 0.5,
                 gain = 1e11 * u.V/u.A,
                 saturation = 110 * u.picowatt,
                 adc_noise=62.5 * u.uV,
                 saturation_wavelength = 1550 * u.nm,
                 resp_wavelength_nm: "np.ndarray | None" = None,
                 noise_bandwidth:float=20*u.Hz,
                 sample_rate:float = 50 * u.Hz,
                 adc_gain:float = (2**15)/2.048/u.V,
                 resp_values: "np.ndarray | None" = None) -> None:
        super().__init__(name)
        self.in_p = self.add_port("in", PortDirection.IN)

        # Detector noise model (simple, scalar)
        self.noise = noise
        self.saturation = saturation
        self.adc_noise = adc_noise
        self.gain = gain
        self.resp_wavelength_nm = resp_wavelength_nm
        self.resp_values = resp_values.to(u.A/u.W) if resp_values is not None else None
        self.saturation_wavelength = saturation_wavelength.value
        self.noise_bandwidth = noise_bandwidth
        self.sample_rate = sample_rate
        self.adc_gain = adc_gain

        # responsivity in A/W
        self._resp_a_per_w = lambda grid_nm : np.interp(grid_nm, resp_wavelength_nm, self.resp_values.value).clip(0, np.inf)*u.A/u.W

    def observe(self, fluence: Spectrum, *, grid_nm: np.ndarray, texp_s: float = 1.0) -> "Detection":
        """
        Integrate electrons on a caller-supplied wavelength grid.

        Parameters
        ----------
        fluence : Spectrum
            Source spectrum. Its evaluate_on(grid) should yield photons/s/nm by default.
        grid_nm : array-like
            Wavelength grid in nm on which to evaluate.
        texp_s : float
            Exposure time in seconds.

        Returns
        -------
        Detection
            (levels, photons, noise, saturation_mask) — same structure you use today.
        """
        grid_nm = np.asarray(grid_nm, dtype=float)
        photons = fluence(grid_nm, photons=True)
        photon_energy = photons * (const.h * const.c / (grid_nm*u.nm).to(u.m))/u.s  # watts
        photon_noise_energy = np.sqrt(photons) * (const.h * const.c / (grid_nm * u.nm).to(u.m)) / u.s

        volts = ((photon_energy * self._resp_a_per_w(grid_nm)).sum() * self.gain).to('V')
        shot_noise_volts = ((photon_noise_energy * self._resp_a_per_w(grid_nm)).sum() * self.gain).to('V')

        device_noise_volts = self.noise*np.sqrt(self.noise_bandwidth) * self._resp_a_per_w(self.saturation_wavelength)  * self.gain

        # ((7.5e-15 * np.sqrt(20) * .95e11 * 1e3 / 2))
        # (2.048 / (2 ** 15) * 1e3)
        # adc_noise = ((7.5e-15*sqrt(20)*.95e11*1e3/2))/(2.048/(2**15)*1e3)

        total_noise = np.sqrt(device_noise_volts**2 + shot_noise_volts**2 + self.adc_noise**2).to(u.V)

        signal = self.adc_gain*volts.to(u.V)
        noise = self.adc_gain*total_noise.to(u.V)

        saturation_v = (self.saturation*self.gain*self._resp_a_per_w(self.saturation_wavelength)).to('V')
        saturation = np.floor(self.adc_gain*saturation_v)

        return Detection(levels=fluence, signal=signal.value, noise=noise.value, saturation=saturation.value, total_noise=True)

pd_yj = Photodiode("yj", resp_wavelength_nm=FEMTO_QE_TC[0], resp_values=FEMTO_QE_TC[1],
                          noise=7.5 * u.femtowatt / u.Hz ** 0.5,  # high-impedance termination
                          gain= 1e11 * u.V/u.A/2, #/2 because 50ohm termination
                          noise_bandwidth=20 * u.Hz,
                          saturation=110 * u.picowatt
                          )
pd_hk = Photodiode("hk", resp_wavelength_nm=THOR_QE_TC[0], resp_values=THOR_QE_TC[1],
                          noise=2.11 * u.picowatt / u.Hz ** 0.5 * 3.5,  # 50ohm termination,  3.5 is fudge based on plot in datasheet
                          gain=4750*u.kV/u.A/2,  #/2 because 50ohm termination and what we are using
                          saturation=1.706 * u.microwatt,
                          noise_bandwidth=500*u.Hz,
                          saturation_wavelength=2330 * u.nm
                          # technically saturation will happen about 20 mV sooner because of the bias offset
                          )
    

```

## Communication and power lifetime

Throughput fault stops also use the console/MQTT warning path, including when
the measurement detects an expired response deadline before background work runs.

The existing laser auto-off work probes the checked TEC-state register of started (including zero-current)
channels once per second, including channels whose shutdown failed. It does not
depend on heater mode, setpoint changes, or the 20 Hz measurement loop. Heater
control retains its ten-second cadence. Housekeeping's existing ambient work
reads the DS2408 port once per second and publishes all three logical outputs.
These intervals include work execution/scheduling overhead and are not hard
real-time deadlines. No new thread, workqueue, or retry loop is introduced.

A successful driver response refreshes the owner's five-second communication
deadline. Local calculations, requested settings, and a busy bus do not count as
responses. Maiman retains the last response timestamp and last error within each
operation, so a partial read can report its error without hiding successful
responses. Numerical laser estimates always use confirmed setpoints; they return
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

Relay and temperature 1-Wire transfers now use dedicated UARTs, eliminating the
GPIO driver's interrupt-masked waveforms. Housekeeping and the DS2408 driver
retain relay I/O serialization; Maiman no longer locks either 1-Wire bus.
Confirm sustained-loss shutdown, runtime margin, and the unexplained acquisition
gap using hardware captures.

### Laser zero level versus shutdown

A manual `laser value=0` is a zero-current update; it preserves driver readiness and
any existing auto-off deadline. `laser stop=true` is explicit shutdown. Captures and
notebook cleanup must use explicit stop when they intend shutdown. Laser auto-off
and measurement-owned expiry retain shutdown ownership at zero current. Temporary
zero levels keep PD collection alive, with undefined throughput at zero source power.
The [Maiman interface notes](api/maiman_laser.md#bench-transaction-timing-diagnostics)
describe application transaction and quiet-interval logs, and their measurement
limits, for the next bench capture.
