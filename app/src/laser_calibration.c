/**
 * Reference-anchored laser calibration on the existing throughput thread.
 *
 * Fixed-attenuation segments obey V=b+a_j*(I-threshold). Segment gains absorb
 * PD gain, path loss and FVOA scale error; the vendor reference sets the absolute
 * mW/mA scale. Profiling the a_j leaves only threshold and baseline for the
 * shared two-variable fitter. Zero-current windows constrain the shared offset.
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
#include "command.h"
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

#define data calibration_data.laser
#define meta data.meta
#define result data.result
#define work calibration_scratch.laser
#define RECORDS CALIBRATION_RECORD_COUNT
#define SETTLE_MS 100U
#define PEAK_TARGET_MV 1850.0
#define TARGET_MV 1500.0
#define RANGE_LOW_MV 250.0
#define Q_VARIANCE (0.0625 * 0.0625 / 12.0)
#define OPT_LOSS 1U
#define OPT_NOISE 2U
#define OPT_OTHER 4U
#define OPT_PERSIST 8U
#define OTHER_RECORDS 24U
#define WIRE_RECORD_SIZE 24U
#define WIRE_CHUNK_RECORDS (COO_CMD_PAYLOAD_MAX / WIRE_RECORD_SIZE)
#define WIRE_METADATA_SIZE 320U

/* Flags: classification in bits 0..1, event in 2..4, output in bit 7. */
enum point_class { POINT_OK, POINT_SATURATED, POINT_LOW, POINT_ADC_ERROR };
enum point_event { EVENT_SWEEP, EVENT_PROBE, EVENT_OVERLAP, EVENT_REFERENCE, EVENT_DARK, EVENT_REFINE };
enum phase { PHASE_IDLE, PHASE_WARM, PHASE_WINDOW, PHASE_FIT, PHASE_PAUSE };
enum stage { STAGE_DARK, STAGE_SWEEP, STAGE_REFINE, STAGE_END, STAGE_OTHER_DARK, STAGE_OTHER_REFERENCE };

static struct {
	enum phase phase;
	enum stage stage;
	int64_t window_started_ms, temperature_started_ms, next_temperature_ms;
	double atten_db, search_low_db, search_high_db, refine_low_ma, refine_high_ma;
	uint8_t attenuator, grid_count, grid_index, segment, segments, event;
	uint8_t search_tries, search_limit, stable_temperatures, reference_count;
	uint8_t refine_count, refine_index;
	bool searching, refined;
} run;
static struct laser_calibration_status status_snapshot;
static K_MUTEX_DEFINE(status_lock);
static atomic_t fit_cancel;
static K_SEM_DEFINE(fit_done, 0, 1);

static bool is_other(const struct laser_cal_record *r) { return (r->flags & 0x80U) != 0; }
static unsigned event_of(const struct laser_cal_record *r) { return (r->flags >> 2) & 7U; }
static double current_of(const struct laser_cal_record *r) { return (double)r->current_tenth_ma / 10.0; }

/* Per-reading empirical variance; the PD stores a population RMS. One surviving
 * ADC sample still supplies a mean, but cannot supply a temporal variance. */
static double reading_variance(const struct laser_cal_record *r)
{
	return r->good > 1 ? (double)r->rms_mv * (double)r->rms_mv * r->good / (r->good - 1U) : 0.0;
}

static double mean_weight(const struct laser_cal_record *r)
{
	return r->good / fmax(reading_variance(r), work.dark_variance);
}

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
	double variance_sum = 0.0, dof = 0.0, sum = 0.0;
	*weight = 0.0;
	for (uint8_t i = 0; i < result.records; ++i) {
		const struct laser_cal_record *r = &data.records[i];
		if (is_other(r) != other || event_of(r) != EVENT_DARK || r->good == 0 ||
		    (r->flags & 3U) == POINT_SATURATED) continue;
		if (r->good > 1) {
			variance_sum += reading_variance(r) * (r->good - 1U);
			dof += r->good - 1U;
		}
	}
	/* Means with one surviving sample remain retained, but cannot establish
	 * the empirical off-state variance needed for fit uncertainties. */
	if (!(dof > 0.0)) return -ERANGE;
	work.dark_variance = fmax(variance_sum / dof, Q_VARIANCE);
	for (uint8_t i = 0; i < result.records; ++i) {
		const struct laser_cal_record *r = &data.records[i];
		if (is_other(r) != other || event_of(r) != EVENT_DARK || r->good == 0 ||
		    (r->flags & 3U) == POINT_SATURATED) continue;
		double w = mean_weight(r);
		*weight += w;
		sum += w * (double)r->net_mv;
	}
	if (!(*weight > 0.0)) return -ERANGE;
	*baseline = sum / *weight;
	return 0;
}

/* Rebuild weighted sufficient statistics for a contiguous current interval.
 * Repeated anchor windows add precision but do not count as distinct currents. */
static int select_fit_points(double low, double high, double *smallest)
{
	memset(work.sums, 0, sizeof(work.sums));
	memset(work.count, 0, sizeof(work.count));
	memset(work.include, 0, sizeof(work.include));
	for (uint8_t i = 0; i < result.records; ++i) {
		const struct laser_cal_record *r = &data.records[i];
		double x = current_of(r);
		if (!curve_point(r) || x < low - 1e-7 || x > high + 1e-7) continue;
		work.include[i] = 1;
		bool distinct = true;
		for (uint8_t k = 0; k < i; ++k) {
			if (work.include[k] && data.records[k].segment == r->segment &&
			    data.records[k].current_tenth_ma == r->current_tenth_ma) distinct = false;
		}
		work.count[r->segment] += distinct;
	}
	unsigned distinct_total = 0;
	*smallest = INFINITY;
	for (uint8_t i = 0; i < result.records; ++i) {
		const struct laser_cal_record *r = &data.records[i];
		if (!work.include[i]) continue;
		if (work.count[r->segment] < 3) { work.include[i] = 0; continue; }
		double x = current_of(r), w = mean_weight(r), y = (double)r->net_mv;
		double *s = work.sums[r->segment];
		s[0] += w; s[1] += w*x; s[2] += w*x*x; s[3] += w*y; s[4] += w*x*y;
		*smallest = fmin(*smallest, x);
		bool distinct = true;
		for (uint8_t k = 0; k < i; ++k) {
			if (work.include[k] && data.records[k].current_tenth_ma == r->current_tenth_ma) distinct = false;
		}
		distinct_total += distinct;
	}
	return distinct_total >= 6 ? 0 : -ERANGE;
}

/* Profile each gain at every trial. Differentiating that analytic solution keeps
 * the optimizer two-dimensional even when several attenuation ranges are used. */
static int laser_fit_evaluate(void *unused, const double p[2], bool derivatives,
			      struct calibration_normal *n)
{
	ARG_UNUSED(unused);
	double t = p[0], b = p[1];
	for (unsigned j = 0; j < LASER_CAL_SEGMENTS; ++j) {
		const double *s = work.sums[j];
		work.gain[j] = 0.0;
		if (!(s[0] > 0.0)) continue;
		double sx = s[1] - t*s[0];
		double denominator = s[2] - 2.0*t*s[1] + t*t*s[0];
		double numerator = s[4] - t*s[3] - b*sx;
		if (!(denominator > 0.0) || !(numerator > 0.0)) return -ERANGE;
		work.gain[j] = numerator / denominator;
		work.dt[j] = (-(s[3] - b*s[0]) + 2.0*work.gain[j]*sx) / denominator;
		work.db[j] = -sx / denominator;
	}
	for (uint8_t i = 0; i < result.records; ++i) {
		if (atomic_get(&fit_cancel)) return -ECANCELED;
		if (!work.include[i]) continue;
		const struct laser_cal_record *r = &data.records[i];
		unsigned j = r->segment;
		double x = current_of(r) - t, w = mean_weight(r);
		double residual = b + work.gain[j]*x - (double)r->net_mv;
		n->cost += w*residual*residual;
		if (derivatives) {
			double j0 = work.dt[j]*x - work.gain[j], j1 = 1.0 + work.db[j]*x;
			n->h00 += w*j0*j0; n->h01 += w*j0*j1; n->h11 += w*j1*j1;
			n->g0 += w*j0*residual; n->g1 += w*j1*residual;
		}
	}
	double residual = b - work.baseline_mean;
	n->cost += work.baseline_weight*residual*residual;
	if (derivatives) { n->h11 += work.baseline_weight; n->g1 += work.baseline_weight*residual; }
	return isfinite(n->cost) ? 0 : -ERANGE;
}

