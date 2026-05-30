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
                 adc_noise=0.187 * u.uV,
                 saturation_wavelength = 1550 * u.nm,
                 resp_wavelength_nm: "np.ndarray | None" = None,
                 noise_bandwidth:float=20*u.Hz,
                 sample_rate:float = 50 * u.Hz,
                 adc_gain:float = (2**16-1)/(2*6.144)/u.V,
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
        # (2 * 6.144 / (2 ** 16 - 1) * 1e3)
        # adc_noise = ((7.5e-15*sqrt(20)*.95e11*1e3/2))/(2*6.144/(2**16-1)*1e3)

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