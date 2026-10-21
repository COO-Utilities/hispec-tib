/* Small, allocation-free double-precision solves shared by optical calibrations. */
#ifndef HISPEC_CALIBRATION_FIT_H
#define HISPEC_CALIBRATION_FIT_H

#include <stdbool.h>

struct calibration_normal {
	double cost;
	/* Symmetric J'J and J'r, in the caller's parameter coordinates. */
	double h00;
	double h01;
	double h11;
	double g0;
	double g1;
};

/* Evaluate the entire model, optionally accumulating J'J and J'r. The caller
 * owns weights, analytic derivatives, profiling and cancellation (-ECANCELED).
 * A cost-only invalid trial may return -ERANGE; other errors stop the solve.
 */
typedef int (*calibration_evaluate_fn)(void *context, const double p[2],
				     bool derivatives, struct calibration_normal *normal);

/* Solve a positive-definite symmetric 2x2 system. No damping is added here. */
int calibration_solve_2x2(double h00, double h01, double h11,
			double rhs0, double rhs1, double result[2]);

/* Thirty damped analytic-Jacobian Gauss-Newton iterations, preserving the
 * attenuator fitter's damping schedule and component-wise bounded trials.
 * p is the initial guess and last accepted result. Tolerances use each
 * parameter's units. Reaching the iteration limit is not an error or a physical
 * acceptance decision. Pure CPU work; the evaluator owns cancellation checks.
 */
int calibration_fit_2(calibration_evaluate_fn evaluate, void *context,
		      const double lower[2], const double upper[2],
		      const double tolerance[2], double p[2]);

#endif
