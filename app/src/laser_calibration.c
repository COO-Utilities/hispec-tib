/**
 * Reference-anchored laser calibration on the existing throughput thread.
 *
 * Same-current overlaps normalize fixed-attenuation segments after subtracting
 * this run's zero-current baseline. The measured vendor reference fixes power
 * scale without assuming linearity there. A reference-anchored quadratic uses
 * the shared 2x2 solver; threshold is its zero and efficiency is the average
 * slope from threshold to reference. The validated operating floor excludes the
 * knee. This empirical shape cannot distinguish diode curvature from PD error.
 * The reference also measures launch+return loss; configured return and external
 * loopback losses are subtracted once to update the selected launch route.
 *
 * Acquisition facts survive rejected fits and stop/error cleanup until either
 * calibration starts again. Temporal variance and mean-curve residuals remain
 * separate. Effective noise can include the illuminated PD and FVOAs; the
 * assumed electrical FVOA noise and static fit RMS are not subtracted from it.
 * Validate settling and these physical assumptions on the bench before treating
 * inferred loss/noise as an independently measured component property.
 */
#include "laser_calibration.h"
#include "calibration_fit.h"
#include "calibration_storage.h"
#include "attenuator_calibration.h"
#include "app_settings.h"
#include "app_output.h"
#include "devices.h"
#include "housekeeping.h"
#include "mems_switching.h"
#include "throughput_monitor.h"

#include <errno.h>
#include <float.h>
#include <math.h>
#include <string.h>
#include <zephyr/sys/byteorder.h>
#include <coo_commons/json_utils.h>

LOG_MODULE_REGISTER(laser_calibration, LOG_LEVEL_INF);

/* File-local views into the shared unions; these aliases allocate no storage.
 * calibration_lock protects acquisition and replacement. During fitting, the
 * worker owns these views until a cancelling command joins it via fit_done. */
#define data calibration_data.laser
#define meta data.meta
#define result data.result
#define work calibration_scratch.laser
#define RECORDS CALIBRATION_RECORD_COUNT
#define SETTLE_MS 100U
#define PEAK_TARGET_MV 1850.0
#define Q_VARIANCE (0.0625 * 0.0625 / 12.0) /* Uniform 0.0625-mV ADC quantization. */
#define OPT_LOSS 1U
#define OPT_NOISE 2U
#define OPT_OTHER 4U
#define OPT_PERSIST 8U
#define OPT_APPLY 16U
#define OTHER_RECORDS 24U
#define WIRE_RECORD_SIZE 24U
#define WIRE_CHUNK_RECORDS (COO_CMD_PAYLOAD_MAX / WIRE_RECORD_SIZE)
#define WIRE_METADATA_SIZE 432U

/* Internal reasons preserve the originating guard instead of guessing a cause
 * from ERANGE/EIO in command glue. Only strings leave the calibration API. */
enum calibration_error {
	CAL_INVALID_REQUEST = -2000,
	CAL_REFERENCE_RANGE,
	CAL_DARK_REQUIRED,
	CAL_DARK_BUSY,
	CAL_PD_OFF,
	CAL_ROUTE_UNAVAILABLE,
	CAL_PD_GAIN_INVALID,
	CAL_DARK_UNAVAILABLE,
	CAL_OVERLAP_UNAVAILABLE,
	CAL_REFERENCE_UNAVAILABLE,
	CAL_INSUFFICIENT_POINTS,
	CAL_FIT_FAILED,
	CAL_NONMONOTONIC,
	CAL_THRESHOLD_RANGE,
	CAL_INSUFFICIENT_PRECISION,
	CAL_NONLINEAR,
	CAL_NOISE_FIT_FAILED,
	CAL_LOSS_FIT_FAILED,
	CAL_ATTEN_RANGE,
	CAL_STALE_RUN,
	CAL_NOT_ACCEPTED,
	CAL_CORE_NOT_APPLIED,
	CAL_LOSS_NOT_ACCEPTED,
	CAL_NOISE_NOT_ACCEPTED,
	CAL_INVALID_SETTINGS,
	CAL_SOURCE_STOPPED,
	CAL_NO_SAMPLES,
	CAL_TEMPERATURE_TIMEOUT,
	CAL_WINDOW_TIMEOUT,
	CAL_PD_RANGE,
	CAL_ATTEN_WRITE,
	CAL_CURRENT_WRITE,
	CAL_TEMPERATURE_UNAVAILABLE,
	CAL_ROUTE_WRITE,
};

const char *laser_calibration_error_message(int error)
{
	switch (error) {
	case 0: return NULL;
	case CAL_INVALID_REQUEST: return "invalid calibration options";
	case CAL_REFERENCE_RANGE: return "reference outside current or temperature limits";
	case CAL_DARK_REQUIRED: return "dark measurement required";
	case CAL_DARK_BUSY: return "dark measurement in progress";
	case CAL_PD_OFF: return "pd off";
	case CAL_ROUTE_UNAVAILABLE: return "loopback route unavailable";
	case CAL_PD_GAIN_INVALID: return "invalid pd gain or return transmission";
	case CAL_DARK_UNAVAILABLE: return "usable same-run dark required";
	case CAL_OVERLAP_UNAVAILABLE: return "usable range overlap required";
	case CAL_REFERENCE_UNAVAILABLE: return "three usable reference windows required";
	case CAL_INSUFFICIENT_POINTS: return "six distinct usable currents required";
	case CAL_FIT_FAILED: return "fit failed";
	case CAL_NONMONOTONIC: return "fit is not increasing";
	case CAL_THRESHOLD_RANGE: return "fit threshold outside usable range";
	case CAL_INSUFFICIENT_PRECISION: return "insufficient precision for residual limit";
	case CAL_NONLINEAR: return "too non-linear";
	case CAL_NOISE_FIT_FAILED: return "noise fit failed";
	case CAL_LOSS_FIT_FAILED: return "static loss fit failed";
	case CAL_ATTEN_RANGE: return "reference outside calibrated attenuation";
	case CAL_STALE_RUN: return "calibration run was replaced";
	case CAL_NOT_ACCEPTED: return "core fit not accepted";
	case CAL_CORE_NOT_APPLIED: return "core fit not applied";
	case CAL_LOSS_NOT_ACCEPTED: return "requested route loss not accepted";
	case CAL_NOISE_NOT_ACCEPTED: return "noise fit not accepted";
	case CAL_INVALID_SETTINGS: return "fitted settings are invalid";
	case CAL_SOURCE_STOPPED: return "laser stopped during acquisition";
	case CAL_NO_SAMPLES: return "no valid pd samples";
	case CAL_TEMPERATURE_TIMEOUT: return "reference temperature not reached";
	case CAL_WINDOW_TIMEOUT: return "pd window timed out";
	case CAL_PD_RANGE: return "usable pd range not found";
	case CAL_ATTEN_WRITE: return "attenuator write failed";
	case CAL_CURRENT_WRITE: return "laser current write failed";
	case CAL_TEMPERATURE_UNAVAILABLE: return "laser temperature unavailable";
	case CAL_ROUTE_WRITE: return "loopback route change failed";
	case -ECANCELED: return "stopped";
	case -EBUSY: return "calibration or hardware busy";
	case -ENODATA: return "no retained laser calibration";
	case -ENOSPC: return "calibration buffer full";
	case -EPERM: return "laser bank off";
	/* Unclassified failures retain their actual symbolic errno. Do not label
	 * every numeric-range failure ERANGE or invent a more specific diagnosis. */
#define UNEXPECTED(name) case -name: return "unexpected error (" #name ")"
	UNEXPECTED(EINVAL);
	UNEXPECTED(ERANGE);
	UNEXPECTED(EIO);
	UNEXPECTED(EACCES);
	UNEXPECTED(ENODEV);
	UNEXPECTED(ENOENT);
	UNEXPECTED(ENOMEM);
	UNEXPECTED(EAGAIN);
	UNEXPECTED(ETIMEDOUT);
	UNEXPECTED(ENOTSUP);
	UNEXPECTED(EADDRNOTAVAIL);
#undef UNEXPECTED
	default: return "unexpected error";
	}
}

/* Flags: classification in bits 0..1, event in 2..4, output in bit 7. */
enum point_class {
	POINT_OK,
	POINT_SATURATED,
	POINT_LOW,
	POINT_ADC_ERROR,
};

enum point_event {
	EVENT_SWEEP,
	EVENT_PROBE,
	EVENT_OVERLAP,
	EVENT_REFERENCE,
	EVENT_DARK,
	EVENT_REFINE,
};

/* Phase says what the worker is waiting for; stage says which observation the
 * sweep needs next. Temperature drift changes phase without losing the stage. */
enum phase {
	PHASE_IDLE,
	PHASE_WARM,
	PHASE_WINDOW,
	PHASE_FIT,
	PHASE_PAUSE,
};

enum stage {
	STAGE_DARK,
	STAGE_SWEEP,
	STAGE_REFINE,
	STAGE_END,
	STAGE_OTHER_DARK,
	STAGE_OTHER_REFERENCE,
};

static struct {
	enum phase phase;
	enum stage stage;
	int64_t window_started_ms;
	int64_t temperature_started_ms;
	int64_t next_temperature_ms;
	double atten_db;
	double search_low_db;
	double search_high_db;
	double refine_low_ma;
	double refine_high_ma;
	uint8_t attenuator;
	uint8_t grid_count;
	uint8_t grid_index;
	uint8_t segment;
	uint8_t segments;
	uint8_t event;
	uint8_t search_tries;
	uint8_t search_limit;
	uint8_t stable_temperatures;
	uint8_t reference_count;
	uint8_t refine_count;
	uint8_t refine_index;
	bool searching;
	bool refined;
} run;

/* Readers see only a complete published snapshot, including while the fitter
 * updates result without calibration_lock. Lock order is calibration then status. */
static struct laser_calibration_status status_snapshot;
static K_MUTEX_DEFINE(status_lock);
static atomic_t fit_cancel;
static K_SEM_DEFINE(fit_done, 0, 1);

/** Decode which output owns a retained window; no state changes. */
static bool is_other(const struct laser_cal_record *r)
{
	return (r->flags & 0x80U) != 0;
}

/** Decode the acquisition event, excluding classification and output bits. */
static unsigned event_of(const struct laser_cal_record *r)
{
	return (r->flags >> 2) & 7U;
}

/** Convert the stored driver-grid current to commanded mA, not corrected mA. */
static double current_of(const struct laser_cal_record *r)
{
	return (double)r->current_tenth_ma / 10.0;
}

/* Per-reading empirical variance; the PD stores a population RMS. One surviving
 * ADC sample still supplies a mean, but cannot supply a temporal variance. */
static double reading_variance(const struct laser_cal_record *r)
{
	return r->good > 1 ? (double)r->rms_mv * (double)r->rms_mv * r->good / (r->good - 1U) : 0.0;
}

/* Inverse variance of the window mean. The measured off-state floor prevents
 * a quiet or single-sample window from acquiring unbounded statistical weight. */
static double mean_weight(const struct laser_cal_record *r)
{
	return r->good / fmax(reading_variance(r), work.dark_variance);
}

/* Only settled, usable sweep windows identify the current curve. Ranging probes
 * have no accepted segment gain; dark and other-output windows have separate fits. */
static bool curve_point(const struct laser_cal_record *r)
{
	return !is_other(r) && (r->flags & 3U) == POINT_OK && r->good > 0 &&
		event_of(r) != EVENT_PROBE && event_of(r) != EVENT_DARK &&
		r->segment < LASER_CAL_SEGMENTS && isfinite(r->net_mv);
}

/* Estimate an empirical off-state reading variance, then constrain the shared
 * residual baseline with off-state means. Configured dark uncertainty is shared
 * and cancels against these same-run measurements; do not average it away. */
