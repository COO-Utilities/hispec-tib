/**
 * Laser calibration on the existing throughput thread.
 *
 * Every sampled current receives fixed-DAC high/low conditions; the upper block
 * also gets a middle level. Early/late windows and exact-condition revisits keep
 * settling, actuator history and PD-level dependence observable. Acquisition
 * completes before fitting; a rejected model never removes raw evidence.
 *
 * The raw-PD GLS fit uses calibrated FVOA transmission, one common calibration
 * uncertainty per DAC pair, and a shared drifting baseline constrained by darks.
 * The vendor power reference establishes scale inside a fit that must reach the
 * configured maximum and agree with the unit's vendor trace. Scalar nuisance
 * elimination and the existing 2x2 solver avoid an observation covariance matrix.
 * Efficiency remains the uncorrected linear-fit slope; a separate curve reference
 * expresses the quadratic correction without redefining that slope.
 *
 * Noise estimates describe within-window temporal variance, not static FVOA fit
 * error. Absolute loss remains conditional on PD gain, vendor power, return and
 * external loopback loss. The experiment cannot by itself prove which optical
 * component causes an inconsistent response; diagnostics precede application.
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
#define RECORDS LASER_CAL_RECORD_COUNT
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
#define WIRE_METADATA_SIZE 614U
#define FLAG_EARLY 0x20U
#define FLAG_LATE_SECOND 0x40U

/* Immutable unit-specific vendor traces from the power_vs_current.txt files in
 * doc/datasheets. Those files identify source reports, temperature and optical
 * reference plane; they are approximate digitizations, not raw vendor measurements.
 * Each retained knot bounds piecewise-linear compression error to 0.1% of vendor
 * reference power at every original point. Compare only knots inside the fitted
 * interval; never extrapolate the vendor trace. Pairs are {mA, mW}. */
