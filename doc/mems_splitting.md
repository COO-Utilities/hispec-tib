# MEMS routing and three-way splitting

MEMS switches latch an optical path after an electrical pulse. Firmware states
`A` and `B` name the two control choices; they must not be confused with optical
port letters on the component. The [hardware diagrams](hardware_profiles.md)
show API and PCB names. `mems/split` is available on AS, where synchronized
switching divides each channel's light among three outputs in time.

## Loss-corrected time fractions

Let requested delivered-power fractions be $r_1,r_2,r_3$, summing to one, and
saved per-output transmissions be $t_i>0$. The ideal time fractions are

```{math}
d_i=\frac{r_i/t_i}{\sum_{j=1}^3r_j/t_j}.
```

This is the normalized inverse-loss solution used by
`split_correct_for_transmission()`. It assumes source power is constant through
the cycle and ignores switching transients. The route's first switch uses
selected-state fraction $d_1$, the middle switch stays in its route state, and
the third uses $d_1+d_2$. Aligned boundaries then give output intervals
$d_1,d_2,d_3$; independent unsynchronized toggle duty cycles would not do this.

## Quantization and actuation limits

AS uses 20 ms pulse units. The maximum five actuation pulses per second requires
at least $m=\lceil200/20\rceil=10$ units between opposite pulses on a switch.
For boundary fractions $b\in\{d_1,d_1+d_2\}$ that are strictly between zero and
one, automatic period selection uses

```{math}
N=\left\lceil\max\left(2m,\max_b\frac{m}{b},
                                      \max_b\frac{m}{1-b}\right)\right\rceil.
```

With an explicit `cycle_ms`, the period is rounded to 20 ms units instead of
expanded. Each mixed boundary is rounded and clamped:

```{math}
k(b)=\operatorname{clip}(\lfloor bN+0.5\rfloor,m,N-m).
```

Exact zero/one boundaries remain 0/$N$ and require no toggling. A mixed cycle
shorter than $2m$ is rejected. Let $k_1=k(d_1)$ and
$k_3=\max[k_1,k(d_1+d_2)]$. Reported time fractions and predicted delivered ratios
are

```{math}
a=\frac{1}{N}(k_1,k_3-k_1,N-k_3),\qquad
o_i=\frac{a_i t_i}{\sum_j a_jt_j}.
```

Thus `requested`, `actual` and `output` answer different questions. A small
middle interval can quantize to zero even though each switch obeys its own
actuation spacing. Read the result instead of assuming exact requested ratios.
For equal requested thirds and transmissions $(1,0.5,1)$, ideal duties are
$(0.25,0.5,0.25)$, automatic $N=40$ (800 ms), and the delivered ratios are exactly
one third in the ideal model. With equal transmissions the automatic 600 ms
period has ten units per output.

Direct `mems/switch` toggling uses the same per-switch spacing rule, with 20 ms
FFSW or 50 ms FFLS pulse units. Its reported `duty_cycle` is the **A-state**
fraction; split per-switch readback is the fraction in the route's selected state.
A fixed requested period can clamp duty; an automatic period grows to realize
extreme ratios. Static `force` requests coalesce to one pulse.

## Ownership, expiry and persistence

The router thread owns GPIO-expander pulses and cleanup, using kernel-uptime
deadlines. Commands update intent under the router lock and return before the
next physical pulse. State is last commanded/pulsed intent, not optical feedback.
At expiry toggling stops; status/route readback derives from switch state.
Late service clears pulses and skips stale transitions rather than replaying a
burst. I2C failure can leave a route partially applied and is reported without
pretending earlier successful steps were rolled back.

Commands persist static intent or the original toggle/split request once.
Per-tick progress is not written to flash. If boot resume is enabled, it restarts
the requested duration, not the remaining pre-reset interval. See
[settings](settings.md), [wire specification](commands.md#memssplit), and
[C contracts](api/mems_switching.md).