static int fit_baseline(bool other, double *baseline, double *weight)
{
	double variance_sum = 0.0;
	double dof = 0.0;
	double sum = 0.0;

	*weight = 0.0;
	/* Pool within-window variance first; drift between means is not temporal
	 * noise within a window and must not inflate this floor. */
	for (uint8_t i = 0; i < result.records; ++i) {
		const struct laser_cal_record *r = &data.records[i];
		if (is_other(r) != other || event_of(r) != EVENT_DARK || r->good == 0 ||
		    (r->flags & 3U) == POINT_SATURATED) {
			continue;
		}
		if (r->good > 1) {
			variance_sum += reading_variance(r) * (r->good - 1U);
			dof += r->good - 1U;
		}
	}
	/* Means with one surviving sample remain retained, but cannot establish
	 * the empirical off-state variance needed for fit uncertainties. */
	if (!(dof > 0.0)) {
		return -ERANGE;
	}
	work.dark_variance = fmax(variance_sum / dof, Q_VARIANCE);

	/* Now combine dark means using the same mean-weight rule as the curve. */
	for (uint8_t i = 0; i < result.records; ++i) {
		const struct laser_cal_record *r = &data.records[i];
		if (is_other(r) != other || event_of(r) != EVENT_DARK || r->good == 0 ||
		    (r->flags & 3U) == POINT_SATURATED) {
			continue;
		}

		double w = mean_weight(r);
		*weight += w;
		sum += w * (double)r->net_mv;
	}
	if (!(*weight > 0.0)) {
		return -ERANGE;
	}

	*baseline = sum / *weight;
	return 0;
}

/* Normalize only through measured same-current overlaps. This removes the
 * unknown fixed loss and FVOA scale without assuming a linear current curve.
 * The three reference windows then set the absolute mW scale. No I/O. */
static int normalize_curve(void)
{
	for (unsigned i = 0; i < RECORDS; ++i) {
		work.power_mw[i] = NAN;
		work.variance_mw2[i] = NAN;
	}
	for (unsigned j = 0; j < LASER_CAL_SEGMENTS; ++j) {
		work.gain[j] = NAN;
	}
	work.baseline_mean = NAN;
	work.dark_variance = NAN;
	int rc = fit_baseline(false, &work.baseline_mean, &work.baseline_weight);
	if (rc != 0) {
		return CAL_DARK_UNAVAILABLE;
	}

	/* Gains are initially relative to segment zero. Each new segment starts
	 * at the previous segment's last current; repeats at that level are pooled. */
	work.gain[0] = 1.0;
	for (unsigned j = 1; j < LASER_CAL_SEGMENTS; ++j) {
		int first = -1;
		for (uint8_t i = 0; i < result.records; ++i) {
			if (curve_point(&data.records[i]) && data.records[i].segment == j) {
				first = i;
				break;
			}
		}
		if (first < 0) {
			continue; /* An entirely below-SNR segment supplies no fit points. */
		}

		double sum[2] = {0};
		double weight[2] = {0};
		for (uint8_t i = 0; i < result.records; ++i) {
			const struct laser_cal_record *r = &data.records[i];
			if (!curve_point(r) || r->current_tenth_ma != data.records[first].current_tenth_ma ||
			    (r->segment != j && r->segment != j - 1U)) {
				continue;
			}
			unsigned side = r->segment == j ? 1U : 0U;
			double w = mean_weight(r);
			sum[side] += w * ((double)r->net_mv - work.baseline_mean);
			weight[side] += w;
		}
		if (!(sum[0] > 0.0) || !(sum[1] > 0.0) ||
		    !(weight[0] > 0.0) || !(weight[1] > 0.0) || !isfinite(work.gain[j - 1U])) {
			return CAL_OVERLAP_UNAVAILABLE;
		}
		work.gain[j] = work.gain[j - 1U] * (sum[1] / weight[1]) / (sum[0] / weight[0]);
	}

	double reference_sum = 0.0;
	double reference_weight = 0.0;
	unsigned references = 0;
	for (uint8_t i = 0; i < result.records; ++i) {
		const struct laser_cal_record *r = &data.records[i];
		if (!curve_point(r) || event_of(r) != EVENT_REFERENCE ||
		    fabs(current_of(r) - meta.ref_ma) > 0.05 || !(work.gain[r->segment] > 0.0)) {
			continue;
		}
		double scale = work.gain[r->segment];
		double w = mean_weight(r) * scale * scale;
		reference_sum += w * ((double)r->net_mv - work.baseline_mean) / scale;
		reference_weight += w;
		++references;
	}
	if (references < 3 || !(reference_sum > 0.0) || !(reference_weight > 0.0)) {
		return CAL_REFERENCE_UNAVAILABLE;
	}

	double reference_mv = reference_sum / reference_weight;
	for (unsigned j = 0; j < LASER_CAL_SEGMENTS; ++j) {
		work.gain[j] *= reference_mv / meta.ref_mw;
	}
	for (uint8_t i = 0; i < result.records; ++i) {
		const struct laser_cal_record *r = &data.records[i];
		if (!curve_point(r) || !(work.gain[r->segment] > 0.0)) {
			continue;
		}
		double gain = work.gain[r->segment];
		work.power_mw[i] = ((double)r->net_mv - work.baseline_mean) / gain;
		work.variance_mw2[i] = 1.0 / mean_weight(r) / (gain * gain);
	}
	return 0;
}

/* Fit P_ref + c1*(T1(z)-T1(z_ref)) + c2*(T2(z)-T2(z_ref)). Scaling
 * current to [-1,1] keeps the two-column normal equation well conditioned.
 * This is a direct solve, not a nonlinear optimizer or an assumption that the
 * reference lies on a straight line. Writes diagnostics even on rejection. */
static int fit_interval(double low, double high, bool quadratic,
			struct laser_calibration_fit *fit)
{
	memset(fit, 0, sizeof(*fit));
	fit->baseline_mv = work.baseline_mean;
	fit->floor_ma = low;
	fit->max_ma = high;
	fit->threshold_se_ma = NAN;
	fit->rms_fraction = NAN;
	fit->max_residual_fraction = NAN;

	double center = 0.5 * (low + high);
	double scale = 0.5 * (high - low);
	double z_ref = (meta.ref_ma - center) / scale;
	double h00 = 0.0, h01 = 0.0, h11 = 0.0;
	double g0 = 0.0, g1 = 0.0;
	for (uint8_t i = 0; i < result.records; ++i) {
		const struct laser_cal_record *r = &data.records[i];
		double current = current_of(r);
		if (!isfinite(work.power_mw[i]) || current < low || current > high) {
			continue;
		}
		double z = (current - center) / scale;
		double b0 = z - z_ref;
		double b1 = 2.0 * (z * z - z_ref * z_ref);
		double w = 1.0 / (double)work.variance_mw2[i];
		double y = (double)work.power_mw[i] - meta.ref_mw;
		h00 += w * b0 * b0;
		h01 += w * b0 * b1;
		h11 += w * b1 * b1;
		g0 += w * b0 * y;
		g1 += w * b1 * y;
		++fit->points;
	}
	if (!(h00 > 0.0) || !isfinite(h00)) {
		return CAL_FIT_FAILED;
	}

	double c[2] = {g0 / h00, 0.0};
	if (quadratic) {
		double s0 = sqrt(h00);
		double s1 = sqrt(h11);
		double cross = h01 / s0 / s1;
		if (!(1.0 - cross * cross > 1e-12) ||
		    calibration_solve_2x2(1.0, cross, 1.0, g0 / s0, g1 / s1, c) != 0) {
			return CAL_FIT_FAILED;
		}
		c[0] /= s0;
		c[1] /= s1;
	}

	/* Solve for the increasing branch's zero using a rationalized root.
	 * Store average efficiency through the reference, never the tangent at
	 * threshold. Round q to its actual settings precision before scoring. */
	double q = 2.0 * c[1] / (scale * scale);
	double slope_ref = (c[0] + 4.0 * c[1] * z_ref) / scale;
	double discriminant = slope_ref * slope_ref - 4.0 * q * meta.ref_mw;
	if (!(discriminant > 0.0) || !(slope_ref > 0.0)) {
		return CAL_NONMONOTONIC;
	}
	double x_ref = 2.0 * meta.ref_mw / (slope_ref + sqrt(discriminant));
	fit->threshold_ma = meta.ref_ma - x_ref;
	fit->efficiency_mw_per_ma = meta.ref_mw / x_ref;
	fit->quadratic_correction_mw_per_ma2 = (float)q;
	fit->valid = isfinite(fit->threshold_ma) && isfinite(fit->efficiency_mw_per_ma) &&
		isfinite(fit->quadratic_correction_mw_per_ma2);
	if (!fit->valid) {
		return CAL_FIT_FAILED;
	}

	/* Conditional local covariance: measured normalization is frozen. This
	 * excludes common bridge/anchor error and vendor-reference uncertainty. */
	double z_threshold = (fit->threshold_ma - center) / scale;
	double j0 = -(z_threshold - z_ref) / sqrt(discriminant);
	double j1 = -2.0 * (z_threshold * z_threshold - z_ref * z_ref) / sqrt(discriminant);
	double variance = quadratic ?
		(h11 * j0 * j0 - 2.0 * h01 * j0 * j1 + h00 * j1 * j1) / (h00 * h11 - h01 * h01) :
		j0 * j0 / h00;
	fit->threshold_se_ma = sqrt(fmax(0.0, variance));

	q = fit->quadratic_correction_mw_per_ma2;
	double slope_zero = fit->efficiency_mw_per_ma - q * x_ref;
	double slope_high = slope_zero + 2.0 * q * (high - fit->threshold_ma);
	bool physical = fit->threshold_ma >= 0.0 && low >= fit->threshold_ma + 0.1 - 1e-7 &&
		slope_zero > 0.0 && slope_high > 0.0;
	bool precise = true;
	double sum = 0.0;
	double maximum = 0.0;
	double limit = meta.max_fit_residual_pct / 100.0;
	for (uint8_t i = 0; i < result.records; ++i) {
		const struct laser_cal_record *r = &data.records[i];
		double current = current_of(r);
		if (!isfinite(work.power_mw[i]) || current < low || current > high) {
			continue;
		}
		double x = current - fit->threshold_ma;
		double power = x * (fit->efficiency_mw_per_ma + q * (x - x_ref));
		if (!(power > 0.0)) {
			physical = false;
			continue;
		}
		double residual = ((double)work.power_mw[i] - power) / power;
		double gain = work.gain[r->segment];
		double sigma = sqrt((double)work.variance_mw2[i] + 1.0 / work.baseline_weight / (gain * gain));
		precise &= 3.0 * sigma <= limit * power;
		sum += residual * residual;
		maximum = fmax(maximum, fabs(residual));
	}
	fit->rms_fraction = sqrt(sum / fit->points);
	fit->max_residual_fraction = maximum;
	if (!physical) {
		return CAL_THRESHOLD_RANGE;
	}
	if (!precise) {
		return CAL_INSUFFICIENT_PRECISION;
	}
	if (maximum > limit) {
		return CAL_NONLINEAR;
	}
	fit->accepted = true;
	return 0;
}

/* Select the most precise measured window at a current for lower-bound
 * refinement. Normalization has already identified each segment's mV/mW. */
static int best_at_current(double current)
{
	int best = -1;
	double precision = -1.0;
	for (uint8_t i = 0; i < result.records; ++i) {
		const struct laser_cal_record *r = &data.records[i];
		if (!curve_point(r) || fabs(current_of(r) - current) > 1e-7 ||
		    !(work.gain[r->segment] > 0.0)) {
			continue;
		}
		double score = mean_weight(r) * (double)r->net_mv * (double)r->net_mv;
		if (score > precision) {
			precision = score;
			best = i;
		}
	}
	return best;
}

/* Two-column NNLS by testing the interior, both axes, and the origin. Columns
 * are scaled before solving; unidentifiable terms must not overwrite settings.
 * Negative observed excess variance stays signed to avoid a positive bias.
 * Uses the accepted interval and measured segment gains; writes noise estimates only.
 * This is cancellable CPU work, with no hardware/settings/persistence effects. */