static int fit_interval(double low, double high, double p[2])
{
	double smallest;
	int rc = select_fit_points(low, high, &smallest);
	if (rc != 0) return rc;
	const double lower[2] = {0.0, -2048.0}, upper[2] = {smallest - 0.01, 2048.0};
	const double tolerance[2] = {1.0e-6, 1.0e-6};
	if (!(upper[0] > 0.0)) return -ERANGE;
	p[0] = CLAMP(p[0], lower[0], upper[0]);
	rc = calibration_fit_2(laser_fit_evaluate, NULL, lower, upper, tolerance, p);
	/* The last trial may have been rejected. Refresh profiled gains at the
	 * returned parameters before calculating diagnostics or another interval. */
	struct calibration_normal n = {0};
	if (rc == 0) rc = laser_fit_evaluate(NULL, p, true, &n);
	if (rc == 0) {
		double determinant = n.h00*n.h11 - n.h01*n.h01;
		if (!(determinant > 0.0)) return -ERANGE;
		result.fit.threshold_se_ma = sqrt(n.h11 / determinant);
	}
	return rc;
}

/* Segments below the seed interval get a provisional scale from their three
 * highest distinct current levels. The final joint fit refines eligible gains;
 * this provisional scale is only used to find the knee/upper roll-off. */
static void extend_segment_gains(const double p[2])
{
	for (unsigned j = 0; j < LASER_CAL_SEGMENTS; ++j) {
		if (work.gain[j] > 0.0) continue;
		double ceiling = INFINITY, numerator = 0.0, denominator = 0.0;
		for (unsigned level = 0; level < 3; ++level) {
			double next = -1.0;
			for (uint8_t i = 0; i < result.records; ++i) {
				const struct laser_cal_record *r = &data.records[i];
				double x = current_of(r);
				if (curve_point(r) && r->segment == j && x < ceiling && x > p[0] + 0.01) next = fmax(next, x);
			}
			if (next < 0.0) break;
			for (uint8_t i = 0; i < result.records; ++i) {
				const struct laser_cal_record *r = &data.records[i];
				if (!curve_point(r) || r->segment != j || fabs(current_of(r)-next) > 1e-7) continue;
				double w = mean_weight(r), x = next-p[0];
				numerator += w*x*((double)r->net_mv-p[1]); denominator += w*x*x;
			}
			ceiling = next - 1e-7;
		}
		if (denominator > 0.0) work.gain[j] = fmax(0.0, numerator / denominator);
	}
}

/* Choose one highest-precision observation per distinct current (including
 * overlaps), so repeated reference windows cannot create a sustained departure. */
static int best_at_current(double current)
{
	int best = -1;
	double precision = -1.0;
	for (uint8_t i = 0; i < result.records; ++i) {
		const struct laser_cal_record *r = &data.records[i];
		if (!curve_point(r) || fabs(current_of(r)-current) > 1e-7 || !(work.gain[r->segment] > 0.0)) continue;
		double score = mean_weight(r) * (double)r->net_mv * (double)r->net_mv;
		if (score > precision) { precision = score; best = i; }
	}
	return best;
}

/* Extend from the reference until three significant >2% departures. Levels
 * without enough precision to establish 2% linearity do not lower the floor. */
static double linear_boundary(const double p[2], bool upward, bool *seed_failed)
{
	double last = meta.ref_ma, boundary = meta.ref_ma;
	double seed_low = meta.seed_threshold_ma + 0.25*(meta.ref_ma-meta.seed_threshold_ma);
	unsigned bad = 0;
	for (unsigned level = 0; level < RECORDS; ++level) {
		double next = upward ? (double)INFINITY : -1.0;
		for (uint8_t i = 0; i < result.records; ++i) {
			const struct laser_cal_record *r = &data.records[i];
			double x = current_of(r);
			if (!curve_point(r) || x <= p[0]+0.01) continue;
			if (upward ? x > last+1e-7 : x < last-1e-7) next = upward ? fmin(next,x) : fmax(next,x);
		}
		if (!isfinite(next) || next < 0.0) break;
		last = next;
		int index = best_at_current(next);
		if (index < 0) continue;
		const struct laser_cal_record *r = &data.records[index];
		double predicted = work.gain[r->segment]*(next-p[0]);
		double departure = fabs((double)r->net_mv-p[1]-predicted);
		double error = sqrt(1.0/mean_weight(r) + 1.0/work.baseline_weight);
		if (departure > 0.02*predicted && departure > 3.0*error) {
			if (++bad == 3) {
				if (!upward && next >= seed_low) *seed_failed = true;
				break;
			}
		} else {
			bad = 0;
			if (3.0*error <= 0.02*predicted) boundary = next;
		}
	}
	return boundary;
}

/* Two-column NNLS by testing the interior, both axes, and the origin. Columns
 * are scaled before solving; unidentifiable terms must not overwrite settings.
 * Negative observed excess variance stays signed to avoid a positive bias. */
static int fit_noise(const double p[2])
{
	double c2 = 0.0, f2 = 0.0;
	for (unsigned pass = 0; pass < 3; ++pass) {
		double h00=0.0,h01=0.0,h11=0.0,g0=0.0,g1=0.0;
		unsigned count = 0;
		for (uint8_t i = 0; i < result.records; ++i) {
			if (atomic_get(&fit_cancel)) return -ECANCELED;
			if (!work.include[i] || data.records[i].good < 2) continue;
			const struct laser_cal_record *r = &data.records[i];
			double gain = work.gain[r->segment]/result.fit.efficiency_mw_per_ma;
			double mu = work.gain[r->segment]*(current_of(r)-p[0]);
			double x0=gain*gain, x1=mu*mu, y=reading_variance(r)-work.dark_variance;
			double v = work.dark_variance+x0*c2+x1*f2;
			double w = pass == 0 ? 1.0 : (r->good-1U)/(2.0*v*v);
			h00+=w*x0*x0; h01+=w*x0*x1; h11+=w*x1*x1; g0+=w*x0*y; g1+=w*x1*y;
			++count;
		}
		if (count < 6 || !(h00 > 0.0) || !(h11 > 0.0) || !isfinite(h00) || !isfinite(h11)) return -ERANGE;
		double scale0=sqrt(h00),scale1=sqrt(h11),cross=h01/scale0/scale1;
		if (1.0-cross*cross < 1e-12) return -ERANGE;
		double candidates[4][2] = {{0.0,0.0},{fmax(0.0,g0/h00),0.0},{0.0,fmax(0.0,g1/h11)},{0.0,0.0}};
		double interior[2];
		int rc = calibration_solve_2x2(1.0,cross,1.0,g0/scale0,g1/scale1,interior);
		if (rc == 0 && interior[0] >= 0.0 && interior[1] >= 0.0) {
			candidates[3][0]=interior[0]/scale0; candidates[3][1]=interior[1]/scale1;
		}
		double best=INFINITY;
		for (unsigned k=0;k<4;++k) {
			double a=candidates[k][0],b=candidates[k][1];
			double cost=h00*a*a+2.0*h01*a*b+h11*b*b-2.0*(g0*a+g1*b);
			if (cost < best) { best=cost; c2=a; f2=b; }
		}
	}
	result.fit.constant_noise_mw=sqrt(c2);
	result.fit.fractional_noise=sqrt(f2);
	return isfinite(c2) && isfinite(f2) ? 0 : -ERANGE;
}

