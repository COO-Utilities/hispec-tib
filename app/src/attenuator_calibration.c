/**
 * @file attenuator_calibration.c
 * @brief Bridge-normalized automatic FVOA attenuator calibration.
 *
 * This is a lab acquisition routine driven by the throughput monitor thread.
 * It uses the configured photodiode dark value, never measures a private dark,
 * and retains raw records even when the final fit is not accepted. The routine
 * deliberately does not use a datasheet voltage schedule: it finds usable
 * regions by binary searches against photodiode saturation and SNR.
 *
 * The controlling model is a photodiode usable band, not a single "good/bad"
 * threshold. Saturated samples are too bright, below-SNR samples are too dim,
 * and usable samples are retained for fitting if their normalized transmission
 * is inside the attenuator model domain. During a DUT sweep, increasing the DUT
 * DAC increases attenuation and should move the photodiode signal downward.
 * Therefore a saturated sweep point is diagnostic evidence to keep sweeping
 * toward more DUT attenuation; it is not a bridge trigger. A below-SNR sweep
 * point marks the dim edge of the current segment and starts bridge
 * normalization from the latest retained usable DUT anchor.
 *
 * Bridge normalization holds the DUT fixed and opens the companion FVOA to raise
 * the photodiode signal back near the bright side of the usable band. Companion
 * DAC direction is the inverse of signal: lower companion DAC raises signal,
 * higher companion DAC attenuates more. The companion search maintains a
 * saturated low-DAC side and a more-attenuated high-DAC side, while separately
 * remembering the lowest usable companion DAC candidate. A failed confirmation
 * at that candidate is folded back into the companion bracket; swept-DUT backoff
 * is reserved for cases where the held DUT anchor itself is no longer a good
 * bridge point or the companion search exhausts its bounded range.
 *
 * Each accepted bridge contributes an after/before photodiode ratio to the
 * cumulative segment scale. Retained records store only measured acquisition
 * facts: DAC positions, laser level, net photodiode signal, signal error,
 * maximum window value, event, classification, and segment. Bridge scale,
 * scaled signal, relative transmission, dB attenuation, fit inclusion, and
 * residuals are derived in one pass after acquisition so the retained record
 * cannot confuse raw measurements with model products.
 */

#include "attenuator_calibration.h"

#include <errno.h>
#include <math.h>
#include <string.h>

#include <coo_commons/json_utils.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/sys/util.h>

#include "app_settings.h"
#include "attenuator.h"
#include "command.h"
#include "devices.h"
#include "mems_switching.h"
#include "throughput_monitor.h"

LOG_MODULE_REGISTER(attenuator_calibration, LOG_LEVEL_INF);

#define ATTEN_CAL_DEFAULT_DWELL_MS 300U
#define ATTEN_CAL_MAX_DWELL_MS 2000U
#define ATTEN_CAL_ADC_LSB_MV 0.1875
#define ATTEN_CAL_ADC_CLIP_MV 5000.0
#define ATTEN_CAL_SEARCH_STEP_MV 5.0
#define ATTEN_CAL_MAX_SEARCH_TRIES 16U
#define ATTEN_CAL_MIN_FIT_POINTS ATTENUATOR_CAL_MIN_FIT_POINTS
#define ATTEN_CAL_MIN_TX 1.0e-10
#define ATTEN_CAL_MAX_TX 0.999999
#define ATTEN_CAL_MIN_FIT_CORR 0.85
#define ATTEN_CAL_MIN_DB_ERR 1.0e-6
#define ATTEN_CAL_SNR_USABLE 5.0
#define ATTEN_CAL_DAC_SIGMA_MV 3.0
#define ATTEN_CAL_BRIDGE_BACKOFF_SIGNAL_FRACTION 0.80
#define ATTEN_CAL_FIT_MAX_ITER 30U
#define ATTEN_CAL_FIT_INITIAL_LAMBDA 1.0e-3
#define ATTEN_CAL_FIT_MIN_SLOPE 1.0e-12
#define ATTEN_CAL_FIT_MAX_SLOPE 1.0
#define ATTEN_CAL_TELEMETRY_TOPIC_SUFFIX "atten"
#define ATTEN_CAL_DATA_CHUNK_HEADER_SIZE 16U
#define ATTEN_CAL_DATA_CHUNK_RECORD_SIZE 27U
#define ATTEN_CAL_DATA_CHUNK_RECORDS \
	((COO_CMD_PAYLOAD_MAX - ATTEN_CAL_DATA_CHUNK_HEADER_SIZE) / \
	 ATTEN_CAL_DATA_CHUNK_RECORD_SIZE)
#define ATTEN_CAL_DATA_CHUNK_MAGIC0 'H'
#define ATTEN_CAL_DATA_CHUNK_MAGIC1 'A'
#define ATTEN_CAL_DATA_CHUNK_MAGIC2 'C'
#define ATTEN_CAL_DATA_CHUNK_MAGIC3 '4'
#define ATTEN_CAL_DATA_CHUNK_VERSION 3U

enum atten_cal_state {
	ATTEN_CAL_STATE_INACTIVE = 0,
	ATTEN_CAL_STATE_RUNNING,
	ATTEN_CAL_STATE_COMPLETE,
	ATTEN_CAL_STATE_ERROR,
};

enum atten_cal_mode {
	ATTEN_CAL_MODE_NONE = 0,
	ATTEN_CAL_MODE_TIB_AUTO,
};

enum atten_cal_phase {
	ATTEN_CAL_PHASE_NONE = 0,
	ATTEN_CAL_PHASE_WAIT_WINDOW,
};

enum atten_cal_measure_kind {
	ATTEN_CAL_MEASURE_NONE = 0,
	ATTEN_CAL_MEASURE_INITIAL_PROBE,
	ATTEN_CAL_MEASURE_REFERENCE,
	ATTEN_CAL_MEASURE_SWEEP,
	ATTEN_CAL_MEASURE_BRIDGE_BEFORE,
	ATTEN_CAL_MEASURE_BRIDGE_PROBE,
	ATTEN_CAL_MEASURE_BRIDGE_AFTER,
};

enum atten_cal_record_event {
	ATTEN_CAL_EVENT_POINT = 0,
	ATTEN_CAL_EVENT_INITIAL_PROBE,
	ATTEN_CAL_EVENT_REFERENCE,
	ATTEN_CAL_EVENT_BRIDGE_BEFORE,
	ATTEN_CAL_EVENT_BRIDGE_PROBE,
	ATTEN_CAL_EVENT_BRIDGE_AFTER,
};

enum atten_cal_record_classification {
	ATTEN_CAL_CLASSIFICATION_OK = 0,
	ATTEN_CAL_CLASSIFICATION_SATURATED,
	ATTEN_CAL_CLASSIFICATION_BELOW_SNR,
	ATTEN_CAL_CLASSIFICATION_ADC_ERROR,
};

struct atten_cal_measurement {
	double signal_mv;
	double signal_err_mv;
	double snr;
	double max_mv;
	bool saturated;
	bool usable;
	enum atten_cal_record_classification classification;
};

struct atten_cal_record {
	float sweep_mv;
	float other_mv;
	float laser_pct;
	float signal_mv;
	float signal_err_mv;
	float max_mv;
	uint8_t event;
	uint8_t classification;
	uint8_t segment;
};

BUILD_ASSERT(ATTEN_CAL_DATA_CHUNK_RECORDS > 0U,
	     "calibration data chunk must carry at least one record");

struct atten_cal_bridge {
	uint8_t boundary_index;
	uint8_t before_record_index;
	uint8_t after_record_index;
};

struct atten_cal_fit_point {
	uint8_t record_index;
	double dac_mv;
	double measured_db;
	double measured_db_err;
	double tx;
};

struct atten_cal_state_data {
	enum atten_cal_state state;
	enum atten_cal_mode mode;
	enum atten_cal_phase phase;
	enum atten_cal_measure_kind measure_kind;
	uint8_t attenuator_index;
	uint8_t physical_index;
	uint8_t point_index;
	uint8_t segment_id;
	uint32_t dwell_ms;
	bool persistent;
	enum hispec_laser_id laser;
	enum photodiode_channel channel;
	uint8_t laser_level_index;
	double laser_percent;
	/* Current commanded DAC pair for the active measurement. */
	double sweep_mv;
	double other_mv;
	/* Companion search bracket and latest usable candidate. */
	double search_low_mv;
	double search_high_mv;
	double search_candidate_mv;
	bool search_candidate_valid;
	uint8_t search_tries;
	/* DUT sweep bracket; bridge anchoring scans retained usable records. */
	double sweep_low_mv;
	double sweep_high_mv;
	/* Held-DUT bridge measurement and bounded swept-anchor backoff state. */
	double bridge_before_signal_mv;
	uint8_t bridge_before_index;
	bool bridge_before_index_valid;
	double bridge_start_other_mv;
	uint8_t bridge_backoff_remaining;
	int64_t wait_until_ms;
	int last_error;
	struct atten_cal_record records[ATTENUATOR_PHYSICAL_COUNT][ATTENUATOR_CAL_RECORD_COUNT];
	struct atten_cal_bridge bridges[ATTENUATOR_PHYSICAL_COUNT][ATTENUATOR_CAL_RECORD_COUNT];
	uint8_t record_count[ATTENUATOR_PHYSICAL_COUNT];
	uint8_t bridge_count[ATTENUATOR_PHYSICAL_COUNT];
	bool record_overflow[ATTENUATOR_PHYSICAL_COUNT];
	struct attenuator_calibration_fit_metrics fit[ATTENUATOR_PHYSICAL_COUNT];
};

static const double initial_laser_levels_pct[] = {100.0, 50.0, 5.0};

static struct atten_cal_state_data cal;
static K_MUTEX_DEFINE(cal_lock);
static struct coo_cmd_response cal_telemetry_msg;
static struct atten_cal_fit_point cal_fit_points[ATTENUATOR_CAL_RECORD_COUNT];

static void copy_status_locked(struct attenuator_calibration_status *status);
static void auto_schedule_measure_locked(enum atten_cal_measure_kind kind,
					 double sweep_mv, double other_mv);
static void auto_fit_locked(void);

/** Return the JSON/status spelling for an internal calibration state. */
static const char *state_name(enum atten_cal_state state)
{
	switch (state) {
	case ATTEN_CAL_STATE_INACTIVE:
		return "inactive";
	case ATTEN_CAL_STATE_RUNNING:
		return "running";
	case ATTEN_CAL_STATE_COMPLETE:
		return "complete";
	case ATTEN_CAL_STATE_ERROR:
		return "error";
	default:
		return "unknown";
	}
}

/** Return the JSON/status spelling for an internal calibration mode. */
static const char *mode_name(enum atten_cal_mode mode)
{
	switch (mode) {
	case ATTEN_CAL_MODE_NONE:
		return "none";
	case ATTEN_CAL_MODE_TIB_AUTO:
		return "tib_auto";
	default:
		return "unknown";
	}
}