static int fit_noise(void)
{
	/* c2 is source-referred constant variance in mW^2; f2 is fractional
	 * variance. Reweight twice after the initial unweighted estimate. */
	double c2 = 0.0;
	double f2 = 0.0;

	for (unsigned pass = 0; pass < 3; ++pass) {
		double h00 = 0.0;
		double h01 = 0.0;
		double h11 = 0.0;
		double g0 = 0.0;
		double g1 = 0.0;
		unsigned count = 0;

		for (uint8_t i = 0; i < result.records; ++i) {
			if (atomic_get(&fit_cancel)) {
				return -ECANCELED;
			}
			if (!work.include[i] || data.records[i].good < 2) {
				continue;
			}

			/* Variance model: dark + gain^2*c2 + mean_signal^2*f2.
			 * Weight the variance estimate using the Gaussian approximation
			 * Var(s^2) = 2*v^2/(n-1) for n independent readings. */
			const struct laser_cal_record *r = &data.records[i];
			double gain = work.gain[r->segment];
			double x = current_of(r) - result.fit.threshold_ma;
			double x_ref = meta.ref_ma - result.fit.threshold_ma;
			double mu = gain * x * (result.fit.efficiency_mw_per_ma +
				(double)result.fit.quadratic_correction_mw_per_ma2 * (x - x_ref));
			double x0 = gain * gain;
			double x1 = mu * mu;
			double y = reading_variance(r) - work.dark_variance;
			double v = work.dark_variance + x0 * c2 + x1 * f2;
			double w = pass == 0 ? 1.0 : (r->good - 1U) / (2.0 * v * v);
			h00 += w * x0 * x0;
			h01 += w * x0 * x1;
			h11 += w * x1 * x1;
			g0 += w * x0 * y;
			g1 += w * x1 * y;
			++count;
		}

		/* Both columns need finite support and sufficient independence. An
		 * unidentifiable split must not overwrite either configured noise term. */
		if (count < 6 || !(h00 > 0.0) || !(h11 > 0.0) || !isfinite(h00) || !isfinite(h11)) {
			return CAL_NOISE_FIT_FAILED;
		}
		double scale0 = sqrt(h00);
		double scale1 = sqrt(h11);
		double cross = h01 / scale0 / scale1;
		if (1.0 - cross * cross < 1e-12) {
			return CAL_NOISE_FIT_FAILED;
		}

		/* The nonnegative optimum is at the origin, on an axis, or at the
		 * unconstrained interior solution. Compare all feasible candidates. */
		double candidates[4][2] = {
			{0.0, 0.0},
			{fmax(0.0, g0 / h00), 0.0},
			{0.0, fmax(0.0, g1 / h11)},
			{0.0, 0.0},
		};
		double interior[2];
		int rc = calibration_solve_2x2(1.0, cross, 1.0, g0 / scale0, g1 / scale1, interior);
		if (rc == 0 && interior[0] >= 0.0 && interior[1] >= 0.0) {
			candidates[3][0] = interior[0] / scale0;
			candidates[3][1] = interior[1] / scale1;
		}

		double best = INFINITY;
		for (unsigned k = 0; k < 4; ++k) {
			double a = candidates[k][0];
			double b = candidates[k][1];
			double cost = h00 * a * a + 2.0 * h01 * a * b + h11 * b * b - 2.0 * (g0 * a + g1 * b);
			if (cost < best) {
				best = cost;
				c2 = a;
				f2 = b;
			}
		}
	}

	result.fit.constant_noise_mw = sqrt(c2);
	result.fit.fractional_noise = sqrt(f2);
	return isfinite(c2) && isfinite(f2) ? 0 : CAL_NOISE_FIT_FAILED;
}

/* Pure numerical phase: immutable raw records, no hardware or publication.
 * Enumerate bounded contiguous intervals, keeping every usable observation in
 * each interval. Curvature may extend a useful range; it cannot hide individual
 * outliers. The floor excludes the knee without moving the model's threshold. */
static int fit_laser(void)
{
	memset(&result.fit, 0, sizeof(result.fit));
	memset(work.include, 0, sizeof(work.include));
	int rc = normalize_curve();
	if (rc != 0) {
		/* An interrupted normalization may still contain relative gains.
		 * Never publish those as an absolute mV/mW calibration. */
		for (unsigned j = 0; j < LASER_CAL_SEGMENTS; ++j) {
			work.gain[j] = NAN;
		}
		return rc;
	}

	unsigned levels = 0;
	for (uint8_t i = 0; i < result.records; ++i) {
		if (!isfinite(work.power_mw[i])) {
			continue;
		}
		uint16_t current = data.records[i].current_tenth_ma;
		unsigned at = 0;
		while (at < levels && work.levels[at] < current) {
			++at;
		}
		if (at < levels && work.levels[at] == current) {
			continue;
		}
		memmove(&work.levels[at + 1U], &work.levels[at], (levels - at) * sizeof(work.levels[0]));
		work.levels[at] = current;
		++levels;
	}
	if (levels < 6) {
		return CAL_INSUFFICIENT_POINTS;
	}

	bool have_candidate = false;
	int best_error = CAL_FIT_FAILED;
	double best_span = -1.0;
	for (int upper = (int)levels - 1; upper >= 5; --upper) {
		double high = work.levels[upper] / 10.0;
		if (high < fmax(meta.ref_ma, meta.nominal_ma)) {
			break; /* An installed maximum must still reach both reference currents. */
		}
		for (unsigned lower = 0; lower + 5U <= (unsigned)upper; ++lower) {
			double low = work.levels[lower] / 10.0;
			double span = high - low;
			if (low >= fmin(meta.ref_ma, meta.nominal_ma) ||
			    (result.fit.accepted && span < best_span - 1e-7)) {
				break;
			}
			for (unsigned order = 1; order <= 2; ++order) {
				if (atomic_get(&fit_cancel)) {
					return -ECANCELED;
				}
				struct laser_calibration_fit candidate;
				int error = fit_interval(low, high, order == 2, &candidate);
				bool better = !have_candidate;
				if (have_candidate) {
					if (candidate.accepted != result.fit.accepted) {
						better = candidate.accepted;
					} else if (candidate.valid != result.fit.valid) {
						better = candidate.valid;
					} else if (fabs(span - best_span) > 1e-7) {
						better = span > best_span;
					} else if (fabs(low - result.fit.floor_ma) > 1e-7) {
						better = low < result.fit.floor_ma;
					} else if ((candidate.quadratic_correction_mw_per_ma2 == 0.0f) !=
						   (result.fit.quadratic_correction_mw_per_ma2 == 0.0f) && candidate.accepted) {
						better = candidate.quadratic_correction_mw_per_ma2 == 0.0f;
					} else {
						better = candidate.rms_fraction < result.fit.rms_fraction;
					}
				}
				if (better) {
					result.fit = candidate;
					best_error = error;
					best_span = span;
					have_candidate = true;
				}
			}
		}
	}

	/* Retain exact support for the selected diagnostic or accepted fit. A failed
	 * fit is still reviewable; it never turns into acquisition failure. */
	if (have_candidate) {
		for (uint8_t i = 0; i < result.records; ++i) {
			double current = current_of(&data.records[i]);
			work.include[i] = isfinite(work.power_mw[i]) &&
				current >= result.fit.floor_ma && current <= result.fit.max_ma;
		}
	}
	if (result.fit.accepted) {
		result.fit.noise_error = fit_noise();
		result.fit.noise_valid = result.fit.noise_error == 0;
	}
	return best_error;
}

/* Hardware ownership: only the throughput thread advances acquisition. Command
 * handlers serialize starts/stops with this same lock; fitting releases it so
 * a stop can cancel and join without exposing a half-written status snapshot. */

/* Copy a coherent status under status_lock and optionally enqueue best-effort
 * telemetry. Caller holds calibration_lock; queue failure never blocks progress. */
static void publish_status(struct laser_calibration_status *out, bool emit)
{
	k_mutex_lock(&status_lock, K_FOREVER);
	status_snapshot = result;
	if (out != NULL) {
		*out = result;
	}
	k_mutex_unlock(&status_lock);

	if (!emit) {
		return;
	}

	struct coo_cmd_response *msg = &calibration_telemetry;
	memset(msg, 0, sizeof(*msg));
	if (laser_calibration_format_status(msg->payload, sizeof(msg->payload), &result) != 0) {
		return;
	}
	msg->payload_len = strlen(msg->payload);
	(void)app_output_emit(&(struct coo_cmd_runtime_emit_args){
		.type = COO_CMD_RUNTIME_EMIT_DATA,
		.delivery = COO_CMD_RUNTIME_EMIT_BEST_EFFORT,
		.suffix = "laser/calibrate",
		.out = msg,
	});
}

/** Copy the last published status. May wait on status_lock; performs no I/O. */
void laser_calibration_get_status(struct laser_calibration_status *status)
{
	if (status == NULL) {
		return;
	}

	k_mutex_lock(&status_lock, K_FOREVER);
	*status = status_snapshot;
	k_mutex_unlock(&status_lock);
}

/** Check path ownership using the published snapshot, including recabling and
 * unconfirmed shutdown. May wait on status_lock; performs no hardware I/O. */
bool laser_calibration_active(void)
{
	k_mutex_lock(&status_lock, K_FOREVER);
	bool active = status_snapshot.state == LASER_CAL_RUNNING ||
		status_snapshot.state == LASER_CAL_FITTING ||
		status_snapshot.state == LASER_CAL_AWAITING_OUTPUT ||
		status_snapshot.shutdown_pending;
	k_mutex_unlock(&status_lock);
	return active;
}

/* Stop only this run's emission, leaving its TEC operating. Blocks on laser I/O
 * under calibration_lock. Shutdown failure retains the source identity for an
 * explicit stop retry; current_ma changes only after confirmed shutdown. */
static int stop_source(void)
{
	int rc = hispec_laser_stop_output(result.laser, false, NULL, 0);
	result.shutdown_pending = rc != 0;
	if (rc == 0) {
		result.current_ma = 0.0f;
	}
	return rc;
}

/* Terminate acquisition while retaining records and the original error. Attempts
 * source shutdown and releases the PD auto-off hold, then queues status. Caller
 * holds calibration_lock; stop_source may block on I/O. No settings are applied. */
static void fail_acquisition(int error)
{
	run.phase = PHASE_IDLE;
	result.error = error;
	result.state = LASER_CAL_ERROR;
	(void)stop_source();
	housekeeping_photodiode_autooff_inhibit((enum housekeeping_power_output)meta.channel, false);
	publish_status(NULL, true);
}

/**
 * Cancel acquisition/fitting and stop the owned source without erasing data.
 * Called by the command executor; may wait for numerical work and hardware I/O.
 * Already-applied settings remain active. Publishes the resulting status; failed
 * shutdown keeps the source owned so a subsequent explicit stop can retry it.
 */
int laser_calibration_stop(struct laser_calibration_status *status)
{
	k_mutex_lock(&calibration_lock, K_FOREVER);
	/* The union may now contain attenuator records. Its owner alone may touch it. */
	if (atomic_get(&calibration_data_kind) != CALIBRATION_DATA_LASER) {
		k_mutex_unlock(&calibration_lock);
		laser_calibration_get_status(status);
		return 0;
	}

	if (run.phase == PHASE_FIT) {
		atomic_set(&fit_cancel, 1);
		/* Fitting finishes at low priority and reacquires this lock to signal
		 * completion. Release it before joining to avoid deadlock/inheritance. */
		k_mutex_unlock(&calibration_lock);
		(void)k_sem_take(&fit_done, K_FOREVER);
		k_mutex_lock(&calibration_lock, K_FOREVER);
	}

	int rc = 0;
	if (run.phase != PHASE_IDLE || result.shutdown_pending) {
		run.phase = PHASE_IDLE;
		rc = stop_source();
		result.state = rc == 0 ? LASER_CAL_STOPPED : LASER_CAL_ERROR;
		result.error = rc;
		housekeeping_photodiode_autooff_inhibit((enum housekeeping_power_output)meta.channel, false);
	}

	publish_status(status, true);
	k_mutex_unlock(&calibration_lock);
	return rc;
}

