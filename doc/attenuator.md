# Attenuator model and control

A logical attenuator contains two FVOAs in series. DAC voltage drives an amplifier
and then a MEMS shutter across the optical mode. Device-to-device turn-on variation
makes a fixed datasheet voltage schedule inadequate for calibration. The model
approximates a Gaussian mode clipped by a moving edge, with a finite leakage floor
and an empirical correction. [Automatic calibration](attenuator_calibration.md)
measures the two devices separately using overlapping detector ranges.

## Voltage, transmission and dB

Let $v$ be DAC output in mV, $g$ the amplifier gain (default 1.533), $v_{50}$
`fvoa_50pct_mv`, $s$ `slope_inv_fvoa_mv`, and $L$ `max_atten_db`.
The FVOA sees $u=gv$ mV. With $E=\operatorname{erf}(4)$:

```{math}
h(v)=\frac{E-\operatorname{erf}[s(gv-v_{50})]}{2E},\qquad
q(v)=\operatorname{clip}\left(\frac{h(v)}{h(0)},0,1\right),
```

```{math}
\ell=10^{-L/10},\qquad T_0(v)=\ell+(1-\ell)q(v),\qquad
B(v)=-10\log_{10}T_0(v).
```

Open normalization makes $T_0(0)=1$. Consequently $v_{50}$ is the half-shutter
coordinate of the underlying curve, not an exact 3.0103 dB point after open
normalization, leakage and correction. Runtime evaluates an equivalent
complementary-error-function difference to preserve the small tail in float.

The correction uses Chebyshev polynomials $T_i(x)$ (distinct from transmission),
with $T_0(x)=1$, $T_1(x)=x$ and $T_{i+1}(x)=2xT_i(x)-T_{i-1}(x)$:

```{math}
B_s=-10\log_{10}(0.99),\quad
t=\operatorname{clip}\left(\frac{B-B_s}{L-B_s},0,1\right),\quad x=2t-1,
\qquad C(B)=t(1-t)\sum_{i=0}^{5}c_iT_i(x).
```

Within the supported range, $A(v)=B(v)+C[B(v)]$ and $T(v)=10^{-A(v)/10}$.
Let $B_c+C(B_c)=A_c$, where $A_c$ is `max_calibrated_db`. Above this boundary
the correction continues as

```{math}
C_{\rm tail}(B)=C(B_c)\frac{L-B}{L-B_c}.
```

This joins continuously and returns to the leakage floor; the derivative can
have a corner. Values beyond the calibrated endpoint remain estimates.
`max_atten_db` is the asymptotic model floor, `max_calibrated_db` the supported
operating ceiling, and the finite DAC limit may reach neither exactly.

## Inverse commands and quantization

A `level` request is linear transmission: convert it to $A=-10\log_{10}T$,
allocate total dB between devices, then invert each monotonic model. The firmware
uses 24 bisections over the reachable DAC span, with endpoint comparisons tolerant
to float roundoff (four ULPs, at least $10^{-6}$ dB). It writes the nearest legal
12-bit DAC code and reports the forward model at that confirmed code.

```{math}
v_{\rm DAC}=3300\,\frac{n}{4096}\ {\rm mV},\quad n\in\{0,\ldots,4095\},
\qquad u=g v_{\rm DAC}.
```

For example, code 2048 means 1650 mV DAC and 2529.45 mV FVOA drive at gain 1.533.
A millivolt command takes the direct code path; the readback dB/level is still a
model estimate. A requested dB need not be exactly representable by a code.

Adding total dB uses the less-attenuated device until balanced, then shares the
remaining change. Removing dB starts with the more-attenuated device. Thus
$(36,0)\to41$ dB gives $(36,5)$, and $(39,35)\to40$ dB gives $(20,20)$.
An unchanged total makes no allocation change. This avoids driving one FVOA
straight into its poorly constrained plateau. Automatic control can rebalance
opposing directions when first entering the calibrated range; ordinary moves
keep both devices in the requested direction. Partial DAC writes preserve
confirmed successes and report failure, without retry or rollback.

## Transmission uncertainty

For each FVOA, let $r_i$ be stored fit `rms_db` and $e_i$ the electrical dB error.
The configured drive noise is **10 mV RMS after the amplifier** by default:

```{math}
e_i=\left|\frac{dA_i}{dv_{\rm DAC}}\right|\frac{\sigma_{u_i}}{g_i},\qquad
\sigma_A=\sqrt{r_1^2+r_2^2+e_1^2+e_2^2},\qquad
\sigma_T=T\frac{\ln10}{10}\sigma_A.
```

The derivative includes the empirical correction and continuation. Model, device
and electrical contributions are assumed independent in this first-order estimate.
Large dB scatter need not produce a symmetric transmission distribution. Stored
curve RMS is not temporal noise; it does not average away with stream length.
No frequency-response or averaging correction is inferred from scope RMS.
The [Python noise tools](python_helpers.md#noise-calculations) can evaluate
separate measured drive RMS values without changing firmware settings.

## Implementation contracts

`attenuator.c` owns DAC I/O, confirmed voltage, allocation and model evaluation.
Pure estimation copies confirmed state under a short mutex and performs no I/O.
Physical writes serialize I2C operations; coefficient persistence belongs to
`app_settings.c`. [C reference](api/attenuator_control.md) documents the entry
points; [commands](commands.md#atten) defines the wire interface.