/* 1028y: BF69016, 0.33–249.01 mA. */
static const float vendor_1028y[][2] = {
	{0.3300f, 0.16700f}, {12.5700f, 0.16700f}, {14.8800f, 0.33300f}, {19.8400f, 1.16700f},
	{22.4900f, 1.54200f}, {25.1300f, 2.12500f}, {27.4500f, 2.41700f}, {30.0900f, 2.91700f},
	{32.4100f, 3.25000f}, {35.0500f, 3.83300f}, {37.3700f, 4.12500f}, {40.0100f, 4.75000f},
	{42.6600f, 5.12500f}, {44.9700f, 5.62500f}, {47.6200f, 6.08300f}, {49.9300f, 6.54200f},
	{52.5800f, 7.16700f}, {54.8900f, 7.50000f}, {57.5400f, 8.00000f}, {62.5000f, 9.08300f},
	{65.1500f, 9.45800f}, {67.4600f, 10.00000f}, {72.4200f, 10.87500f}, {75.0700f, 11.45800f},
	{82.3400f, 12.75000f}, {94.9100f, 15.29200f}, {97.5500f, 15.62500f}, {99.8700f, 16.20800f},
	{102.5100f, 16.62500f}, {110.1200f, 18.20800f}, {117.3900f, 19.54200f}, {120.0400f, 20.12500f},
	{122.3500f, 20.45800f}, {125.0000f, 21.04200f}, {137.5700f, 23.45800f}, {142.5300f, 24.50000f},
	{144.8400f, 24.79200f}, {147.4900f, 25.41700f}, {152.4500f, 26.41700f}, {160.0500f, 27.79200f},
	{162.3700f, 28.29200f}, {169.9700f, 29.58300f}, {174.9300f, 30.62500f}, {177.5800f, 31.04200f},
	{184.8500f, 32.50000f}, {190.1500f, 33.33300f}, {195.1100f, 34.45800f}, {197.4200f, 34.75000f},
	{202.3800f, 35.62500f}, {205.0300f, 36.20800f}, {207.3400f, 36.50000f}, {214.9500f, 38.00000f},
	{224.8700f, 39.66700f}, {227.5100f, 40.25000f}, {230.1600f, 40.58300f}, {232.4700f, 41.00000f},
	{240.0800f, 42.16700f}, {242.3900f, 42.41700f}, {245.0400f, 43.00000f}, {249.0100f, 43.50000f},
};
/* 1270j: 8881AFR, 10.04–60.41 mA. */
static const float vendor_1270j[][2] = {
	{10.0400f, 0.12900f}, {10.9900f, 0.32400f}, {16.0100f, 1.46700f}, {16.9600f, 1.70400f},
	{21.0300f, 2.65300f}, {23.0200f, 3.17100f}, {23.9700f, 3.30000f}, {25.0100f, 3.60200f},
	{27.0000f, 4.09900f}, {28.0400f, 4.33600f}, {28.9900f, 4.57300f}, {30.9900f, 5.02600f},
	{32.9800f, 5.52200f}, {38.0000f, 6.70900f}, {39.0300f, 6.96800f}, {43.0200f, 7.89500f},
	{45.9600f, 8.60700f}, {52.9700f, 10.20300f}, {54.0100f, 10.46200f}, {57.0400f, 11.15300f},
	{59.9800f, 11.86400f}, {60.4100f, 11.95100f},
};
/* 1430yj: 8897ACR, 3.99–55.53 mA. */
static const float vendor_1430yj[][2] = {
	{3.9900f, 0.11100f}, {5.0300f, 0.29600f}, {7.0300f, 0.77700f}, {9.0300f, 1.20200f},
	{11.0300f, 1.66400f}, {15.0200f, 2.55200f}, {17.0200f, 2.97700f}, {21.9700f, 4.10500f},
	{23.9700f, 4.53000f}, {27.0000f, 5.21400f}, {29.0000f, 5.63900f}, {29.9600f, 5.86100f},
	{33.0000f, 6.50800f}, {34.0300f, 6.74900f}, {36.0300f, 7.17400f}, {36.9900f, 7.39600f},
	{38.0300f, 7.61800f}, {38.9900f, 7.80300f}, {40.9900f, 8.24700f}, {44.0200f, 8.89400f},
	{44.9800f, 9.07900f}, {46.9800f, 9.50400f}, {48.9700f, 9.94800f}, {50.9700f, 10.37300f},
	{52.0100f, 10.57600f}, {52.9700f, 10.79800f}, {55.5300f, 11.31600f},
};
/* 1430hk: 8895ACR, 3.99–55.53 mA. */
static const float vendor_1430hk[][2] = {
	{3.9900f, 0.15100f}, {9.9900f, 1.51000f}, {11.0300f, 1.72600f}, {13.9800f, 2.39400f},
	{15.0200f, 2.67500f}, {15.9800f, 2.84700f}, {17.0200f, 3.08500f}, {17.9800f, 3.27900f},
	{19.0100f, 3.53800f}, {19.9700f, 3.73200f}, {21.9700f, 4.18500f}, {23.0100f, 4.40100f},
	{25.9700f, 5.09100f}, {27.9600f, 5.50100f}, {33.0000f, 6.62200f}, {34.9900f, 7.03200f},
	{38.0300f, 7.72300f}, {40.9900f, 8.32700f}, {42.0200f, 8.56400f}, {44.0200f, 8.97400f},
	{50.0100f, 10.29000f}, {50.9700f, 10.48400f}, {52.0100f, 10.72100f}, {54.0100f, 11.13100f},
	{55.5300f, 11.47600f},
};
/* 1510h: 8882ACT, 5.03–55.53 mA. */
static const float vendor_1510h[][2] = {
	{5.0300f, 0.25800f}, {9.9900f, 1.44300f}, {13.0200f, 2.13200f}, {15.0200f, 2.62800f},
	{17.0200f, 3.08000f}, {19.9700f, 3.79100f}, {21.9700f, 4.24300f}, {23.0100f, 4.50200f},
	{25.9700f, 5.19100f}, {27.0000f, 5.44900f}, {33.0000f, 6.82800f}, {34.0300f, 7.08600f},
	{36.0300f, 7.53800f}, {38.9900f, 8.24900f}, {42.9800f, 9.15400f}, {44.0200f, 9.41200f},
	{46.0200f, 9.86500f}, {46.9800f, 10.10200f}, {50.9700f, 11.00600f}, {52.0100f, 11.26500f},
	{54.0100f, 11.69500f}, {54.9700f, 11.93200f}, {55.5300f, 12.04000f},
};
/* 2330k: D4520009, 0.96–119.52 mA. */
static const float vendor_2330k[][2] = {
	{0.9600f, 0.00900f}, {17.0500f, 0.01200f}, {18.0100f, 0.02700f}, {19.9200f, 0.07700f},
	{21.9900f, 0.12400f}, {24.0600f, 0.18700f}, {26.9300f, 0.28400f}, {28.0500f, 0.31700f},
	{29.0000f, 0.33200f}, {29.9600f, 0.37600f}, {31.0800f, 0.41500f}, {32.0300f, 0.45600f},
	{33.9400f, 0.51200f}, {35.0600f, 0.56000f}, {37.9300f, 0.66300f}, {39.0400f, 0.70800f},
	{40.0000f, 0.73400f}, {43.9800f, 0.87400f}, {44.9400f, 0.90300f}, {46.0600f, 0.95300f},
	{47.0100f, 0.98900f}, {47.9700f, 1.01600f}, {48.9200f, 1.05100f}, {50.0400f, 1.10200f},
	{51.9500f, 1.16700f}, {54.0200f, 1.25000f}, {58.0100f, 1.37100f}, {58.9600f, 1.41200f},
	{60.0800f, 1.44200f}, {61.0400f, 1.48400f}, {62.9500f, 1.55500f}, {65.0200f, 1.62600f},
	{65.9800f, 1.64900f}, {68.0500f, 1.72600f}, {72.0300f, 1.85700f}, {72.9900f, 1.89800f},
	{75.0600f, 1.95100f}, {76.0200f, 1.98100f}, {76.9700f, 2.02200f}, {79.0400f, 2.08200f},
	{83.0300f, 2.22100f}, {84.9400f, 2.28000f}, {86.0600f, 2.32700f}, {87.0100f, 2.35700f},
	{87.9700f, 2.37500f}, {88.9200f, 2.41000f}, {91.9500f, 2.49000f}, {93.0700f, 2.52900f},
	{95.9400f, 2.61500f}, {97.0500f, 2.65600f}, {98.0100f, 2.65900f}, {98.9600f, 2.69500f},
	{99.9200f, 2.71200f}, {101.0400f, 2.74800f}, {101.9900f, 2.76600f}, {105.9800f, 2.87800f},
	{106.9300f, 2.87800f}, {108.0500f, 2.91100f}, {109.0000f, 2.93100f}, {109.9600f, 2.96100f},
	{111.0800f, 2.97300f}, {112.9900f, 3.02600f}, {113.9400f, 3.04400f}, {116.0200f, 3.10300f},
	{116.9700f, 3.11500f}, {117.9300f, 3.14200f}, {119.0400f, 3.15900f}, {119.5200f, 3.15900f},
};
static const struct {
	const float (*point)[2];
	unsigned count;
	double ref_mw;
} vendor_reference[] = {
	[HISPEC_LASER_1028_Y] = {vendor_1028y, ARRAY_SIZE(vendor_1028y), 43.8},
	[HISPEC_LASER_1270_J] = {vendor_1270j, ARRAY_SIZE(vendor_1270j), 10},
	[HISPEC_LASER_1430_YJ] = {vendor_1430yj, ARRAY_SIZE(vendor_1430yj), 10},
	[HISPEC_LASER_1430_HK] = {vendor_1430hk, ARRAY_SIZE(vendor_1430hk), 10},
	[HISPEC_LASER_1510_H] = {vendor_1510h, ARRAY_SIZE(vendor_1510h), 10},
	[HISPEC_LASER_2330_K] = {vendor_2330k, ARRAY_SIZE(vendor_2330k), 3.18},
};

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
	CAL_REFERENCE_UNAVAILABLE,
	CAL_INSUFFICIENT_POINTS,
	CAL_FIT_FAILED,
	CAL_NONMONOTONIC,
	CAL_THRESHOLD_RANGE,
	CAL_NONLINEAR,
	CAL_MAXIMUM_UNAVAILABLE,
	CAL_VENDOR_MISMATCH,
	CAL_NOISE_FIT_FAILED,
	CAL_LOSS_FIT_FAILED,
	CAL_ATTEN_RANGE,
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
	CAL_PD_COVERAGE,
	CAL_DARK_UNSTABLE,
	CAL_REVISIT_SHIFT,
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
	case CAL_REFERENCE_UNAVAILABLE: return "two usable late reference windows required";
	case CAL_INSUFFICIENT_POINTS: return "six distinct usable currents required";
	case CAL_FIT_FAILED: return "fit failed";
	case CAL_NONMONOTONIC: return "fit is not increasing";
	case CAL_THRESHOLD_RANGE: return "fit threshold outside usable range";
	case CAL_NONLINEAR: return "full-range residual exceeds uncertainty allowance";
	case CAL_MAXIMUM_UNAVAILABLE: return "usable measurement at configured maximum required";
	case CAL_VENDOR_MISMATCH: return "curve disagrees with vendor characterization";
	case CAL_NOISE_FIT_FAILED: return "noise fit failed";
	case CAL_LOSS_FIT_FAILED: return "static loss fit failed";
	case CAL_ATTEN_RANGE: return "reference outside calibrated attenuation";
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
	case CAL_PD_COVERAGE: return "two pd levels required at each fitted current";
	case CAL_DARK_UNSTABLE: return "measurement inconsistency: dark baseline changed";
	case CAL_REVISIT_SHIFT: return "measurement inconsistency: repeated condition changed";
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

/* Flags: classification 0..1, event 2..4, early 5, second late 6, output 7. */
enum point_class {
	POINT_OK,
	POINT_SATURATED,
	POINT_LOW,
	POINT_ADC_ERROR,
};

enum point_event {
	EVENT_SWEEP,
	EVENT_PROBE,
	EVENT_RESERVED,
	EVENT_REFERENCE,
	EVENT_DARK,
	EVENT_REVISIT,
};

/* Phase says what the worker is waiting for; stage says which observation the
 * sweep needs next. Temperature drift changes phase without losing the stage. */
enum phase {
	PHASE_IDLE,
	PHASE_WARM,
	PHASE_WINDOW,
	PHASE_SETTLE,
	PHASE_FIT,
	PHASE_PAUSE,
};