/** Return the public physical-FVOA name for a physical index. */
static const char *physical_name(uint8_t physical_index)
{
	return physical_index == 0U ? "dac1" : "dac2";
}

/** Return the retained-record and telemetry event spelling for an event id. */
static const char *record_event_name(uint8_t event)
{
	switch ((enum atten_cal_record_event)event) {
	case ATTEN_CAL_EVENT_POINT:
		return "point";
	case ATTEN_CAL_EVENT_INITIAL_PROBE:
		return "initial_probe";
	case ATTEN_CAL_EVENT_REFERENCE:
		return "reference";
	case ATTEN_CAL_EVENT_BRIDGE_BEFORE:
		return "bridge_before";
	case ATTEN_CAL_EVENT_BRIDGE_PROBE:
		return "bridge_probe";
	case ATTEN_CAL_EVENT_BRIDGE_AFTER:
		return "bridge_after";
	default:
		return "unknown";
	}
}

/** Return the retained-record and telemetry classification spelling for an id. */
static const char *record_classification_name(uint8_t classification)
{
	switch ((enum atten_cal_record_classification)classification) {
	case ATTEN_CAL_CLASSIFICATION_OK:
		return "ok";
	case ATTEN_CAL_CLASSIFICATION_SATURATED:
		return "saturated";
	case ATTEN_CAL_CLASSIFICATION_BELOW_SNR:
		return "below_snr";
	case ATTEN_CAL_CLASSIFICATION_ADC_ERROR:
		return "adc_error";
	default:
		return "unknown";
	}
}

/** Clamp a requested dwell to the calibration-supported averaging interval. */
static uint32_t clamp_dwell(uint32_t dwell_ms)
{
	if (dwell_ms == 0U) {
		return ATTEN_CAL_DEFAULT_DWELL_MS;
	}
	return MIN(dwell_ms, ATTEN_CAL_MAX_DWELL_MS);
}

/** Estimate calibration progress from physical index and retained record count. */
static uint8_t complete_percent_locked(void)
{
	uint16_t count;

	if (cal.state == ATTEN_CAL_STATE_INACTIVE) {
		return 0U;
	}
	if (cal.state == ATTEN_CAL_STATE_COMPLETE) {
		return 100U;
	}
	count = (uint16_t)cal.physical_index * ATTENUATOR_CAL_RECORD_COUNT +
		cal.record_count[cal.physical_index];
	return (uint8_t)MIN(99U, (count * 100U) /
			    (ATTENUATOR_PHYSICAL_COUNT * ATTENUATOR_CAL_RECORD_COUNT));
}

/** Decide whether a normalized transmission lies in the invertible fit domain. */
static bool point_valid_for_fit(double tx)
{
	return tx > ATTEN_CAL_MIN_TX && tx < ATTEN_CAL_MAX_TX;
}

/** Publish one already-formatted best-effort calibration telemetry message. */
static void atten_cal_publish_telemetry(struct coo_cmd_response *msg)
{
	if (msg == NULL) {
		return;
	}
	msg->payload_len = strlen(msg->payload);
	(void)coo_cmd_runtime_emit(
		command_runtime_get(),
		&(const struct coo_cmd_runtime_emit_args){
			.type = COO_CMD_RUNTIME_EMIT_DATA,
			.delivery = COO_CMD_RUNTIME_EMIT_BEST_EFFORT,
			.suffix = ATTEN_CAL_TELEMETRY_TOPIC_SUFFIX,
			.out = msg,
		});
}

/** Start a common telemetry JSON object populated with current calibration state. */
static struct coo_cmd_response *atten_cal_telemetry_begin(size_t *off,
							  const char *event)
{
	struct coo_cmd_response *msg = &cal_telemetry_msg;

	if (off == NULL) {
		return NULL;
	}

	memset(msg, 0, sizeof(*msg));
	*off = 0U;
	if (coo_json_append(msg->payload, sizeof(msg->payload), off,
			    "{\"event\":\"%s\",\"state\":\"%s\",\"mode\":\"%s\","
			    "\"physical\":\"%s\",\"attenuator\":%u,"
			    "\"complete_pct\":%u,\"record_count\":%u,"
			    "\"segment\":%u,\"sweep_mv\":%.6f,"
			    "\"other_mv\":%.6f,\"laser_pct\":%.6f",
			    event == NULL ? "status" : event,
			    state_name(cal.state), mode_name(cal.mode),
			    physical_name(cal.physical_index), cal.attenuator_index,
			    complete_percent_locked(),
			    cal.record_count[cal.physical_index],
			    cal.segment_id, cal.sweep_mv, cal.other_mv,
			    cal.laser_percent) != 0) {
		return NULL;
	}
	return msg;
}

/** Emit a short state-transition telemetry message with the last error code. */
static void atten_cal_emit_simple(const char *event)
{
	size_t off = 0U;
	struct coo_cmd_response *msg = atten_cal_telemetry_begin(&off, event);

	if (msg == NULL) {
		return;
	}
	if (coo_json_append(msg->payload, sizeof(msg->payload),
			    &off, ",\"error\":%d}", cal.last_error) != 0) {
		return;
	}
	atten_cal_publish_telemetry(msg);
}

/** Emit telemetry after a new DAC pair setpoint has been issued. */
static void atten_cal_emit_set(const char *event)
{
	size_t off = 0U;
	struct coo_cmd_response *msg = atten_cal_telemetry_begin(&off, event);

	if (msg == NULL) {
		return;
	}
	if (coo_json_append(msg->payload, sizeof(msg->payload), &off, "}") != 0) {
		return;
	}
	atten_cal_publish_telemetry(msg);
}

/** Emit telemetry for a retained measurement record. */
static void atten_cal_emit_record(const struct atten_cal_record *record)
{
	size_t off = 0U;
	struct coo_cmd_response *msg;

	if (record == NULL) {
		return;
	}
	msg = atten_cal_telemetry_begin(&off, record_event_name(record->event));
	if (msg == NULL) {
		return;
	}
	if (coo_json_append(msg->payload, sizeof(msg->payload), &off,
			    ",\"i\":%u,\"classification\":\"%s\","
			    "\"signal_mv\":%.6f,\"signal_err_mv\":%.6f,"
			    "\"max_mv\":%.6f}",
			    cal.point_index,
			    record_classification_name(record->classification),
			    (double)record->signal_mv,
			    (double)record->signal_err_mv,
			    (double)record->max_mv) != 0) {
		return;
	}
	atten_cal_publish_telemetry(msg);
}

/** Emit telemetry for one physical-FVOA fit result. */
static void atten_cal_emit_fit(uint8_t physical,
			       const struct attenuator_calibration_fit_metrics *fit)
{
	size_t off = 0U;
	struct coo_cmd_response *msg = atten_cal_telemetry_begin(&off, "fit");

	if (msg == NULL) {
		return;
	}
	if (coo_json_append(msg->payload, sizeof(msg->payload), &off,
			    ",\"fit_physical\":\"%s\",\"valid\":%s,"
			    "\"accepted\":%s,\"points\":%u,"
			    "\"fvoa_50pct_mv\":%.12g,"
			    "\"slope_inv_fvoa_mv\":%.12g,"
			    "\"corr\":%.12g,\"rms_db\":%.12g,"
			    "\"max_abs_db\":%.12g,\"min_tx\":%.12g,"
			    "\"max_tx\":%.12g,\"fvoa_span_mv\":%.6f}",
			    physical_name(physical),
			    fit != NULL && fit->valid ? "true" : "false",
			    fit != NULL && fit->accepted ? "true" : "false",
			    fit != NULL ? fit->points : 0U,
			    fit != NULL ? (double)fit->fvoa_50pct_mv : (double)NAN,
			    fit != NULL ? (double)fit->slope_inv_fvoa_mv : (double)NAN,
			    fit != NULL ? (double)fit->correlation : (double)NAN,
			    fit != NULL ? (double)fit->rms_db : (double)NAN,
			    fit != NULL ? (double)fit->max_abs_db : (double)NAN,
			    fit != NULL ? (double)fit->min_tx : (double)NAN,
			    fit != NULL ? (double)fit->max_tx : (double)NAN,
			    fit != NULL ? (double)fit->fvoa_span_mv : (double)NAN) != 0) {
		return;
	}
	atten_cal_publish_telemetry(msg);
}

/**
 * Decide whether a photodiode window is pinned against the ADC rail.
 *
 * Saturation is based on the mean, not the max excursion: a noisy rail sample is
 * diagnostic, but a saturated diode has the whole averaging window at the wall.
 */
static bool sample_is_saturated(const struct photodiode_window_result *window)
{
	return window != NULL && window->mean_net_mv >= ATTEN_CAL_ADC_CLIP_MV;
}

/**
 * Convert the current photodiode configurable window into the calibration model.
 *
 * This classifies the window into the three bands used by the acquisition
 * logic: saturated/too bright, usable, or below-SNR/too dim. It does not retry
 * ADC reads; the photodiode sampler has already folded failed samples into the
 * window status and uncertainty.
 */
static void measurement_from_window(const struct photodiode_window_result *window,
				    struct atten_cal_measurement *measurement)
{
	if (measurement == NULL) {
		return;
	}
	memset(measurement, 0, sizeof(*measurement));
	measurement->classification = ATTEN_CAL_CLASSIFICATION_ADC_ERROR;

	if (window == NULL || !window->valid ||
	    window->sample_length == window->failed_samples) {
		measurement->classification = ATTEN_CAL_CLASSIFICATION_ADC_ERROR;
		return;
	}

	measurement->signal_mv = window->mean_net_mv;
	measurement->signal_err_mv = window->mean_net_err_mv;
	measurement->max_mv = window->max_mv;
	measurement->saturated = sample_is_saturated(window);
	if (!(measurement->signal_err_mv > 0.0) || !isfinite(measurement->signal_err_mv)) {
		measurement->signal_err_mv = ATTEN_CAL_ADC_LSB_MV;
	}

	if (measurement->saturated) {
		measurement->snr = NAN;
		measurement->classification = ATTEN_CAL_CLASSIFICATION_SATURATED;
		return;
	}

	measurement->snr = measurement->signal_mv / measurement->signal_err_mv;
	if (measurement->signal_mv <= 0.0 ||
	    !isfinite(measurement->snr) ||
	    measurement->snr < ATTEN_CAL_SNR_USABLE) {
		measurement->classification = ATTEN_CAL_CLASSIFICATION_BELOW_SNR;
		return;
	}

	measurement->classification = ATTEN_CAL_CLASSIFICATION_OK;
	measurement->usable = true;
}

/**
 * Retain one raw measurement for later host inspection and fit preparation.
 *
 * Retained records deliberately contain only acquisition facts. Bridge scaling,
 * normalized transmission, dB conversion, residuals, and fit inclusion are
 * derived later from these records and the accepted bridge table.
 */
static bool append_record_locked(enum atten_cal_record_event event,
				 const struct atten_cal_measurement *measurement,
				 struct atten_cal_record **out)
{
	struct atten_cal_record *record;
	uint8_t physical = cal.physical_index;
	uint8_t index;

