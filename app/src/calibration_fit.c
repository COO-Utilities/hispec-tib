#include "calibration_fit.h"

#include <errno.h>
#include <math.h>

int calibration_solve_2x2(double h00, double h01, double h11,
			double rhs0, double rhs1, double result[2])
{
	double det = h00 * h11 - h01 * h01;
	if (!(det > 0.0) || !isfinite(det)) return -ERANGE;
	result[0] = (h11 * rhs0 - h01 * rhs1) / det;
	result[1] = (h00 * rhs1 - h01 * rhs0) / det;
	return isfinite(result[0]) && isfinite(result[1]) ? 0 : -ERANGE;
}

int calibration_fit_2(calibration_evaluate_fn evaluate, void *context,
		      const double lower[2], const double upper[2],
		      const double tolerance[2], double p[2])
{
	struct calibration_normal n = {0};
	double lambda = 1.0e-3;
	int rc = evaluate(context, p, false, &n);
	if (rc != 0 || !isfinite(n.cost)) return rc != 0 ? rc : -ERANGE;
	double cost = n.cost;
	for (unsigned iter = 0; iter < 30; ++iter) {
		n = (struct calibration_normal){0};
		rc = evaluate(context, p, true, &n);
		if (rc != 0) return rc;
		double delta[2], trial[2];
		rc = calibration_solve_2x2(n.h00 + lambda, n.h01, n.h11 + lambda,
					 -n.g0, -n.g1, delta);
		if (rc != 0) return rc;
		for (unsigned k = 0; k < 2; ++k) {
			trial[k] = fmin(fmax(p[k] + delta[k], lower[k]), upper[k]);
		}
		n = (struct calibration_normal){0};
		rc = evaluate(context, trial, false, &n);
		if (rc != 0 && rc != -ERANGE) return rc;
		if (rc == 0 && isfinite(n.cost) && n.cost < cost) {
			bool converged = fabs(trial[0] - p[0]) < tolerance[0] &&
				fabs(trial[1] - p[1]) < tolerance[1];
			p[0] = trial[0];
			p[1] = trial[1];
			cost = n.cost;
			if (converged) break;
			lambda = fmax(lambda * 0.3, 1.0e-12);
		} else {
			lambda = fmin(lambda * 10.0, 1.0e12);
		}
	}
	return 0;
}