enum stage {
	STAGE_DARK,
	STAGE_SWEEP,
	STAGE_MIDDLE,
	STAGE_LOW,
	STAGE_REVISIT,
	STAGE_MID_DARK,
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
	int64_t condition_started_ms;
	double primary_db;
	uint8_t attenuator;
	uint8_t grid_count;
	uint8_t grid_index;
	uint8_t segment;
	uint8_t segments;
	uint8_t event;
	uint8_t search_tries;
	uint8_t search_limit;
	uint8_t stable_temperatures;
	uint8_t condition_window;
	uint8_t segment_start;
	uint8_t segment_end;
	uint8_t primary_record;
	uint8_t primary_count;
	uint8_t middle_index;
	bool searching;
	bool ranged;
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
	return !is_other(r) && !(r->flags & FLAG_EARLY) && (r->flags & 3U) == POINT_OK && r->good > 0 &&
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
		if (is_other(r) != other || event_of(r) != EVENT_DARK || (r->flags & FLAG_EARLY) || r->good == 0 ||
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
		if (is_other(r) != other || event_of(r) != EVENT_DARK || (r->flags & FLAG_EARLY) || r->good == 0 ||
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

/* A condition is two consecutive late windows at identical current/DAC codes.
 * Their scatter is a floor on mean variance: slow FVOA changes do not disappear
 * by dividing a within-window RMS by the ADC sample count. The early window is
 * retained only for settling diagnostics. No hardware or settings side effects. */
static int prepare_fit(void)
{
	memset(&work, 0, sizeof(work));
	int rc = fit_baseline(false, &work.baseline_mean, &work.baseline_weight);
	if (rc != 0) {
		return CAL_DARK_UNAVAILABLE;
	}
	work.time_scale_s = data.records[result.records - 1U].elapsed_ms / 1000.0;
	work.atten_sigma = log(10.0) / 10.0 * hypot(meta.coeff[0].rms_db, meta.coeff[1].rms_db);
	if (!isfinite(work.atten_sigma) || work.atten_sigma < 0.0) {
		return CAL_ATTEN_RANGE;
	}

	for (unsigned i = 0; i < result.records; ++i) {
		work.mean[i] = NAN;
		work.variance[i] = NAN;
		const struct laser_cal_record *r = &data.records[i];
		if (i == 0 || is_other(r) || !(r->flags & FLAG_LATE_SECOND)) {
			continue;
		}
		const struct laser_cal_record *a = &data.records[i - 1U];
		bool dark = event_of(r) == EVENT_DARK;
		if (r->good == 0 || a->good == 0 ||
		    (dark ? ((r->flags & 3U) == POINT_SATURATED || (a->flags & 3U) == POINT_SATURATED)
			  : (!curve_point(r) || !curve_point(a)))) {
			continue;
		}
		work.mean[i] = 0.5 * ((double)r->net_mv + (double)a->net_mv);
		double independent = 0.25 * (1.0 / mean_weight(r) + 1.0 / mean_weight(a));
		double difference = (double)r->net_mv - (double)a->net_mv;
		work.variance[i] = fmax(independent, 0.5 * difference * difference);
		if (dark) {
			continue;
		}

		double db = 0.0;
		for (unsigned j = 0; j < 2; ++j) {
			struct atten_model_eval e;
			double mv = (double)r->dac_code[j] * (double)meta.dac_full_scale_mv[j] / 4096.0;
			if (!atten_model_eval(&meta.coeff[j], (float)mv, &e) ||
			    e.db > fmin(meta.coeff[j].max_calibrated_db, 55.0) + 0.05) {
				return CAL_ATTEN_RANGE;
			}
			db += e.db;
		}
		work.transmission[r->segment] = pow(10.0, -db / 10.0);
		if (fabs(current_of(r) - meta.ref_ma) < 0.05 && work.reference_tx == 0.0) {
			work.reference_tx = work.transmission[r->segment];
		}
	}
	if (!(work.reference_tx > 0.0)) {
		return CAL_REFERENCE_UNAVAILABLE;
	}
	for (unsigned j = 0; j < LASER_CAL_SEGMENTS; ++j) {
		work.transmission[j] /= work.reference_tx;
	}
	return 0;
}

/* Build one raw-mV design row. The common baseline has intercept and elapsed
 * time slope. The reference must lie inside the fitted interval, sharing its
 * measured amplitude with the curve. Unknowns: baseline[2], reference amplitude,
 * shape[1 or 2]. A high-current discrepancy must not be hidden by truncation. */
static bool fit_row(unsigned i, double low, double high, bool quadratic, double x[6])
{
	const struct laser_cal_record *r = &data.records[i];
	if (!isfinite((double)work.mean[i])) {
		return false;
	}
	memset(x, 0, 6 * sizeof(*x));
	x[0] = 1.0;
	x[1] = r->elapsed_ms / (1000.0 * work.time_scale_s);
	if (event_of(r) == EVENT_DARK) {
		return true;
	}

	double ma = current_of(r);
	if (ma < low || ma > high) {
		return false;
	}
	double t = work.transmission[r->segment];
	double z = (2.0 * ma - low - high) / (high - low);
	double zr = (2.0 * meta.ref_ma - low - high) / (high - low);
	x[2] = t;
	x[3] = t * (z - zr);
	x[4] = quadratic ? t * 2.0 * (z * z - zr * zr) : 0.0;
	return true;
}

/* Accumulate GLS without an N-by-N covariance matrix. A DAC pair has one common
 * fractional calibration error (ln(10)/10 times the pair's RMS dB), shared by
 * every current and revisit at those exact codes. Different pairs are treated
 * as independent because the atten calibration does not supply covariance.
 * Sherman-Morrison profiles that rank-one term; it is not added independently
 * to each point, and measured bridge ratios never define the normalization. */
static void fit_normal(double low, double high, bool quadratic, unsigned n)
{
	memset(work.normal, 0, sizeof(work.normal));
	for (unsigned group = 0; group <= LASER_CAL_SEGMENTS; ++group) {
		double v[6] = {0};
		double uy = 0.0;
		double denominator = 1.0;
		for (unsigned i = 0; i < result.records; ++i) {
			const struct laser_cal_record *r = &data.records[i];
			unsigned g = event_of(r) == EVENT_DARK ? LASER_CAL_SEGMENTS : r->segment;
			double x[6];
			if (g != group || !fit_row(i, low, high, quadratic, x)) {
				continue;
			}
			double w = 1.0 / (double)work.variance[i];
			double signal = 0.0;
			for (unsigned k = 2; k < n; ++k) {
				signal += x[k] * work.beta[k];
			}
			double u = work.atten_sigma * signal;
			denominator += w * u * u;
			uy += w * u * (double)work.mean[i];
			for (unsigned j = 0; j < n; ++j) {
				v[j] += w * x[j] * u;
				work.normal[j][n] += w * x[j] * (double)work.mean[i];
				for (unsigned k = 0; k < n; ++k) {
					work.normal[j][k] += w * x[j] * x[k];
				}
			}
		}
		for (unsigned j = 0; j < n; ++j) {
			work.normal[j][n] -= v[j] * uy / denominator;
			for (unsigned k = 0; k < n; ++k) {
				work.normal[j][k] -= v[j] * v[k] / denominator;
			}
		}
	}
}

/* Profile the scalar nuisance coordinates, then reuse the validated 2x2 solve
 * for the remaining shape coordinates. Diagonal scaling avoids comparing PD
 * baseline units with large deattenuated amplitudes. Six is a fixed upper bound;
 * both matrices live in the existing shared scratch union, not on this stack.
 * A supplied RHS solves a covariance projection without inverting the matrix. */
static int solve_normal(unsigned n, const double *rhs, double solution[6])
{
	double scale[6];
	for (unsigned j = 0; j < n; ++j) {
		scale[j] = sqrt(work.normal[j][j]);
		if (!(scale[j] > 0.0) || !isfinite(scale[j])) {
			return CAL_FIT_FAILED;
		}
	}
	for (unsigned j = 0; j < n; ++j) {
		for (unsigned k = 0; k < n; ++k) {
			work.equations[j][k] = work.normal[j][k] / scale[j] / scale[k];
		}
		work.equations[j][n] = (rhs == NULL ? work.normal[j][n] : rhs[j]) / scale[j];
	}
	for (unsigned p = 0; p + 2U < n; ++p) {
		double pivot = work.equations[p][p];
		if (!(pivot > 1e-12)) {
			return CAL_FIT_FAILED;
		}
		for (unsigned j = p + 1U; j < n; ++j) {
			double multiplier = work.equations[j][p] / pivot;
			for (unsigned k = p + 1U; k <= n; ++k) {
				work.equations[j][k] -= multiplier * work.equations[p][k];
			}
		}
	}
	unsigned a = n - 2U;
	int rc = calibration_solve_2x2(work.equations[a][a], work.equations[a][a + 1U],
		work.equations[a + 1U][a + 1U], work.equations[a][n], work.equations[a + 1U][n], &solution[a]);
	if (rc != 0) {
		return CAL_FIT_FAILED;
	}
	for (int j = (int)n - 3; j >= 0; --j) {
		double value = work.equations[j][n];
		for (unsigned k = j + 1U; k < n; ++k) {
			value -= work.equations[j][k] * solution[k];
		}
		solution[j] = value / work.equations[j][j];
	}
	for (unsigned j = 0; j < n; ++j) {
		solution[j] /= scale[j];
		if (!isfinite(solution[j])) {
			return CAL_FIT_FAILED;
		}
	}
	return 0;
}

/* Fit in raw PD units with shared baseline/reference/attenuation uncertainty.
 * Reweight the gain covariance using predicted signal, not a noisy observation.
 * The linear efficiency argument is from the separate uncorrected linear fit
 * on exactly this support. The quadratic's curve reference translates that
 * slope into the installed representation without changing its interpretation. */
static int fit_interval(double low, double high, bool quadratic, double linear_efficiency,
			struct laser_calibration_fit *fit)
{
	memset(fit, 0, sizeof(*fit));
	fit->floor_ma = low;
	fit->max_ma = high;
	fit->threshold_se_ma = NAN;
	fit->rms_fraction = fit->max_residual_fraction = NAN;
	unsigned n = quadratic ? 5U : 4U;
	memset(work.beta, 0, sizeof(work.beta));
	work.beta[0] = work.baseline_mean;

	bool converged = false;
	for (unsigned iteration = 0; iteration < 20; ++iteration) {
		if (atomic_get(&fit_cancel)) {
			return -ECANCELED;
		}
		fit_normal(low, high, quadratic, n);
		double next[6] = {0};
		int rc = solve_normal(n, NULL, next);
		if (rc != 0) {
			return rc;
		}
		double change = 0.0;
		for (unsigned i = 0; i < result.records; ++i) {
			double x[6];
			if (!fit_row(i, low, high, quadratic, x)) {
				continue;
			}
			double old = 0.0;
			double updated = 0.0;
			for (unsigned k = 0; k < n; ++k) {
				old += x[k] * work.beta[k];
				updated += x[k] * next[k];
			}
			change = fmax(change, fabs(updated - old) / fmax(1.0, fabs(updated)));
		}
		memcpy(work.beta, next, sizeof(work.beta));
		if (iteration > 0 && change < 1e-6) {
			converged = true;
			break;
		}
	}
	if (!converged || !(work.beta[2] > 0.0)) {
		return CAL_FIT_FAILED;
	}
	fit_normal(low, high, quadratic, n);
	double center = 0.5 * (low + high);
	double scale = 0.5 * (high - low);
	double zr = (meta.ref_ma - center) / scale;
	double k = work.beta[2] / meta.ref_mw;
	double c = quadratic ? work.beta[4] : 0.0;
	double p0 = work.beta[2] - work.beta[3] * zr - 2.0 * c * zr * zr;
	p0 /= k;
	double slope = work.beta[3] / (scale * k);
	fit->efficiency_mw_per_ma2 = (float)(2.0 * c / (scale * scale * k));
	double q = fit->efficiency_mw_per_ma2;
	double discriminant = slope * slope - 4.0 * q * p0;
	if (!(discriminant > 0.0) || !(slope > 0.0)) {
		return CAL_NONMONOTONIC;
	}
	fit->threshold_ma = center - 2.0 * p0 / (slope + sqrt(discriminant));
	fit->efficiency_mw_per_ma = quadratic ? linear_efficiency : slope;
	fit->efficiency_ref_ma = q == 0.0 ? meta.ref_ma : fit->threshold_ma +
		(fit->efficiency_mw_per_ma - sqrt(discriminant)) / q;
	fit->baseline_mv = work.beta[0];
	if (!isfinite(fit->threshold_ma) || !(fit->efficiency_mw_per_ma > 0.0) ||
	    !isfinite(fit->efficiency_ref_ma)) {
		return CAL_FIT_FAILED;
	}
	if (fit->threshold_ma < 0.0 || low < fit->threshold_ma + 0.1 - 1e-7 ||
	    slope + 2.0 * q * (high - center) <= 0.0 ||
	    (q != 0.0 && fit->efficiency_ref_ma <= fit->threshold_ma)) {
		return CAL_THRESHOLD_RANGE;
	}

	/* Implicit-root derivative and full GLS covariance include the nuisance
	 * baseline, reference measurement and common FVOA errors. Vendor power is
	 * still assumed exact; this is conditional precision, not absolute accuracy. */
	double z0 = (fit->threshold_ma - center) / scale;
	double gradient[6] = {0};
	gradient[2] = -1.0 / (k * sqrt(discriminant));
	gradient[3] = -(z0 - zr) / (k * sqrt(discriminant));
	if (quadratic) {
		gradient[4] = -2.0 * (z0 * z0 - zr * zr) /
			(k * sqrt(discriminant));
	}
	double covariance[6] = {0};
	if (solve_normal(n, gradient, covariance) != 0) {
		return CAL_FIT_FAILED;
	}
	double variance = 0.0;
	for (unsigned j = 0; j < n; ++j) {
		variance += gradient[j] * covariance[j];
	}
	fit->threshold_se_ma = sqrt(fmax(0.0, variance));

	/* Report the profiled gain shifts separately. These estimates explain
	 * allowed common calibration error; they do not update attenuator settings. */
	for (unsigned group = 0; group < LASER_CAL_SEGMENTS; ++group) {
		double numerator = 0.0;
		double denominator = 1.0;
		for (unsigned i = 0; i < result.records; ++i) {
			double x[6];
			if (data.records[i].segment != group || !fit_row(i, low, high, quadratic, x)) {
				continue;
			}
			double signal = 0.0;
			for (unsigned j = 2; j < n; ++j) {
				signal += x[j] * work.beta[j];
			}
			double residual = (double)work.mean[i] - work.beta[0] - x[1] * work.beta[1] - signal;
			double w = work.atten_sigma * work.atten_sigma / (double)work.variance[i];
			numerator += w * signal * residual;
			denominator += w * signal * signal;
		}
		work.gain_shift[group] = numerator / denominator;
		work.gain[group] = k * work.transmission[group];
		if (work.atten_sigma > 0.0) {
			fit->atten_shift_sigma = fmax(fit->atten_shift_sigma, fabs(work.gain_shift[group]) / work.atten_sigma);
		}
	}

	double limit = meta.max_fit_residual_pct / 100.0;
	double residual_sum = 0.0;
	bool residuals_pass = true;
	bool covered = true;
	fit->max_residual_fraction = 0.0;
	for (unsigned i = 0; i < result.records; ++i) {
		const struct laser_cal_record *r = &data.records[i];
		double x[6];
		if (!fit_row(i, low, high, quadratic, x)) {
			continue;
		}
		double baseline = work.beta[0] + x[1] * work.beta[1];
		double observed = (double)work.mean[i] - baseline;
		if (event_of(r) == EVENT_DARK) {
			fit->dark_residual_mv = fmax(fit->dark_residual_mv, fabs(observed));
			continue;
		}
		double signal = 0.0;
		for (unsigned j = 2; j < n; ++j) {
			signal += x[j] * work.beta[j];
		}
		double predicted = signal * (1.0 + (double)work.gain_shift[r->segment]);
		if (!(predicted > 0.0)) {
			return CAL_THRESHOLD_RANGE;
		}
		double residual = observed / predicted - 1.0;
		residual_sum += residual * residual;
		fit->max_residual_fraction = fmax(fit->max_residual_fraction, fabs(residual));
		/* Measured scatter is uncertainty, not evidence of nonlinearity.
		 * Keep the reported residual raw; apply the allowance only here. */
		residuals_pass &= fabs(observed - predicted) <=
			limit * predicted + 3.0 * sqrt((double)work.variance[i]);
		fit->points += 2; /* Exact transmitted support contains both late windows. */

		bool paired = false;
		for (unsigned j = 0; j < result.records; ++j) {
			const struct laser_cal_record *rj = &data.records[j];
			if (j == i || !isfinite(work.mean[j]) || rj->current_tenth_ma != r->current_tenth_ma) {
				continue;
			}
			double bj = work.beta[0] + work.beta[1] * rj->elapsed_ms / (1000.0 * work.time_scale_s);
			double sj = (double)work.mean[j] - bj;
			if (!(observed > 0.0) || !(sj > 0.0)) {
				continue;
			}
			if (rj->segment != r->segment) {
				double ratio = sj / observed;
				paired |= ratio >= 2.0 || ratio <= 0.5;
				double corrected = ratio * work.gain[r->segment] * (1.0 + (double)work.gain_shift[r->segment]) /
					(work.gain[rj->segment] * (1.0 + (double)work.gain_shift[rj->segment]));
				fit->band_fraction = fmax(fit->band_fraction, fabs(corrected - 1.0));
			}
		}
		covered &= paired;
	}
	fit->rms_fraction = sqrt(residual_sum / (fit->points / 2U));
	fit->valid = true;
	if (!covered) {
		return CAL_PD_COVERAGE;
	}
	if (!residuals_pass) {
		return CAL_NONLINEAR;
	}

	/* Fine-tune trusted characterization; do not reinterpret a system upset
	 * as a new diode curve. Use a fixed fraction of vendor reference power so
	 * the allowance does not collapse near threshold. Project the full GLS
	 * covariance through P(I)=Pref*Q(I)/R, including the measured scale and
	 * baseline/gain nuisance uncertainty. Neither those nuisance estimates nor
	 * this conditional SE identify the physical cause of a discrepancy. */
	bool vendor_overlap = false;
	for (unsigned i = 0; i < vendor_reference[result.laser].count; ++i) {
		double ma = vendor_reference[result.laser].point[i][0];
		if (ma < low || ma > high) {
			continue;
		}
		vendor_overlap = true;
		double z = (ma - center) / scale;
		double power = meta.ref_mw + (work.beta[3] * (z - zr) + 2.0 * c * (z * z - zr * zr)) / k;
		memset(gradient, 0, sizeof(gradient));
		gradient[2] = (meta.ref_mw - power) / work.beta[2];
		gradient[3] = (z - zr) / k;
		gradient[4] = quadratic ? 2.0 * (z * z - zr * zr) / k : 0.0;
		if (solve_normal(n, gradient, covariance) != 0) {
			fit->valid = false;
			return CAL_FIT_FAILED;
		}
		variance = 0.0;
		for (unsigned j = 0; j < n; ++j) {
			variance += gradient[j] * covariance[j];
		}
		if (fabs(power - (double)vendor_reference[result.laser].point[i][1]) >
		    limit * vendor_reference[result.laser].ref_mw + 3.0 * sqrt(fmax(0.0, variance))) {
			return CAL_VENDOR_MISMATCH;
		}
	}
	if (!vendor_overlap) {
		return CAL_VENDOR_MISMATCH;
	}
	fit->accepted = true;
	return 0;
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
			double gain = work.gain[r->segment] * (1.0 + (double)work.gain_shift[r->segment]);
			double x = current_of(r) - result.fit.threshold_ma;
			double x_ref = result.fit.efficiency_ref_ma - result.fit.threshold_ma;
			double mu = gain * x * (result.fit.efficiency_mw_per_ma +
				(double)result.fit.efficiency_mw_per_ma2 * (x - x_ref));
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
 * Search only the floor, keeping every usable observation through the configured
 * maximum, including the reference. The floor excludes the knee without moving
 * the threshold; neither a bad upper region nor an outlier can be trimmed away. */
static int fit_laser(void)
{
	memset(&result.fit, 0, sizeof(result.fit));
	memset(work.include, 0, sizeof(work.include));
	int rc = prepare_fit();
	if (rc != 0) {
		/* An incomplete preparation cannot establish absolute mV/mW gains. */
		for (unsigned j = 0; j < LASER_CAL_SEGMENTS; ++j) {
			work.gain[j] = NAN;
		}
		return rc;
	}

	unsigned levels = 0;
	for (uint8_t i = 0; i < result.records; ++i) {
		if (!isfinite((double)work.mean[i]) || event_of(&data.records[i]) == EVENT_DARK) {
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

	/* Acquisition uses the 0.1-mA driver grid. Missing top data is a failed
	 * full-range calibration, never permission to lower the operating limit. */
	if (work.levels[levels - 1U] != (uint16_t)llround(meta.max_ma * 10.0)) {
		return CAL_MAXIMUM_UNAVAILABLE;
	}
	double high = work.levels[levels - 1U] / 10.0;
	bool have_candidate = false;
	int best_error = CAL_FIT_FAILED;
	for (unsigned lower = 0; lower + 5U < levels && !result.fit.accepted; ++lower) {
		double low = work.levels[lower] / 10.0;
		if (low > meta.ref_ma) {
			break;
		}
		double linear_efficiency = NAN;
		for (unsigned order = 1; order <= 2; ++order) {
			if (atomic_get(&fit_cancel)) {
				return -ECANCELED;
			}
			struct laser_calibration_fit candidate;
			int error = fit_interval(low, high, order == 2, linear_efficiency, &candidate);
			if (order == 1) {
				linear_efficiency = candidate.efficiency_mw_per_ma;
			}
			if (!have_candidate) {
				best_error = error;
			}
			if (!candidate.valid) {
				continue;
			}
			/* Ascending floors and line-first order resolve ties. A rejected
			 * diagnostic candidate minimizes maximum raw residual, not span.
			 * The first pass is the lowest usable floor, preferring a line. */
			if (!have_candidate || candidate.accepted ||
			    candidate.max_residual_fraction < result.fit.max_residual_fraction - 1e-12) {
				result.fit = candidate;
				best_error = error;
				have_candidate = true;
			}
			if (result.fit.accepted) {
				break;
			}
		}
	}

	/* Re-solve only the selected interval to retain its normalization and
	 * nuisance estimates. Other candidates must not leak into the download. */
	if (have_candidate) {
		double low = result.fit.floor_ma;
		double high = result.fit.max_ma;
		bool quadratic = result.fit.efficiency_mw_per_ma2 != 0.0f;
		struct laser_calibration_fit line;
		best_error = fit_interval(low, high, false, NAN, &line);
		if (quadratic) {
			best_error = fit_interval(low, high, true, line.efficiency_mw_per_ma, &result.fit);
		} else {
			result.fit = line;
		}
		work.baseline_mean = work.beta[0];
		for (unsigned i = 1; i < result.records; ++i) {
			double current = current_of(&data.records[i]);
			if (isfinite((double)work.mean[i]) && event_of(&data.records[i]) != EVENT_DARK &&
			    current >= low && current <= high) {
				work.include[i] = work.include[i - 1U] = 1;
			}
		}
	}
	/* Test controls once, against the selected baseline. Revisit/dark failure
	 * rejects the entire calibration; never search for another floor to evade
	 * it. Compare repeat differences in mV with their combined measured SE.
	 * Keep fractional diagnostics only where the signal resolves a ratio. */
	if (result.fit.valid) {
		double limit = meta.max_fit_residual_pct / 100.0;
		for (unsigned i = 0; i < result.records; ++i) {
			const struct laser_cal_record *r = &data.records[i];
			if (!isfinite(work.mean[i])) {
				continue;
			}
			double si = (double)work.mean[i] - work.beta[0] -
				work.beta[1] * r->elapsed_ms / (1000.0 * work.time_scale_s);
			if (event_of(r) == EVENT_DARK &&
			    fabs(si) > fmax(3.0 * sqrt((double)work.variance[i]), 5.0 * sqrt(Q_VARIANCE))) {
				best_error = CAL_DARK_UNSTABLE;
				result.fit.accepted = false;
			}
			if (event_of(r) != EVENT_REVISIT) {
				continue;
			}
			for (unsigned j = 0; j < i; ++j) {
				const struct laser_cal_record *rj = &data.records[j];
				if (!isfinite(work.mean[j]) || rj->current_tenth_ma != r->current_tenth_ma ||
				    rj->dac_code[0] != r->dac_code[0] || rj->dac_code[1] != r->dac_code[1]) {
					continue;
				}
				double sj = (double)work.mean[j] - work.beta[0] -
					work.beta[1] * rj->elapsed_ms / (1000.0 * work.time_scale_s);
				if (si > 10.0 * sqrt(work.variance[i]) && sj > 10.0 * sqrt(work.variance[j])) {
					result.fit.revisit_fraction = fmax(result.fit.revisit_fraction, fabs(si / sj - 1.0));
				}
				if (fabs(si - sj) > limit * fmax(0.0, 0.5 * (si + sj)) +
				    3.0 * sqrt((double)work.variance[i] + (double)work.variance[j])) {
					best_error = CAL_REVISIT_SHIFT;
					result.fit.accepted = false;
				}
			}
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
	run.condition_window = 0;
	run.condition_started_ms = k_uptime_get();
	return begin_window(event);
}

/* Insert directly into a small descending, quantized grid. No second point
 * buffer: probes and repeated conditions are retained only as raw records. */
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
	for (unsigned i = 0; i < 9; ++i) {
		add_grid_current(meta.max_ma - (meta.max_ma - meta.seed_threshold_ma) * i / 8.0);
	}

	double span = meta.max_ma - meta.seed_threshold_ma;
	for (unsigned i = 0; i < 9; ++i) {
		add_grid_current(meta.seed_threshold_ma + 0.1 * pow(span / 0.1, i / 8.0));
	}
	for (unsigned i = 0; i < 5; ++i) {
		add_grid_current(meta.seed_threshold_ma * i / 4.0);
	}
	add_grid_current(meta.ref_ma);
	add_grid_current(0.9 * meta.seed_threshold_ma);
	add_grid_current(1.1 * meta.seed_threshold_ma);
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
		/* Thermal qualification can interrupt a longer requested settling wait.
		 * It does not replace that wait before the first late window. */
		if (run.condition_window == 1 && now - run.condition_started_ms < meta.settle_ms) {
			run.phase = PHASE_SETTLE;
			return 0;
		}
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
	r->segment = UINT8_MAX;
	if (!result.output_index && run.event != EVENT_PROBE && run.event != EVENT_DARK) {
		for (unsigned i = 0; i + 1U < result.records; ++i) {
			const struct laser_cal_record *previous = &data.records[i];
			if (previous->segment < LASER_CAL_SEGMENTS &&
			    previous->dac_code[0] == r->dac_code[0] && previous->dac_code[1] == r->dac_code[1]) {
				r->segment = previous->segment;
				break;
			}
		}
		if (r->segment == UINT8_MAX) {
			if (run.segments >= LASER_CAL_SEGMENTS) {
				return -ENOSPC;
			}
			r->segment = run.segments++;
		}
	}

	/* Peak clipping invalidates the mean even if the mean itself is in range.
	 * Low-SNR classification uses uncertainty of the surviving-sample mean. */
	double sigma = fmax(window->rms_mv, sqrt(Q_VARIANCE));
	unsigned classification = r->good == 0 ? POINT_ADC_ERROR :
		window->max_mv >= 2000.0 ? POINT_SATURATED :
		window->mean_net_mv < 5.0 * sigma / sqrt(r->good) ? POINT_LOW : POINT_OK;
	r->flags = (uint8_t)(classification | (run.event << 2) |
		(run.condition_window == 0 ? FLAG_EARLY : run.condition_window == 2 ? FLAG_LATE_SECOND : 0) |
		(result.output_index ? 0x80U : 0));
	return r->good == 0 ? CAL_NO_SAMPLES : 0;
}

/* Restore the exact DAC pair of a retained observation at the same or lower
 * current. Ranging fallback and boundary revisits must not substitute a newly
 * solved allocation. Caller holds calibration_lock; this blocks on DAC I/O. */
static int restore_recorded_attenuation(const struct laser_cal_record *r)
{
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

	struct attenuator a;
	attenuator_snapshot(&attenuators[run.attenuator], &a);
	run.atten_db = a.attenuation_db;
	return 0;
}

/* Bracket a usable PD range without using a clipped mean. Each attenuator
 * change creates a new fixed-gain segment only once a usable setting is found.
 * Called under calibration_lock; initializes search state and sleeps to start
 * the first probe window. Raw windows from earlier attempts remain retained. */
static int start_search(unsigned limit)
{
	run.searching = true;
	run.condition_window = 0;
	run.search_tries = 0;
	run.search_limit = limit;
	run.search_low_db = 0.0;
	run.search_high_db = fmin(meta.coeff[0].max_calibrated_db, 55.0) +
		fmin(meta.coeff[1].max_calibrated_db, 55.0);
	return begin_window(EVENT_PROBE);
}

/* Seek the preferred PD level, then fall back to an actually usable probe.
 * Headroom and target proximity guide ranging; only the retained classification
 * determines measurement validity. Exhausted probes or a quantized no-op do not
 * discard usable evidence. Caller holds calibration_lock; actuator/window I/O
 * can block. Fresh early/late windows, not ranging probes, supply the fit. */
static int search_step(const struct photodiode_window_result *w)
{
	++run.search_tries;
	double signal = w->mean_net_mv;
	const struct laser_cal_record *latest = &data.records[result.records - 1U];
	bool headroom = w->max_mv < PEAK_TARGET_MV;
	bool preferred = (latest->flags & 3U) == POINT_OK && headroom && signal >= 0.65 * meta.pd_target_mv &&
		signal <= 1.15 * meta.pd_target_mv;
	/* Zero attenuation can expose the optical floor. Retain that evidence and
	 * continue downward even if it is too dim to support a fit. */
	bool at_floor = run.atten_db < 0.05 && (latest->flags & 3U) != POINT_SATURATED;

	if (!preferred && !at_floor && run.search_tries < run.search_limit) {
		if (headroom && signal < meta.pd_target_mv) {
			run.search_high_db = run.atten_db;
		} else {
			run.search_low_db = run.atten_db;
		}

		/* Above preferred headroom, bisect toward more attenuation. Near the
		 * noise floor, avoid a logarithm of an unreliable signal. */
		double next;
		if (!headroom) {
			next = 0.5 * (run.atten_db + run.search_high_db);
		} else if (signal <= 5.0 * fmax(w->rms_mv, sqrt(Q_VARIANCE))) {
			next = run.atten_db - 10.0;
		} else {
			next = run.atten_db - 10.0 * log10(meta.pd_target_mv / signal);
		}
		next = CLAMP(next, run.search_low_db, run.search_high_db);
		if (fabs(next - run.atten_db) >= 0.02) {
			int rc = set_attenuation(next);
			return rc == 0 ? begin_window(EVENT_PROBE) : rc;
		}
	}

	if (!preferred && !at_floor) {
		/* Probe records already contain classification and exact DAC codes, so
		 * fallback needs no separate buffer or extra acquisition attempt. A
		 * valid observation remains usable even when it misses the requested
		 * level or the preferred 1850-mV peak headroom. */
		const struct laser_cal_record *best = NULL;
		double distance = DBL_MAX;
		for (unsigned i = result.records - run.search_tries; i < result.records; ++i) {
			const struct laser_cal_record *probe = &data.records[i];
			double difference = fabs((double)probe->net_mv - meta.pd_target_mv);
			if ((probe->flags & 3U) == POINT_OK && difference < distance) {
				best = probe;
				distance = difference;
			}
		}
		if (best == NULL) {
			return CAL_PD_RANGE;
		}
		if (best != latest) {
			int rc = restore_recorded_attenuation(best);
			if (rc != 0) {
				return rc;
			}
		}
	}

	run.searching = false;
	if (result.output_index) {
		return set_current(meta.ref_ma, EVENT_REFERENCE);
	}
	if (run.ranged) {
		++run.grid_index; /* The old boundary already has two PD levels. */
	}
	run.ranged = true;
	run.stage = STAGE_SWEEP;
	run.segment_start = run.grid_index;
	run.primary_record = result.records;
	run.primary_db = run.atten_db;
	++run.primary_count;
	double ma = data.grid[run.grid_index] / 10.0;
	return set_current(ma, fabs(ma - meta.ref_ma) < 0.05 ? EVENT_REFERENCE : EVENT_SWEEP);
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
	    request->settle_ms < 100 || request->settle_ms > 10000 ||
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

	meta.started_ms = (uint64_t)k_uptime_get();
	meta.laser = request->laser;
	meta.channel = channel;
	meta.fiber = request->fiber;
	meta.first_fei = fei;
	meta.dwell_ms = request->dwell_ms;
	meta.settle_ms = result.settle_ms = request->settle_ms;
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

/* Infer loss from the measured reference, conditional on PD responsivity,
 * configured return and external loopback. First-output scale comes from the
 * joint raw fit. A second output has its own off-state/reference acquisition.
 * No hardware/settings effects; application remains gated by core acceptance. */
static int fit_route(unsigned output_index)
{
	struct laser_calibration_route_fit *f = &result.route[output_index];
	double response;
	if (output_index == 0) {
		if (!result.fit.valid || !(work.reference_tx > 0.0)) {
			return CAL_LOSS_FIT_FAILED;
		}
		response = work.beta[2] / meta.ref_mw / work.reference_tx;
	} else {
		double baseline;
		double weight;
		int rc = fit_baseline(true, &baseline, &weight);
		if (rc != 0) {
			return CAL_DARK_UNAVAILABLE;
		}
		double sum = 0.0;
		double total = 0.0;
		unsigned count = 0;
		for (unsigned i = 0; i < result.records; ++i) {
			const struct laser_cal_record *r = &data.records[i];
			if (!is_other(r) || event_of(r) != EVENT_REFERENCE || (r->flags & FLAG_EARLY) ||
			    (r->flags & 3U) != POINT_OK || r->good == 0) {
				continue;
			}
			double db = 0.0;
			for (unsigned j = 0; j < 2; ++j) {
				struct atten_model_eval e;
				double mv = (double)r->dac_code[j] * (double)meta.dac_full_scale_mv[j] / 4096.0;
				if (!atten_model_eval(&meta.coeff[j], (float)mv, &e) ||
				    e.db > fmin(meta.coeff[j].max_calibrated_db, 55.0) + 0.05) {
					return CAL_ATTEN_RANGE;
				}
				db += e.db;
			}
			double transmission = pow(10.0, -db / 10.0);
			double w = mean_weight(r) * transmission * transmission;
			sum += w * ((double)r->net_mv - baseline) / transmission;
			total += w;
			++count;
		}
		if (count < 2 || !(total > 0.0)) {
			return CAL_REFERENCE_UNAVAILABLE;
		}
		response = sum / total / meta.ref_mw;
	}
	f->combined_loss_db = 10.0 * log10(meta.pd_gain_mv_per_mw / response) - meta.loopback_loss_db[output_index];
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
		settings.properties.efficiency_mw_per_ma2 = f->efficiency_mw_per_ma2;
		settings.properties.efficiency_ref_ma = f->efficiency_ref_ma;
		settings.min_autolevel_current_ma = f->floor_ma;
		if (noise) {
			settings.constant_noise_mw = f->constant_noise_mw;
			settings.fractional_noise = f->fractional_noise;
		}
		if (hispec_laser_validate_channel_settings(result.laser, &settings) != 0) {
			f->apply_error = CAL_INVALID_SETTINGS;
			return f->apply_error;
		}
	}

	/* Validate before relinquishing any stream or changing the active model.
	 * This path belongs to automatic calibration; manual imports use settings. */
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

/* Every positive grid current receives a high- and low-signal condition at
 * fixed DAC settings. Reverse the low sweep to separate current direction from
 * PD level, then revisit the exact primary boundary condition. No fitted gain
 * or provisional curve influences these choices. Each condition retains one
 * early and two late windows; waiting for settle_ms is cancellable. */
static int advance_window(const struct photodiode_window_result *w)
{
	if (run.searching) {
		return search_step(w);
	}
	if (++run.condition_window < 3) {
		if (run.condition_window == 1 && k_uptime_get() - run.condition_started_ms < meta.settle_ms) {
			run.phase = PHASE_SETTLE;
			return 0;
		}
		return begin_window(run.event);
	}

	switch (run.stage) {
	case STAGE_DARK:
	case STAGE_OTHER_DARK: {
		bool other = run.stage == STAGE_OTHER_DARK;
		run.stage = other ? STAGE_OTHER_REFERENCE : STAGE_SWEEP;
		int rc = set_current(other ? meta.ref_ma : meta.max_ma, EVENT_PROBE);
		return rc == 0 ? start_search(other ? 15 : 16) : rc;
	}
	case STAGE_SWEEP: {
		bool last = run.grid_index + 2U >= run.grid_count;
		bool low = w->mean_net_mv < meta.pd_target_mv / 6.0 &&
			run.primary_count < 6 && run.primary_db >= 0.05;
		if (!last && !low && w->max_mv < PEAK_TARGET_MV) {
			return next_sweep_current();
		}
		run.segment_end = run.grid_index;
		/* Only the upper block needs a third PD level. Additional attenuation
		 * is installed before increasing current back to the top of that block. */
		if (run.primary_count == 1) {
			run.stage = STAGE_MIDDLE;
			run.middle_index = 0;
			int rc = set_attenuation(run.primary_db - 10.0 * log10(0.6));
			return rc == 0 ? set_current(data.grid[0] / 10.0, fabs(data.grid[0] / 10.0 - meta.ref_ma) < 0.05 ? EVENT_REFERENCE : EVENT_SWEEP) : rc;
		}
		run.stage = STAGE_LOW;
		int rc = set_attenuation(run.primary_db + 10.0 * log10(4.0));
		return rc == 0 ? set_current(data.grid[run.grid_index] / 10.0, EVENT_SWEEP) : rc;
	}
	case STAGE_MIDDLE:
		if (++run.middle_index < 5 && run.middle_index <= run.segment_end) {
			double ma = data.grid[run.middle_index] / 10.0;
			return set_current(ma, fabs(ma - meta.ref_ma) < 0.05 ? EVENT_REFERENCE : EVENT_SWEEP);
		}
		run.stage = STAGE_LOW;
		run.grid_index = run.segment_end;
		{
			int rc = set_attenuation(run.primary_db + 10.0 * log10(4.0));
			double ma = data.grid[run.grid_index] / 10.0;
			return rc == 0 ? set_current(ma, fabs(ma - meta.ref_ma) < 0.05 ? EVENT_REFERENCE : EVENT_SWEEP) : rc;
		}
	case STAGE_LOW:
		if (run.grid_index > run.segment_start) {
			double ma = data.grid[--run.grid_index] / 10.0;
			return set_current(ma, fabs(ma - meta.ref_ma) < 0.05 ? EVENT_REFERENCE : EVENT_SWEEP);
		}
		/* Reduce current before returning to the more transmissive DAC pair. */
		run.stage = STAGE_REVISIT;
		run.grid_index = run.segment_end;
		{
			int rc = set_current(data.grid[run.grid_index] / 10.0, EVENT_REVISIT);
			if (rc == 0) {
				rc = restore_recorded_attenuation(&data.records[run.primary_record]);
			}
			return rc == 0 ? set_current(result.current_ma, EVENT_REVISIT) : rc;
		}
	case STAGE_REVISIT:
		if (run.primary_count == 1) {
			run.stage = STAGE_MID_DARK;
			return set_current(0.0, EVENT_DARK);
		}
		/* Fall through: the current boundary is already paired and revisited. */
		__fallthrough;
	case STAGE_MID_DARK:
		if (run.grid_index + 2U >= run.grid_count) {
			run.stage = STAGE_END;
			return set_current(0.0, EVENT_DARK);
		}
		if (run.primary_count >= 6 || run.atten_db < 0.05) {
			/* At the optical floor, continue all remaining paired currents at
			 * this transmission. Lack of signal is evidence, not a stop reason. */
			run.stage = STAGE_SWEEP;
			run.segment_start = run.grid_index + 1U;
			run.primary_record = result.records;
			return next_sweep_current();
		}
		{
			int rc = set_current(data.grid[run.grid_index] / 10.0, EVENT_PROBE);
			return rc == 0 ? start_search(4) : rc;
		}
	case STAGE_OTHER_REFERENCE:
		run.stage = STAGE_END;
		return set_current(0.0, EVENT_DARK);

	case STAGE_END: {
		int rc = stop_source();
		if (rc != 0) {
			return rc;
		}
		run.phase = PHASE_FIT;
		result.state = LASER_CAL_FITTING;
		result.acquisition_complete = true;
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
	    (run.phase != PHASE_WARM && run.phase != PHASE_WINDOW && run.phase != PHASE_SETTLE)) {
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

	if (run.phase == PHASE_SETTLE && k_uptime_get() - run.condition_started_ms >= meta.settle_ms) {
		rc = begin_window(run.event);
		if (rc != 0) {
			fail_acquisition(rc);
		}
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
			meta.baseline_slope_mv_per_s = work.beta[1] / work.time_scale_s;
			memcpy(meta.segment_gain_shift, work.gain_shift, sizeof(meta.segment_gain_shift));
			meta.dark_variance_mv2 = work.dark_variance;
			memcpy(meta.segment_mv_per_mw, work.gain, sizeof(meta.segment_mv_per_mw));
			memset(meta.fit_support, 0, sizeof(meta.fit_support));
			for (uint8_t i = 0; i < result.records; ++i) {
				if (work.include[i]) {
					meta.fit_support[i / 8U] |= 1U << (i % 8U);
				}
			}
		}
		{
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
		s.properties.nominal_current_ma = p->nominal_current_ma;
		s.properties.efficiency_mw_per_ma2 = 0.0f;
		s.properties.threshold_current_ma = p->threshold_current_ma;
		s.properties.efficiency_mw_per_ma = p->efficiency_mw_per_ma;
		s.properties.efficiency_ref_ma = p->efficiency_ref_ma;
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

	APPEND("{\"state\":\"%s\",\"laser\":\"%s\",\"output\":\"%s_%s\","
		"\"records\":%u,\"dwell_ms\":%u,\"settle_ms\":%u,\"current_ma\":%.1f,"
		"\"acquisition_complete\":%s,\"overflow\":%s,\"shutdown_pending\":%s",
		states[s->state], hispec_laser_name(s->laser),
		s->laser <= HISPEC_LASER_1430_YJ ? "yj" : "hk",
		(s->first_fei ^ (s->output_index != 0)) ? "fei" : "ao", s->records, s->dwell_ms, s->settle_ms,
		(double)s->current_ma, BOOL(s->acquisition_complete), BOOL(s->overflow), BOOL(s->shutdown_pending));
	ERROR("error", s->error);

	APPEND(",\"fit\":{\"valid\":%s,\"accepted\":%s,\"applied\":%s,\"persisted\":%s,\"n\":%u",
		BOOL(f->valid), BOOL(f->accepted), BOOL(f->applied), BOOL(f->persisted), f->points);
	ERROR("error", f->error);
	ERROR("apply_error", f->apply_error);
	ERROR("persist_error", f->persist_error);
	NUMBER("threshold_ma", f->threshold_ma, f->valid);
	NUMBER("efficiency_mw_per_ma", f->efficiency_mw_per_ma, f->valid);
	NUMBER("efficiency_mw_per_ma2", f->efficiency_mw_per_ma2, f->valid);
	NUMBER("efficiency_ref_ma", f->efficiency_ref_ma, f->valid);
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
		bytes[4] = 4;
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

		/* Bytes 16..27: acquisition timing (uptime, not UTC). */
		sys_put_le32(meta.dwell_ms, bytes + 16);
		sys_put_le64(meta.started_ms, bytes + 20);

		/* Bytes 28..147: fifteen frozen optical/laser assumptions as doubles. */
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
		size_t off = 28;
		for (unsigned i = 0; i < ARRAY_SIZE(values); ++i, off += 8) {
			put_double(bytes + off, values[i]);
		}

		/* Bytes 148..171: DAC scale/limit pairs and observed temperature range. */
		for (unsigned i = 0; i < 2; ++i, off += 4) {
			put_float(bytes + off, meta.dac_full_scale_mv[i]);
		}
		for (unsigned i = 0; i < 2; ++i, off += 4) {
			put_float(bytes + off, meta.dac_limit_mv[i]);
		}
		put_float(bytes + off, meta.temp_min_c);
		put_float(bytes + off + 4, meta.temp_max_c);
		off += 8;

		/* Bytes 172..315: two FVOA snapshots, each six doubles then six floats. */
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
		/* Bytes 316..505: policy, calibrated gains, baseline and exact
		 * 240-bit support. Timing and diagnostics follow; all encoding is explicit. */
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
		sys_put_le32(meta.settle_ms, bytes + off);
		put_double(bytes + off + 4, meta.baseline_slope_mv_per_s);
		off += 12;
		for (unsigned j = 0; j < LASER_CAL_SEGMENTS; ++j, off += 4) {
			put_float(bytes + off, meta.segment_gain_shift[j]);
		}
		const double diagnostics[] = {result.fit.revisit_fraction, result.fit.band_fraction,
			result.fit.atten_shift_sigma, result.fit.dark_residual_mv};
		for (unsigned j = 0; j < ARRAY_SIZE(diagnostics); ++j, off += 8) {
			put_double(bytes + off, result.fit.valid ? diagnostics[j] : (double)NAN);
		}
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