/* Route names are the board table's names, including the distinct 1430 input.
 * The route-loss key is input_to_output, identical to the MEMS/throughput API. */
static void route_names(enum hispec_laser_id laser, bool fei, char fiber,
			char input[16], char output[16], char pd_input[16], char pd_output[16])
{
	const char *band = laser <= HISPEC_LASER_1430_YJ ? "yj" : "hk";
	bool special = laser == HISPEC_LASER_1430_YJ || laser == HISPEC_LASER_1430_HK;
	snprintk(input, 16, "%s_%s", band, special ? "1430" : "laser");
	snprintk(output, 16, "%s_%s", band, fei ? "fei" : "ao");
	snprintk(pd_input, 16, "%s_%s", band, fiber == 'S' ? "sm" : "mm");
	snprintk(pd_output, 16, "%s_pd", band);
}

/* Accept only a launch output in the laser's band, returning its AO/FEI selector.
 * This is name validation only; route existence is checked before acquisition. */
static int validate_output(enum hispec_laser_id laser, const char *output, bool *fei)
{
	if (laser < 0 || laser >= HISPEC_LASER_COUNT || output == NULL) {
		return -EINVAL;
	}

	const char *ao = laser <= HISPEC_LASER_1430_YJ ? "yj_ao" : "hk_ao";
	const char *other = laser <= HISPEC_LASER_1430_YJ ? "yj_fei" : "hk_fei";
	if (strcmp(output, ao) != 0 && strcmp(output, other) != 0) {
		return -EINVAL;
	}

	*fei = strcmp(output, other) == 0;
	return 0;
}

/* Apply a calibrated pair allocation and retain its confirmed attenuation.
 * Called under calibration_lock; blocks on DAC I/O. A failed/unknown DAC state
 * cannot supply a trustworthy gain segment for subsequent observations. */
static int set_attenuation(double db)
{
	if (!attenuator_set_db(&attenuators[run.attenuator], db, true)) {
		return CAL_ATTEN_WRITE;
	}

	struct attenuator a;
	attenuator_snapshot(&attenuators[run.attenuator], &a);
	if (!a.dac_cfg1.valid || !a.dac_cfg2.valid) {
		return CAL_ATTEN_WRITE;
	}

	run.atten_db = a.attenuation_db;
	return 0;
}

/* Settle after every actuator write, then exclude any ADC conversion already
 * in flight. Window sample_length counts attempted samples, including failures.
 * Sleeps under calibration_lock, resets the sampler-owned configurable window,
 * then marks the run waiting for that window; does not publish or persist. */
static int begin_window(unsigned event)
{
	k_msleep(SETTLE_MS);
	int duration = photodiode_set_configurable_window_duration(meta.channel, meta.dwell_ms);
	if (duration < 0) {
		return duration;
	}

	meta.dwell_ms = result.dwell_ms = (uint32_t)duration;
	run.event = event;
	run.window_started_ms = k_uptime_get();
	run.phase = PHASE_WINDOW;
	return 0;
}

/* Command mA on the driver's grid, then begin a settled PD window. Caller holds
 * calibration_lock; laser I/O and the settle delay can block. A failed write
 * leaves the last confirmed current in status and does not start a new window. */
static int set_current(double ma, unsigned event)
{
	int rc = hispec_laser_set_current_ma(result.laser, ma, NULL, 0);
	if (rc != 0) {
		return rc == -EPERM ? rc : CAL_CURRENT_WRITE;
	}

	result.current_ma = (float)ma;
	return begin_window(event);
}

/* Insert directly into a small descending, quantized grid. No second point
 * buffer: overlaps/probes and refinement are retained only as raw records. */
static void add_grid_current(double ma)
{
	uint16_t q = (uint16_t)llround(CLAMP(ma, 0.0, meta.max_ma) * 10.0);
	unsigned i = 0;

	while (i < run.grid_count && data.grid[i] > q) {
		++i;
	}
	if (i < run.grid_count && data.grid[i] == q) {
		return;
	}

	__ASSERT_NO_MSG(run.grid_count < LASER_CAL_GRID_COUNT);
	memmove(&data.grid[i + 1], &data.grid[i], (run.grid_count - i) * sizeof(data.grid[0]));
	data.grid[i] = q;
	++run.grid_count;
}

/* Build the bounded descending current grid from the frozen settings. Linear
 * points cover the operating range, logarithmic overdrive points resolve the
 * knee, and below-threshold points reach zero. The exact anchor is always added.
 * Runs during locked setup; quantized duplicates use no additional grid slots. */
static void make_grid(void)
{
	for (unsigned i = 0; i < 33; ++i) {
		add_grid_current(meta.max_ma - (meta.max_ma - meta.seed_threshold_ma) * i / 32.0);
	}

	double span = meta.ref_ma - meta.seed_threshold_ma;
	for (unsigned i = 0; i < 20; ++i) {
		add_grid_current(meta.seed_threshold_ma + 0.1 * pow(span / 0.1, i / 19.0));
	}

	for (unsigned i = 0; i < 9; ++i) {
		add_grid_current(meta.seed_threshold_ma * i / 8.0);
	}
	add_grid_current(meta.ref_ma);
}

/* Temperature is checked once per second. A drift discards the current partial
 * window and resumes only after three consecutive checks within 0.1 C.
 * Called by the worker under calibration_lock; reads laser status via Modbus
 * and may sleep when restarting a settled window. Each warm-up is bounded. */
static int check_temperature(void)
{
	int64_t now = k_uptime_get();
	if (now < run.next_temperature_ms) {
		return 0;
	}
	run.next_temperature_ms = now + 1000;

	struct hispec_laser_status s;
	int rc = hispec_laser_get_status(result.laser, false, &s);
	if (rc != 0) {
		return rc;
	}

	/* The reference anchor assumes controlled temperature; a stopped TEC or
	 * unknown reading cannot qualify the optical measurements. */
	double temp = s.tec_temperature_measured_c;
	if (!isfinite(temp) || !s.tec_started) {
		return CAL_TEMPERATURE_UNAVAILABLE;
	}
	meta.temp_min_c = fminf(meta.temp_min_c, (float)temp);
	meta.temp_max_c = fmaxf(meta.temp_max_c, (float)temp);

	if (fabs(temp - meta.ref_temp_c) > 0.1) {
		if (run.phase != PHASE_WARM) {
			run.temperature_started_ms = now;
		}
		run.stable_temperatures = 0;
		run.phase = PHASE_WARM;
	} else if (run.phase == PHASE_WARM && ++run.stable_temperatures >= 3) {
		return begin_window(run.event);
	}

	return run.phase == PHASE_WARM && now - run.temperature_started_ms >= 60000 ? CAL_TEMPERATURE_TIMEOUT : 0;
}

/* Reserve the second output's dark/ranging/reference windows before allowing
 * the first sweep to fill the shared record array. No storage is allocated. */
static unsigned record_limit(void)
{
	return result.output_index == 0 && (meta.options & OPT_OTHER) ? RECORDS - OTHER_RECORDS : RECORDS;
}

/* Append one window with confirmed DAC codes, even if unusable for fitting.
 * Caller holds calibration_lock. A full buffer never overwrites evidence;
 * zero good samples are retained before reporting terminal acquisition failure. */
static int retain_window(const struct photodiode_window_result *window)
{
	if (result.records >= record_limit()) {
		result.overflow = true;
		return -ENOSPC;
	}

	struct attenuator a;
	attenuator_snapshot(&attenuators[run.attenuator], &a);
	struct laser_cal_record *r = &data.records[result.records++];
	r->elapsed_ms = (uint32_t)(k_uptime_get() - meta.started_ms);
	r->net_mv = (float)window->mean_net_mv;
	r->rms_mv = (float)window->rms_mv;
	r->current_tenth_ma = (uint16_t)lroundf(result.current_ma * 10.0f);
	r->dac_code[0] = (uint16_t)lroundf(a.dac_cfg1.voltage * 4096.0f / meta.dac_full_scale_mv[0]);
	r->dac_code[1] = (uint16_t)lroundf(a.dac_cfg2.voltage * 4096.0f / meta.dac_full_scale_mv[1]);
	r->max_raw = window->max_raw;
	r->failed = (uint8_t)window->failed_samples;
	r->good = (uint8_t)(window->sample_length - window->failed_samples);
	r->segment = run.segment;

	/* Peak clipping invalidates the mean even if the mean itself is in range.
	 * Low-SNR classification uses uncertainty of the surviving-sample mean. */
	double sigma = fmax(window->rms_mv, sqrt(Q_VARIANCE));
	unsigned classification = r->good == 0 ? POINT_ADC_ERROR :
		window->max_mv >= 2000.0 ? POINT_SATURATED :
		window->mean_net_mv < 5.0 * sigma / sqrt(r->good) ? POINT_LOW : POINT_OK;
	r->flags = (uint8_t)(classification | (run.event << 2) | (result.output_index ? 0x80U : 0));
	return r->good == 0 ? CAL_NO_SAMPLES : 0;
}

/* Bracket a usable PD range without using a clipped mean. Each attenuator
 * change creates a new fixed-gain segment only once a usable setting is found.
 * Called under calibration_lock; initializes search state and sleeps to start
 * the first probe window. Raw windows from earlier attempts remain retained. */
static int start_search(unsigned limit)
{
	/* A new DAC pair replaces any earlier incomplete anchor group at this
	 * output. Keep those windows as curve/overlap observations. */
	if (fabs((double)result.current_ma - meta.ref_ma) < 0.05) {
		for (uint8_t i = 0; i < result.records; ++i) {
			struct laser_cal_record *r = &data.records[i];
			if (is_other(r) == (result.output_index != 0) && event_of(r) == EVENT_REFERENCE) {
				r->flags = (r->flags & 0x83U) | (EVENT_SWEEP << 2);
			}
		}
	}

	run.searching = true;
	run.search_tries = 0;
	run.search_limit = limit;
	run.search_low_db = 0.0;
	run.search_high_db = fmin(meta.coeff[0].max_calibrated_db, 55.0) +
		fmin(meta.coeff[1].max_calibrated_db, 55.0);
	return begin_window(EVENT_PROBE);
}

/* Use the completed probe to accept a gain segment or select the next DAC pair.
 * Caller holds calibration_lock; DAC I/O and the next window's settling delay
 * can block. Search limits and minimum movement keep ranging bounded. */
static int search_step(const struct photodiode_window_result *w)
{
	++run.search_tries;
	double signal = w->mean_net_mv;
	bool safe = w->max_mv < PEAK_TARGET_MV;
	bool near_target = signal >= 0.65 * meta.pd_target_mv && signal <= 1.15 * meta.pd_target_mv;
	bool usable = safe && (near_target || (signal < meta.pd_target_mv && run.atten_db < 0.05));

	/* Accept a lower signal at zero attenuation or after exhausting probes,
	 * but never promote a window whose peaks exceed the ranging headroom. */
	if (usable || (run.search_tries >= run.search_limit && safe &&
		      signal >= meta.pd_target_mv / 6.0 && signal <= 1.15 * meta.pd_target_mv)) {
		run.searching = false;
		if (!result.output_index && run.segments >= LASER_CAL_SEGMENTS) {
			return -ENOSPC;
		}
		run.segment = result.output_index ? UINT8_MAX : run.segments++;
		run.reference_count = 0;
		return begin_window(fabs((double)result.current_ma - meta.ref_ma) < 0.05
			? EVENT_REFERENCE : EVENT_OVERLAP);
	}
	if (run.search_tries >= run.search_limit) {
		return CAL_PD_RANGE;
	}

	if (safe && signal < meta.pd_target_mv) {
		run.search_high_db = run.atten_db;
	} else {
		run.search_low_db = run.atten_db;
	}

	/* A clipped mean cannot predict the needed attenuation, so bisect that
	 * bracket. Near the noise floor, use a bounded 10-dB reduction instead of
	 * a logarithm of an unreliable signal. Otherwise aim directly at meta.pd_target_mv. */
	double next;
	if (!safe) {
		next = 0.5 * (run.atten_db + run.search_high_db);
	} else if (signal <= 5.0 * fmax(w->rms_mv, sqrt(Q_VARIANCE))) {
		next = run.atten_db - 10.0;
	} else {
		next = run.atten_db - 10.0 * log10(meta.pd_target_mv / signal);
	}
	next = CLAMP(next, run.search_low_db, run.search_high_db);
	if (fabs(next - run.atten_db) < 0.02) {
		return CAL_PD_RANGE;
	}

	int rc = set_attenuation(next);
	return rc == 0 ? begin_window(EVENT_PROBE) : rc;
}