	if (out != NULL) {
		*out = NULL;
	}
	if (measurement == NULL || physical >= ATTENUATOR_PHYSICAL_COUNT) {
		return false;
	}
	if (cal.record_count[physical] >= ATTENUATOR_CAL_RECORD_COUNT) {
		cal.record_overflow[physical] = true;
		return false;
	}

	index = cal.record_count[physical]++;
	record = &cal.records[physical][index];
	memset(record, 0, sizeof(*record));

	record->sweep_mv = (float)cal.sweep_mv;
	record->other_mv = (float)cal.other_mv;
	record->laser_pct = (float)cal.laser_percent;
	record->signal_mv = (float)measurement->signal_mv;
	record->signal_err_mv = (float)measurement->signal_err_mv;
	record->max_mv = (float)measurement->max_mv;
	record->event = (uint8_t)event;
	record->classification = (uint8_t)measurement->classification;
	record->segment = cal.segment_id;

	cal.point_index = index;
	atten_cal_emit_record(record);
	if (out != NULL) {
		*out = record;
	}
	return true;
}

/** Write the swept and companion FVOA DAC voltages for a logical attenuator. */
static bool set_physical_pair(uint8_t attenuator_index,
			      uint8_t sweep_physical,
			      double sweep_mv,
			      double other_mv)
{
	struct attenuator *atten;

	if (!devices_attenuator_channel_available(attenuator_index) ||
	    sweep_physical >= ATTENUATOR_PHYSICAL_COUNT) {
		return false;
	}

	atten = &attenuators[attenuator_index];
	if (!attenuator_set_physical_voltage(atten, sweep_physical, sweep_mv)) {
		return false;
	}
	return attenuator_set_physical_voltage(atten, sweep_physical == 0U ? 1U : 0U,
					       other_mv);
}

/** Reset all calibration state while preserving the requested top-level state. */
static void reset_locked(enum atten_cal_state state)
{
	memset(&cal, 0, sizeof(cal));
	cal.state = state;
	cal.mode = ATTEN_CAL_MODE_NONE;
	cal.phase = ATTEN_CAL_PHASE_NONE;
	cal.dwell_ms = ATTEN_CAL_DEFAULT_DWELL_MS;
	cal.other_mv = ATTENUATOR_DRIVE_MAX_MV;
	cal.laser_percent = 100.0;
	cal.sweep_high_mv = ATTENUATOR_DRIVE_MAX_MV;
}

/** Copy internal calibration state into the public command/status structure. */
static void copy_status_locked(struct attenuator_calibration_status *status)
{
	if (status == NULL) {
		return;
	}

	memset(status, 0, sizeof(*status));
	status->state = state_name(cal.state);
	status->mode = mode_name(cal.mode);
	status->physical = physical_name(cal.physical_index);
	if (cal.fit[0].accepted && cal.fit[1].accepted) {
		status->fit = "ok";
	} else if (cal.last_error != 0) {
		status->fit = "failed";
	} else {
		status->fit = "none";
	}
	status->attenuator_index = cal.attenuator_index;
	status->physical_index = cal.physical_index;
	status->point_index = cal.point_index;
	status->point_count = ATTENUATOR_CAL_RECORD_COUNT;
	status->dwell_ms = cal.dwell_ms == 0U ? ATTEN_CAL_DEFAULT_DWELL_MS : cal.dwell_ms;
	status->complete_pct = complete_percent_locked();
	status->current_mv = cal.sweep_mv;
	status->other_mv = cal.other_mv;
	status->last_error = cal.last_error;
	memcpy(status->fit_metrics, cal.fit, sizeof(status->fit_metrics));
}

/** Put calibration into terminal error state and emit the corresponding telemetry. */
static void auto_error_locked(int error)
{
	cal.last_error = error;
	cal.state = ATTEN_CAL_STATE_ERROR;
	cal.phase = ATTEN_CAL_PHASE_NONE;
	atten_cal_emit_simple("error");
}

/** Apply one of the bounded initial laser levels used to find a safe start. */
static bool auto_set_laser_level_locked(uint8_t level_index)
{
	if (level_index >= ARRAY_SIZE(initial_laser_levels_pct)) {
		return false;
	}
	cal.laser_level_index = level_index;
	cal.laser_percent = initial_laser_levels_pct[level_index];
	if (hispec_laser_set_output_percent_autooff(cal.laser, cal.laser_percent, 0U) != 0) {
		auto_error_locked(-EIO);
		return false;
	}
	return true;
}

/** Return the next midpoint for companion-FVOA binary searches. */
static double search_midpoint_locked(void)
{
	return (cal.search_low_mv + cal.search_high_mv) * 0.5;
}

/** Return the next midpoint for DUT-FVOA binary sweeps. */
static double sweep_midpoint_locked(void)
{
	return (cal.sweep_low_mv + cal.sweep_high_mv) * 0.5;
}

/**
 * Reset the companion-FVOA search bracket.
 *
 * Companion DAC direction is inverted relative to photodiode signal: lower DAC
 * opens the companion and raises signal, while higher DAC attenuates more. The
 * low side of this bracket is therefore the too-bright/saturated side; the high
 * side is the more-attenuated side. A usable candidate is tracked separately
 * because the high bracket can also be a below-SNR point.
 */
static void companion_search_begin_locked(double saturated_low_mv,
					  double attenuated_high_mv,
					  bool high_is_usable_candidate)
{
	cal.search_low_mv = CLAMP(saturated_low_mv, 0.0, ATTENUATOR_DRIVE_MAX_MV);
	cal.search_high_mv = CLAMP(attenuated_high_mv, 0.0, ATTENUATOR_DRIVE_MAX_MV);
	cal.search_candidate_mv = cal.search_high_mv;
	cal.search_candidate_valid = high_is_usable_candidate;
	cal.search_tries = 0U;
}

/** Forget the current companion candidate when a repeat proves it is not usable. */
static void companion_search_invalidate_current_locked(void)
{
	if (cal.search_candidate_valid &&
	    fabs(cal.search_candidate_mv - cal.other_mv) <= ATTEN_CAL_SEARCH_STEP_MV) {
		cal.search_candidate_valid = false;
	}
}

/**
 * Fold one companion-search measurement into the shared bracket.
 *
 * Saturated means the companion is too open, so the saturated low side moves up.
 * Below-SNR means the companion is still too attenuated, so the high side moves
 * down. A usable measurement becomes the current candidate and the search keeps
 * moving lower in DAC to find the brightest non-saturated point.
 */
static bool companion_search_note_measurement_locked(
	const struct atten_cal_measurement *measurement)
{
	if (measurement == NULL) {
		return false;
	}
	if (measurement->classification == ATTEN_CAL_CLASSIFICATION_SATURATED) {
		cal.search_low_mv = cal.other_mv;
		companion_search_invalidate_current_locked();
	} else if (measurement->usable) {
		cal.search_high_mv = cal.other_mv;
		cal.search_candidate_mv = cal.other_mv;
		cal.search_candidate_valid = true;
	} else if (measurement->classification == ATTEN_CAL_CLASSIFICATION_BELOW_SNR) {
		cal.search_high_mv = cal.other_mv;
		companion_search_invalidate_current_locked();
	} else {
		return false;
	}
	cal.search_tries++;
	return true;
}

/** Return true when the companion bracket has reached its bounded resolution. */
static bool companion_search_complete_locked(void)
{
	return cal.search_high_mv - cal.search_low_mv <= ATTEN_CAL_SEARCH_STEP_MV ||
	       cal.search_tries >= ATTEN_CAL_MAX_SEARCH_TRIES;
}

/**
 * Reject a bridge confirmation measurement and keep searching the companion.
 *
 * A saturated repeat means the candidate was too open; reopen the high side to
 * the bridge entry point if the normal binary search had already collapsed onto
 * the candidate. A below-SNR or ratio-failed repeat means the candidate did not
 * raise signal enough for normalization, so the high side moves down.
 */
static void companion_search_reject_current_locked(
	enum atten_cal_record_classification classification)
{
	if (classification == ATTEN_CAL_CLASSIFICATION_SATURATED) {
		if (cal.search_high_mv <= cal.other_mv + ATTEN_CAL_SEARCH_STEP_MV) {
			cal.search_high_mv = cal.bridge_start_other_mv;
		}
		cal.search_low_mv = cal.other_mv;
	} else {
		cal.search_high_mv = cal.other_mv;
	}
	companion_search_invalidate_current_locked();
	cal.search_tries++;
}

/** Return whether a retained record can anchor bridge normalization. */
static bool record_is_bridge_anchor(const struct atten_cal_record *record)
{
	return record != NULL &&
	       (record->event == ATTEN_CAL_EVENT_REFERENCE ||
		record->event == ATTEN_CAL_EVENT_POINT ||
		record->event == ATTEN_CAL_EVENT_BRIDGE_AFTER) &&
	       record->classification == ATTEN_CAL_CLASSIFICATION_OK &&
	       record->signal_mv > 0.0f;
}

/** Find the latest usable DUT point in the current segment. */
static const struct atten_cal_record *latest_bridge_anchor_locked(uint8_t physical)
{
	if (physical >= ATTENUATOR_PHYSICAL_COUNT) {
		return NULL;
	}
	for (uint8_t i = cal.record_count[physical]; i > 0U; --i) {
		const struct atten_cal_record *record = &cal.records[physical][i - 1U];

		if (record->segment == cal.segment_id &&
		    record_is_bridge_anchor(record)) {
			return record;
		}
	}
	return NULL;
}

/** Drop accepted bridge entries that point into records being overwritten. */
static void truncate_bridges_after_record_locked(uint8_t physical, uint8_t record_count)
{
	if (physical >= ATTENUATOR_PHYSICAL_COUNT) {
		return;
	}
	while (cal.bridge_count[physical] > 0U) {
		const struct atten_cal_bridge *bridge =
			&cal.bridges[physical][cal.bridge_count[physical] - 1U];

		if (bridge->boundary_index < record_count &&
		    bridge->before_record_index < record_count &&
		    bridge->after_record_index < record_count) {
			return;
		}
		cal.bridge_count[physical]--;
	}
}