/* Pure numerical phase: immutable acquisition data, no hardware or publishing.
 * The shared workspace is reused for region selection, profiling and variance. */
static int fit_laser(void)
{
	memset(&result.fit, 0, sizeof(result.fit));
	int rc = fit_baseline(false, &work.baseline_mean, &work.baseline_weight);
	if (rc != 0) return rc;
	double p[2] = {meta.seed_threshold_ma, work.baseline_mean};
	double seed_low = meta.seed_threshold_ma + 0.25*(meta.ref_ma-meta.seed_threshold_ma);
	rc = fit_interval(seed_low, meta.ref_ma, p);
	if (rc != 0) return rc;
	extend_segment_gains(p);
	bool seed_failed = false;
	double low = linear_boundary(p, false, &seed_failed);
	double high = linear_boundary(p, true, &seed_failed);
	/* The reference itself must agree; a roll-off at ref_ma cannot supply the
	 * fixed anchor for an accepted linear efficiency model. */
	int anchor = best_at_current(meta.ref_ma);
	if (anchor < 0) return -ERANGE;
	const struct laser_cal_record *r = &data.records[anchor];
	double mu = work.gain[r->segment]*(meta.ref_ma-p[0]);
	double err = sqrt(1.0/mean_weight(r)+1.0/work.baseline_weight);
	seed_failed |= fabs((double)r->net_mv-p[1]-mu) > fmax(0.02*mu,3.0*err);
	if (!seed_failed) rc = fit_interval(low, high, p);
	result.fit.valid = true;
	result.fit.threshold_ma = p[0]; result.fit.baseline_mv = p[1];
	result.fit.efficiency_mw_per_ma = meta.ref_mw/(meta.ref_ma-p[0]);
	result.fit.floor_ma = low; result.fit.linear_max_ma = high;
	if (rc != 0 || seed_failed) return rc != 0 ? rc : -ERANGE;
	double sum=0.0;
	for (uint8_t i=0;i<result.records;++i) {
		if (!work.include[i]) continue;
		r=&data.records[i];
		double predicted=work.gain[r->segment]*(current_of(r)-p[0]);
		double residual=((double)r->net_mv-p[1]-predicted)/predicted;
		sum+=residual*residual; ++result.fit.points;
	}
	result.fit.rms_fraction=sqrt(sum/result.fit.points);
	double eta=result.fit.efficiency_mw_per_ma;
	result.fit.accepted = fabs(p[0]-meta.default_threshold_ma) <= fmax(1.0,0.05*meta.ref_ma) &&
		eta >= 0.75*meta.default_efficiency_mw_per_ma && eta <= 1.25*meta.default_efficiency_mw_per_ma &&
		low <= meta.nominal_ma && low >= p[0]+0.1-1e-7 && low < meta.ref_ma;
	result.fit.noise_error = fit_noise(p);
	result.fit.noise_valid = result.fit.noise_error == 0;
	return result.fit.accepted ? 0 : -ERANGE;
}

/* Hardware ownership: only the throughput thread advances acquisition. Command
 * handlers serialize starts/stops with this same lock; fitting releases it so
 * a stop can cancel and join without exposing a half-written status snapshot. */
static void publish_status(struct laser_calibration_status *out, bool emit)
{
	k_mutex_lock(&status_lock, K_FOREVER);
	status_snapshot = result;
	if (out != NULL) *out = result;
	k_mutex_unlock(&status_lock);
	if (!emit) return;
	struct coo_cmd_response *msg = &calibration_telemetry;
	memset(msg, 0, sizeof(*msg));
	if (laser_calibration_format_status(msg->payload, sizeof(msg->payload), &result) != 0) return;
	msg->payload_len = strlen(msg->payload);
	(void)coo_cmd_runtime_emit(command_runtime_get(), &(struct coo_cmd_runtime_emit_args){
		.type = COO_CMD_RUNTIME_EMIT_DATA, .delivery = COO_CMD_RUNTIME_EMIT_BEST_EFFORT,
		.suffix = "laser/calibrate", .out = msg,
	});
}

void laser_calibration_get_status(struct laser_calibration_status *status)
{
	if (status == NULL) return;
	k_mutex_lock(&status_lock, K_FOREVER);
	*status = status_snapshot;
	k_mutex_unlock(&status_lock);
}

bool laser_calibration_active(void)
{
	k_mutex_lock(&status_lock, K_FOREVER);
	bool active = status_snapshot.state == LASER_CAL_RUNNING || status_snapshot.state == LASER_CAL_FITTING ||
		status_snapshot.state == LASER_CAL_AWAITING_OUTPUT || status_snapshot.shutdown_pending;
	k_mutex_unlock(&status_lock);
	return active;
}

/* Shutdown failure retains the source identity for an explicit stop retry. */
static int stop_source(void)
{
	int rc = hispec_laser_stop_output(result.laser, false, NULL, 0);
	result.shutdown_pending = rc != 0;
	if (rc == 0) result.current_ma = 0.0f;
	return rc;
}

static void fail_acquisition(int error)
{
	run.phase = PHASE_IDLE;
	result.error = error;
	result.state = LASER_CAL_ERROR;
	(void)stop_source();
	housekeeping_photodiode_autooff_inhibit((enum housekeeping_power_output)meta.channel, false);
	publish_status(NULL, true);
}