/* Advance to the next quantized mA setpoint, ending with a zero-current dark
 * window. Called under calibration_lock; set_current blocks on I/O and settling. */
static int next_sweep_current(void)
{
	/* The grid must end in zero; reaching its end without the terminal dark
	 * window is an acquisition failure rather than successful completion. */
	if (++run.grid_index >= run.grid_count) {
		return -ERANGE;
	}

	double ma = data.grid[run.grid_index] / 10.0;
	if (ma == 0.0) {
		run.stage = STAGE_END;
		return set_current(0.0, EVENT_DARK);
	}

	run.reference_count = 0;
	return set_current(ma, fabs(ma - meta.ref_ma) < 0.05 ? EVENT_REFERENCE : EVENT_SWEEP);
}

/* Acquire one selected output's baseline, then range at max (first output) or
 * at the reference (second). MEMS intent follows existing route persistence.
 * Caller holds calibration_lock. Setup blocks on source/MEMS/DAC/TEC I/O, then
 * inhibits PD auto-off and hands thermal qualification to the throughput tick. */
static int prepare_output(void)
{
	char input[16];
	char output[16];
	char pd_input[16];
	char pd_output[16];
	bool fei = result.first_fei ^ (result.output_index != 0);
	route_names(result.laser, fei, meta.fiber, input, output, pd_input, pd_output);

	/* Route only after confirmed emission-off, and begin at the maximum
	 * calibrated attenuation so the first illuminated probe has headroom. */
	int rc = stop_source();
	if (rc == 0) {
		rc = mems_router_apply_named_route(&router, input, output, false, NULL, NULL);
		if (rc != 0) {
			return CAL_ROUTE_WRITE;
		}
	}
	if (rc == 0) {
		rc = mems_router_apply_named_route(&router, pd_input, pd_output, false, NULL, NULL);
		if (rc != 0) {
			return CAL_ROUTE_WRITE;
		}
	}
	double high = fmin(meta.coeff[0].max_calibrated_db, 55.0) +
		fmin(meta.coeff[1].max_calibrated_db, 55.0);
	if (rc == 0) {
		rc = set_attenuation(high);
	}
	if (rc == 0) {
		rc = hispec_laser_set_tec_temperature_c(result.laser, meta.ref_temp_c);
	}
	if (rc != 0) {
		return rc;
	}

	housekeeping_photodiode_autooff_inhibit((enum housekeeping_power_output)meta.channel, true);
	run.stage = result.output_index ? STAGE_OTHER_DARK : STAGE_DARK;
	run.event = EVENT_DARK;
	run.phase = PHASE_WARM;
	run.stable_temperatures = 0;
	run.temperature_started_ms = k_uptime_get();
	run.next_temperature_ms = 0;
	return 0;
}

/**
 * Start a current sweep using a snapshot of the optical and laser settings.
 *
 * Called by the command executor. Validation precedes cancellation of the old
 * operation; after its confirmed shutdown, this start replaces the shared raw
 * dataset under calibration_lock. Hardware setup may block on Modbus/MEMS/DAC
 * I/O. The throughput thread subsequently owns warm-up, sampling and fitting.
 *
 * Setup stops all laser outputs, selects the loopback routes, sets high
 * attenuation and starts the TEC at ref_temp_c. Status is copied and queued as
 * best-effort telemetry. A setup error retains the new run's error/shutdown
 * state. Settings application and optional persistence happen after fitting.
 */
int laser_calibration_start(const struct laser_calibration_request *request,
			    struct laser_calibration_status *status)
{
	bool fei;

	/* Reject an unusable request before disturbing the current calibration.
	 * The other output only measures its reference loss, so it requires loss
	 * fitting and a known external loopback loss to separate cable from route. */
	if (request == NULL ||
	    validate_output(request->laser, request->output, &fei) != 0 ||
	    (request->fiber != 'M' && request->fiber != 'S') ||
	    request->dwell_ms < 100 || request->dwell_ms > 2000 ||
	    !isfinite(request->max_fit_residual_pct) || request->max_fit_residual_pct <= 0.0 ||
	    request->max_fit_residual_pct >= 100.0 ||
	    !isfinite(request->pd_target_mv) || request->pd_target_mv <= 0.0 ||
	    request->pd_target_mv >= PEAK_TARGET_MV || (!request->apply && request->persist) ||
	    (request->other_output && !request->determine_static_loss) ||
	    (request->determine_static_loss &&
	     (!isfinite(request->loopback_loss_db) || request->loopback_loss_db < 0.0))) {
		return CAL_INVALID_REQUEST;
	}

	struct app_laser_channel_settings settings;
	const struct hispec_laser_driver_profile *profile;
	int rc = hispec_laser_get_channel_settings(request->laser, &settings);

	if (rc == 0) {
		rc = hispec_laser_get_driver_profile(request->laser, &profile);
	}
	if (rc != 0) {
		return rc;
	}

	/* The anchor must be reachable within configured limits and above the
	 * seed threshold. Its temperature is also the acquisition temperature. */
	const laserprops_t *p = &settings.properties;
	if (p->ref_ma > p->max_current_ma || p->ref_ma <= p->threshold_current_ma + 0.1 ||
	    p->ref_temp_c < p->operating_temp_range_c.min_c ||
	    p->ref_temp_c > p->operating_temp_range_c.max_c) {
		return CAL_REFERENCE_RANGE;
	}

	uint8_t index;
	if (attenuator_index_from_laser_id(request->laser, &index) != 0 ||
	    !devices_attenuator_channel_available(index)) {
		return -ENODEV;
	}

	/* PD calibration and power are prerequisites supplied by the operator.
	 * A pending dark operation would change the offset during acquisition. */
	uint8_t channel = request->laser <= HISPEC_LASER_1430_YJ
		? PHOTODIODE_CHANNEL_YJ : PHOTODIODE_CHANNEL_HK;
	struct app_photodiode_settings pd_settings;
	app_settings_get_photodiode(&pd_settings);
	const struct app_pd_channel_settings *pd = &pd_settings.channel[channel];

	if (!isfinite(pd->dark.mean_mv) || pd->dark.mean_mv < PHOTODIODE_DARK_MIN_MV ||
	    pd->dark.mean_mv > PHOTODIODE_DARK_MAX_MV) {
		return CAL_DARK_REQUIRED;
	}

	struct photodiode_status pd_status;
	photodiode_get_status(&pd_status);
	if (pd_status.channel[0].dark_pending || pd_status.channel[1].dark_pending) {
		return CAL_DARK_BUSY;
	}

	bool powered = false;
	rc = housekeeping_power_get((enum housekeeping_power_output)channel, &powered);
	if (rc != 0 || !powered) {
		return rc != 0 ? rc : CAL_PD_OFF;
	}

	/* Both physical routes must exist. Return transmission and PD gain are
	 * fixed assumptions for loss inference, not additional fitted parameters. */
	char input[16];
	char output[16];
	char pd_input[16];
	char pd_output[16];
	char key[40];
	route_names(request->laser, fei, request->fiber, input, output, pd_input, pd_output);
	if (mems_router_get_route(&router, input, output) == NULL ||
	    mems_router_get_route(&router, pd_input, pd_output) == NULL) {
		return CAL_ROUTE_UNAVAILABLE;
	}

	snprintk(key, sizeof(key), "%s_to_%s", pd_input, pd_output);
	double return_tx;
	rc = app_settings_get_route_loss(key, hispec_laser_name(request->laser), &return_tx);
	double wavelength = hispec_laser_estimate_wavelength_nm(p, p->ref_temp_c, p->ref_ma);
	double gain = 1000.0 / photodiode_power_uw_from_mv_at_wavelength(1.0, wavelength, pd);
	if (rc != 0 || !isfinite(gain) || !(gain > 0.0) || !(return_tx > 0.0) || return_tx > 1.0) {
		return CAL_PD_GAIN_INVALID;
	}

	/* Join any fitting before reusing its data/scratch storage. A failed STOP
	 * leaves the old source owned by its operation; do not replace that state. */
	rc = attenuator_calibration_stop(NULL);
	if (rc == 0) {
		rc = laser_calibration_stop(NULL);
	}
	if (rc == 0) {
		rc = throughput_monitor_stop(PHOTODIODE_CHANNEL_COUNT, NULL);
	}
	if (rc != 0) {
		return rc;
	}

	/* This is the dataset replacement boundary. Later setup errors belong to
	 * this new run and must remain visible along with any retained records. */
	k_mutex_lock(&calibration_lock, K_FOREVER);
	memset(&data, 0, sizeof(data));
	memset(&run, 0, sizeof(run));
	atomic_set(&calibration_data_kind, CALIBRATION_DATA_LASER);
	atomic_clear(&fit_cancel);

	meta.run_id = ++calibration_run_id;
	result.run_id = meta.run_id;
	meta.started_ms = (uint64_t)k_uptime_get();
	meta.laser = request->laser;
	meta.channel = channel;
	meta.fiber = request->fiber;
	meta.first_fei = fei;
	meta.dwell_ms = request->dwell_ms;
	meta.options = (request->determine_static_loss ? OPT_LOSS : 0) |
		(request->update_noise ? OPT_NOISE : 0) |
		(request->other_output ? OPT_OTHER : 0) |
		(request->persist ? OPT_PERSIST : 0) |
		(request->apply ? OPT_APPLY : 0);

	/* Freeze both the mutable acquisition settings and compiled reference
	 * properties. Later application must not change the assumptions in the data. */
	meta.max_fit_residual_pct = request->max_fit_residual_pct;
	meta.pd_target_mv = request->pd_target_mv;
	meta.baseline_mv = NAN;
	meta.dark_variance_mv2 = NAN;
	for (unsigned j = 0; j < LASER_CAL_SEGMENTS; ++j) {
		meta.segment_mv_per_mw[j] = NAN;
	}
	meta.ref_mw = p->ref_mw;
	meta.ref_ma = p->ref_ma;
	meta.ref_temp_c = p->ref_temp_c;
	meta.max_ma = hispec_laser_quantize_current_ma(p->max_current_ma, 0.0, p->max_current_ma);
	meta.nominal_ma = p->nominal_current_ma;
	meta.seed_threshold_ma = p->threshold_current_ma;
	meta.default_threshold_ma = profile->properties->threshold_current_ma;
	meta.default_efficiency_mw_per_ma = profile->properties->efficiency_mw_per_ma;
	meta.current_cal_pct = settings.current_cal_pct;
	meta.pd_gain_mv_per_mw = gain;
	meta.dark_mv = pd->dark.mean_mv;
	meta.dark_rms_mv = pd->dark.rms_mv;
	meta.return_tx = return_tx;
	meta.loopback_loss_db[0] = meta.loopback_loss_db[1] = request->loopback_loss_db;
	meta.temp_min_c = INFINITY;
	meta.temp_max_c = -INFINITY;

	struct attenuator a;
	attenuator_snapshot(&attenuators[index], &a);
	meta.coeff[0] = a.coeff1;
	meta.coeff[1] = a.coeff2;
	meta.dac_full_scale_mv[0] = a.dac_cfg1.ideal_full_scale_mv;
	meta.dac_full_scale_mv[1] = a.dac_cfg2.ideal_full_scale_mv;
	meta.dac_limit_mv[0] = a.dac_cfg1.drive_limit_mv;
	meta.dac_limit_mv[1] = a.dac_cfg2.drive_limit_mv;