/** Set DAC voltages for a measurement and wait one photodiode configurable window. */
static void auto_schedule_measure_locked(enum atten_cal_measure_kind kind,
					 double sweep_mv, double other_mv)
{
	const char *event = "set";

	cal.measure_kind = kind;
	cal.sweep_mv = CLAMP(sweep_mv, 0.0, ATTENUATOR_DRIVE_MAX_MV);
	cal.other_mv = CLAMP(other_mv, 0.0, ATTENUATOR_DRIVE_MAX_MV);
	if (!set_physical_pair(cal.attenuator_index, cal.physical_index,
			       cal.sweep_mv, cal.other_mv)) {
		auto_error_locked(-EIO);
		return;
	}
	throughput_monitor_note_attenuator_changed(cal.attenuator_index);
	switch (kind) {
	case ATTEN_CAL_MEASURE_INITIAL_PROBE:
		event = "initial_probe_set";
		break;
	case ATTEN_CAL_MEASURE_REFERENCE:
	case ATTEN_CAL_MEASURE_SWEEP:
		event = "point_set";
		break;
	case ATTEN_CAL_MEASURE_BRIDGE_BEFORE:
		event = "bridge_before_set";
		break;
	case ATTEN_CAL_MEASURE_BRIDGE_PROBE:
		event = "bridge_probe_set";
		break;
	case ATTEN_CAL_MEASURE_BRIDGE_AFTER:
		event = "bridge_after_set";
		break;
	default:
		break;
	}
	atten_cal_emit_set(event);
	cal.wait_until_ms = k_uptime_get() + cal.dwell_ms;
	cal.phase = ATTEN_CAL_PHASE_WAIT_WINDOW;
}

/** Initialize acquisition state for the current physical FVOA and schedule its first probe. */
static void auto_start_next_physical_locked(void)
{
	uint8_t physical = cal.physical_index;

	cal.point_index = 0U;
	cal.segment_id = 0U;
	cal.search_tries = 0U;
	cal.sweep_mv = 0.0;
	cal.other_mv = ATTENUATOR_DRIVE_MAX_MV;
	companion_search_begin_locked(0.0, ATTENUATOR_DRIVE_MAX_MV, false);
	cal.sweep_low_mv = 0.0;
	cal.sweep_high_mv = ATTENUATOR_DRIVE_MAX_MV;
	cal.bridge_before_signal_mv = 0.0;
	cal.bridge_before_index = 0U;
	cal.bridge_before_index_valid = false;
	cal.bridge_start_other_mv = cal.other_mv;
	cal.laser_level_index = 0U;
	cal.laser_percent = initial_laser_levels_pct[0];
	cal.record_count[physical] = 0U;
	cal.bridge_count[physical] = 0U;
	cal.record_overflow[physical] = false;
	memset(cal.records[physical], 0, sizeof(cal.records[physical]));
	memset(cal.bridges[physical], 0, sizeof(cal.bridges[physical]));
	memset(&cal.fit[physical], 0, sizeof(cal.fit[physical]));

	if (!auto_set_laser_level_locked(0U)) {
		return;
	}
	auto_schedule_measure_locked(ATTEN_CAL_MEASURE_INITIAL_PROBE,
				     0.0, ATTENUATOR_DRIVE_MAX_MV);
	atten_cal_emit_simple("physical_start");
}

/** Finish the current physical FVOA and either start the second one or fit the pair. */
static void auto_finish_physical_locked(void)
{
	LOG_INF("atten cal physical complete physical=%s records=%u overflow=%d",
		physical_name(cal.physical_index),
		cal.record_count[cal.physical_index],
		cal.record_overflow[cal.physical_index] ? 1 : 0);

	if (cal.physical_index == 0U) {
		cal.physical_index = 1U;
		auto_start_next_physical_locked();
		return;
	}
	auto_fit_locked();
}

/**
 * Back off to an earlier retained swept-DUT anchor and overwrite marginal records.
 *
 * This is deliberately not the normal response to companion-search noise.
 * Saturated or below-SNR companion repeats should refine the companion bracket;
 * backoff is for cases where the held DUT anchor cannot support a useful bridge
 * after bounded probing.
 */
static bool auto_bridge_with_backoff_locked(const char *context)
{
	uint8_t physical = cal.physical_index;
	uint8_t total;
	double current_sweep = cal.sweep_low_mv;
	double floor_mv = ATTEN_CAL_BRIDGE_BACKOFF_SIGNAL_FRACTION *
			  ATTEN_CAL_ADC_CLIP_MV;
	const struct atten_cal_record *current = NULL;

	if (physical >= ATTENUATOR_PHYSICAL_COUNT) {
		auto_error_locked(-EINVAL);
		return false;
	}

	total = cal.record_count[physical];
	for (uint8_t i = total; i > 0U; --i) {
		const struct atten_cal_record *record = &cal.records[physical][i - 1U];

		if (record->segment == cal.segment_id &&
		    record_is_bridge_anchor(record) &&
		    fabs((double)record->sweep_mv - current_sweep) <= 1.0e-6) {
			current = record;
			break;
		}
	}

	if (cal.bridge_backoff_remaining == 0U ||
	    (current != NULL && (double)current->signal_mv <= floor_mv)) {
		LOG_WRN("atten cal bridge failed physical=%s sweep=%.3f context=%s "
			"remaining=%u signal_mv=%.3f floor_mv=%.3f",
			physical_name(physical), current_sweep,
			context == NULL ? "unknown" : context,
			cal.bridge_backoff_remaining,
			current == NULL ? (double)NAN : (double)current->signal_mv,
			floor_mv);
		auto_error_locked(-ERANGE);
		return false;
	}

	for (uint8_t i = total; i > 0U; --i) {
		const struct atten_cal_record *record = &cal.records[physical][i - 1U];

		if (record->segment != cal.segment_id ||
		    !record_is_bridge_anchor(record) ||
		    (double)record->sweep_mv >= current_sweep - 1.0e-6 ||
		    fabs((double)record->other_mv - cal.bridge_start_other_mv) >
			    ATTEN_CAL_SEARCH_STEP_MV) {
			continue;
		}

		cal.record_count[physical] = i;
		truncate_bridges_after_record_locked(physical, i);
		cal.point_index = i - 1U;
		cal.sweep_low_mv = record->sweep_mv;
		cal.sweep_high_mv = ATTENUATOR_DRIVE_MAX_MV;
		cal.sweep_mv = cal.sweep_low_mv;
		cal.other_mv = cal.bridge_start_other_mv;
		cal.bridge_before_signal_mv = 0.0;
		cal.bridge_before_index = 0U;
		cal.bridge_before_index_valid = false;
		cal.bridge_backoff_remaining--;
		LOG_INF("atten cal bridge backoff physical=%s from=%.3f to=%.3f "
			"context=%s remaining=%u",
			physical_name(physical), current_sweep, cal.sweep_low_mv,
			context == NULL ? "unknown" : context,
			cal.bridge_backoff_remaining);
		atten_cal_emit_simple("bridge_backoff");
		auto_schedule_measure_locked(ATTEN_CAL_MEASURE_BRIDGE_BEFORE,
					     cal.sweep_low_mv,
					     cal.bridge_start_other_mv);
		return true;
	}

	LOG_WRN("atten cal bridge failed physical=%s sweep=%.3f context=%s no_candidate",
		physical_name(physical), current_sweep,
		context == NULL ? "unknown" : context);
	auto_error_locked(-ERANGE);
	return false;
}

/**
 * Start bridge normalization from the latest usable retained DUT anchor.
 *
 * `sweep_low_mv` may have been advanced by saturated diagnostic sweep records,
 * so the bridge anchor is recovered from retained usable records in the current
 * segment instead of trusting the current sweep bracket value.
 */
static void auto_begin_bridge_locked(void)
{
	uint8_t physical = cal.physical_index;
	uint8_t segment_points = 0U;
	const struct atten_cal_record *anchor;

	if (cal.other_mv <= ATTEN_CAL_SEARCH_STEP_MV) {
		auto_finish_physical_locked();
		return;
	}
	for (uint8_t i = 0U; i < cal.record_count[physical]; ++i) {
		const struct atten_cal_record *record = &cal.records[physical][i];

		if (record->segment == cal.segment_id &&
		    record_is_bridge_anchor(record)) {
			segment_points++;
		}
	}
	anchor = latest_bridge_anchor_locked(physical);
	if (anchor == NULL) {
		auto_error_locked(-ERANGE);
		return;
	}
	cal.sweep_low_mv = anchor->sweep_mv;
	cal.sweep_mv = cal.sweep_low_mv;
	cal.bridge_start_other_mv = cal.other_mv;
	cal.bridge_backoff_remaining = segment_points / 2U;
	auto_schedule_measure_locked(ATTEN_CAL_MEASURE_BRIDGE_BEFORE,
				     cal.sweep_low_mv, cal.other_mv);
}

/** Handle companion-search probes used to find the initial usable open reference. */
static void auto_handle_initial_probe_locked(const struct atten_cal_measurement *measurement)
{
	struct atten_cal_record *record = NULL;

	(void)append_record_locked(ATTEN_CAL_EVENT_INITIAL_PROBE, measurement, &record);
	if (record == NULL) {
		auto_error_locked(-ENOSPC);
		return;
	}

	if (measurement->saturated && cal.other_mv >= ATTENUATOR_DRIVE_MAX_MV - ATTEN_CAL_SEARCH_STEP_MV) {
		if (cal.laser_level_index + 1U >= ARRAY_SIZE(initial_laser_levels_pct)) {
			auto_error_locked(-ERANGE);
			return;
		}
		if (!auto_set_laser_level_locked((uint8_t)(cal.laser_level_index + 1U))) {
			return;
		}
		companion_search_begin_locked(0.0, ATTENUATOR_DRIVE_MAX_MV, false);
		auto_schedule_measure_locked(ATTEN_CAL_MEASURE_INITIAL_PROBE,
					     0.0, ATTENUATOR_DRIVE_MAX_MV);
		return;
	}

	if (!companion_search_note_measurement_locked(measurement)) {
		auto_error_locked(measurement != NULL &&
				  measurement->classification == ATTEN_CAL_CLASSIFICATION_ADC_ERROR ?
				  -EIO : -ERANGE);
		return;
	}

	if (companion_search_complete_locked()) {
		if (!cal.search_candidate_valid) {
			auto_error_locked(-ERANGE);
			return;
		}
		auto_schedule_measure_locked(ATTEN_CAL_MEASURE_REFERENCE,
					     0.0, cal.search_candidate_mv);
		return;
	}

	auto_schedule_measure_locked(ATTEN_CAL_MEASURE_INITIAL_PROBE,
				     0.0, search_midpoint_locked());
}

/** Accept the DUT-open reference that normalizes the first sweep segment. */
static void auto_handle_reference_locked(const struct atten_cal_measurement *measurement)
{
	struct atten_cal_record *record = NULL;

	if (measurement == NULL || !measurement->usable) {
		(void)append_record_locked(ATTEN_CAL_EVENT_REFERENCE, measurement, NULL);
		auto_error_locked(-ERANGE);
		return;
	}

	(void)append_record_locked(ATTEN_CAL_EVENT_REFERENCE, measurement, &record);
	if (record == NULL) {
		auto_error_locked(-ENOSPC);
		return;
	}

	cal.sweep_low_mv = 0.0;
	cal.sweep_high_mv = ATTENUATOR_DRIVE_MAX_MV;
	auto_schedule_measure_locked(ATTEN_CAL_MEASURE_SWEEP,
				     sweep_midpoint_locked(), cal.other_mv);
}

/**
 * Handle ordinary DUT sweep measurements and decide whether to continue or bridge.
 *
 * Saturated sweep points are too bright and advance the DUT toward more
 * attenuation. Below-SNR sweep points are too dim and bracket the end of the
 * current usable segment, which is the normal bridge trigger.
 */