int laser_calibration_stop(struct laser_calibration_status *status)
{
	k_mutex_lock(&calibration_lock, K_FOREVER);
	if (atomic_get(&calibration_data_kind) != CALIBRATION_DATA_LASER) {
		k_mutex_unlock(&calibration_lock);
		laser_calibration_get_status(status);
		return 0;
	}
	if (run.phase == PHASE_FIT) {
		atomic_set(&fit_cancel, 1);
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

static int validate_output(enum hispec_laser_id laser, const char *output, bool *fei)
{
	if (laser < 0 || laser >= HISPEC_LASER_COUNT || output == NULL) return -EINVAL;
	const char *ao = laser <= HISPEC_LASER_1430_YJ ? "yj_ao" : "hk_ao";
	const char *other = laser <= HISPEC_LASER_1430_YJ ? "yj_fei" : "hk_fei";
	if (strcmp(output, ao) != 0 && strcmp(output, other) != 0) return -EINVAL;
	*fei = strcmp(output, other) == 0;
	return 0;
}

static int set_attenuation(double db)
{
	if (!attenuator_set_db(&attenuators[run.attenuator], db, true)) return -EIO;
	struct attenuator a;
	attenuator_snapshot(&attenuators[run.attenuator], &a);
	if (!a.dac_cfg1.valid || !a.dac_cfg2.valid) return -EIO;
	run.atten_db = a.attenuation_db;
	return 0;
}

/* Settle after every actuator write, then exclude any ADC conversion already
 * in flight. Window sample_length counts attempted samples, including failures. */
static int begin_window(unsigned event)
{
	k_msleep(SETTLE_MS);
	int duration = photodiode_set_configurable_window_duration(meta.channel, meta.dwell_ms);
	if (duration < 0) return duration;
	meta.dwell_ms = result.dwell_ms = (uint32_t)duration;
	run.event = event;
	run.window_started_ms = k_uptime_get();
	run.phase = PHASE_WINDOW;
	return 0;
}

static int set_current(double ma, unsigned event)
{
	int rc = hispec_laser_set_current_ma(result.laser, ma, NULL, 0);
	if (rc != 0) return rc;
	result.current_ma = (float)ma;
	return begin_window(event);
}

/* Insert directly into a small descending, quantized grid. No second point
 * buffer: overlaps/probes and refinement are retained only as raw records. */
static void add_grid_current(double ma)
{
	uint16_t q = (uint16_t)llround(CLAMP(ma, 0.0, meta.max_ma)*10.0);
	unsigned i = 0;
	while (i < run.grid_count && data.grid[i] > q) ++i;
	if (i < run.grid_count && data.grid[i] == q) return;
	__ASSERT_NO_MSG(run.grid_count < LASER_CAL_GRID_COUNT);
	memmove(&data.grid[i+1], &data.grid[i], (run.grid_count-i)*sizeof(data.grid[0]));
	data.grid[i] = q;
	++run.grid_count;
}

static void make_grid(void)
{
	for (unsigned i=0; i<33; ++i) add_grid_current(meta.max_ma-(meta.max_ma-meta.seed_threshold_ma)*i/32.0);
	double span=meta.ref_ma-meta.seed_threshold_ma;
	for (unsigned i=0; i<20; ++i) add_grid_current(meta.seed_threshold_ma+0.1*pow(span/0.1,i/19.0));
	for (unsigned i=0; i<9; ++i) add_grid_current(meta.seed_threshold_ma*i/8.0);
	add_grid_current(meta.ref_ma);
}

/* Temperature is checked once per second. A drift discards the current partial
 * window and resumes only after three consecutive checks within 0.1 C. */
static int check_temperature(void)
{
	int64_t now=k_uptime_get();
	if (now < run.next_temperature_ms) return 0;
	run.next_temperature_ms=now+1000;
	struct hispec_laser_status s;
	int rc=hispec_laser_get_status(result.laser,false,&s);
	if (rc != 0) return rc;
	double temp=s.tec_temperature_measured_c;
	if (!isfinite(temp) || !s.tec_started) return -EIO;
	meta.temp_min_c=fminf(meta.temp_min_c,(float)temp);
	meta.temp_max_c=fmaxf(meta.temp_max_c,(float)temp);
	if (fabs(temp-meta.ref_temp_c) > 0.1) {
		if (run.phase != PHASE_WARM) run.temperature_started_ms=now;
		run.stable_temperatures=0;
		run.phase=PHASE_WARM;
	} else if (run.phase == PHASE_WARM && ++run.stable_temperatures >= 3) {
		return begin_window(run.event);
	}
	return run.phase == PHASE_WARM && now-run.temperature_started_ms >= 60000 ? -ETIMEDOUT : 0;
}

static unsigned record_limit(void)
{
	return result.output_index == 0 && (meta.options & OPT_OTHER) ? RECORDS-OTHER_RECORDS : RECORDS;
}

static int retain_window(const struct photodiode_window_result *window)
{
	if (result.records >= record_limit()) { result.overflow=true; return -ENOSPC; }
	struct attenuator a;
	attenuator_snapshot(&attenuators[run.attenuator],&a);
	struct laser_cal_record *r=&data.records[result.records++];
	r->elapsed_ms=(uint32_t)(k_uptime_get()-meta.started_ms);
	r->net_mv=(float)window->mean_net_mv;
	r->rms_mv=(float)window->rms_mv;
	r->current_tenth_ma=(uint16_t)lroundf(result.current_ma*10.0f);
	r->dac_code[0]=(uint16_t)lroundf(a.dac_cfg1.voltage*4096.0f/meta.dac_full_scale_mv[0]);
	r->dac_code[1]=(uint16_t)lroundf(a.dac_cfg2.voltage*4096.0f/meta.dac_full_scale_mv[1]);
	r->max_raw=window->max_raw;
	r->failed=(uint8_t)window->failed_samples;
	r->good=(uint8_t)(window->sample_length-window->failed_samples);
	r->segment=run.segment;
	double sigma=fmax(window->rms_mv,sqrt(Q_VARIANCE));
	unsigned classification=r->good == 0 ? POINT_ADC_ERROR : window->max_mv >= 2000.0 ? POINT_SATURATED :
		window->mean_net_mv < 5.0*sigma/sqrt(r->good) ? POINT_LOW : POINT_OK;
	r->flags=(uint8_t)(classification | (run.event<<2) | (result.output_index ? 0x80U : 0));
	return r->good == 0 ? -EIO : 0;
}

/* Bracket a usable PD range without using a clipped mean. Each attenuator
 * change creates a new fixed-gain segment only once a usable setting is found. */
static int start_search(unsigned limit)
{
	/* A new DAC pair replaces any earlier incomplete anchor group at this
	 * output. Keep those windows as curve/overlap observations. */
	if (fabs((double)result.current_ma-meta.ref_ma) < 0.05) {
		for (uint8_t i=0;i<result.records;++i) {
			struct laser_cal_record *r=&data.records[i];
			if (is_other(r) == (result.output_index != 0) && event_of(r) == EVENT_REFERENCE)
				r->flags=(r->flags & 0x83U) | (EVENT_SWEEP<<2);
		}
	}
	run.searching=true;
	run.search_tries=0;
	run.search_limit=limit;
	run.search_low_db=0.0;
	run.search_high_db=fmin(meta.coeff[0].max_calibrated_db,55.0)+fmin(meta.coeff[1].max_calibrated_db,55.0);
	return begin_window(EVENT_PROBE);
}

static int search_step(const struct photodiode_window_result *w)
{
	++run.search_tries;
	double signal=w->mean_net_mv;
	bool safe=w->max_mv < PEAK_TARGET_MV;
	bool usable=safe && (signal >= 0.65*TARGET_MV || run.atten_db < 0.05);
	if (usable || (run.search_tries >= run.search_limit && safe && signal >= RANGE_LOW_MV)) {
		run.searching=false;
		if (!result.output_index && run.segments >= LASER_CAL_SEGMENTS) return -ENOSPC;
		run.segment=result.output_index ? UINT8_MAX : run.segments++;
		run.reference_count=0;
		return begin_window(fabs((double)result.current_ma-meta.ref_ma)<0.05 ? EVENT_REFERENCE : EVENT_OVERLAP);
	}
	if (run.search_tries >= run.search_limit) return -ERANGE;
	if (safe && signal < TARGET_MV) run.search_high_db=run.atten_db;
	else run.search_low_db=run.atten_db;
	double next;
	if (!safe) next=0.5*(run.atten_db+run.search_high_db);
	else if (signal <= 5.0*fmax(w->rms_mv,sqrt(Q_VARIANCE))) next=run.atten_db-10.0;
	else next=run.atten_db-10.0*log10(TARGET_MV/signal);
	next=CLAMP(next,run.search_low_db,run.search_high_db);
	if (fabs(next-run.atten_db) < 0.02) return -ERANGE;
	int rc=set_attenuation(next);
	return rc == 0 ? begin_window(EVENT_PROBE) : rc;
}

static int next_sweep_current(void)
{
	if (++run.grid_index >= run.grid_count) return -ERANGE; /* Grid ends in zero. */
	double ma=data.grid[run.grid_index]/10.0;
	if (ma == 0.0) { run.stage=STAGE_END; return set_current(0.0,EVENT_DARK); }
	run.reference_count=0;
	return set_current(ma,fabs(ma-meta.ref_ma)<0.05 ? EVENT_REFERENCE : EVENT_SWEEP);
}

/* Acquire one selected output's baseline, then range at max (first output) or
 * at the reference (second). MEMS intent follows existing route persistence. */
static int prepare_output(void)
{
	char input[16],output[16],pd_input[16],pd_output[16];
	bool fei=result.first_fei ^ (result.output_index != 0);
	route_names(result.laser,fei,meta.fiber,input,output,pd_input,pd_output);
	int rc=stop_source();
	if (rc == 0) rc=mems_router_apply_named_route(&router,input,output,false,NULL,NULL);
	if (rc == 0) rc=mems_router_apply_named_route(&router,pd_input,pd_output,false,NULL,NULL);
	double high=fmin(meta.coeff[0].max_calibrated_db,55.0)+fmin(meta.coeff[1].max_calibrated_db,55.0);
	if (rc == 0) rc=set_attenuation(high);
	if (rc == 0) rc=hispec_laser_set_tec_temperature_c(result.laser,meta.ref_temp_c);
	if (rc != 0) return rc;
	housekeeping_photodiode_autooff_inhibit((enum housekeeping_power_output)meta.channel,true);
	run.stage=result.output_index ? STAGE_OTHER_DARK : STAGE_DARK;
	run.event=EVENT_DARK;
	run.phase=PHASE_WARM;
	run.stable_temperatures=0;
	run.temperature_started_ms=k_uptime_get();
	run.next_temperature_ms=0;
	return 0;
}

int laser_calibration_start(const struct laser_calibration_request *request, struct laser_calibration_status *status)
{
	bool fei;
	if (request == NULL || validate_output(request->laser,request->output,&fei) != 0 ||
	    (request->fiber != 'M' && request->fiber != 'S') || request->dwell_ms < 100 || request->dwell_ms > 2000 ||
	    (request->other_output && !request->determine_static_loss) ||
	    (request->determine_static_loss && (!isfinite(request->loopback_loss_db) || request->loopback_loss_db < 0.0))) return -EINVAL;
	struct app_laser_channel_settings settings;
	const struct hispec_laser_driver_profile *profile;
	int rc=hispec_laser_get_channel_settings(request->laser,&settings);
	if (rc == 0) rc=hispec_laser_get_driver_profile(request->laser,&profile);
	if (rc != 0) return rc;
	const laserprops_t *p=&settings.properties;
	if (p->ref_ma > p->max_current_ma || p->ref_ma <= p->threshold_current_ma+0.1 ||
	    p->ref_temp_c < p->operating_temp_range_c.min_c || p->ref_temp_c > p->operating_temp_range_c.max_c) return -ERANGE;
	uint8_t index;
	if (attenuator_index_from_laser_id(request->laser,&index) != 0 || !devices_attenuator_channel_available(index)) return -ENODEV;
	uint8_t channel=request->laser <= HISPEC_LASER_1430_YJ ? PHOTODIODE_CHANNEL_YJ : PHOTODIODE_CHANNEL_HK;
	struct app_photodiode_settings pd_settings;
	app_settings_get_photodiode(&pd_settings);
	const struct app_pd_channel_settings *pd=&pd_settings.channel[channel];
	if (!isfinite(pd->dark.mean_mv) || pd->dark.mean_mv < PHOTODIODE_DARK_MIN_MV ||
	    pd->dark.mean_mv > PHOTODIODE_DARK_MAX_MV) return -ERANGE;
	struct photodiode_status pd_status;
	photodiode_get_status(&pd_status);
	if (pd_status.channel[0].dark_pending || pd_status.channel[1].dark_pending) return -EBUSY;
	bool powered=false;
	rc=housekeeping_power_get((enum housekeeping_power_output)channel,&powered);
	if (rc != 0 || !powered) return rc != 0 ? rc : -EACCES;
	char input[16],output[16],pd_input[16],pd_output[16],key[40];
	route_names(request->laser,fei,request->fiber,input,output,pd_input,pd_output);
	if (mems_router_get_route(&router,input,output) == NULL || mems_router_get_route(&router,pd_input,pd_output) == NULL) return -EINVAL;
	snprintk(key,sizeof(key),"%s_to_%s",pd_input,pd_output);
	double return_tx;
	rc=app_settings_get_route_loss(key,hispec_laser_name(request->laser),&return_tx);
	double wavelength=hispec_laser_estimate_wavelength_nm(p,p->ref_temp_c,p->ref_ma);
	double gain=1000.0/photodiode_power_uw_from_mv_at_wavelength(1.0,wavelength,pd);
	if (rc != 0 || !isfinite(gain) || !(gain > 0.0) || !(return_tx > 0.0) || return_tx > 1.0) return -ERANGE;
	rc=attenuator_calibration_stop(NULL);
	if (rc == 0) rc=laser_calibration_stop(NULL);
	if (rc == 0) rc=throughput_monitor_stop(PHOTODIODE_CHANNEL_COUNT,NULL);
	if (rc != 0) return rc;
	k_mutex_lock(&calibration_lock,K_FOREVER);
	memset(&data,0,sizeof(data)); memset(&run,0,sizeof(run));
	atomic_set(&calibration_data_kind,CALIBRATION_DATA_LASER);
	atomic_clear(&fit_cancel);
	meta.run_id=++calibration_run_id; meta.started_ms=(uint64_t)k_uptime_get();
	meta.laser=request->laser; meta.channel=channel; meta.fiber=request->fiber; meta.first_fei=fei;
	meta.dwell_ms=request->dwell_ms;
	meta.options=(request->determine_static_loss?OPT_LOSS:0)|(request->update_noise?OPT_NOISE:0)|
		(request->other_output?OPT_OTHER:0)|(request->persist?OPT_PERSIST:0);
	meta.ref_mw=p->ref_mw; meta.ref_ma=p->ref_ma; meta.ref_temp_c=p->ref_temp_c;
	meta.max_ma=hispec_laser_quantize_current_ma(p->max_current_ma,0.0,p->max_current_ma);
	meta.nominal_ma=p->nominal_current_ma; meta.seed_threshold_ma=p->threshold_current_ma;
	meta.default_threshold_ma=profile->properties->threshold_current_ma;
	meta.default_efficiency_mw_per_ma=profile->properties->efficiency_mw_per_ma;
	meta.current_cal_pct=settings.current_cal_pct; meta.pd_gain_mv_per_mw=gain;
	meta.dark_mv=pd->dark.mean_mv; meta.dark_rms_mv=pd->dark.rms_mv;
	meta.return_tx=return_tx; meta.loopback_loss_db[0]=meta.loopback_loss_db[1]=request->loopback_loss_db;
	meta.temp_min_c=INFINITY; meta.temp_max_c=-INFINITY;
	struct attenuator a;
	attenuator_snapshot(&attenuators[index],&a);
	meta.coeff[0]=a.coeff1; meta.coeff[1]=a.coeff2;
	meta.dac_full_scale_mv[0]=a.dac_cfg1.ideal_full_scale_mv; meta.dac_full_scale_mv[1]=a.dac_cfg2.ideal_full_scale_mv;
	meta.dac_limit_mv[0]=a.dac_cfg1.drive_limit_mv; meta.dac_limit_mv[1]=a.dac_cfg2.drive_limit_mv;
	run.attenuator=index;
	result.laser=request->laser; result.first_fei=fei; result.state=LASER_CAL_RUNNING; result.dwell_ms=meta.dwell_ms;
	make_grid();
	/* Shared laser inputs can combine several diodes. Quiesce them before
	 * capturing the baseline; subsequent cleanup owns only this run's source. */
	rc=hispec_laser_stop_all_outputs(false);
	if (rc == 0) rc=prepare_output();
	if (rc != 0) fail_acquisition(rc);
	publish_status(status,true);
	k_mutex_unlock(&calibration_lock);
	return rc;
}

/* Loss is conditional on the reference, PD responsivity, FVOA calibration,
 * supplied external loopback and configured return transmission. These cannot
 * all be independently identified by the same optical observation. */
static int fit_route(unsigned output_index)
{
	struct laser_calibration_route_fit *f=&result.route[output_index];
	double baseline,weight;
	int rc=fit_baseline(output_index != 0,&baseline,&weight);
	if (rc != 0) return rc;
	double sum=0.0,total=0.0;
	unsigned count=0;
	uint16_t codes[2]={0};
	for (uint8_t i=0; i<result.records; ++i) {
		const struct laser_cal_record *r=&data.records[i];
		if (is_other(r) != (output_index != 0) || event_of(r) != EVENT_REFERENCE ||
		    (r->flags & 3U) != POINT_OK || r->good == 0 || fabs(current_of(r)-meta.ref_ma)>0.05) continue;
		if (count > 0 && (codes[0] != r->dac_code[0] || codes[1] != r->dac_code[1])) return -ERANGE;
		codes[0]=r->dac_code[0]; codes[1]=r->dac_code[1];
		double w=mean_weight(r);
		sum+=w*(double)r->net_mv; total+=w; ++count;
	}
	if (count < 3 || !(sum/total-baseline > 0.0)) return -ERANGE;
	double atten_db=0.0;
	for (unsigned j=0; j<2; ++j) {
		struct atten_model_eval e;
		double mv=(double)codes[j]*(double)meta.dac_full_scale_mv[j]/4096.0;
		if (!atten_model_eval(&meta.coeff[j],(float)mv,&e) ||
		    e.db > fmin(meta.coeff[j].max_calibrated_db,55.0)+0.05) return -ERANGE;
		atten_db+=e.db;
	}
	double pd_mw=(sum/total-baseline)/meta.pd_gain_mv_per_mw;
	f->combined_loss_db=10.0*log10(meta.ref_mw/pd_mw)-atten_db-meta.loopback_loss_db[output_index];
	f->outbound_loss_db=f->combined_loss_db+10.0*log10(meta.return_tx);
	f->accepted=isfinite(f->outbound_loss_db) && f->outbound_loss_db >= 0.0;
	return f->accepted ? 0 : -ERANGE;
}

/* Application and flash writes are separate so a failed persistence operation
 * cannot be reported as though an already-active RAM calibration was undone. */
static void apply_results(void)
{
	struct laser_calibration_fit *f=&result.fit;
	bool persist=(meta.options & OPT_PERSIST) != 0;
	if (result.output_index == 0 && f->accepted) {
		struct app_laser_channel_settings s;
		f->error=hispec_laser_get_channel_settings(result.laser,&s);
		if (f->error == 0) {
			s.properties.threshold_current_ma=f->threshold_ma;
			s.properties.efficiency_mw_per_ma=f->efficiency_mw_per_ma;
			s.min_autolevel_current_ma=f->floor_ma;
			bool noise=(meta.options & OPT_NOISE) && f->noise_valid;
			if (noise) { s.constant_noise_mw=f->constant_noise_mw; s.fractional_noise=f->fractional_noise; }
			f->error=hispec_laser_validate_channel_settings(result.laser,&s);
			if (f->error == 0) f->error=hispec_laser_update_channel_settings(result.laser,&s,false,NULL,0);
			f->applied=f->error == 0;
			f->noise_applied=f->applied && noise;
			if (f->applied && persist) {
				f->persist_error=app_settings_update_laser_channel(result.laser,&s,true);
				f->persisted=f->persist_error == 0;
				f->noise_persisted=f->persisted && noise;
			}
		}
	}
	struct laser_calibration_route_fit *r=&result.route[result.output_index];
	if ((meta.options & OPT_LOSS) && r->accepted) {
		char input[16],output[16],pd_input[16],pd_output[16],key[40];
		route_names(result.laser,result.first_fei ^ (result.output_index != 0),meta.fiber,input,output,pd_input,pd_output);
		snprintk(key,sizeof(key),"%s_to_%s",input,output);
		double tx=pow(10.0,-r->outbound_loss_db/10.0);
		r->error=app_settings_set_route_loss(key,hispec_laser_name(result.laser),tx,false);
		r->applied=r->error == 0;
		if (r->applied && persist) {
			r->persist_error=app_settings_set_route_loss(key,hispec_laser_name(result.laser),tx,true);
			r->persisted=r->persist_error == 0;
		}
	}
}

/* Reuse the floor observation's exact DAC setting and segment. At most eight
 * interior 0.1-mA points refine the bracket, with space reserved for final zero. */
static int start_refinement(void)
{
	run.refined=true;
	if (!result.fit.valid || result.fit.error != 0) return 0;
	double high=result.fit.floor_ma,low=-1.0;
	int best=best_at_current(high);
	if (best < 0) return 0;
	for (uint8_t i=0;i<result.records;++i) {
		const struct laser_cal_record *r=&data.records[i];
		double x=current_of(r);
		if (!is_other(r) && event_of(r) != EVENT_PROBE && x < high-0.05) low=fmax(low,x);
	}
	if (low < 0.0 || high-low < 0.15) return 0;
	unsigned available=record_limit()-result.records;
	if (available <= 1) return 0;
	run.refine_count=(uint8_t)MIN(MIN(8U,available-1U),(unsigned)lround((high-low)*10.0)-1U);
	if (run.refine_count == 0) return 0;
	run.refine_low_ma=low; run.refine_high_ma=high; run.refine_index=0;
	const struct laser_cal_record *r=&data.records[best];
	run.segment=r->segment;
	struct attenuator_target target[2];
	for (unsigned j=0;j<2;++j) target[j]=(struct attenuator_target){
		.value=(double)r->dac_code[j]*(double)meta.dac_full_scale_mv[j]/4096.0, .voltage_mv=true,.update=true};
	if (!attenuator_set_pair(&attenuators[run.attenuator],target)) return -EIO;
	run.stage=STAGE_REFINE;
	double ma=round((high-(high-low)/(run.refine_count+1U))*10.0)/10.0;
	int rc=set_current(ma,EVENT_REFINE);
	return rc == 0 ? 1 : rc;
}

static int advance_window(const struct photodiode_window_result *w)
{
	if (run.searching) return search_step(w);
	switch (run.stage) {
	case STAGE_DARK:
	case STAGE_OTHER_DARK: {
		bool other=run.stage == STAGE_OTHER_DARK;
		run.stage=other ? STAGE_OTHER_REFERENCE : STAGE_SWEEP;
		int rc=set_current(other ? meta.ref_ma : meta.max_ma,EVENT_PROBE);
		return rc == 0 ? start_search(16) : rc;
	}
	case STAGE_SWEEP:
		if (w->max_mv >= PEAK_TARGET_MV || (w->mean_net_mv < RANGE_LOW_MV && run.atten_db >= 0.05)) {
			/* Retain the pre-range point for overlap at this same current. */
			return start_search(4);
		}
		if (run.event == EVENT_REFERENCE && ++run.reference_count < 3) return begin_window(EVENT_REFERENCE);
		return next_sweep_current();
	case STAGE_OTHER_REFERENCE:
		if (run.event == EVENT_REFERENCE && ++run.reference_count < 3) return begin_window(EVENT_REFERENCE);
		run.stage=STAGE_END;
		return set_current(0.0,EVENT_DARK);
	case STAGE_REFINE:
		if (++run.refine_index < run.refine_count) {
			double ma=run.refine_high_ma-(run.refine_high_ma-run.refine_low_ma)*(run.refine_index+1U)/(run.refine_count+1U);
			return set_current(round(ma*10.0)/10.0,EVENT_REFINE);
		}
		run.stage=STAGE_END;
		return set_current(0.0,EVENT_DARK);
	case STAGE_END: {
		int rc=stop_source();
		if (rc != 0) return rc;
		run.phase=PHASE_FIT;
		result.state=LASER_CAL_FITTING;
		atomic_clear(&fit_cancel);
		k_sem_reset(&fit_done);
		return 0;
	}
	}
	return -EINVAL;
}

void laser_calibration_tick(const struct photodiode_status *pd)
{
	k_mutex_lock(&calibration_lock,K_FOREVER);
	if (atomic_get(&calibration_data_kind) != CALIBRATION_DATA_LASER ||
	    (run.phase != PHASE_WARM && run.phase != PHASE_WINDOW)) {
		k_mutex_unlock(&calibration_lock); return;
	}
	bool powered=false;
	int rc=housekeeping_power_get_confirmed((enum housekeeping_power_output)meta.channel,&powered);
	if (rc == 0 && !powered) rc=-EIO;
	bool emitting=false;
	if (rc == 0) rc=hispec_laser_output_status(result.laser,&emitting);
	if (rc == 0 && result.current_ma > 0.0f && !emitting) rc=-EIO;
	if (rc == 0) rc=check_temperature();
	if (rc != 0) { fail_acquisition(rc); k_mutex_unlock(&calibration_lock); return; }
	const struct photodiode_window_result *w=&pd->channel[meta.channel].configurable_window;
	bool ready=run.phase == PHASE_WINDOW && w->end_ms > run.window_started_ms &&
		w->sample_length >= meta.dwell_ms/PHOTODIODE_SAMPLE_INTERVAL_MS;
	if (ready) {
		rc=retain_window(w);
		if (rc == 0) rc=advance_window(w);
		if (rc != 0) fail_acquisition(rc);
		publish_status(NULL,true);
	} else if (run.phase == PHASE_WINDOW && k_uptime_get()-run.window_started_ms > meta.dwell_ms+3000) {
		fail_acquisition(-ETIMEDOUT);
	}
	bool fitting=run.phase == PHASE_FIT;
	k_mutex_unlock(&calibration_lock);
	if (!fitting) return;

	/* Same worker and priority discipline as attenuator fitting. No new stack,
	 * heap, scheduler or queue. Stop joins this bounded numerical section. */
	int priority=k_thread_priority_get(k_current_get());
	k_thread_priority_set(k_current_get(),K_LOWEST_APPLICATION_THREAD_PRIO);
	if (result.output_index == 0) result.fit.error=fit_laser();
	if ((meta.options & OPT_LOSS) && !atomic_get(&fit_cancel))
		result.route[result.output_index].error=fit_route(result.output_index);
	k_thread_priority_set(k_current_get(),priority);
	k_mutex_lock(&calibration_lock,K_FOREVER);
	if (atomic_get(&fit_cancel)) {
		run.phase=PHASE_PAUSE; /* stop caller performs terminal state/cleanup */
	} else {
		int refinement=0;
		if (result.output_index == 0 && !run.refined) {
			refinement=start_refinement();
		}
		if (refinement < 0) fail_acquisition(refinement);
		else if (refinement > 0) result.state=LASER_CAL_RUNNING;
		else {
			result.acquisition_complete=true;
			apply_results();
			bool pause=result.output_index == 0 && (meta.options & OPT_OTHER);
			run.phase=pause ? PHASE_PAUSE : PHASE_IDLE;
			result.state=pause ? LASER_CAL_AWAITING_OUTPUT : LASER_CAL_COMPLETE;
			/* Keep the PD available while the operator recables. Settings are
			 * still owned by this run, so a paused PD cannot be re-enabled manually. */
			if (!pause) housekeeping_photodiode_autooff_inhibit((enum housekeeping_power_output)meta.channel,false);
		}
		publish_status(NULL,true);
	}
	k_sem_give(&fit_done);
	k_mutex_unlock(&calibration_lock);
}

int laser_calibration_continue(bool set_loopback, double loss, struct laser_calibration_status *status)
{
	if (set_loopback && (!isfinite(loss) || loss < 0.0)) return -EINVAL;
	k_mutex_lock(&calibration_lock,K_FOREVER);
	if (atomic_get(&calibration_data_kind) != CALIBRATION_DATA_LASER || result.state != LASER_CAL_AWAITING_OUTPUT) {
		k_mutex_unlock(&calibration_lock); return -EINVAL;
	}
	if (set_loopback) meta.loopback_loss_db[1]=loss;
	result.output_index=1;
	result.acquisition_complete=false;
	result.state=LASER_CAL_RUNNING;
	run.reference_count=0;
	int rc=prepare_output();
	if (rc != 0) fail_acquisition(rc);
	publish_status(status,true);
	k_mutex_unlock(&calibration_lock);
	return rc;
}

int laser_calibration_restore_defaults(enum hispec_laser_id laser, const char *output, bool persist,
				      struct laser_calibration_restore_result *out)
{
	memset(out,0,sizeof(*out));
	bool fei;
	int rc=validate_output(laser,output,&fei);
	if (rc != 0) return rc;
	rc=attenuator_calibration_stop(NULL);
	if (rc == 0) rc=laser_calibration_stop(NULL);
	if (rc == 0) rc=throughput_monitor_stop(PHOTODIODE_CHANNEL_COUNT,NULL);
	if (rc == 0) rc=hispec_laser_stop_output(laser,false,NULL,0);
	if (rc != 0) return rc;
	const struct hispec_laser_driver_profile *profile;
	rc=hispec_laser_get_driver_profile(laser,&profile);
	if (rc != 0) return rc;
	const laserprops_t *p=profile->properties;
	struct app_laser_channel_settings s;
	out->laser_error=hispec_laser_get_channel_settings(laser,&s);
	if (out->laser_error == 0) {
		s.properties.threshold_current_ma=p->threshold_current_ma;
		s.properties.efficiency_mw_per_ma=p->efficiency_mw_per_ma;
		s.properties.ref_mw=p->ref_mw; s.properties.ref_ma=p->ref_ma; s.properties.ref_temp_c=p->ref_temp_c;
		s.min_autolevel_current_ma=p->threshold_current_ma+0.1;
		s.constant_noise_mw=HISPEC_LASER_DEFAULT_NOISE_FLOOR_FRACTION*(p->max_current_ma-p->threshold_current_ma)*p->efficiency_mw_per_ma;
		s.fractional_noise=HISPEC_LASER_DEFAULT_FRACTIONAL_NOISE;
		out->laser_error=hispec_laser_validate_channel_settings(laser,&s);
		if (out->laser_error == 0) out->laser_error=hispec_laser_update_channel_settings(laser,&s,false,NULL,0);
		out->laser_applied=out->laser_error == 0;
		if (out->laser_applied && persist) {
			out->laser_persist_error=app_settings_update_laser_channel(laser,&s,true);
			out->laser_persisted=out->laser_persist_error == 0;
		}
	}
	char input[16],route_output[16],pd_input[16],pd_output[16],key[40];
	route_names(laser,fei,'S',input,route_output,pd_input,pd_output);
	snprintk(key,sizeof(key),"%s_to_%s",input,route_output);
	out->route_error=app_settings_reset_route_loss(key,hispec_laser_name(laser),false);
	out->route_applied=out->route_error == 0;
	if (out->route_applied && persist) {
		out->route_persist_error=app_settings_reset_route_loss(key,hispec_laser_name(laser),true);
		out->route_persisted=out->route_persist_error == 0;
	}
	return out->laser_error != 0 ? out->laser_error : out->route_error != 0 ? out->route_error :
		out->laser_persist_error != 0 ? out->laser_persist_error : out->route_persist_error;
}

int laser_calibration_format_status(char *payload, size_t size, const struct laser_calibration_status *s)
{
	static const char *const states[]={"inactive","running","fitting","awaiting_output","complete","stopped","error"};
	const struct laser_calibration_fit *f=&s->fit;
	size_t off=0;
#define BOOL(v) ((v)?"true":"false")
#define APPEND(...) do { if (coo_json_append(payload,size,&off,__VA_ARGS__) != 0) return -ENOSPC; } while (0)
#define NUMBER(key,value,valid) do { APPEND(",\"" key "\":"); \
	if (coo_json_append_float_or_null(payload,size,&off,(valid)?(value):(double)NAN,6) != 0) return -ENOSPC; } while (0)
	APPEND("{\"state\":\"%s\",\"laser\":\"%s\",\"output\":\"%s_%s\",\"records\":%u,\"dwell_ms\":%u,"
		"\"current_ma\":%.1f,\"acquisition_complete\":%s,\"overflow\":%s,\"shutdown_pending\":%s,\"error\":%d,",
		states[s->state],hispec_laser_name(s->laser),s->laser <= HISPEC_LASER_1430_YJ ? "yj":"hk",
		(s->first_fei ^ (s->output_index != 0)) ? "fei":"ao",s->records,s->dwell_ms,(double)s->current_ma,
		BOOL(s->acquisition_complete),BOOL(s->overflow),BOOL(s->shutdown_pending),s->error);
	APPEND("\"fit\":{\"valid\":%s,\"accepted\":%s,\"applied\":%s,\"persisted\":%s,\"error\":%d,\"persist_error\":%d,\"n\":%u",
		BOOL(f->valid),BOOL(f->accepted),BOOL(f->applied),BOOL(f->persisted),f->error,f->persist_error,f->points);
	NUMBER("threshold_ma",f->threshold_ma,f->valid);
	NUMBER("efficiency_mw_per_ma",f->efficiency_mw_per_ma,f->valid);
	NUMBER("floor_ma",f->floor_ma,f->valid);
	NUMBER("linear_max_ma",f->linear_max_ma,f->valid);
	NUMBER("baseline_mv",f->baseline_mv,f->valid);
	NUMBER("rms_fraction",f->rms_fraction,f->valid);
	NUMBER("threshold_se_ma",f->threshold_se_ma,f->valid);
	APPEND("},\"noise\":{\"valid\":%s,\"applied\":%s,\"persisted\":%s,\"error\":%d",
		BOOL(f->noise_valid),BOOL(f->noise_applied),BOOL(f->noise_persisted),f->noise_error);
	NUMBER("constant_mw",f->constant_noise_mw,f->noise_valid);
	NUMBER("fractional",f->fractional_noise,f->noise_valid);
	APPEND("},\"routes\":[");
	for (unsigned i=0;i<2;++i) {
		const struct laser_calibration_route_fit *r=&s->route[i];
		APPEND("%s{\"accepted\":%s,\"applied\":%s,\"persisted\":%s,\"error\":%d,\"persist_error\":%d",
			i ? ",":"",BOOL(r->accepted),BOOL(r->applied),BOOL(r->persisted),r->error,r->persist_error);
		NUMBER("combined_db",r->combined_loss_db,r->accepted);
		NUMBER("outbound_db",r->outbound_loss_db,r->accepted);
		APPEND("}");
	}
	APPEND("]}");
#undef NUMBER
#undef APPEND
#undef BOOL
	return 0;
}

/* IEEE-754 little-endian wire values; explicit encoding avoids target padding. */
static void put_float(uint8_t *bytes, float value)
{
	uint32_t bits;
	memcpy(&bits,&value,sizeof(bits)); sys_put_le32(bits,bytes);
}
static void put_double(uint8_t *bytes, double value)
{
	uint64_t bits;
	memcpy(&bits,&value,sizeof(bits)); sys_put_le64(bits,bytes);
}

int laser_calibration_write_data(void *payload, size_t size, int chunk, size_t *written)
{
	uint8_t *bytes=payload;
	*written=0;
	k_mutex_lock(&calibration_lock,K_FOREVER);
	int rc=0;
	if (atomic_get(&calibration_data_kind) != CALIBRATION_DATA_LASER) { rc=-ENODATA; goto done; }
	if (chunk == -1) {
		if (size < WIRE_METADATA_SIZE) { rc=-ENOSPC; goto done; }
		memset(bytes,0,WIRE_METADATA_SIZE);
		memcpy(bytes,"HLC1",4); bytes[4]=1; bytes[5]=result.state; bytes[6]=meta.laser; bytes[7]=meta.channel;
		bytes[8]=meta.fiber; bytes[9]=meta.first_fei; bytes[10]=meta.options; bytes[11]=WIRE_RECORD_SIZE;
		bytes[12]=result.records; bytes[13]=WIRE_CHUNK_RECORDS; bytes[14]=run.segments;
		sys_put_le32(meta.run_id,bytes+16); sys_put_le32(meta.dwell_ms,bytes+20); sys_put_le64(meta.started_ms,bytes+24);
		const double values[]={meta.ref_mw,meta.ref_ma,meta.ref_temp_c,meta.max_ma,meta.nominal_ma,meta.seed_threshold_ma,
			meta.current_cal_pct,meta.pd_gain_mv_per_mw,meta.dark_mv,meta.dark_rms_mv,meta.return_tx,
			meta.loopback_loss_db[0],meta.loopback_loss_db[1],meta.default_threshold_ma,meta.default_efficiency_mw_per_ma};
		size_t off=32;
		for (unsigned i=0;i<ARRAY_SIZE(values);++i,off+=8) put_double(bytes+off,values[i]);
		for (unsigned i=0;i<2;++i,off+=4) put_float(bytes+off,meta.dac_full_scale_mv[i]);
		for (unsigned i=0;i<2;++i,off+=4) put_float(bytes+off,meta.dac_limit_mv[i]);
		put_float(bytes+off,meta.temp_min_c); put_float(bytes+off+4,meta.temp_max_c); off+=8;
		for (unsigned i=0;i<2;++i) {
			const struct attenuator_model_coeffs *c=&meta.coeff[i];
			const double coefficients[]={c->fvoa_50pct_mv,c->slope_inv_fvoa_mv,c->max_atten_db,c->max_calibrated_db,c->gain,c->rms_db};
			for (unsigned j=0;j<ARRAY_SIZE(coefficients);++j,off+=8) put_double(bytes+off,coefficients[j]);
			for (unsigned j=0;j<ATTENUATOR_MODEL_CORRECTION_TERMS;++j,off+=4) put_float(bytes+off,c->correction_coeff[j]);
		}
		__ASSERT_NO_MSG(off == WIRE_METADATA_SIZE);
		*written=off;
	} else {
		if (chunk < 0 || (unsigned)chunk*WIRE_CHUNK_RECORDS >= result.records) { rc=-ERANGE; goto done; }
		unsigned start=(unsigned)chunk*WIRE_CHUNK_RECORDS;
		unsigned count=MIN(WIRE_CHUNK_RECORDS,result.records-start);
		if (size < count*WIRE_RECORD_SIZE) { rc=-ENOSPC; goto done; }
		for (unsigned i=0;i<count;++i) {
			uint8_t *b=bytes+i*WIRE_RECORD_SIZE;
			const struct laser_cal_record *r=&data.records[start+i];
			sys_put_le32(r->elapsed_ms,b); put_float(b+4,r->net_mv); put_float(b+8,r->rms_mv);
			sys_put_le16(r->current_tenth_ma,b+12); sys_put_le16(r->dac_code[0],b+14); sys_put_le16(r->dac_code[1],b+16);
			sys_put_le16((uint16_t)r->max_raw,b+18); b[20]=r->good; b[21]=r->failed; b[22]=r->segment; b[23]=r->flags;
		}
		*written=count*WIRE_RECORD_SIZE;
	}
done:
	k_mutex_unlock(&calibration_lock);
	return rc;
}