	run.attenuator = index;
	result.laser = request->laser;
	result.first_fei = fei;
	result.state = LASER_CAL_RUNNING;
	result.dwell_ms = meta.dwell_ms;
	make_grid();

	/* Shared laser inputs can combine several diodes. Quiesce them before
	 * capturing the baseline; subsequent cleanup owns only this run's source. */
	rc = hispec_laser_stop_all_outputs(false);
	if (rc == 0) {
		rc = prepare_output();
	}
	if (rc != 0) {
		fail_acquisition(rc);
	}

	publish_status(status, true);
	k_mutex_unlock(&calibration_lock);
	return rc;
}

/* Loss is conditional on the reference, PD responsivity, FVOA calibration,
 * supplied external loopback and configured return transmission. These cannot
 * all be independently identified by the same optical observation.
 * Fits one output using its own baseline and three reference windows. Writes
 * that route result and reuses baseline scratch; no hardware or settings I/O. */
static int fit_route(unsigned output_index)
{
	struct laser_calibration_route_fit *f = &result.route[output_index];
	double baseline;
	double weight;
	int rc = fit_baseline(output_index != 0, &baseline, &weight);
	if (rc != 0) {
		return CAL_DARK_UNAVAILABLE;
	}

	/* A common DAC pair lets us combine reference windows before converting
	 * their mean voltage to power. Mixing attenuation settings would bias it. */
	double sum = 0.0;
	double total = 0.0;
	unsigned count = 0;
	uint16_t codes[2] = {0};
	for (uint8_t i = 0; i < result.records; ++i) {
		const struct laser_cal_record *r = &data.records[i];
		if (is_other(r) != (output_index != 0) || event_of(r) != EVENT_REFERENCE ||
		    (r->flags & 3U) != POINT_OK || r->good == 0 ||
		    fabs(current_of(r) - meta.ref_ma) > 0.05) {
			continue;
		}
		if (count > 0 && (codes[0] != r->dac_code[0] || codes[1] != r->dac_code[1])) {
			return CAL_REFERENCE_UNAVAILABLE;
		}
		codes[0] = r->dac_code[0];
		codes[1] = r->dac_code[1];

		double w = mean_weight(r);
		sum += w * (double)r->net_mv;
		total += w;
		++count;
	}
	if (count < 3 || !(sum / total - baseline > 0.0)) {
		return CAL_REFERENCE_UNAVAILABLE;
	}

	/* Infer fixed loss only where the existing FVOA calibration is supported;
	 * otherwise an extrapolation error would be attributed to the route. */
	double atten_db = 0.0;
	for (unsigned j = 0; j < 2; ++j) {
		struct atten_model_eval e;
		double mv = (double)codes[j] * (double)meta.dac_full_scale_mv[j] / 4096.0;
		if (!atten_model_eval(&meta.coeff[j], (float)mv, &e) ||
		    e.db > fmin(meta.coeff[j].max_calibrated_db, 55.0) + 0.05) {
			return CAL_ATTEN_RANGE;
		}
		atten_db += e.db;
	}

	/* Combined loss includes launch and return. Since return_tx is a
	 * transmission, adding 10*log10(return_tx) subtracts its positive dB loss. */
	double pd_mw = (sum / total - baseline) / meta.pd_gain_mv_per_mw;
	f->combined_loss_db = 10.0 * log10(meta.ref_mw / pd_mw) - atten_db - meta.loopback_loss_db[output_index];
	f->outbound_loss_db = f->combined_loss_db + 10.0 * log10(meta.return_tx);
	f->accepted = isfinite(f->outbound_loss_db) && f->outbound_loss_db >= 0.0;
	return f->accepted ? 0 : CAL_LOSS_FIT_FAILED;
}

/* Validate every requested result before changing any settings. Numerical
 * acceptance survives an application failure. RAM and NVS outcomes remain
 * separate because a failed flash write cannot undo an active RAM update.
 * Caller holds calibration_lock; settings/driver/NVS work may block. */
static int apply_results(bool core, bool loss, bool noise, bool persist,
			 unsigned first_route, unsigned route_count)
{
	struct laser_calibration_fit *f = &result.fit;
	f->apply_error = 0;
	if (!f->accepted) {
		f->apply_error = CAL_NOT_ACCEPTED;
		return f->apply_error;
	}
	/* The second output cannot bypass a failed first-stage application
	 * (including rejected requested noise/loss). Its route belongs to the
	 * accepted core that was actually installed, not merely a candidate. */
	if (!core && !f->applied) {
		f->apply_error = CAL_CORE_NOT_APPLIED;
		return f->apply_error;
	}
	if (noise && !f->noise_valid) {
		f->apply_error = CAL_NOISE_NOT_ACCEPTED;
		return f->apply_error;
	}
	if (loss) {
		for (unsigned i = first_route; i < first_route + route_count; ++i) {
			if (!(meta.options & OPT_LOSS) || !result.route[i].accepted) {
				f->apply_error = CAL_LOSS_NOT_ACCEPTED;
				return f->apply_error;
			}
		}
	}

	struct app_laser_channel_settings settings;
	if (core) {
		f->apply_error = hispec_laser_get_channel_settings(result.laser, &settings);
		if (f->apply_error != 0) {
			return f->apply_error;
		}
		settings.properties.threshold_current_ma = f->threshold_ma;
		settings.properties.efficiency_mw_per_ma = f->efficiency_mw_per_ma;
		settings.properties.quadratic_correction_mw_per_ma2 = f->quadratic_correction_mw_per_ma2;
		settings.properties.max_current_ma = f->max_ma;
		settings.min_autolevel_current_ma = f->floor_ma;
		/* The fitted correction refers to this frozen anchor. A later user
		 * edit must not silently change the meaning of a retained candidate. */
		if (settings.properties.ref_ma != meta.ref_ma || settings.properties.ref_mw != meta.ref_mw ||
		    settings.properties.ref_temp_c != meta.ref_temp_c || settings.current_cal_pct != meta.current_cal_pct) {
			f->apply_error = CAL_STALE_RUN;
			return f->apply_error;
		}
		if (noise) {
			settings.constant_noise_mw = f->constant_noise_mw;
			settings.fractional_noise = f->fractional_noise;
		}
		if (hispec_laser_validate_channel_settings(result.laser, &settings) != 0) {
			f->apply_error = CAL_INVALID_SETTINGS;
			return f->apply_error;
		}
	}

	/* A reviewed apply may follow unrelated streaming. Stop it only after
	 * candidate validation; a bad run ID or rejected fit has no hardware effect. */
	int first_error = throughput_monitor_stop(PHOTODIODE_CHANNEL_COUNT, NULL);
	if (first_error != 0) {
		f->apply_error = first_error;
		return first_error;
	}
	if (core) {
		f->apply_error = hispec_laser_update_channel_settings(result.laser, &settings, false, NULL, 0);
		if (f->apply_error != 0) {
			return f->apply_error;
		}
		f->applied = true;
		f->persisted = false;
		f->persist_error = 0;
		f->noise_applied |= noise;
		f->noise_persisted = false;
		if (persist) {
			f->persist_error = app_settings_update_laser_channel(result.laser, &settings, true);
			f->persisted = f->persist_error == 0;
			f->noise_persisted = f->noise_applied && f->persisted;
			first_error = f->persist_error;
		}
	}

	if (loss) {
		for (unsigned i = first_route; i < first_route + route_count; ++i) {
			struct laser_calibration_route_fit *r = &result.route[i];
			char input[16], output[16], pd_input[16], pd_output[16], key[40];
			route_names(result.laser, result.first_fei ^ (i != 0), meta.fiber,
				input, output, pd_input, pd_output);
			snprintk(key, sizeof(key), "%s_to_%s", input, output);
			double tx = pow(10.0, -r->outbound_loss_db / 10.0);
			r->apply_error = app_settings_set_route_loss(key, hispec_laser_name(result.laser), tx, false);
			r->applied = r->apply_error == 0;
			r->persisted = false;
			r->persist_error = 0;
			if (r->applied && persist) {
				r->persist_error = app_settings_set_route_loss(key, hispec_laser_name(result.laser), tx, true);
				r->persisted = r->persist_error == 0;
			}
			if (first_error == 0) {
				first_error = r->apply_error != 0 ? r->apply_error : r->persist_error;
			}
		}
	}
	return first_error;
}

/** Apply the retained candidate after human review, using the same domain path
 * as automatic field application. Serializes against dataset replacement;
 * never resumes acquisition. May block on shutdown, driver settings and NVS. */
int laser_calibration_apply(uint32_t run_id, bool update_loss, bool update_noise,
			    bool persist, struct laser_calibration_status *status)
{
	k_mutex_lock(&calibration_lock, K_FOREVER);
	int rc;
	if (atomic_get(&calibration_data_kind) != CALIBRATION_DATA_LASER || meta.run_id != run_id) {
		rc = CAL_STALE_RUN;
	} else if (run.phase != PHASE_IDLE || result.shutdown_pending) {
		rc = -EBUSY;
	} else {
		rc = apply_results(true, update_loss, update_noise, persist, 0, result.output_index + 1U);
		publish_status(status, true);
	}
	k_mutex_unlock(&calibration_lock);
	return rc;
}

/* Reuse the floor observation's exact DAC setting and segment. At most eight
 * interior 0.1-mA points refine the bracket, with space reserved for final zero.
 * Called under calibration_lock after fitting. Returns 1 when acquisition
 * resumes, 0 when no refinement is possible, or a hardware error. May block on
 * DAC/current I/O and settling; does not apply or persist the provisional fit. */
static int start_refinement(void)
{
	run.refined = true;
	if (!result.fit.valid || result.fit.error != 0) {
		return 0;
	}

	double high = result.fit.floor_ma;
	double low = -1.0;
	int best = best_at_current(high);
	if (best < 0) {
		return 0;
	}
	for (uint8_t i = 0; i < result.records; ++i) {
		const struct laser_cal_record *r = &data.records[i];
		double x = current_of(r);
		if (!is_other(r) && event_of(r) != EVENT_PROBE && x < high - 0.05) {
			low = fmax(low, x);
		}
	}

	/* Need a measured lower bracket and an interior driver-grid point. Keep
	 * one record for the final dark window even when storage is almost full. */
	if (low < 0.0 || high - low < 0.15) {
		return 0;
	}
	unsigned available = record_limit() - result.records;
	if (available <= 1) {
		return 0;
	}
	run.refine_count = (uint8_t)MIN(MIN(8U, available - 1U), (unsigned)lround((high - low) * 10.0) - 1U);
	if (run.refine_count == 0) {
		return 0;
	}

	run.refine_low_ma = low;
	run.refine_high_ma = high;
	run.refine_index = 0;
	const struct laser_cal_record *r = &data.records[best];
	run.segment = r->segment;
	struct attenuator_target target[2];
	for (unsigned j = 0; j < 2; ++j) {
		target[j] = (struct attenuator_target){
			.value = (double)r->dac_code[j] * (double)meta.dac_full_scale_mv[j] / 4096.0,
			.voltage_mv = true,
			.update = true,
		};
	}
	if (!attenuator_set_pair(&attenuators[run.attenuator], target)) {
		return CAL_ATTEN_WRITE;
	}

	run.stage = STAGE_REFINE;
	double ma = round((high - (high - low) / (run.refine_count + 1U)) * 10.0) / 10.0;
	int rc = set_current(ma, EVENT_REFINE);
	return rc == 0 ? 1 : rc;
}

/* Advance after a window has been retained. Caller holds calibration_lock;
 * transitions may command hardware and sleep for settling. Final dark capture
 * must be followed by confirmed emission-off before exposing PHASE_FIT. */