static void auto_handle_sweep_locked(const struct atten_cal_measurement *measurement)
{
	struct atten_cal_record *record = NULL;

	if (measurement == NULL) {
		auto_error_locked(-EINVAL);
		return;
	}
	(void)append_record_locked(ATTEN_CAL_EVENT_POINT, measurement, &record);
	if (record == NULL) {
		auto_error_locked(-ENOSPC);
		return;
	}

	if (measurement->usable) {
		cal.sweep_low_mv = cal.sweep_mv;
		if (ATTENUATOR_DRIVE_MAX_MV - cal.sweep_low_mv <= ATTEN_CAL_SEARCH_STEP_MV) {
			auto_finish_physical_locked();
			return;
		}
	} else if (measurement->classification == ATTEN_CAL_CLASSIFICATION_SATURATED) {
		cal.sweep_low_mv = cal.sweep_mv;
		if (ATTENUATOR_DRIVE_MAX_MV - cal.sweep_low_mv <= ATTEN_CAL_SEARCH_STEP_MV) {
			auto_error_locked(-ERANGE);
			return;
		}
	} else if (measurement->classification == ATTEN_CAL_CLASSIFICATION_BELOW_SNR) {
		cal.sweep_high_mv = cal.sweep_mv;
	} else if (measurement->classification == ATTEN_CAL_CLASSIFICATION_ADC_ERROR) {
		auto_error_locked(-EIO);
		return;
	} else {
		auto_error_locked(-ERANGE);
		return;
	}

	if (cal.sweep_high_mv - cal.sweep_low_mv <= ATTEN_CAL_SEARCH_STEP_MV) {
		if (cal.sweep_high_mv >= ATTENUATOR_DRIVE_MAX_MV - ATTEN_CAL_SEARCH_STEP_MV) {
			auto_finish_physical_locked();
		} else {
			auto_begin_bridge_locked();
		}
		return;
	}

	auto_schedule_measure_locked(ATTEN_CAL_MEASURE_SWEEP,
				     sweep_midpoint_locked(), cal.other_mv);
}

/**
 * Record the low-signal side of a bridge and begin opening the companion FVOA.
 *
 * The DUT stays fixed. The current companion setting is a usable but low-signal
 * candidate; subsequent probes search toward lower companion DAC values to find
 * the brightest candidate that is still not saturated.
 */
static void auto_handle_bridge_before_locked(const struct atten_cal_measurement *measurement)
{
	struct atten_cal_record *record = NULL;

	(void)append_record_locked(ATTEN_CAL_EVENT_BRIDGE_BEFORE, measurement, &record);
	if (record == NULL) {
		auto_error_locked(-ENOSPC);
		return;
	}
	if (measurement == NULL || !measurement->usable || !(measurement->signal_mv > 0.0)) {
		int error = -ERANGE;

		if (measurement != NULL &&
		    (measurement->classification == ATTEN_CAL_CLASSIFICATION_SATURATED ||
		     measurement->classification == ATTEN_CAL_CLASSIFICATION_BELOW_SNR)) {
			(void)auto_bridge_with_backoff_locked(
				record_classification_name((uint8_t)measurement->classification));
			return;
		}
		if (measurement != NULL &&
		    measurement->classification == ATTEN_CAL_CLASSIFICATION_ADC_ERROR) {
			error = -EIO;
		}
		auto_error_locked(error);
		return;
	}

	cal.bridge_before_signal_mv = measurement->signal_mv;
	cal.bridge_before_index = cal.point_index;
	cal.bridge_before_index_valid = true;
	companion_search_begin_locked(0.0, cal.other_mv, true);
	auto_schedule_measure_locked(ATTEN_CAL_MEASURE_BRIDGE_PROBE,
				     cal.sweep_low_mv, search_midpoint_locked());
}

/** Continue bridge companion search or schedule the current usable candidate for confirmation. */
static void auto_continue_bridge_search_locked(const char *backoff_context)
{
	if (companion_search_complete_locked()) {
		if (!cal.search_candidate_valid ||
		    cal.search_candidate_mv >= cal.bridge_start_other_mv - ATTEN_CAL_SEARCH_STEP_MV) {
			(void)auto_bridge_with_backoff_locked(backoff_context);
			return;
		}
		auto_schedule_measure_locked(ATTEN_CAL_MEASURE_BRIDGE_AFTER,
					     cal.sweep_low_mv, cal.search_candidate_mv);
		return;
	}

	auto_schedule_measure_locked(ATTEN_CAL_MEASURE_BRIDGE_PROBE,
				     cal.sweep_low_mv, search_midpoint_locked());
}

/** Handle companion-FVOA bridge probes during the lowest-usable-DAC search. */
static void auto_handle_bridge_probe_locked(const struct atten_cal_measurement *measurement)
{
	struct atten_cal_record *record = NULL;

	(void)append_record_locked(ATTEN_CAL_EVENT_BRIDGE_PROBE, measurement, &record);
	if (record == NULL) {
		auto_error_locked(-ENOSPC);
		return;
	}

	if (!companion_search_note_measurement_locked(measurement)) {
		auto_error_locked(measurement != NULL &&
				  measurement->classification == ATTEN_CAL_CLASSIFICATION_ADC_ERROR ?
				  -EIO : -ERANGE);
		return;
	}

	auto_continue_bridge_search_locked("bridge_probe_no_headroom");
}

/**
 * Accept bridge normalization, update scale variance, and resume DUT sweeping.
 *
 * Failed confirmations are retained as bridge probes and folded back into the
 * companion bracket. Only an accepted confirmation is retained as `bridge_after`;
 * it defines the new segment scale and can anchor a later bridge, but it is not
 * a fit candidate because it is correlated with the scale it defines.
 */
static void auto_handle_bridge_after_locked(const struct atten_cal_measurement *measurement)
{
	struct atten_cal_record *record = NULL;
	double ratio;

	if (measurement == NULL || !measurement->usable ||
	    !(cal.bridge_before_signal_mv > 0.0) ||
	    !(measurement->signal_mv > 0.0)) {
		int error = -ERANGE;

		(void)append_record_locked(ATTEN_CAL_EVENT_BRIDGE_PROBE, measurement, &record);
		if (measurement != NULL && record == NULL) {
			auto_error_locked(-ENOSPC);
			return;
		}
		if (measurement != NULL &&
		    (measurement->classification == ATTEN_CAL_CLASSIFICATION_SATURATED ||
		     measurement->classification == ATTEN_CAL_CLASSIFICATION_BELOW_SNR)) {
			companion_search_reject_current_locked(measurement->classification);
			auto_continue_bridge_search_locked(
				record_classification_name((uint8_t)measurement->classification));
			return;
		}
		if (measurement != NULL &&
		    measurement->classification == ATTEN_CAL_CLASSIFICATION_ADC_ERROR) {
			error = -EIO;
		}
		auto_error_locked(error);
		return;
	}

	ratio = measurement->signal_mv / cal.bridge_before_signal_mv;
	if (!(ratio > 1.0) || !isfinite(ratio)) {
		(void)append_record_locked(ATTEN_CAL_EVENT_BRIDGE_PROBE, measurement, &record);
		if (record == NULL) {
			auto_error_locked(-ENOSPC);
			return;
		}
		companion_search_reject_current_locked(ATTEN_CAL_CLASSIFICATION_BELOW_SNR);
		auto_continue_bridge_search_locked("bridge_ratio");
		return;
	}

	cal.segment_id++;

	(void)append_record_locked(ATTEN_CAL_EVENT_BRIDGE_AFTER, measurement, &record);
	if (record == NULL) {
		auto_finish_physical_locked();
		return;
	}
	if (cal.bridge_before_index_valid &&
	    cal.bridge_count[cal.physical_index] < ATTENUATOR_CAL_RECORD_COUNT) {
		struct atten_cal_bridge *bridge =
			&cal.bridges[cal.physical_index][cal.bridge_count[cal.physical_index]++];

		bridge->boundary_index = cal.point_index;
		bridge->before_record_index = cal.bridge_before_index;
		bridge->after_record_index = cal.point_index;
	}
	cal.bridge_before_index = 0U;
	cal.bridge_before_index_valid = false;

	cal.sweep_low_mv = cal.sweep_mv;
	cal.sweep_high_mv = ATTENUATOR_DRIVE_MAX_MV;
	auto_schedule_measure_locked(ATTEN_CAL_MEASURE_SWEEP,
				     sweep_midpoint_locked(), cal.other_mv);
}

/** Dispatch a completed measurement to the handler for the scheduled measure kind. */
static void auto_handle_measurement_locked(const struct atten_cal_measurement *measurement)
{
	switch (cal.measure_kind) {
	case ATTEN_CAL_MEASURE_INITIAL_PROBE:
		auto_handle_initial_probe_locked(measurement);
		break;
	case ATTEN_CAL_MEASURE_REFERENCE:
		auto_handle_reference_locked(measurement);
		break;
	case ATTEN_CAL_MEASURE_SWEEP:
		auto_handle_sweep_locked(measurement);
		break;
	case ATTEN_CAL_MEASURE_BRIDGE_BEFORE:
		auto_handle_bridge_before_locked(measurement);
		break;
	case ATTEN_CAL_MEASURE_BRIDGE_PROBE:
		auto_handle_bridge_probe_locked(measurement);
		break;
	case ATTEN_CAL_MEASURE_BRIDGE_AFTER:
		auto_handle_bridge_after_locked(measurement);
		break;
	case ATTEN_CAL_MEASURE_NONE:
	default:
		auto_error_locked(-EINVAL);
		break;
	}
}

/** Return true when a raw record can become a model fit point. */
static bool record_is_fit_candidate(const struct atten_cal_record *record)
{
	if (record == NULL ||
	    record->classification != ATTEN_CAL_CLASSIFICATION_OK ||
	    !(record->signal_mv > 0.0f) ||
	    !(record->signal_err_mv > 0.0f)) {
		return false;
	}
	return record->event == ATTEN_CAL_EVENT_POINT ||
	       record->event == ATTEN_CAL_EVENT_BRIDGE_BEFORE;
}

/** Locate the open-reference record that defines relative transmission. */
static const struct atten_cal_record *reference_record_locked(uint8_t physical)
{
	if (physical >= ATTENUATOR_PHYSICAL_COUNT) {
		return NULL;
	}
	for (uint8_t i = 0U; i < cal.record_count[physical]; ++i) {
		const struct atten_cal_record *record = &cal.records[physical][i];

		if (record->event == ATTEN_CAL_EVENT_REFERENCE &&
		    record->classification == ATTEN_CAL_CLASSIFICATION_OK &&
		    record->signal_mv > 0.0f &&
		    record->signal_err_mv > 0.0f) {
			return record;
		}
	}
	return NULL;
}

/**
 * Build cumulative bridge scales from accepted before/after bridge records.
 *
 * The acquisition state records only bridge indices. This helper centralizes
 * the ratio and variance propagation so the retained record stays raw and the
 * fit has one authoritative normalization path.
 */
