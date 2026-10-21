# Calibration fitting

`calibration_fit.c` is the allocation-free, double-precision two-parameter solver
shared by [attenuator](attenuator_calibration.md) and
[laser](laser_calibration.md) calibration. Each caller owns its physical model,
weights, analytic derivatives, parameter scaling, cancellation and acceptance.
A numerical return of success is not proof of convergence or physical validity.

For weighted residuals $r_i(p)=\sqrt{w_i}[m_i(p)-y_i]$ and the caller's local
Jacobian $J_{ij}=\sqrt{w_i}\,\partial m_i/\partial p_j$, treating weights as fixed
for that derivative, the evaluator supplies

```{math}
F=\sum_i r_i^2,\qquad H=J^T J,\qquad g=J^T r.
```

At the accepted point, the solver calculates

```{math}
(H+\lambda I)\Delta=-g,\qquad
p'_j=\operatorname{clip}(p_j+\Delta_j,l_j,u_j).
```

For a symmetric $2\times2$ system $M x=b$, it requires finite positive
determinant $D=M_{00}M_{11}-M_{01}^2$ and returns

```{math}
x_0=\frac{M_{11}b_0-M_{01}b_1}{D},\qquad
x_1=\frac{M_{00}b_1-M_{01}b_0}{D}.
```

Damping starts at $10^{-3}$. A finite, strictly lower cost accepts the bounded
trial and multiplies damping by 0.3, down to $10^{-12}$. Otherwise the accepted
point remains unchanged and damping grows by ten, up to $10^{12}$. An accepted
step smaller than both caller-supplied component tolerances stops the iteration.
The limit is 30 iterations; reaching it returns the best accepted parameters with
success, leaving physical acceptance to the caller. A cost-only `-ERANGE` trial
can be retried with more damping; cancellation and other errors terminate.

The diagonal damping is in the caller's coordinates, so changing parameter units
changes the numerical problem. Attenuator calibration holds the inferred floor
fixed while fitting two shape parameters, then uses its own correction solver.
Laser calibration profiles each segment gain analytically before fitting shared
threshold and baseline, then solves nonnegative noise variances separately.
These are distinct objectives, not interchangeable uses of one generic fit.

Numerical work temporarily uses priority 14 with the calibration control mutex
released. Stop/start cancels and joins before reusing the shared dataset/scratch.
No heap, hardware I/O or NVS write belongs to this solver; accepted results are
applied and optionally persisted by the calibration owner. See the
[C contracts](api/calibration.md).