static int advance_window(const struct photodiode_window_result *w)
{
	if (run.searching) {
		return search_step(w);
	}

	switch (run.stage) {
	case STAGE_DARK:
	case STAGE_OTHER_DARK: {
		bool other = run.stage == STAGE_OTHER_DARK;
		run.stage = other ? STAGE_OTHER_REFERENCE : STAGE_SWEEP;
		int rc = set_current(other ? meta.ref_ma : meta.max_ma, EVENT_PROBE);
		return rc == 0 ? start_search(16) : rc;
	}
	case STAGE_SWEEP:
		if (w->max_mv >= PEAK_TARGET_MV || (w->mean_net_mv < (meta.pd_target_mv / 6.0) && run.atten_db >= 0.05)) {
			/* Retain the pre-range point for overlap at this same current. */
			return start_search(4);
		}
		if (run.event == EVENT_REFERENCE && ++run.reference_count < 3) {
			return begin_window(EVENT_REFERENCE);
		}
		return next_sweep_current();

	case STAGE_OTHER_REFERENCE:
		if (run.event == EVENT_REFERENCE && ++run.reference_count < 3) {
			return begin_window(EVENT_REFERENCE);
		}
		run.stage = STAGE_END;
		return set_current(0.0, EVENT_DARK);

	case STAGE_REFINE:
		if (++run.refine_index < run.refine_count) {
			double ma = run.refine_high_ma - (run.refine_high_ma - run.refine_low_ma) *
				(run.refine_index + 1U) / (run.refine_count + 1U);
			return set_current(round(ma * 10.0) / 10.0, EVENT_REFINE);
		}
		run.stage = STAGE_END;
		return set_current(0.0, EVENT_DARK);

	case STAGE_END: {
		int rc = stop_source();
		if (rc != 0) {
			return rc;
		}
		run.phase = PHASE_FIT;
		result.state = LASER_CAL_FITTING;
		atomic_clear(&fit_cancel);
		k_sem_reset(&fit_done);
		return 0;
	}
	}
	return -EINVAL;
}

/**
 * Advance acquisition from a sampler snapshot on the existing throughput thread.
 *
 * Holds calibration_lock while checking hardware, retaining windows and
 * selecting the next operation. Those calls may block on I/O and settling.
 * Fitting temporarily runs at the lowest application priority without the lock;
 * start/stop must cancel and join before replacing its data or scratch storage.
 *
 * On completion, applies accepted settings and optional NVS writes, then queues
 * status. A requested second output pauses with emission off and PD ownership
 * retained. Numerical rejection is reported separately from acquisition failure.
 */
void laser_calibration_tick(const struct photodiode_status *pd)
{
	k_mutex_lock(&calibration_lock, K_FOREVER);
	/* Another calibration may own the union, or this run may be paused/done.
	 * Only warm-up and window acquisition are advanced by a new sampler tick. */
	if (atomic_get(&calibration_data_kind) != CALIBRATION_DATA_LASER ||
	    (run.phase != PHASE_WARM && run.phase != PHASE_WINDOW)) {
		k_mutex_unlock(&calibration_lock);
		return;
	}

	/* A lost PD supply or autonomous laser stop makes a window physically
	 * invalid, even if the sampler still has a numerically usable average. */
	bool powered = false;
	int rc = housekeeping_power_get_confirmed((enum housekeeping_power_output)meta.channel, &powered);
	if (rc == 0 && !powered) {
		rc = CAL_PD_OFF;
	}
	bool emitting = false;
	if (rc == 0) {
		rc = hispec_laser_output_status(result.laser, &emitting);
	}
	if (rc == 0 && result.current_ma > 0.0f && !emitting) {
		rc = CAL_SOURCE_STOPPED;
	}
	if (rc == 0) {
		rc = check_temperature();
	}
	if (rc != 0) {
		fail_acquisition(rc);
		k_mutex_unlock(&calibration_lock);
		return;
	}

	/* Ignore the snapshot from before the last reset until a full fresh window
	 * has arrived. Attempted samples include failures; retain_window decides
	 * whether enough good samples survived to continue acquisition. */
	const struct photodiode_window_result *w = &pd->channel[meta.channel].configurable_window;
	bool ready = run.phase == PHASE_WINDOW && w->end_ms > run.window_started_ms &&
		w->sample_length >= meta.dwell_ms / PHOTODIODE_SAMPLE_INTERVAL_MS;
	if (ready) {
		rc = retain_window(w);
		if (rc == 0) {
			rc = advance_window(w);
		}
		if (rc != 0) {
			fail_acquisition(rc);
		}
		publish_status(NULL, true);
	} else if (run.phase == PHASE_WINDOW && k_uptime_get() - run.window_started_ms > meta.dwell_ms + 3000) {
		fail_acquisition(CAL_WINDOW_TIMEOUT);
	}

	bool fitting = run.phase == PHASE_FIT;
	k_mutex_unlock(&calibration_lock);
	if (!fitting) {
		return;
	}

	/* Same worker and priority discipline as attenuator fitting. No new stack,
	 * heap, scheduler or queue. Stop joins this bounded numerical section. */
	int priority = k_thread_priority_get(k_current_get());
	k_thread_priority_set(k_current_get(), K_LOWEST_APPLICATION_THREAD_PRIO);
	if (result.output_index == 0) {
		result.fit.error = fit_laser();
	}
	if ((meta.options & OPT_LOSS) && !atomic_get(&fit_cancel)) {
		result.route[result.output_index].error = fit_route(result.output_index);
	}
	k_thread_priority_set(k_current_get(), priority);

	k_mutex_lock(&calibration_lock, K_FOREVER);
	if (atomic_get(&fit_cancel)) {
		/* The waiting stop caller owns terminal state and shutdown cleanup. */
		run.phase = PHASE_PAUSE;
	} else {
		/* Publish normalization and exact support only after numerical work
		 * rejoins the data lock. Downloads never observe a half-written fit. */
		if (result.output_index == 0) {
			meta.baseline_mv = work.baseline_mean;
			meta.dark_variance_mv2 = work.dark_variance;
			memcpy(meta.segment_mv_per_mw, work.gain, sizeof(meta.segment_mv_per_mw));
			memset(meta.fit_support, 0, sizeof(meta.fit_support));
			for (uint8_t i = 0; i < result.records; ++i) {
				if (work.include[i]) {
					meta.fit_support[i / 8U] |= 1U << (i % 8U);
				}
			}
		}
		int refinement = 0;
		if (result.output_index == 0 && !run.refined) {
			refinement = start_refinement();
		}
		if (refinement < 0) {
			fail_acquisition(refinement);
		} else if (refinement > 0) {
			result.state = LASER_CAL_RUNNING;
		} else {
			result.acquisition_complete = true;
			if (meta.options & OPT_APPLY) {
				(void)apply_results(result.output_index == 0, (meta.options & OPT_LOSS) != 0,
					result.output_index == 0 && (meta.options & OPT_NOISE),
					(meta.options & OPT_PERSIST) != 0, result.output_index, 1);
			}
			bool pause = result.output_index == 0 && (meta.options & OPT_OTHER);
			run.phase = pause ? PHASE_PAUSE : PHASE_IDLE;
			result.state = pause ? LASER_CAL_AWAITING_OUTPUT : LASER_CAL_COMPLETE;

			/* Keep the PD available while the operator recables. Settings are
			 * still owned by this run, so a paused PD cannot be re-enabled manually. */
			if (!pause) {
				housekeeping_photodiode_autooff_inhibit((enum housekeeping_power_output)meta.channel, false);
			}
		}
		publish_status(NULL, true);
	}

	/* Release a joining command only after the worker has stopped using the
	 * unlocked fit workspace and committed its phase transition under the lock. */
	k_sem_give(&fit_done);
	k_mutex_unlock(&calibration_lock);
}

/** Resume the other-output reference measurement after operator recabling.
 * Called by the command executor; retains first-stage results and optionally
 * replaces only the second output's external loss assumption in dB. Setup may
 * block on hardware I/O; status is queued and the worker resumes acquisition. */
int laser_calibration_continue(bool set_loopback, double loss,
			       struct laser_calibration_status *status)
{
	if (set_loopback && (!isfinite(loss) || loss < 0.0)) {
		return -EINVAL;
	}

	k_mutex_lock(&calibration_lock, K_FOREVER);
	/* Only the explicit recabling pause can resume. A stopped/replaced run
	 * must not reinterpret another operation's storage or restart emission. */
	if (atomic_get(&calibration_data_kind) != CALIBRATION_DATA_LASER || result.state != LASER_CAL_AWAITING_OUTPUT) {
		k_mutex_unlock(&calibration_lock);
		return -EINVAL;
	}

	if (set_loopback) {
		meta.loopback_loss_db[1] = loss;
	}
	result.output_index = 1;
	result.acquisition_complete = false;
	result.state = LASER_CAL_RUNNING;
	run.reference_count = 0;

	int rc = prepare_output();
	if (rc != 0) {
		fail_acquisition(rc);
	}
	publish_status(status, true);
	k_mutex_unlock(&calibration_lock);
	return rc;
}

/**
 * Restore compiled calibration properties and one exact outbound loss override.
 * Called by the command executor. Stops and joins calibration/throughput before
 * stopping the selected source; may block on hardware and optional NVS writes.
 *
 * Current calibration, identity and unrelated TEC policy remain
 * operator settings. Core and route restoration report independent results in
 * the required out argument; flash failure never implies a RAM rollback.
 * Retained acquisition records and historical fit results are not rewritten.
 */
int laser_calibration_restore_defaults(enum hispec_laser_id laser, const char *output, bool persist,
					      struct laser_calibration_restore_result *out)
{
	memset(out, 0, sizeof(*out));
	bool fei;
	int rc = validate_output(laser, output, &fei);
	if (rc != 0) {
		return rc;
	}

	/* Do not rewrite settings while another owner may still drive the path,
	 * or after emission-off could not be confirmed. */
	rc = attenuator_calibration_stop(NULL);
	if (rc == 0) {
		rc = laser_calibration_stop(NULL);
	}
	if (rc == 0) {
		rc = throughput_monitor_stop(PHOTODIODE_CHANNEL_COUNT, NULL);
	}
	if (rc == 0) {
		rc = hispec_laser_stop_output(laser, false, NULL, 0);
	}
	if (rc != 0) {
		return rc;
	}

	const struct hispec_laser_driver_profile *profile;
	rc = hispec_laser_get_driver_profile(laser, &profile);
	if (rc != 0) {
		return rc;
	}
	const laserprops_t *p = profile->properties;

	/* Restore only calibration-owned fields. Validate the combined settings:
	 * preserved user limits may no longer admit the compiled threshold/floor. */
	struct app_laser_channel_settings s;
	out->laser_error = hispec_laser_get_channel_settings(laser, &s);
	if (out->laser_error == 0) {
		s.properties.max_current_ma = p->max_current_ma;
		s.properties.quadratic_correction_mw_per_ma2 = 0.0f;
		s.properties.threshold_current_ma = p->threshold_current_ma;
		s.properties.efficiency_mw_per_ma = p->efficiency_mw_per_ma;
		s.properties.ref_mw = p->ref_mw;
		s.properties.ref_ma = p->ref_ma;
		s.properties.ref_temp_c = p->ref_temp_c;
		s.min_autolevel_current_ma = p->threshold_current_ma + 0.1;
		s.constant_noise_mw = HISPEC_LASER_DEFAULT_NOISE_FLOOR_FRACTION *
			(p->max_current_ma - p->threshold_current_ma) * p->efficiency_mw_per_ma;
		s.fractional_noise = HISPEC_LASER_DEFAULT_FRACTIONAL_NOISE;

		out->laser_error = hispec_laser_validate_channel_settings(laser, &s);
		if (out->laser_error == 0) {
			out->laser_error = hispec_laser_update_channel_settings(laser, &s, false, NULL, 0);
		}
		out->laser_applied = out->laser_error == 0;

		if (out->laser_applied && persist) {
			out->laser_persist_error = app_settings_update_laser_channel(laser, &s, true);
			out->laser_persisted = out->laser_persist_error == 0;
		}
	}