static int build_segment_scales_locked(uint8_t physical,
				       double *segment_scale,
				       double *segment_rel_var)
{
	if (physical >= ATTENUATOR_PHYSICAL_COUNT ||
	    segment_scale == NULL || segment_rel_var == NULL) {
		return -EINVAL;
	}
	for (uint8_t i = 0U; i < ATTENUATOR_CAL_RECORD_COUNT; ++i) {
		segment_scale[i] = NAN;
		segment_rel_var[i] = NAN;
	}
	segment_scale[0] = 1.0;
	segment_rel_var[0] = 0.0;

	for (uint8_t i = 0U; i < cal.bridge_count[physical]; ++i) {
		const struct atten_cal_bridge *bridge = &cal.bridges[physical][i];
		const struct atten_cal_record *before;
		const struct atten_cal_record *after;
		uint8_t next_segment;
		double ratio;
		double ratio_rel_var;

		if (bridge->before_record_index >= cal.record_count[physical] ||
		    bridge->after_record_index >= cal.record_count[physical]) {
			return -ERANGE;
		}
		before = &cal.records[physical][bridge->before_record_index];
		after = &cal.records[physical][bridge->after_record_index];
		next_segment = after->segment;
		if (before->classification != ATTEN_CAL_CLASSIFICATION_OK ||
		    after->classification != ATTEN_CAL_CLASSIFICATION_OK ||
		    before->segment >= ATTENUATOR_CAL_RECORD_COUNT ||
		    next_segment >= ATTENUATOR_CAL_RECORD_COUNT ||
		    !isfinite(segment_scale[before->segment]) ||
		    !(before->signal_mv > 0.0f) ||
		    !(after->signal_mv > 0.0f) ||
		    !(before->signal_err_mv > 0.0f) ||
		    !(after->signal_err_mv > 0.0f)) {
			return -ERANGE;
		}

		ratio = (double)after->signal_mv / (double)before->signal_mv;
		if (!(ratio > 1.0) || !isfinite(ratio)) {
			return -ERANGE;
		}
		ratio_rel_var =
			((double)before->signal_err_mv / (double)before->signal_mv) *
			((double)before->signal_err_mv / (double)before->signal_mv) +
			((double)after->signal_err_mv / (double)after->signal_mv) *
			((double)after->signal_err_mv / (double)after->signal_mv);
		segment_scale[next_segment] = segment_scale[before->segment] * ratio;
		segment_rel_var[next_segment] =
			segment_rel_var[before->segment] + ratio_rel_var;
	}
	return 0;
}

/**
 * Derive dB-space fit points from raw records and the bridge table.
 *
 * Each returned point has a measured attenuation and propagated uncertainty.
 * Bridge-after records are intentionally not fit candidates: they define the
 * next segment scale and are correlated with that scale by construction.
 */
static int build_fit_points_locked(uint8_t physical, double gain,
				   struct atten_cal_fit_point *points,
				   uint8_t *point_count_out)
{
	static double segment_scale[ATTENUATOR_CAL_RECORD_COUNT];
	static double segment_rel_var[ATTENUATOR_CAL_RECORD_COUNT];
	const struct atten_cal_record *reference;
	double open_signal;
	double open_err;
	double open_rel_var;
	uint8_t point_count = 0U;
	int rc;

	if (points == NULL || point_count_out == NULL || !(gain > 0.0)) {
		return -EINVAL;
	}
	*point_count_out = 0U;

	reference = reference_record_locked(physical);
	if (reference == NULL) {
		return -ERANGE;
	}
	open_signal = (double)reference->signal_mv;
	open_err = (double)reference->signal_err_mv;
	open_rel_var = (open_err / open_signal) * (open_err / open_signal);

	rc = build_segment_scales_locked(physical, segment_scale, segment_rel_var);
	if (rc != 0) {
		return rc;
	}

	for (uint8_t i = 0U; i < cal.record_count[physical]; ++i) {
		const struct atten_cal_record *record = &cal.records[physical][i];
		double scale;
		double signal;
		double signal_err;
		double tx;
		double rel_var;
		double db;
		double db_err;

		if (!record_is_fit_candidate(record) ||
		    record->segment >= ATTENUATOR_CAL_RECORD_COUNT ||
		    !isfinite(segment_scale[record->segment])) {
			continue;
		}

		scale = segment_scale[record->segment];
		signal = (double)record->signal_mv;
		signal_err = (double)record->signal_err_mv;
		tx = signal / (open_signal * scale);
		if (!point_valid_for_fit(tx)) {
			continue;
		}
		rel_var = (signal_err / signal) * (signal_err / signal) +
			  segment_rel_var[record->segment] + open_rel_var;
		db = -10.0 * log10(tx);
		db_err = (10.0 / log(10.0)) * sqrt(MAX(rel_var, 0.0));
		if (!isfinite(db) || !(db_err > 0.0) || !isfinite(db_err)) {
			continue;
		}

		points[point_count].record_index = i;
		points[point_count].dac_mv = (double)record->sweep_mv;
		points[point_count].measured_db = db;
		points[point_count].measured_db_err = MAX(db_err, ATTEN_CAL_MIN_DB_ERR);
		points[point_count].tx = tx;
		point_count++;
	}

	if (point_count < ATTEN_CAL_MIN_FIT_POINTS) {
		return -ERANGE;
	}
	*point_count_out = point_count;
	return 0;
}

/** Evaluate the physical attenuator model in relative-attenuation dB. */
static double fit_model_db(double fvoa_50pct_mv, double slope_inv_fvoa_mv,
			   double gain, double dac_mv)
{
	const struct attenuator_model_coeffs coeffs = {
		.fvoa_50pct_mv = fvoa_50pct_mv,
		.slope_inv_fvoa_mv = slope_inv_fvoa_mv,
		.gain = gain,
	};

	return attenuator_model_voltage_to_db(&coeffs, dac_mv);
}

/** Estimate model dB sensitivity to DAC voltage for x-error propagation. */
static double fit_model_d_db_d_dac(double fvoa_50pct_mv, double slope_inv_fvoa_mv,
				   double gain, double dac_mv)
{
	double step = MAX(ATTEN_CAL_DAC_SIGMA_MV, 0.5);
	double lo = MAX(0.0, dac_mv - step);
	double hi = MIN(ATTENUATOR_DRIVE_MAX_MV, dac_mv + step);
	double db_lo;
	double db_hi;

	if (!(hi > lo)) {
		return 0.0;
	}
	db_lo = fit_model_db(fvoa_50pct_mv, slope_inv_fvoa_mv, gain, lo);
	db_hi = fit_model_db(fvoa_50pct_mv, slope_inv_fvoa_mv, gain, hi);
	if (!isfinite(db_lo) || !isfinite(db_hi)) {
		return 0.0;
	}
	return (db_hi - db_lo) / (hi - lo);
}

/** Return one weighted dB residual for the current fit parameters. */
static double fit_point_residual(const struct atten_cal_fit_point *point,
				 double fvoa_50pct_mv,
				 double slope_inv_fvoa_mv,
				 double gain)
{
	double model_db;
	double d_db_d_dac;
	double sigma_db;

	if (point == NULL) {
		return NAN;
	}
	model_db = fit_model_db(fvoa_50pct_mv, slope_inv_fvoa_mv,
				gain, point->dac_mv);
	d_db_d_dac = fit_model_d_db_d_dac(fvoa_50pct_mv, slope_inv_fvoa_mv,
					  gain, point->dac_mv);
	sigma_db = sqrt(point->measured_db_err * point->measured_db_err +
			(d_db_d_dac * ATTEN_CAL_DAC_SIGMA_MV) *
			(d_db_d_dac * ATTEN_CAL_DAC_SIGMA_MV));
	if (!isfinite(model_db) || !(sigma_db > 0.0) || !isfinite(sigma_db)) {
		return NAN;
	}
	return (model_db - point->measured_db) / MAX(sigma_db, ATTEN_CAL_MIN_DB_ERR);
}

/** Sum weighted squared residuals for a complete fit candidate. */
static double fit_cost(const struct atten_cal_fit_point *points, uint8_t point_count,
		       double fvoa_50pct_mv, double slope_inv_fvoa_mv,
		       double gain)
{
	double cost = 0.0;

	for (uint8_t i = 0U; i < point_count; ++i) {
		double residual = fit_point_residual(&points[i], fvoa_50pct_mv,
						     slope_inv_fvoa_mv, gain);

		if (!isfinite(residual)) {
			return INFINITY;
		}
		cost += residual * residual;
	}
	return cost;
}

/** Choose a data-derived initial coefficient guess for the dB-space optimizer. */
static void fit_initial_guess(const struct atten_cal_fit_point *points,
			      uint8_t point_count, double gain,
			      double *fvoa_50pct_mv,
			      double *slope_inv_fvoa_mv)
{
	double max_fvoa_mv = ATTENUATOR_DRIVE_MAX_MV * gain;
	double min_x = INFINITY;
	double max_x = 0.0;
	double closest_err = INFINITY;
	double f50 = 0.5 * max_fvoa_mv;
	double slope;

	for (uint8_t i = 0U; i < point_count; ++i) {
		double x = points[i].dac_mv * gain;
		double err = fabs(points[i].measured_db - 3.01029995664);

		min_x = MIN(min_x, x);
		max_x = MAX(max_x, x);
		if (err < closest_err) {
			closest_err = err;
			f50 = x;
		}
	}

	if (isfinite(min_x) && max_x > min_x) {
		slope = 8.0 / (max_x - min_x);
	} else {
		slope = 8.0 / max_fvoa_mv;
	}
	*fvoa_50pct_mv = CLAMP(f50, 1.0, 2.0 * max_fvoa_mv);
	*slope_inv_fvoa_mv = CLAMP(slope, ATTEN_CAL_FIT_MIN_SLOPE,
				    ATTEN_CAL_FIT_MAX_SLOPE);
}

/**
 * Optimize two model coefficients with damped finite-difference Gauss-Newton.
 *
 * The objective is measured/model attenuation residual in dB divided by the
 * propagated dB uncertainty. The implementation is intentionally local and
 * deterministic so the notebook can reproduce it without SciPy.
 */
static int fit_optimize_db(const struct atten_cal_fit_point *points,
			   uint8_t point_count, double gain,
			   double *fvoa_50pct_mv,
			   double *slope_inv_fvoa_mv)
{
	double max_fvoa_mv = ATTENUATOR_DRIVE_MAX_MV * gain;
	double f50;
	double slope;
	double lambda = ATTEN_CAL_FIT_INITIAL_LAMBDA;
	double cost;

	if (points == NULL || fvoa_50pct_mv == NULL ||
	    slope_inv_fvoa_mv == NULL || point_count < ATTEN_CAL_MIN_FIT_POINTS) {
		return -EINVAL;
	}
	fit_initial_guess(points, point_count, gain, &f50, &slope);
	cost = fit_cost(points, point_count, f50, slope, gain);
	if (!isfinite(cost)) {
		return -ERANGE;
	}

	for (uint8_t iter = 0U; iter < ATTEN_CAL_FIT_MAX_ITER; ++iter) {
		double step_f50 = MAX(fabs(f50) * 1.0e-5, 0.1);
		double step_slope = MAX(fabs(slope) * 1.0e-5, 1.0e-8);
		double h00 = 0.0;
		double h01 = 0.0;
		double h11 = 0.0;
		double g0 = 0.0;
		double g1 = 0.0;
		double det;
		double d_f50;
		double d_slope;
		double trial_f50;
		double trial_slope;
		double trial_cost;

		for (uint8_t i = 0U; i < point_count; ++i) {
			double r = fit_point_residual(&points[i], f50, slope, gain);
			double r_f50 = fit_point_residual(&points[i],
							  f50 + step_f50,
							  slope, gain);
			double r_slope = fit_point_residual(&points[i], f50,
							    slope + step_slope,
							    gain);
			double j0;
			double j1;

			if (!isfinite(r) || !isfinite(r_f50) || !isfinite(r_slope)) {
				return -ERANGE;
			}
			j0 = (r_f50 - r) / step_f50;
			j1 = (r_slope - r) / step_slope;
			h00 += j0 * j0;
			h01 += j0 * j1;
			h11 += j1 * j1;
			g0 += j0 * r;
			g1 += j1 * r;
		}

		h00 += lambda;
		h11 += lambda;
		det = h00 * h11 - h01 * h01;
		if (!(det > 0.0) || !isfinite(det)) {
			return -ERANGE;
		}
		d_f50 = (-h11 * g0 + h01 * g1) / det;
		d_slope = (h01 * g0 - h00 * g1) / det;
		trial_f50 = CLAMP(f50 + d_f50, 1.0, 2.0 * max_fvoa_mv);
		trial_slope = CLAMP(slope + d_slope,
				    ATTEN_CAL_FIT_MIN_SLOPE,
				    ATTEN_CAL_FIT_MAX_SLOPE);
		trial_cost = fit_cost(points, point_count, trial_f50,
				      trial_slope, gain);
		if (isfinite(trial_cost) && trial_cost < cost) {
			if (fabs(trial_f50 - f50) < 1.0e-6 &&
			    fabs(trial_slope - slope) < 1.0e-12) {
				f50 = trial_f50;
				slope = trial_slope;
				cost = trial_cost;
				break;
			}
			f50 = trial_f50;
			slope = trial_slope;
			cost = trial_cost;
			lambda = MAX(lambda * 0.3, 1.0e-12);
		} else {
			lambda = MIN(lambda * 10.0, 1.0e12);
		}
	}

	*fvoa_50pct_mv = f50;
	*slope_inv_fvoa_mv = slope;
	return 0;
}

/** Fit one physical FVOA's retained records to the firmware attenuator model. */
static int fit_one_physical_locked(uint8_t physical,
				   struct attenuator_calibration_fit_metrics *out)
{
	struct attenuator *atten = &attenuators[cal.attenuator_index];
	double gain = physical == 0U ? atten->coeff1.gain : atten->coeff2.gain;
	double fvoa_50pct_mv = 0.0;
	double slope_inv_fvoa_mv = 0.0;
	double min_tx = 1.0;
	double max_tx = 0.0;
	double min_x = ATTENUATOR_DRIVE_MAX_MV * gain;
	double max_x = 0.0;
	double sum_sq_db = 0.0;
	double max_abs_db = 0.0;
	double sum_model = 0.0;
	double sum_measured = 0.0;
	double sum_model_model = 0.0;
	double sum_measured_measured = 0.0;
	double sum_model_measured = 0.0;
	uint8_t point_count = 0U;
	int rc;

	if (out == NULL || physical >= ATTENUATOR_PHYSICAL_COUNT || !(gain > 0.0)) {
		return -EINVAL;
	}
	memset(out, 0, sizeof(*out));

	rc = build_fit_points_locked(physical, gain, cal_fit_points, &point_count);
	if (rc != 0) {
		return rc;
	}
	rc = fit_optimize_db(cal_fit_points, point_count, gain,
			     &fvoa_50pct_mv, &slope_inv_fvoa_mv);
	if (rc != 0) {
		return rc;
	}

	for (uint8_t i = 0U; i < point_count; ++i) {
		double model_db = fit_model_db(fvoa_50pct_mv, slope_inv_fvoa_mv,
					       gain, cal_fit_points[i].dac_mv);
		double residual_db = model_db - cal_fit_points[i].measured_db;
		double x = cal_fit_points[i].dac_mv * gain;

		if (!isfinite(model_db) || !isfinite(residual_db)) {
			return -ERANGE;
		}
		sum_sq_db += residual_db * residual_db;
		max_abs_db = MAX(max_abs_db, fabs(residual_db));
		min_tx = MIN(min_tx, cal_fit_points[i].tx);
		max_tx = MAX(max_tx, cal_fit_points[i].tx);
		min_x = MIN(min_x, x);
		max_x = MAX(max_x, x);
		sum_model += model_db;
		sum_measured += cal_fit_points[i].measured_db;
		sum_model_model += model_db * model_db;
		sum_measured_measured += cal_fit_points[i].measured_db *
					 cal_fit_points[i].measured_db;
		sum_model_measured += model_db * cal_fit_points[i].measured_db;
	}

	if (point_count < ATTEN_CAL_MIN_FIT_POINTS || !(max_x > min_x)) {
		return -ERANGE;
	}
	{
		double denom_model = (double)point_count * sum_model_model -
				     sum_model * sum_model;
		double denom_measured = (double)point_count * sum_measured_measured -
					sum_measured * sum_measured;

		if (!(denom_model > 0.0) || !(denom_measured > 0.0)) {
			return -ERANGE;
		}
		out->correlation = ((double)point_count * sum_model_measured -
				    sum_model * sum_measured) /
				   sqrt(denom_model * denom_measured);
	}

	out->valid = true;
	out->points = point_count;
	out->fvoa_50pct_mv = fvoa_50pct_mv;
	out->slope_inv_fvoa_mv = slope_inv_fvoa_mv;
	out->rms_db = sqrt(sum_sq_db / (double)point_count);
	out->max_abs_db = max_abs_db;
	out->min_tx = min_tx;
	out->max_tx = max_tx;
	out->fvoa_span_mv = max_x - min_x;
	out->accepted = isfinite(out->correlation) &&
			out->correlation >= ATTEN_CAL_MIN_FIT_CORR &&
			isfinite(out->fvoa_50pct_mv) &&
			out->fvoa_50pct_mv > 0.0 &&
			out->slope_inv_fvoa_mv > 0.0;
	return out->accepted ? 0 : -ERANGE;
}

/** Apply accepted pair fits to runtime control and optionally persist them. */
static int apply_fit_to_settings_locked(void)
{
	struct attenuator *atten = &attenuators[cal.attenuator_index];
	struct app_attenuator_channel_settings stored = {0};
	struct attenuator_model_coeffs physical[ATTENUATOR_PHYSICAL_COUNT] = {
		{
			.fvoa_50pct_mv = cal.fit[0].fvoa_50pct_mv,
			.slope_inv_fvoa_mv = cal.fit[0].slope_inv_fvoa_mv,
			.gain = atten->coeff1.gain,
		},
		{
			.fvoa_50pct_mv = cal.fit[1].fvoa_50pct_mv,
			.slope_inv_fvoa_mv = cal.fit[1].slope_inv_fvoa_mv,
			.gain = atten->coeff2.gain,
		},
	};

	if (!cal.fit[0].accepted || !cal.fit[1].accepted ||
	    !attenuator_model_coefficients_valid(physical)) {
		return -EINVAL;
	}
	if (attenuator_apply_coefficients_preserve_db(atten, physical) != 0) {
		return -EIO;
	}

	stored.physical[0].fvoa_50pct_mv = physical[0].fvoa_50pct_mv;
	stored.physical[0].slope_inv_fvoa_mv = physical[0].slope_inv_fvoa_mv;
	stored.physical[0].gain = physical[0].gain;
	stored.physical[1].fvoa_50pct_mv = physical[1].fvoa_50pct_mv;
	stored.physical[1].slope_inv_fvoa_mv = physical[1].slope_inv_fvoa_mv;
	stored.physical[1].gain = physical[1].gain;
	app_settings_update_attenuator_channel(cal.attenuator_index, &stored, cal.persistent);
	return 0;
}

/** Fit both physical FVOAs and move calibration to complete or error state. */
static void auto_fit_locked(void)
{
	int first_error = 0;
	bool all_accepted = true;

	for (uint8_t physical = 0U; physical < ATTENUATOR_PHYSICAL_COUNT; ++physical) {
		int rc = fit_one_physical_locked(physical, &cal.fit[physical]);

		atten_cal_emit_fit(physical, &cal.fit[physical]);
		if (rc != 0 && first_error == 0) {
			first_error = rc;
		}
		all_accepted = all_accepted && cal.fit[physical].accepted;
	}

	if (!all_accepted) {
		cal.last_error = first_error == 0 ? -ERANGE : first_error;
		cal.state = ATTEN_CAL_STATE_COMPLETE;
		cal.phase = ATTEN_CAL_PHASE_NONE;
		atten_cal_emit_simple("complete");
		return;
	}

	{
		int rc = apply_fit_to_settings_locked();

		if (rc != 0) {
			cal.last_error = rc;
			cal.state = ATTEN_CAL_STATE_ERROR;
			cal.phase = ATTEN_CAL_PHASE_NONE;
			atten_cal_emit_simple("error");
			return;
		}
	}

	cal.last_error = 0;
	cal.state = ATTEN_CAL_STATE_COMPLETE;
	cal.phase = ATTEN_CAL_PHASE_NONE;
	atten_cal_emit_simple("complete");
}

/** Advance automatic calibration after a scheduled photodiode window dwell. */
static void auto_tick_locked(const struct photodiode_status *pd_status, int64_t now_ms)
{
	const struct photodiode_window_result *window;
	struct atten_cal_measurement measurement = {0};

	if (cal.state != ATTEN_CAL_STATE_RUNNING ||
	    cal.mode != ATTEN_CAL_MODE_TIB_AUTO) {
		return;
	}

	switch (cal.phase) {
	case ATTEN_CAL_PHASE_WAIT_WINDOW:
		if (now_ms < cal.wait_until_ms) {
			return;
		}
		if (pd_status == NULL) {
			auto_error_locked(-EINVAL);
			return;
		}
		window = &pd_status->channel[cal.channel].configurable_window;
		measurement_from_window(window, &measurement);
		auto_handle_measurement_locked(&measurement);
		break;
	case ATTEN_CAL_PHASE_NONE:
	default:
			return;
	}
}

/** Start automatic TIB calibration after command parsing has built a request. */
int attenuator_calibration_start_auto(
	const struct attenuator_calibration_auto_request *request,
	struct attenuator_calibration_status *status)
{
	bool replacing;
	int rc;