	/* Route restoration is independent of core restoration. The fiber choice
	 * is irrelevant here: only the outbound input/output names form the key. */
	char input[16];
	char route_output[16];
	char pd_input[16];
	char pd_output[16];
	char key[40];
	route_names(laser, fei, 'S', input, route_output, pd_input, pd_output);
	snprintk(key, sizeof(key), "%s_to_%s", input, route_output);
	out->route_error = app_settings_reset_route_loss(key, hispec_laser_name(laser), false);
	out->route_applied = out->route_error == 0;

	if (out->route_applied && persist) {
		out->route_persist_error = app_settings_reset_route_loss(key, hispec_laser_name(laser), true);
		out->route_persisted = out->route_persist_error == 0;
	}

	return out->laser_error != 0 ? out->laser_error : out->route_error != 0 ? out->route_error :
		out->laser_persist_error != 0 ? out->laser_persist_error : out->route_persist_error;
}

/** Format a coherent caller-owned snapshot. Invalid numbers and absent errors
 * become null. Every failure is text; ENAME is used only for an unexpected
 * errno. No locking/I/O; a short buffer returns ENOSPC instead of truncating. */
int laser_calibration_format_status(char *payload, size_t size,
				    const struct laser_calibration_status *s)
{
	static const char *const states[] = {
		"inactive", "running", "fitting", "awaiting_output", "complete", "stopped", "error",
	};
	const struct laser_calibration_fit *f = &s->fit;
	size_t off = 0;

/* Local append macros keep the short-buffer exit beside each field. Nine
 * significant digits round-trip a stored float correction without long zeros. */
#define BOOL(v) ((v) ? "true" : "false")
#define APPEND(...) do { \
	if (coo_json_append(payload, size, &off, __VA_ARGS__) != 0) { \
		return -ENOSPC; \
	} \
} while (0)
#define NUMBER(key, value, valid) do { \
	APPEND(",\"" key "\":"); \
	if ((valid) && isfinite(value)) { \
		APPEND("%.9g", (double)(value)); \
	} else { \
		APPEND("null"); \
	} \
} while (0)
#define ERROR(key, code) do { \
	const char *message = laser_calibration_error_message(code); \
	APPEND(",\"" key "\":"); \
	if (message != NULL) { \
		APPEND("\"%s\"", message); \
	} else { \
		APPEND("null"); \
	} \
} while (0)

	APPEND("{\"state\":\"%s\",\"run_id\":%u,\"laser\":\"%s\",\"output\":\"%s_%s\","
		"\"records\":%u,\"dwell_ms\":%u,\"current_ma\":%.1f,"
		"\"acquisition_complete\":%s,\"overflow\":%s,\"shutdown_pending\":%s",
		states[s->state], s->run_id, hispec_laser_name(s->laser),
		s->laser <= HISPEC_LASER_1430_YJ ? "yj" : "hk",
		(s->first_fei ^ (s->output_index != 0)) ? "fei" : "ao", s->records, s->dwell_ms,
		(double)s->current_ma, BOOL(s->acquisition_complete), BOOL(s->overflow), BOOL(s->shutdown_pending));
	ERROR("error", s->error);

	APPEND(",\"fit\":{\"valid\":%s,\"accepted\":%s,\"applied\":%s,\"persisted\":%s,\"n\":%u",
		BOOL(f->valid), BOOL(f->accepted), BOOL(f->applied), BOOL(f->persisted), f->points);
	ERROR("error", f->error);
	ERROR("apply_error", f->apply_error);
	ERROR("persist_error", f->persist_error);
	NUMBER("threshold_ma", f->threshold_ma, f->valid);
	NUMBER("efficiency_mw_per_ma", f->efficiency_mw_per_ma, f->valid);
	NUMBER("quadratic_correction_mw_per_ma2", f->quadratic_correction_mw_per_ma2, f->valid);
	NUMBER("floor_ma", f->floor_ma, f->points > 0);
	NUMBER("max_ma", f->max_ma, f->points > 0);
	NUMBER("baseline_mv", f->baseline_mv, f->points > 0);
	NUMBER("rms_fraction", f->rms_fraction, f->valid);
	NUMBER("max_residual_fraction", f->max_residual_fraction, f->valid);
	NUMBER("threshold_se_ma", f->threshold_se_ma, f->valid);

	APPEND("},\"noise\":{\"valid\":%s,\"applied\":%s,\"persisted\":%s",
		BOOL(f->noise_valid), BOOL(f->noise_applied), BOOL(f->noise_persisted));
	ERROR("error", f->noise_error);
	NUMBER("constant_mw", f->constant_noise_mw, f->noise_valid);
	NUMBER("fractional", f->fractional_noise, f->noise_valid);

	APPEND("},\"routes\":[");
	for (unsigned i = 0; i < 2; ++i) {
		const struct laser_calibration_route_fit *r = &s->route[i];
		APPEND("%s{\"accepted\":%s,\"applied\":%s,\"persisted\":%s",
			i ? "," : "", BOOL(r->accepted), BOOL(r->applied), BOOL(r->persisted));
		ERROR("error", r->error);
		ERROR("apply_error", r->apply_error);
		ERROR("persist_error", r->persist_error);
		NUMBER("combined_db", r->combined_loss_db, r->accepted);
		NUMBER("outbound_db", r->outbound_loss_db, r->accepted);
		APPEND("}");
	}
	APPEND("]}");

#undef ERROR
#undef NUMBER
#undef APPEND
#undef BOOL
	return 0;
}

/* IEEE-754 little-endian wire values; explicit encoding avoids target padding. */
static void put_float(uint8_t *bytes, float value)
{
	uint32_t bits;
	memcpy(&bits, &value, sizeof(bits));
	sys_put_le32(bits, bytes);
}

/* Encode a double using the same wire convention; no alignment requirement. */
static void put_double(uint8_t *bytes, double value)
{
	uint64_t bits;
	memcpy(&bits, &value, sizeof(bits));
	sys_put_le64(bits, bytes);
}

/** Copy metadata (chunk=-1) or a bounded raw-record chunk into caller storage.
 * Waits on calibration_lock so replacement and append cannot race the copy.
 * Performs no hardware/network/flash I/O. The caller sends the encoded bytes;
 * -ENODATA means another calibration owns the one shared retained dataset. */
int laser_calibration_write_data(void *payload, size_t size, int chunk, size_t *written)
{
	uint8_t *bytes = payload;
	*written = 0;
	k_mutex_lock(&calibration_lock, K_FOREVER);
	int rc = 0;

	/* Never interpret the attenuator member of the shared union as laser data. */
	if (atomic_get(&calibration_data_kind) != CALIBRATION_DATA_LASER) {
		rc = -ENODATA;
		goto done;
	}

	if (chunk == -1) {
		if (size < WIRE_METADATA_SIZE) {
			rc = -ENOSPC;
			goto done;
		}
		memset(bytes, 0, WIRE_METADATA_SIZE);

		/* Bytes 0..15: format identity and one-byte selectors/counts.
		 * Byte 15 remains reserved/zero. Field order matches the host decoder. */
		memcpy(bytes, "HLC1", 4);
		bytes[4] = 2;
		bytes[5] = result.state;
		bytes[6] = meta.laser;
		bytes[7] = meta.channel;
		bytes[8] = meta.fiber;
		bytes[9] = meta.first_fei;
		bytes[10] = meta.options;
		bytes[11] = WIRE_RECORD_SIZE;
		bytes[12] = result.records;
		bytes[13] = WIRE_CHUNK_RECORDS;
		bytes[14] = run.segments;

		/* Bytes 16..31: run identity and acquisition timing (uptime, not UTC). */
		sys_put_le32(meta.run_id, bytes + 16);
		sys_put_le32(meta.dwell_ms, bytes + 20);
		sys_put_le64(meta.started_ms, bytes + 24);

		/* Bytes 32..151: fifteen frozen optical/laser assumptions as doubles. */
		const double values[] = {
			meta.ref_mw,
			meta.ref_ma,
			meta.ref_temp_c,
			meta.max_ma,
			meta.nominal_ma,
			meta.seed_threshold_ma,
			meta.current_cal_pct,
			meta.pd_gain_mv_per_mw,
			meta.dark_mv,
			meta.dark_rms_mv,
			meta.return_tx,
			meta.loopback_loss_db[0],
			meta.loopback_loss_db[1],
			meta.default_threshold_ma,
			meta.default_efficiency_mw_per_ma,
		};
		size_t off = 32;
		for (unsigned i = 0; i < ARRAY_SIZE(values); ++i, off += 8) {
			put_double(bytes + off, values[i]);
		}

		/* Bytes 152..175: DAC scale/limit pairs and observed temperature range. */
		for (unsigned i = 0; i < 2; ++i, off += 4) {
			put_float(bytes + off, meta.dac_full_scale_mv[i]);
		}
		for (unsigned i = 0; i < 2; ++i, off += 4) {
			put_float(bytes + off, meta.dac_limit_mv[i]);
		}
		put_float(bytes + off, meta.temp_min_c);
		put_float(bytes + off + 4, meta.temp_max_c);
		off += 8;

		/* Bytes 176..319: two FVOA snapshots, each six doubles then six floats. */
		for (unsigned i = 0; i < 2; ++i) {
			const struct attenuator_model_coeffs *c = &meta.coeff[i];
			const double coefficients[] = {
				c->fvoa_50pct_mv,
				c->slope_inv_fvoa_mv,
				c->max_atten_db,
				c->max_calibrated_db,
				c->gain,
				c->rms_db,
			};
			for (unsigned j = 0; j < ARRAY_SIZE(coefficients); ++j, off += 8) {
				put_double(bytes + off, coefficients[j]);
			}
			for (unsigned j = 0; j < ATTENUATOR_MODEL_CORRECTION_TERMS; ++j, off += 4) {
				put_float(bytes + off, c->correction_coeff[j]);
			}
		}
		/* Bytes 320..431: requested policy, measured normalization and the
		 * exact 128-bit support mask. All numbers remain explicit LE values. */
		put_double(bytes + off, meta.max_fit_residual_pct);
		put_double(bytes + off + 8, meta.pd_target_mv);
		off += 16;
		for (unsigned j = 0; j < LASER_CAL_SEGMENTS; ++j, off += 8) {
			put_double(bytes + off, meta.segment_mv_per_mw[j]);
		}
		put_double(bytes + off, meta.baseline_mv);
		put_double(bytes + off + 8, meta.dark_variance_mv2);
		off += 16;
		memcpy(bytes + off, meta.fit_support, sizeof(meta.fit_support));
		off += sizeof(meta.fit_support);
		__ASSERT_NO_MSG(off == WIRE_METADATA_SIZE);
		*written = off;
	} else {
		if (chunk < 0 || (unsigned)chunk * WIRE_CHUNK_RECORDS >= result.records) {
			rc = -ERANGE;
			goto done;
		}
		unsigned start = (unsigned)chunk * WIRE_CHUNK_RECORDS;
		unsigned count = MIN(WIRE_CHUNK_RECORDS, result.records - start);
		if (size < count * WIRE_RECORD_SIZE) {
			rc = -ENOSPC;
			goto done;
		}

		/* Explicit 24-byte records; never copy target struct padding to wire. */
		for (unsigned i = 0; i < count; ++i) {
			uint8_t *b = bytes + i * WIRE_RECORD_SIZE;
			const struct laser_cal_record *r = &data.records[start + i];
			sys_put_le32(r->elapsed_ms, b);
			put_float(b + 4, r->net_mv);
			put_float(b + 8, r->rms_mv);
			sys_put_le16(r->current_tenth_ma, b + 12);
			sys_put_le16(r->dac_code[0], b + 14);
			sys_put_le16(r->dac_code[1], b + 16);
			sys_put_le16((uint16_t)r->max_raw, b + 18);
			b[20] = r->good;
			b[21] = r->failed;
			b[22] = r->segment;
			b[23] = r->flags;
		}
		*written = count * WIRE_RECORD_SIZE;
	}

done:
	k_mutex_unlock(&calibration_lock);
	return rc;
}