	if (request == NULL || request->route_input == NULL ||
	    request->output == NULL || request->pd_input == NULL ||
	    request->pd_output == NULL ||
	    request->output[0] == '\0' ||
	    request->channel < 0 || request->channel >= PHOTODIODE_CHANNEL_COUNT ||
	    !devices_attenuator_channel_available(request->attenuator_index)) {
		return -EINVAL;
	}

	k_mutex_lock(&cal_lock, K_FOREVER);
	replacing = cal.state == ATTEN_CAL_STATE_RUNNING;
	k_mutex_unlock(&cal_lock);
	if (replacing) {
		coo_cmd_runtime_emit(command_runtime_get(),
				     &(const struct coo_cmd_runtime_emit_args){
					     .type = COO_CMD_RUNTIME_EMIT_WARNING,
					     .delivery = COO_CMD_RUNTIME_EMIT_BEST_EFFORT,
					     .code = "atten_calibration_restart",
					     .msg = "restarting attenuator calibration",
				     });
	}
	if (throughput_monitor_any_active()) {
		coo_cmd_runtime_emit(command_runtime_get(),
				     &(const struct coo_cmd_runtime_emit_args){
					     .type = COO_CMD_RUNTIME_EMIT_WARNING,
					     .delivery = COO_CMD_RUNTIME_EMIT_BEST_EFFORT,
					     .code = "throughput_stopped",
					     .msg = "stopping throughput for attenuator calibration",
				     });
	}
	(void)throughput_monitor_stop(PHOTODIODE_CHANNEL_COUNT, NULL);

	rc = mems_router_apply_named_route(&router, request->route_input, request->output,
					   false, NULL, NULL);
	if (rc == 0) {
		rc = mems_router_apply_named_route(&router, request->pd_input, request->pd_output,
						   false, NULL, NULL);
	}
	if (rc != 0) {
		return rc;
	}
	rc = photodiode_set_configurable_window_duration(request->channel,
							 clamp_dwell(request->dwell_ms));
	if (rc != 0) {
		return rc;
	}
	if (!set_physical_pair(request->attenuator_index, 0U,
			       ATTENUATOR_DRIVE_MAX_MV, ATTENUATOR_DRIVE_MAX_MV)) {
		return -EIO;
	}
	rc = hispec_laser_stop_output(request->laser, false);
	if (rc != 0) {
		return rc;
	}

	k_mutex_lock(&cal_lock, K_FOREVER);
	reset_locked(ATTEN_CAL_STATE_RUNNING);
	cal.mode = ATTEN_CAL_MODE_TIB_AUTO;
	cal.phase = ATTEN_CAL_PHASE_NONE;
	cal.attenuator_index = request->attenuator_index;
	cal.physical_index = 0U;
	cal.dwell_ms = clamp_dwell(request->dwell_ms);
	cal.persistent = request->persist;
	cal.laser = request->laser;
	cal.channel = request->channel;
	cal.laser_percent = 0.0;
	atten_cal_emit_simple("start");
	auto_start_next_physical_locked();
	copy_status_locked(status);
	k_mutex_unlock(&cal_lock);
	return 0;
}

/** Stop calibration, discard active sequencing state, and return inactive status. */
int attenuator_calibration_stop(struct attenuator_calibration_status *status)
{
	k_mutex_lock(&cal_lock, K_FOREVER);
	if (cal.state != ATTEN_CAL_STATE_INACTIVE) {
		atten_cal_emit_simple("stop");
	}
	reset_locked(ATTEN_CAL_STATE_INACTIVE);
	copy_status_locked(status);
	k_mutex_unlock(&cal_lock);
	return 0;
}

/** Return a mutex-protected snapshot of current calibration status. */
void attenuator_calibration_get_status(struct attenuator_calibration_status *status)
{
	k_mutex_lock(&cal_lock, K_FOREVER);
	copy_status_locked(status);
	k_mutex_unlock(&cal_lock);
}

/** Report whether calibration currently owns attenuator sequencing. */
bool attenuator_calibration_active(void)
{
	bool active;

	k_mutex_lock(&cal_lock, K_FOREVER);
	active = cal.state == ATTEN_CAL_STATE_RUNNING;
	k_mutex_unlock(&cal_lock);
	return active;
}

/** Append one fit-metrics object to the compact JSON status payload. */
static int append_fit_json(char *payload, size_t payload_len, size_t *off,
			   const char *name,
			   const struct attenuator_calibration_fit_metrics *fit)
{
	if (coo_json_append(payload, payload_len, off, ",\"%s\":{", name) != 0) {
		return -ENOSPC;
	}
	if (fit == NULL || !fit->valid) {
		return coo_json_append(payload, payload_len, off, "\"valid\":false}");
	}
	return coo_json_append(payload, payload_len, off,
		"\"valid\":true,\"accepted\":%s,\"points\":%u,"
		"\"fvoa_50pct_mv\":%.12g,\"slope_inv_fvoa_mv\":%.12g,"
		"\"corr\":%.12g,\"rms_db\":%.12g,\"max_abs_db\":%.12g,"
		"\"min_tx\":%.12g,\"max_tx\":%.12g,\"fvoa_span_mv\":%.6f}",
		fit->accepted ? "true" : "false", fit->points,
		fit->fvoa_50pct_mv, fit->slope_inv_fvoa_mv,
		fit->correlation, fit->rms_db, fit->max_abs_db,
		fit->min_tx, fit->max_tx, fit->fvoa_span_mv);
}

/** Format the compact command response for current calibration status. */
int attenuator_calibration_format_status(
	char *payload, size_t payload_len,
	const struct attenuator_calibration_status *status)
{
	size_t off = 0U;

	if (payload == NULL || status == NULL) {
		return -EINVAL;
	}

	if (coo_json_append(payload, payload_len, &off,
		"{\"state\":\"%s\",\"mode\":\"%s\",\"physical\":\"%s\","
		"\"fit\":\"%s\",\"n\":%u,\"t_ms\":%u,"
		"\"complete_pct\":%u,\"point\":\"%u/%u\","
		"\"mv\":%.6f,\"other_mv\":%.6f,\"error\":%d",
		status->state != NULL ? status->state : "inactive",
		status->mode != NULL ? status->mode : "none",
		status->physical != NULL ? status->physical : "dac1",
		status->fit != NULL ? status->fit : "none",
		status->point_count, status->dwell_ms, status->complete_pct,
		MIN(status->point_index + 1U, status->point_count),
		status->point_count,
		status->current_mv, status->other_mv, status->last_error) != 0 ||
	    append_fit_json(payload, payload_len, &off, "dac1",
			    &status->fit_metrics[0]) != 0 ||
	    append_fit_json(payload, payload_len, &off, "dac2",
			    &status->fit_metrics[1]) != 0 ||
	    coo_json_append(payload, payload_len, &off, "}") != 0) {
		return -ENOSPC;
	}
	return 0;
}

/** Store one IEEE-754 float in the calibration record wire format. */
static void put_le_float(uint8_t *dst, float value)
{
	uint32_t bits;

	memcpy(&bits, &value, sizeof(bits));
	sys_put_le32(bits, dst);
}

/** Serialize one raw retained record without exposing C struct padding. */
static void write_record_wire(uint8_t *dst, const struct atten_cal_record *record)
{
	if (dst == NULL || record == NULL) {
		return;
	}
	put_le_float(dst + 0U, record->sweep_mv);
	put_le_float(dst + 4U, record->other_mv);
	put_le_float(dst + 8U, record->laser_pct);
	put_le_float(dst + 12U, record->signal_mv);
	put_le_float(dst + 16U, record->signal_err_mv);
	put_le_float(dst + 20U, record->max_mv);
	dst[24] = record->event;
	dst[25] = record->classification;
	dst[26] = record->segment;
}

/** Copy retained calibration records into the host binary chunk format. */
int attenuator_calibration_write_data_chunk(void *payload,
					    size_t payload_len,
					    uint8_t physical_index,
					    uint8_t start_index,
					    size_t *written)
{
	uint8_t *bytes = payload;
	uint8_t count;
	uint8_t total;
	uint8_t next;
	size_t byte_count;

	if (payload == NULL || written == NULL ||
	    payload_len < ATTEN_CAL_DATA_CHUNK_HEADER_SIZE ||
	    physical_index >= ATTENUATOR_PHYSICAL_COUNT ||
	    start_index >= ATTENUATOR_CAL_RECORD_COUNT) {
		return -EINVAL;
	}
	*written = 0U;

	k_mutex_lock(&cal_lock, K_FOREVER);
	total = cal.record_count[physical_index];
	count = start_index < total ?
		MIN((uint8_t)ATTEN_CAL_DATA_CHUNK_RECORDS,
		    (uint8_t)(total - start_index)) : 0U;
	next = (start_index + count < total) ? (uint8_t)(start_index + count) : UINT8_MAX;
	byte_count = ATTEN_CAL_DATA_CHUNK_HEADER_SIZE +
		     ((size_t)count * ATTEN_CAL_DATA_CHUNK_RECORD_SIZE);
	if (byte_count > payload_len) {
		k_mutex_unlock(&cal_lock);
		return -ENOSPC;
	}

	memset(bytes, 0, byte_count);
	bytes[0] = ATTEN_CAL_DATA_CHUNK_MAGIC0;
	bytes[1] = ATTEN_CAL_DATA_CHUNK_MAGIC1;
	bytes[2] = ATTEN_CAL_DATA_CHUNK_MAGIC2;
	bytes[3] = ATTEN_CAL_DATA_CHUNK_MAGIC3;
	bytes[4] = ATTEN_CAL_DATA_CHUNK_VERSION;
	bytes[5] = physical_index;
	bytes[6] = start_index;
	bytes[7] = count;
	bytes[8] = total;
	bytes[9] = next;
	bytes[10] = (uint8_t)cal.state;
	bytes[11] = (uint8_t)cal.mode;
	bytes[12] = cal.fit[physical_index].valid ? 1U : 0U;
	bytes[13] = cal.fit[physical_index].accepted ? 1U : 0U;
	bytes[14] = cal.record_overflow[physical_index] ? 1U : 0U;
	bytes[15] = ATTEN_CAL_DATA_CHUNK_RECORD_SIZE;
	for (uint8_t i = 0U; i < count; ++i) {
		write_record_wire(bytes + ATTEN_CAL_DATA_CHUNK_HEADER_SIZE +
				  ((size_t)i * ATTEN_CAL_DATA_CHUNK_RECORD_SIZE),
				  &cal.records[physical_index][start_index + i]);
	}
	*written = byte_count;
	k_mutex_unlock(&cal_lock);
	return 0;
}

/** Public tick hook called by the throughput monitor thread. */
void attenuator_calibration_tick(const struct photodiode_status *pd_status,
				 int64_t now_ms)
{
	k_mutex_lock(&cal_lock, K_FOREVER);
	auto_tick_locked(pd_status, now_ms);
	k_mutex_unlock(&cal_lock);
}
