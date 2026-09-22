/**
 * @file attenuator_calibration.c
 * @brief Bridge-normalized automatic FVOA attenuator calibration.
 *
 * This is a board servicing routine driven by the throughput monitor thread.
 * It uses the configured photodiode dark value indirectly (it never measures its own)
 * through use of net pd signal and retains raw measurement even when the final fit is not accepted. The routine
 * deliberately does not use a datasheet voltage schedule: it finds usable
 * companion settings by binary search, then sweeps the DUT FVOA linearly until
 * the photodiode signal reaches the dim edge of the current segment.
 *
 * The controlling model is a photodiode usable band, not a single "good/bad"
 * threshold. Saturated samples are too bright, below-SNR samples are too dim,
 * and usable samples are retained for fitting if their normalized transmission
 * is inside the attenuator model domain. During a DUT sweep, increasing the DUT
 * DAC increases attenuation and will move the photodiode signal downward (modulo noise or catastrophic failure).
 * Therefore a saturated sweep point is diagnostic evidence to keep sweeping
 * toward more DUT attenuation; it is not a bridge trigger. A below-SNR sweep
 * point marks the dim edge of the current segment and starts bridge
 * normalization from the latest retained usable DUT anchor.
 *
 * Bridge normalization holds the DUT fixed and opens the companion FVOA to raise
 * the photodiode signal back near the bright side of the usable band.
 * DAC direction is the inverse of signal: lower DAC raises signal,
 * higher DAC attenuates more. The companion search maintains a
 * too-bright low-DAC side and a more-attenuated high-DAC side, while separately
 * remembering the lowest usable companion DAC candidate below the 1850 mV
 * raw-window peak target. This leaves headroom below the 2000 mV usable limit;
 * search headroom does not change retained measurement classifications.
 *
 * Each accepted bridge contributes an after/before photodiode ratio to the
 * cumulative segment scale. Retained records store only measured acquisition
 * facts: DAC positions, laser level, net photodiode signal, signal error,
 * maximum window value, event, classification, and segment. Bridge scale,
 * scaled signal, relative transmission, dB attenuation, fit inclusion, and
 * residuals are derived in one pass after acquisition to keep memory low and math centralized.
 */

//TODO go through entire file and see which saturated measuerments are retained.

#include "attenuator_calibration.h"

#include <errno.h>
#include <math.h>
#include <string.h>

#include <coo_commons/json_utils.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/sys/util.h>

#include "app_settings.h"
#include "attenuator.h"
#include "command.h"
#include "devices.h"
#include "housekeeping.h"
#include "mems_switching.h"
#include "throughput_monitor.h"

LOG_MODULE_REGISTER(attenuator_calibration, LOG_LEVEL_INF);

#define ATTEN_CAL_DEFAULT_DWELL_MS 400U
#define ATTEN_CAL_MIN_DWELL_MS 100U
#define ATTEN_CAL_MAX_DWELL_MS 2000U
#define ATTEN_CAL_SETTLE_MS 100U
/* Peak target for reference/bridge searches, below the PD's 2000 mV usable limit. */
#define ATTEN_CAL_SEARCH_MAX_MV 1850.0f
/* Minimum bracket width for companion-FVOA binary searches. */
#define ATTEN_CAL_SEARCH_MIN_STEP_MV 5.0f
/* Fixed DUT-FVOA sweep spacing after the initial open-reference point. */
#define ATTEN_CAL_SWEEP_STEP_MV 50.0f
#define ATTEN_CAL_MAX_SEARCH_TRIES 16U
#define ATTEN_CAL_MIN_FIT_POINTS ATTENUATOR_CAL_MIN_FIT_POINTS
#define ATTEN_CAL_MIN_TX 1.0e-10
#define ATTEN_CAL_MIN_FIT_CORR 0.85
#define ATTEN_CAL_MIN_DB_ERR 1.0e-6
#define ATTENUATOR_FIT_MIN_SIGMA_DB 0.5
#define ATTEN_CAL_TAIL_FLOOR_POINTS 3U
#define ATTEN_CAL_SNR_USABLE 5.0f
#define ATTEN_CAL_DAC_SIGMA_MV 3.0f
#define ATTEN_CAL_FIT_MAX_ITER 30U
#define ATTEN_CAL_FIT_INITIAL_LAMBDA 1.0e-3
#define ATTEN_CAL_FIT_MIN_SLOPE 1.0e-12
#define ATTEN_CAL_FIT_MAX_SLOPE 1.0
#define ATTEN_CAL_CORRECTION_PIVOT_EPS 1.0e-12
#define ATTEN_CAL_CORRECTION_MONOTONIC_EPS_DB 1.0e-4
#define ATTEN_CAL_TELEMETRY_TOPIC_SUFFIX "atten"
#define ATTEN_CAL_DATA_CHUNK_RECORD_SIZE 27U
#define ATTEN_CAL_DATA_METADATA_HEADER_SIZE 19U
#define ATTEN_CAL_DATA_BRIDGE_ENTRY_SIZE 2U
#define ATTEN_CAL_DATA_RECORDS_PER_CHUNK \
	(COO_CMD_PAYLOAD_MAX / ATTEN_CAL_DATA_CHUNK_RECORD_SIZE)
#define ATTEN_CAL_DATA_CHUNK_MAGIC0 'H'
#define ATTEN_CAL_DATA_CHUNK_MAGIC1 'A'
#define ATTEN_CAL_DATA_CHUNK_MAGIC2 'C'
#define ATTEN_CAL_DATA_CHUNK_MAGIC3 '4'
#define ATTEN_CAL_DATA_CHUNK_VERSION 3U
#define ATTEN_CAL_DATA_KIND_METADATA 0U

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
	ATTEN_CAL_PHASE_FITTING,
};

enum atten_cal_measure_kind {
	ATTEN_CAL_MEASURE_INITIAL_PROBE=0,
	ATTEN_CAL_MEASURE_SWEEP,
	ATTEN_CAL_MEASURE_BRIDGE_PROBE,
};

enum atten_cal_record_event {
	ATTEN_CAL_EVENT_POINT = 0,
	ATTEN_CAL_EVENT_INITIAL_PROBE,
	ATTEN_CAL_EVENT_BRIDGE_PROBE,
};

enum atten_cal_record_classification {
	ATTEN_CAL_CLASSIFICATION_OK = 0,
	ATTEN_CAL_CLASSIFICATION_SATURATED,
	ATTEN_CAL_CLASSIFICATION_BELOW_SNR,
	ATTEN_CAL_CLASSIFICATION_ADC_ERROR,
};

struct atten_cal_measurement {
	float signal_mv;
	float signal_err_mv;
	float snr;
	float max_mv;
	enum atten_cal_record_classification classification;
};

struct atten_cal_record {
	float sweep_mv;
	float other_mv;
	uint8_t laser_pct;
	float signal_mv;
	float signal_err_mv;
	float max_mv;
	uint8_t event;
	uint8_t classification;
	uint8_t segment;
};

BUILD_ASSERT(ATTEN_CAL_DATA_RECORDS_PER_CHUNK > 0U,
	     "calibration data chunk must carry at least one record");
BUILD_ASSERT(ATTEN_CAL_DATA_RECORDS_PER_CHUNK <= UINT8_MAX,
	     "calibration data records per chunk must fit in wire metadata");

struct atten_cal_bridge {
	uint8_t before_record_index;
	uint8_t after_record_index;
};

struct atten_cal_fit_point {
	uint8_t record_index;
	float measured_db;
	float measured_db_err;
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
	uint8_t laser_percent;
	/* Current commanded DAC pair for the active measurement. */
	float sweep_mv;
	float other_mv;
	/* Companion search bracket and latest usable candidate. */
	float search_low_mv;
	float search_high_mv;
	float search_candidate_mv;
	bool search_candidate_valid;
	uint8_t search_candidate_record_index;
	uint8_t search_tries;
	/* Held-DUT bridge measurement selected from retained point records. */
	uint8_t bridge_before_index;
	bool bridge_before_index_valid;
	/* Others */
	int64_t window_started_ms;
	int last_error;
	bool shutdown_pending; /* Keep laser identity after a failed fault shutdown. */
	uint8_t reference_record_index[ATTENUATOR_PHYSICAL_COUNT];
	bool reference_record_index_valid[ATTENUATOR_PHYSICAL_COUNT];
	struct atten_cal_record records[ATTENUATOR_PHYSICAL_COUNT][ATTENUATOR_CAL_RECORD_COUNT];
	struct atten_cal_bridge bridges[ATTENUATOR_PHYSICAL_COUNT][ATTENUATOR_CAL_RECORD_COUNT];
	uint8_t record_count[ATTENUATOR_PHYSICAL_COUNT];
	uint8_t bridge_count[ATTENUATOR_PHYSICAL_COUNT];
	bool record_overflow[ATTENUATOR_PHYSICAL_COUNT];
	struct attenuator_calibration_fit_metrics fit[ATTENUATOR_PHYSICAL_COUNT];
};

static const uint8_t initial_laser_levels_pct[] = {100, 50, 5};

static struct atten_cal_state_data cal;
static K_MUTEX_DEFINE(cal_lock);
static atomic_t cal_fit_cancel;
static K_SEM_DEFINE(cal_fit_done, 0, 1);
/* Readers must not wait for the owner to finish a numerical fit or hardware I/O. */
static K_MUTEX_DEFINE(cal_status_lock);
static struct attenuator_calibration_status cal_status = {
	.state = "inactive", .mode = "none", .physical = "dac1", .fit = "none",
	.point_count = ATTENUATOR_CAL_RECORD_COUNT,
};
static struct coo_cmd_response cal_telemetry_msg;
static struct atten_cal_fit_point cal_fit_points[ATTENUATOR_CAL_RECORD_COUNT];

static void publish_status_locked(struct attenuator_calibration_status *status);
static void auto_schedule_measure_locked(enum atten_cal_measure_kind kind, float sweep_mv, float other_mv);
static void auto_begin_bridge_locked(void);
static void auto_close_bridge_locked(uint8_t index);

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
	case ATTEN_CAL_EVENT_BRIDGE_PROBE:
		return "bridge_probe";
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
	count = (uint16_t)cal.physical_index * ATTENUATOR_CAL_RECORD_COUNT + cal.record_count[cal.physical_index];
	return (uint8_t)MIN(99U, (count * 100U) / (ATTENUATOR_PHYSICAL_COUNT * ATTENUATOR_CAL_RECORD_COUNT));
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

	if (off == NULL || event == NULL) {
		return NULL;
	}

	memset(msg, 0, sizeof(*msg));
	*off = 0U;
	if (coo_json_append(msg->payload, sizeof(msg->payload), off,
			    "{\"event\":\"%s\",\"state\":\"%s\",\"mode\":\"%s\","
			    "\"physical\":\"%s\",\"attenuator\":%u,"
			    "\"complete_pct\":%u,\"record_count\":%u,"
			    "\"segment\":%u,\"sweep_mv\":%.3f,"
			    "\"other_mv\":%.3f,\"laser_pct\":%u",
			    event,
			    state_name(cal.state), mode_name(cal.mode),
			    physical_name(cal.physical_index), cal.attenuator_index,
			    complete_percent_locked(),
			    cal.record_count[cal.physical_index],
			    cal.segment_id, (double) cal.sweep_mv, (double) cal.other_mv,
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
			    "\"max_atten_db\":%.12g,\"max_calibrated_db\":%.9g,"
			    "\"max_atten_sigma_db\":%.12g,"
			    "\"correction_coeff\":[%.9g,%.9g,%.9g,%.9g,%.9g,%.9g],"
			    "\"corr\":%.12g,\"rms_db\":%.12g,"
			    "\"max_abs_db\":%.12g,\"min_tx\":%.12g,"
			    "\"max_tx\":%.12g,\"fvoa_span_mv\":%.6f}",
			    physical_name(physical),
			    fit != NULL && fit->valid ? "true" : "false",
			    fit != NULL && fit->accepted ? "true" : "false",
			    fit != NULL ? fit->points : 0U,
			    fit != NULL ? (double)fit->fvoa_50pct_mv : (double)NAN,
			    fit != NULL ? (double)fit->slope_inv_fvoa_mv : (double)NAN,
			    fit != NULL ? (double)fit->max_atten_db : (double)NAN,
			    fit != NULL ? fit->max_calibrated_db : (double)NAN,
			    fit != NULL ? (double)fit->max_atten_sigma_db : (double)NAN,
			    fit != NULL ? (double)fit->correction_coeff[0] : (double)NAN,
			    fit != NULL ? (double)fit->correction_coeff[1] : (double)NAN,
			    fit != NULL ? (double)fit->correction_coeff[2] : (double)NAN,
			    fit != NULL ? (double)fit->correction_coeff[3] : (double)NAN,
			    fit != NULL ? (double)fit->correction_coeff[4] : (double)NAN,
			    fit != NULL ? (double)fit->correction_coeff[5] : (double)NAN,
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
 * Convert the current photodiode configurable window into the calibration model.
 *
 * This classifies the window into the three bands used by the acquisition
 * logic: saturated/too bright, usable, or below-SNR/too dim. It does not retry
 * ADC reads; the photodiode sampler has already folded failed samples into the
 * window status and uncertainty.
 */
static void build_measurement_from_pd_window(const struct photodiode_window_result *window,
				    struct atten_cal_measurement *measurement)
{
	bool saturated;

	memset(measurement, 0, sizeof(*measurement));
	measurement->classification = ATTEN_CAL_CLASSIFICATION_ADC_ERROR;

	if (window == NULL || !window->valid ||
	    window->sample_length == window->failed_samples) {
		measurement->classification = ATTEN_CAL_CLASSIFICATION_ADC_ERROR;
		return;
	}

	measurement->signal_mv = (float) window->mean_net_mv;
	measurement->signal_err_mv = (float) window->mean_net_err_mv;
	measurement->max_mv = (float) window->max_mv;

	/* Calibration ratios require an entirely usable window. Photodiode
	 * saturation or ADC clipping can bias the mean and its normalization.
	 * Use the raw maximum (before dark subtraction) and the manufacturer's
	 * 2000 mV photodiode saturation/linearity limit, referred to the ADC input.
	 * This is not an output clamp: the detector can exceed even the ADC range.
	 * The ADC clips at 2048 mV full scale (2047.9375 mV maximum reported code).
	 * Reject detector saturation before ADC clipping; do not use a mean-only test.
	 * Retain saturated records for inspection, never as fit/bridge anchors.
	 */
	saturated = window->max_mv >= PHOTODIODE_ADC_USABLE_MV;

	if (!(measurement->signal_err_mv > 0.0f) || !isfinite(measurement->signal_err_mv)) {
		measurement->signal_err_mv = (float)PHOTODIODE_ADC_LSB_MV;
	}

	if (saturated) {
		measurement->snr = NAN;
		measurement->classification = ATTEN_CAL_CLASSIFICATION_SATURATED;
		return;
	}

	measurement->snr = measurement->signal_mv / measurement->signal_err_mv;
	if (measurement->signal_mv <= 0.0f ||
	    !isfinite(measurement->snr) ||
	    measurement->snr < ATTEN_CAL_SNR_USABLE) {
		measurement->classification = ATTEN_CAL_CLASSIFICATION_BELOW_SNR;
		return;
	}

	measurement->classification = ATTEN_CAL_CLASSIFICATION_OK;
}

/**
 * Retain one raw measurement for later host inspection and fit preparation.
 *
 * Retained records contain only acquisition facts. Bridge scaling,
 * normalized transmission, dB conversion, residuals, and fit inclusion, etc. are
 * derived later from these records and the accepted bridge table.
 */
static bool append_record_locked(enum atten_cal_record_event event,
				 const struct atten_cal_measurement *measurement,
				 struct atten_cal_record **out)
{
	struct atten_cal_record *record;
	uint8_t physical = cal.physical_index;
	uint8_t index;

	*out = NULL;
	if (physical >= ATTENUATOR_PHYSICAL_COUNT) {
		return false;
	}
	if (cal.record_count[physical] >= ATTENUATOR_CAL_RECORD_COUNT) {
		cal.record_overflow[physical] = true;
		return false;
	}

	index = cal.record_count[physical]++;
	record = &cal.records[physical][index];
	memset(record, 0, sizeof(*record));

	record->sweep_mv = cal.sweep_mv;
	record->other_mv = cal.other_mv;
	record->laser_pct = cal.laser_percent;
	record->signal_mv = measurement->signal_mv;
	record->signal_err_mv = measurement->signal_err_mv;
	record->max_mv = measurement->max_mv;
	record->event = (uint8_t) event;
	record->classification = (uint8_t) measurement->classification;
	record->segment = cal.segment_id;

	cal.point_index = index;
	atten_cal_emit_record(record);
	*out = record;
	return true;
}

/** Write the swept and companion FVOA DAC voltages for a logical attenuator. */
static bool set_physical_pair(uint8_t attenuator_index,
			      uint8_t sweep_physical,
			      float sweep_mv,
			      float other_mv)
{
	struct attenuator *atten;

	if (!devices_attenuator_channel_available(attenuator_index)) {
		return false;
	}

	atten = &attenuators[attenuator_index];
	if (!attenuator_set_physical_voltage(atten, sweep_physical, sweep_mv)) {
		return false;
	}
	return attenuator_set_physical_voltage(atten, sweep_physical == 0U ? 1U : 0U, other_mv);
}

/** Publish a coherent status with cal_lock held; never hold the reader lock over I/O. */
static void publish_status_locked(struct attenuator_calibration_status *out)
{
	struct attenuator_calibration_status *status = &cal_status;

	k_mutex_lock(&cal_status_lock, K_FOREVER);
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
	status->dwell_ms = cal.dwell_ms;
	status->complete_pct = complete_percent_locked();
	status->current_mv = cal.sweep_mv;
	status->other_mv = cal.other_mv;
	status->last_error = cal.last_error;
	status->laser_percent = cal.laser_percent;
	memcpy(status->fit_metrics, cal.fit, sizeof(status->fit_metrics));
	if (out != NULL) {
		*out = *status;
	}
	k_mutex_unlock(&cal_status_lock);
}

/** Put calibration into terminal error state and emit the corresponding telemetry. */
static void auto_error_locked(int error)
{
	housekeeping_photodiode_autooff_inhibit((enum housekeeping_power_output)cal.channel, false);
	cal.last_error = error;
	cal.state = ATTEN_CAL_STATE_ERROR;
	cal.phase = ATTEN_CAL_PHASE_NONE;
	/* Acquisition faults stop the calibration-owned source. A failed stop
	 * leaves its identity available for the user's explicit stop/restart.
	 */
	cal.shutdown_pending = hispec_laser_stop_output(cal.laser, false) != 0;
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
	if (hispec_laser_set_output_percent_autooff(cal.laser, cal.laser_percent, 0U, true) != 0) {
		auto_error_locked(-EIO);
		return false;
	}
	return true;
}

/** Return the next midpoint for companion-FVOA binary searches. */
static float search_midpoint_locked(void)
{
	return (cal.search_low_mv + cal.search_high_mv) * 0.5f;
}

/** Return the next fixed DUT sweep target, clamped at full DAC drive. */
static float next_linear_sweep_mv(float sweep_mv)
{
	return MIN(ATTENUATOR_DRIVE_MAX_MV, sweep_mv + ATTEN_CAL_SWEEP_STEP_MV);
}

/**
 * Reset the companion-FVOA search bracket.
 *
 * Companion DAC direction is inverted relative to photodiode signal: lower DAC
 * opens the companion and raises signal, while higher DAC attenuates more. The
 * low side of this bracket is therefore the at/above-target side; the high
 * side is the more-attenuated side. A usable candidate is tracked separately
 * because the high bracket can also be a below-SNR point.
 */
static void companion_search_begin_locked(float search_low_mv, float search_high_mv)
{
	cal.search_low_mv = CLAMP(search_low_mv, 0.0f, ATTENUATOR_DRIVE_MAX_MV);
	cal.search_high_mv = CLAMP(search_high_mv, 0.0f, ATTENUATOR_DRIVE_MAX_MV);
	cal.search_candidate_mv = cal.search_high_mv;
	cal.search_candidate_valid = false;
	cal.search_tries = 0U;
}

/**
 * Fold one companion-search measurement into the shared bracket.
 *
 * A raw peak at/above the search target needs more companion attenuation, so
 * the drive-voltage low side moves up even when the measurement is still usable.
 * Below-SNR means the companion is too attenuated, so the drive high voltage moves down.
 * A usable measurement becomes the current candidate and the search keeps going to lower attenuation
 * to find the brightest point below the target. Retained classifications still
 * use the PD's 2000 mV usable-input limit.
 */
static bool companion_search_note_measurement_locked(const struct atten_cal_record *record, uint8_t index)
{
	if (record->classification == ATTEN_CAL_CLASSIFICATION_ADC_ERROR) {
		return false;
	}
	if (record->max_mv >= ATTEN_CAL_SEARCH_MAX_MV) {
		cal.search_low_mv = cal.other_mv;
	} else if (record->classification == ATTEN_CAL_CLASSIFICATION_OK) {
		cal.search_high_mv = cal.other_mv;
		cal.search_candidate_mv = cal.other_mv;
		cal.search_candidate_valid = true;
		cal.search_candidate_record_index = index;
	} else if (record->classification == ATTEN_CAL_CLASSIFICATION_BELOW_SNR) {
		cal.search_high_mv = cal.other_mv;
	} else {
		return false;
	}
	cal.search_tries++;
	return true;
}

/**
 * Set DAC voltages, sleep for FVOA settling, then start a full PD averaging window.
 * Sleeps with cal_lock held, pausing the caller (normally the throughput monitor)
 * and calibration stop access. Status uses a separate snapshot; ADC sampling
 * continues independently.
 */
static void auto_schedule_measure_locked(enum atten_cal_measure_kind kind,
					 float sweep_mv, float other_mv)
{
	const char *event = "set";

	cal.measure_kind = kind;
	cal.sweep_mv = CLAMP(sweep_mv, 0.0f, ATTENUATOR_DRIVE_MAX_MV);
	cal.other_mv = CLAMP(other_mv, 0.0f, ATTENUATOR_DRIVE_MAX_MV);

	if (!set_physical_pair(cal.attenuator_index, cal.physical_index, cal.sweep_mv, cal.other_mv)) {
		auto_error_locked(-EIO);
		return;
	}

	/* FVOAs can take 60 ms to respond. Keep their transition outside the full
	 * averaging window by settling after both writes and resetting the PD afterward.
	 */
	k_msleep(ATTEN_CAL_SETTLE_MS);

	switch (kind) {
		case ATTEN_CAL_MEASURE_INITIAL_PROBE:
			event = "initial_probe_set";
			break;
		case ATTEN_CAL_MEASURE_SWEEP:
			event = "point_set";
			break;
		case ATTEN_CAL_MEASURE_BRIDGE_PROBE:
			event = "bridge_probe_set";
			break;
		default:
			break;
	}
	/* The PD owner rounds duration and excludes conversions already in flight.
	 * Wait for its actual sample count, not dwell plus a guessed ADC allowance.
	 */
	int duration = photodiode_set_configurable_window_duration(cal.channel, cal.dwell_ms);
	if (duration < 0) {
		auto_error_locked(duration);
		return;
	}
	cal.dwell_ms = (uint32_t)duration;
	cal.window_started_ms = k_uptime_get();
	atten_cal_emit_set(event);
	cal.phase = ATTEN_CAL_PHASE_WAIT_WINDOW;  // From here execution resumes at auto_tick_locked()
}

/** Initialize acquisition state for the current physical FVOA and schedule its first probe. */
static void auto_start_next_physical_locked(void)
{
	cal.point_index = 0U;
	cal.segment_id = 0U;
	cal.search_tries = 0U;
	cal.sweep_mv = 0.0f;
	cal.other_mv = ATTENUATOR_DRIVE_MAX_MV;

	companion_search_begin_locked(0.0f, ATTENUATOR_DRIVE_MAX_MV);

	cal.bridge_before_index = 0U;
	cal.bridge_before_index_valid = false;

	cal.laser_level_index = 0U;
	cal.laser_percent = initial_laser_levels_pct[0];

	if (!auto_set_laser_level_locked(0U)) {
		return;
	}
	/* Scheduling waits for settling before collecting the first window after the laser change. */
	auto_schedule_measure_locked(ATTEN_CAL_MEASURE_INITIAL_PROBE, 0.0f, ATTENUATOR_DRIVE_MAX_MV);
	atten_cal_emit_simple("physical_start");
}

/** Finish acquisition, stopping the source before handing immutable records to fitting. */
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
	int rc = hispec_laser_stop_output(cal.laser, false);
	housekeeping_photodiode_autooff_inhibit((enum housekeeping_power_output)cal.channel, false);
	if (rc != 0) {
		/* Preserve the data and shutdown responsibility without retrying STOP. */
		cal.shutdown_pending = true;
		cal.last_error = rc;
		cal.state = ATTEN_CAL_STATE_ERROR;
		cal.phase = ATTEN_CAL_PHASE_NONE;
		atten_cal_emit_simple("error");
		return;
	}
	cal.laser_percent = 0;
	atomic_clear(&cal_fit_cancel);
	k_sem_reset(&cal_fit_done);
	cal.phase = ATTEN_CAL_PHASE_FITTING;
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

	if (!companion_search_note_measurement_locked(record, cal.point_index)) {
		auto_error_locked(record->classification == ATTEN_CAL_CLASSIFICATION_ADC_ERROR ? -EIO : -ERANGE);
		return;
	}

	if (record->max_mv >= ATTEN_CAL_SEARCH_MAX_MV &&
		cal.other_mv >= ATTENUATOR_DRIVE_MAX_MV - ATTEN_CAL_SEARCH_MIN_STEP_MV) {

		/* Decrease laser level & try again */
		if (cal.laser_level_index + 1U >= ARRAY_SIZE(initial_laser_levels_pct)) {
			// Can't go fainter, still too bright :(
			auto_error_locked(-ERANGE);
			return;
		}
		if (!auto_set_laser_level_locked((uint8_t)(cal.laser_level_index + 1U))) {
			return;
		}

		/* Scheduling waits for settling before collecting the next window after the laser change. */
		companion_search_begin_locked(0.0f, ATTENUATOR_DRIVE_MAX_MV);
		auto_schedule_measure_locked(ATTEN_CAL_MEASURE_INITIAL_PROBE, 0.0f, ATTENUATOR_DRIVE_MAX_MV);

		return;
	}


	if (cal.search_high_mv - cal.search_low_mv <= ATTEN_CAL_SEARCH_MIN_STEP_MV ||
	    cal.search_tries >= ATTEN_CAL_MAX_SEARCH_TRIES) {

		/* Adopt the brightest retained usable initial probe below the search target. */
		uint8_t reference_index = cal.search_candidate_record_index;
		if (!cal.search_candidate_valid) {
			LOG_INF("atten cal initial probe no viable initial reference. impossible. physical=%s",
				physical_name(cal.physical_index));
			auto_error_locked(-ERANGE);  // Can't go fainter, still too bright
			return;
		}

		cal.reference_record_index[cal.physical_index] = reference_index;
		cal.reference_record_index_valid[cal.physical_index] = true;

		auto_schedule_measure_locked(ATTEN_CAL_MEASURE_SWEEP, 0.0f,
			cal.records[cal.physical_index][reference_index].other_mv);
		return;
	}

	auto_schedule_measure_locked(ATTEN_CAL_MEASURE_INITIAL_PROBE, 0.0f, search_midpoint_locked());
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

	(void)append_record_locked(ATTEN_CAL_EVENT_POINT, measurement, &record);
	if (record == NULL) {
		auto_error_locked(-ENOSPC);
		return;
	}

	if (measurement->classification == ATTEN_CAL_CLASSIFICATION_OK ||
	    measurement->classification == ATTEN_CAL_CLASSIFICATION_SATURATED) {
		if (cal.sweep_mv >= ATTENUATOR_DRIVE_MAX_MV) {
			auto_finish_physical_locked();
			return;
		}
		auto_schedule_measure_locked(ATTEN_CAL_MEASURE_SWEEP, next_linear_sweep_mv(cal.sweep_mv), cal.other_mv);
		return;
	}

	if (measurement->classification == ATTEN_CAL_CLASSIFICATION_BELOW_SNR) {
		if (cal.sweep_mv >= ATTENUATOR_DRIVE_MAX_MV) {
			auto_finish_physical_locked();
			return;
		}
		auto_begin_bridge_locked();
		return;
	}

	if (measurement->classification == ATTEN_CAL_CLASSIFICATION_ADC_ERROR) {
		auto_error_locked(-EIO);
		return;
	}

	auto_error_locked(-ERANGE);
}

/**
 * Start bridge normalization from the latest usable retained DUT anchor.
 *
 * Saturated diagnostic sweep records are not valid bridge anchors, so the held
 * DUT voltage is recovered from retained usable records in the current segment.
 * If no ordinary point is usable, the last accepted bridge-after record can
 * anchor another bridge at the current companion setting. Hold that DUT drive
 * and schedule a companion probe in a new segment.
 */
static void auto_begin_bridge_locked(void)
{
	uint8_t physical = cal.physical_index;
	uint8_t anchor_index = 0U;
	const struct atten_cal_record *anchor = NULL;

	/* The companion is already as open as firmware allows; there is no more range to bridge into. */
	if (cal.other_mv <= ATTEN_CAL_SEARCH_MIN_STEP_MV) {
		auto_finish_physical_locked();
		return;
	}

	/* Find the latest usable DUT point in the current segment. */
	for (uint8_t i = cal.record_count[physical]; i > 0U; --i) {
		const struct atten_cal_record *record = &cal.records[physical][i - 1U];

		if (record->segment == cal.segment_id && record->event == ATTEN_CAL_EVENT_POINT) {
			if (record->classification == ATTEN_CAL_CLASSIFICATION_OK) {
				anchor = record;
				anchor_index = i - 1U;
				break;
			}
		}
	}

	/* The first sweep point after a bridge can be below SNR while the accepted
	 * bridge-after record still supplies a usable anchor at this companion DAC.
	 * Use the accepted bridge table, not an arbitrary retained search probe.
	 */
	if (anchor == NULL && cal.bridge_count[physical] > 0U) {
		const struct atten_cal_bridge *bridge =
			&cal.bridges[physical][cal.bridge_count[physical] - 1U];
		const struct atten_cal_record *record =
			&cal.records[physical][bridge->after_record_index];

		if (record->classification == ATTEN_CAL_CLASSIFICATION_OK &&
		    record->segment == cal.segment_id && record->other_mv == cal.other_mv) {
			anchor = record;
			anchor_index = bridge->after_record_index;
			LOG_INF("atten cal bridge anchor physical=%s segment=%u record=%u sweep_mv=%.3f other_mv=%.3f",
				physical_name(physical), cal.segment_id, anchor_index,
				(double)anchor->sweep_mv, (double)anchor->other_mv);
		}
	}

	if (anchor == NULL) {
		/* Neither a usable sweep point nor an accepted bridge-after anchor exists. */
		LOG_ERR("No usable bridge anchor found in segment %u, should be impossible", cal.segment_id);
		auto_error_locked(-ERANGE);
		return;
	}

	cal.sweep_mv = anchor->sweep_mv;
	cal.bridge_before_index = anchor_index;
	cal.bridge_before_index_valid = true;

	/* Bridge probes and any following DUT sweep points belong to a new scaled segment. */
	cal.segment_id++;

	companion_search_begin_locked(0.0f, cal.other_mv);
	auto_schedule_measure_locked(ATTEN_CAL_MEASURE_BRIDGE_PROBE, cal.sweep_mv, search_midpoint_locked());
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

	if (!companion_search_note_measurement_locked(record, cal.point_index)) {
		auto_error_locked(record->classification == ATTEN_CAL_CLASSIFICATION_ADC_ERROR ? -EIO : -ERANGE);
		return;
	}

	if (cal.search_high_mv - cal.search_low_mv <= ATTEN_CAL_SEARCH_MIN_STEP_MV ||
	    cal.search_tries >= ATTEN_CAL_MAX_SEARCH_TRIES) {

		/* Adopt the brightest retained usable bridge probe below the search target. */

		uint8_t bridge_index = cal.search_candidate_record_index;
		if (!cal.search_candidate_valid) {
			// Search again with a floor > cal.search_candidate_mv

			float search_floor = 0;
			for (uint8_t i = 1; i <= cal.point_index; ++i) {
				const struct atten_cal_record *candidate_record =
					&cal.records[cal.physical_index][cal.point_index - i];

				if (candidate_record->event == ATTEN_CAL_EVENT_BRIDGE_PROBE &&
				    candidate_record->segment == cal.segment_id) {
					if (candidate_record->max_mv >= ATTEN_CAL_SEARCH_MAX_MV) {
						search_floor = fmaxf(search_floor, candidate_record->other_mv);
						break;
					}
				}
			}

			if (search_floor == 0.0f) {
				/* Every bridge probe was below SNR. There is no brighter valid segment left. */
				LOG_INF("atten cal bridge probe all below snr. impossible unless noise conspires with sweep end. physical=%s",
					physical_name(cal.physical_index));
				auto_finish_physical_locked();
				return;
			}

			if (fabsf(search_floor - cal.other_mv) < ATTEN_CAL_SEARCH_MIN_STEP_MV) {
				// No usable probe below the target; try again with a higher attenuation floor.
				// If we are here, I think we should ALWAYS be here (and we should never get here)
				LOG_INF("atten cal bridge probe no viable new point. impossible. physical=%s",
					physical_name(cal.physical_index));
				auto_error_locked(-ERANGE);
				return;
			}

			companion_search_begin_locked(search_floor, cal.other_mv);
			auto_schedule_measure_locked(ATTEN_CAL_MEASURE_BRIDGE_PROBE, cal.sweep_mv, search_midpoint_locked());

			return;
		}

		float ratio = cal.records[cal.physical_index][bridge_index].signal_mv /
			cal.records[cal.physical_index][cal.bridge_before_index].signal_mv;
		bool bad_bridge = (ratio <= 1.0f) || !isfinite(ratio);

		if (bad_bridge) {
			/* This shouldn't be possible, but handling it is the same as ATTEN_CAL_CLASSIFICATION_BELOW_SNR */
			LOG_INF("atten cal bridge probe selected a <1= ratio. impossible. physical=%s",
				physical_name(cal.physical_index));
			auto_error_locked(-ERANGE);
			return;
		}

		auto_close_bridge_locked(bridge_index);
		return;
	}

	auto_schedule_measure_locked(ATTEN_CAL_MEASURE_BRIDGE_PROBE, cal.sweep_mv, search_midpoint_locked());
}

/**
 * Accept bridge normalization and resume DUT sweeping.
 *
 * The accepted bridge probe is retained only as a measured bridge-probe record.
 * Its role as the after side of the bridge is stored in the bridge table.
 */
static void auto_close_bridge_locked(uint8_t index)
{

	/* Adopt the accepted bridge probe as the after side of this segment boundary. */
	cal.other_mv = cal.records[cal.physical_index][index].other_mv;
	if (cal.bridge_before_index_valid && cal.bridge_count[cal.physical_index] < ATTENUATOR_CAL_RECORD_COUNT) {
		struct atten_cal_bridge *bridge =
			&cal.bridges[cal.physical_index][cal.bridge_count[cal.physical_index]++];

		bridge->before_record_index = cal.bridge_before_index;
		bridge->after_record_index = index;
	}

	/* Reset transient bridge state. */
	cal.bridge_before_index = 0U;
	cal.bridge_before_index_valid = false;

	/* Continue the linear DUT sweep from the held anchor voltage. */
	if (cal.sweep_mv >= ATTENUATOR_DRIVE_MAX_MV) {
		auto_finish_physical_locked();
		return;
	}

	auto_schedule_measure_locked(ATTEN_CAL_MEASURE_SWEEP, next_linear_sweep_mv(cal.sweep_mv), cal.other_mv);
}

/** Return true when a raw record can become a model fit point. */
static bool record_is_fit_candidate(const struct atten_cal_record *record)
{
	if (record == NULL ||
	    record->classification != ATTEN_CAL_CLASSIFICATION_OK ||
	    record->signal_err_mv <= 0.0f) {
		return false;
	}
	return record->event == ATTEN_CAL_EVENT_POINT;
}

/**
 * Build cumulative bridge scales from accepted before/after bridge records.
 *
 * The acquisition state records only bridge indices. This helper centralizes
 * the ratio and variance propagation so the retained record stays raw and the
 * fit has one authoritative normalization path.
 */
static int build_segment_scales(uint8_t physical,
				double *segment_scale,
				double *segment_rel_var)
{
	if (physical >= ATTENUATOR_PHYSICAL_COUNT ||
	    segment_scale == NULL || segment_rel_var == NULL) {
		return -EINVAL;
	}
	for (uint8_t i = 0U; i < ATTENUATOR_CAL_RECORD_COUNT; ++i) {
		segment_scale[i] = (double)NAN;
		segment_rel_var[i] = (double)NAN;
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
 * Bridge probes are not fit candidates. Accepted bridge probes define segment
 * scale through the bridge table and remain raw diagnostic measurements.
 */
static int build_fit_points(uint8_t physical,
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

	*point_count_out = 0U;

	if (!cal.reference_record_index_valid[physical]) {
		return -ERANGE;
	}
	reference = &cal.records[physical][cal.reference_record_index[physical]];

	open_signal = (double)reference->signal_mv;
	open_err = (double)reference->signal_err_mv;
	open_rel_var = (open_err / open_signal) * (open_err / open_signal);

	rc = build_segment_scales(physical, segment_scale, segment_rel_var);
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

		/* The reference is a measurement, so valid sweep readings can be
		 * brighter than it. Keep their negative measured dB in the direct
		 * dB-space fit and residuals; rejecting them selects only dimmer noise.
		 */
		if (tx < ATTEN_CAL_MIN_TX) {
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
		points[point_count].measured_db = (float)db;
		points[point_count].measured_db_err =
			(float)MAX(db_err, ATTEN_CAL_MIN_DB_ERR);
		point_count++;
	}

	if (point_count < ATTEN_CAL_MIN_FIT_POINTS) {
		return -ERANGE;
	}
	*point_count_out = point_count;
	return 0;
}

/**
 * Estimate the physical FVOA leakage floor from the final retained fit points.
 *
 * Acquisition sweeps monotonically toward higher DUT attenuation. The final
 * usable points therefore describe the finite transmission floor that remains
 * when the FVOA is effectively shut. This is not optimized as a third
 * Gauss-Newton parameter; it is fixed before the two-shape-parameter fit so the
 * embedded optimizer stays small and reproducible.
 */
static int estimate_max_atten_db(const struct atten_cal_fit_point *points,
				 uint8_t point_count,
				 double *max_atten_db,
				 double *max_atten_sigma_db)
{
	const uint8_t count = ATTEN_CAL_TAIL_FLOOR_POINTS;
	double sum = 0.0;
	double mean;
	double sum_sq = 0.0;
	double sigma;

	if (points == NULL || max_atten_db == NULL || max_atten_sigma_db == NULL ||
	    point_count < count) {
		return -EINVAL;
	}

	for (uint8_t i = point_count - count; i < point_count; ++i) {
		if (!isfinite(points[i].measured_db)) {
			return -ERANGE;
		}
		sum += (double)points[i].measured_db;
	}
	mean = sum / (double)count;
	for (uint8_t i = point_count - count; i < point_count; ++i) {
		double delta = (double)points[i].measured_db - mean;

		sum_sq += delta * delta;
	}

	sigma = sqrt(sum_sq / (double)(count - 1U)) / sqrt((double)count);
	sigma = MAX(sigma, ATTENUATOR_FIT_MIN_SIGMA_DB / sqrt((double)count));
	if (!isfinite(mean) || mean <= 0.0 || !isfinite(sigma) || !(sigma > 0.0)) {
		return -ERANGE;
	}

	*max_atten_db = mean;
	*max_atten_sigma_db = sigma;
	return 0;
}

/** Return one weighted residual, and optionally its analytic fit Jacobian. */
static int fit_point_weighted_eval(const struct atten_cal_fit_point *point,
				   const struct atten_cal_record *records,
				   const struct attenuator_model_coeffs *coeffs,
				   double max_atten_sigma_db,
				   double *residual,
				   double *j_f50,
				   double *j_slope)
{
	struct atten_model_eval eval;
	double sigma_db;

	if (point == NULL || records == NULL || coeffs == NULL || residual == NULL) {
		return -EINVAL;
	}
	if (!atten_model_eval(coeffs, records[point->record_index].sweep_mv, &eval) ||
	    !atten_model_db_sigma(&eval, (double)point->measured_db_err,
				  (double)ATTEN_CAL_DAC_SIGMA_MV,
				  max_atten_sigma_db, &sigma_db)) {
		return -ERANGE;
	}

	sigma_db = MAX(sigma_db, ATTENUATOR_FIT_MIN_SIGMA_DB);
	*residual = (eval.db - (double)point->measured_db) / sigma_db;
	if (!isfinite(*residual)) {
		return -ERANGE;
	}
	if (j_f50 != NULL) {
		*j_f50 = eval.d_db_d_fvoa_50pct_mv / sigma_db;
	}
	if (j_slope != NULL) {
		*j_slope = eval.d_db_d_slope_inv_fvoa_mv / sigma_db;
	}
	return 0;
}

/** Sum weighted squared residuals for a complete fit candidate. */
static double fit_cost(const struct atten_cal_fit_point *points,
		       const struct atten_cal_record *records,
		       uint8_t point_count,
		       const struct attenuator_model_coeffs *coeffs,
		       double max_atten_sigma_db)
{
	double cost = 0.0;

	for (uint8_t i = 0U; i < point_count; ++i) {
		double residual;

		if (fit_point_weighted_eval(&points[i], records, coeffs,
					    max_atten_sigma_db, &residual,
					    NULL, NULL) != 0) {
			return INFINITY;
		}
		cost += residual * residual;
		if (atomic_get(&cal_fit_cancel)) return INFINITY;
	}
	return cost;
}

/** Choose a data-derived initial coefficient guess for the dB-space optimizer. */
static void fit_initial_guess(const struct atten_cal_fit_point *points,
			      const struct atten_cal_record *records,
			      uint8_t point_count, double gain,
			      double *fvoa_50pct_mv,
			      double *slope_inv_fvoa_mv)
{
	double max_fvoa_mv = (double)ATTENUATOR_DRIVE_MAX_MV * gain;
	double min_x = INFINITY;
	double max_x = 0.0;
	double closest_err = INFINITY;
	double f50 = 0.5 * max_fvoa_mv;
	double slope;

	for (uint8_t i = 0U; i < point_count; ++i) {
		double x = (double)records[points[i].record_index].sweep_mv * gain;
		double err = fabs((double)points[i].measured_db - 3.01029995664);

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
 * Optimize two model coefficients with damped analytic-Jacobian Gauss-Newton.
 *
 * The objective is measured/model attenuation residual in dB divided by the
 * propagated dB uncertainty. The propagated uncertainty is recomputed for each
 * candidate coefficient set, but the Jacobian treats that uncertainty as the
 * current point weight rather than differentiating through the weight itself.
 */
static int fit_optimize_db(const struct atten_cal_fit_point *points,
			   const struct atten_cal_record *records,
			   uint8_t point_count, double gain,
			   double max_atten_db, double max_atten_sigma_db,
			   double *fvoa_50pct_mv,
			   double *slope_inv_fvoa_mv)
{
	double max_fvoa_mv = (double)ATTENUATOR_DRIVE_MAX_MV * gain;
	double f50;
	double slope;
	double lambda = ATTEN_CAL_FIT_INITIAL_LAMBDA;
	double cost;
	struct attenuator_model_coeffs coeffs;

	if (points == NULL || records == NULL || fvoa_50pct_mv == NULL ||
	    slope_inv_fvoa_mv == NULL ||
	    point_count < ATTEN_CAL_MIN_FIT_POINTS) {
		return -EINVAL;
	}
	fit_initial_guess(points, records, point_count, gain, &f50, &slope);
	coeffs = (struct attenuator_model_coeffs) {
		.fvoa_50pct_mv = f50,
		.slope_inv_fvoa_mv = slope,
		.max_atten_db = max_atten_db,
		.gain = gain,
	};
	cost = fit_cost(points, records, point_count, &coeffs,
			max_atten_sigma_db);
	if (!isfinite(cost)) {
		return atomic_get(&cal_fit_cancel) ? -ECANCELED : -ERANGE;
	}

	for (uint8_t iter = 0U; iter < ATTEN_CAL_FIT_MAX_ITER; ++iter) {
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
		struct attenuator_model_coeffs trial_coeffs;

		coeffs.fvoa_50pct_mv = f50;
		coeffs.slope_inv_fvoa_mv = slope;
		for (uint8_t i = 0U; i < point_count; ++i) {
			double r;
			double j0;
			double j1;

			if (fit_point_weighted_eval(&points[i], records, &coeffs,
						    max_atten_sigma_db, &r,
						    &j0, &j1) != 0) {
				return -ERANGE;
			}
			h00 += j0 * j0;
			h01 += j0 * j1;
			h11 += j1 * j1;
			g0 += j0 * r;
			g1 += j1 * r;
			if (atomic_get(&cal_fit_cancel)) return -ECANCELED;
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
		trial_coeffs = (struct attenuator_model_coeffs) {
			.fvoa_50pct_mv = trial_f50,
			.slope_inv_fvoa_mv = trial_slope,
			.max_atten_db = max_atten_db,
			.gain = gain,
		};
		trial_cost = fit_cost(points, records, point_count,
				      &trial_coeffs, max_atten_sigma_db);
		if (atomic_get(&cal_fit_cancel)) return -ECANCELED;
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

/** Solve the small dense normal equation used for residual-correction terms. */
static int solve_correction_normal_equation(
	double normal[ATTENUATOR_MODEL_CORRECTION_TERMS][ATTENUATOR_MODEL_CORRECTION_TERMS],
	double rhs[ATTENUATOR_MODEL_CORRECTION_TERMS],
	uint8_t terms,
	float correction_coeff[ATTENUATOR_MODEL_CORRECTION_TERMS])
{
	double matrix[ATTENUATOR_MODEL_CORRECTION_TERMS][ATTENUATOR_MODEL_CORRECTION_TERMS + 1U];

	/* The leading submatrix refits T0..T(terms-1); unused stored terms stay zero. */
	memset(correction_coeff, 0, sizeof(float) * ATTENUATOR_MODEL_CORRECTION_TERMS);
	for (uint8_t row = 0U; row < terms; ++row) {
		for (uint8_t col = 0U; col < terms; ++col) {
			matrix[row][col] = normal[row][col];
		}
		matrix[row][terms] = rhs[row];
	}

	for (uint8_t col = 0U; col < terms; ++col) {
		uint8_t pivot = col;
		double pivot_abs = fabs(matrix[col][col]);

		for (uint8_t row = col + 1U; row < terms; ++row) {
			double value_abs = fabs(matrix[row][col]);

			if (value_abs > pivot_abs) {
				pivot = row;
				pivot_abs = value_abs;
			}
		}
		if (!(pivot_abs > ATTEN_CAL_CORRECTION_PIVOT_EPS) || !isfinite(pivot_abs)) {
			return -ERANGE;
		}
		if (pivot != col) {
			for (uint8_t k = col; k <= terms; ++k) {
				double tmp = matrix[col][k];

				matrix[col][k] = matrix[pivot][k];
				matrix[pivot][k] = tmp;
			}
		}
		for (uint8_t row = 0U; row < terms; ++row) {
			double scale;

			if (row == col) {
				continue;
			}
			scale = matrix[row][col] / matrix[col][col];
			for (uint8_t k = col; k <= terms; ++k) {
				matrix[row][k] -= scale * matrix[col][k];
			}
		}
	}

	for (uint8_t row = 0U; row < terms; ++row) {
		double value = matrix[row][terms] / matrix[row][row];

		if (!isfinite(value)) {
			return -ERANGE;
		}
		correction_coeff[row] = (float)value;
	}
	return 0;
}

/** Check the final curve, including the open region where clipping can leave no fit
 * points. A 1 mV grid checks values and analytic slopes between samples; retained
 * voltages and the calibrated join are also checked. This is numerical validation,
 * not a proof between grid locations. No I/O; returns the first failing location.
 */
static bool fit_curve_valid(const struct attenuator_model_coeffs *coeffs,
                            const struct atten_cal_fit_point *points,
                            const struct atten_cal_record *records, uint8_t point_count,
                            float *failed_mv, struct atten_model_eval *eval)
{
	float join_mv = 0.0f;

	for (uint8_t pass = 0U; pass < 3U; ++pass) {
		double previous_db = -INFINITY;
		uint16_t count = pass == 0U ? (uint16_t)ATTENUATOR_DRIVE_MAX_MV + 1U :
			(pass == 1U ? point_count : 3U);

		if (pass == 2U) {
			/* At the leakage floor there is no separate continuation join. */
			if (coeffs->max_calibrated_db >= coeffs->max_atten_db) break;
			*failed_mv = NAN;
			*eval = (struct atten_model_eval){.db = NAN, .d_db_d_voltage_mv = NAN};
			if (!attenuator_model_db_to_voltage(coeffs, coeffs->max_calibrated_db, &join_mv)) return false;
		}
		for (uint16_t i = 0U; i < count; ++i) {
			float mv;
			if (pass == 0U) {
				mv = (float)i; /* 1 mV grid across the complete final curve. */
			} else if (pass == 1U) {
				mv = records[points[i].record_index].sweep_mv;
			} else {
				mv = i == 1U ? join_mv : nextafterf(join_mv, i == 0U ? -INFINITY : INFINITY);
			}
			*failed_mv = CLAMP(mv, 0.0f, ATTENUATOR_DRIVE_MAX_MV);
			*eval = (struct atten_model_eval){.db = NAN, .d_db_d_voltage_mv = NAN};
			if (!atten_model_eval(coeffs, *failed_mv, eval) ||
			    eval->d_db_d_voltage_mv < 0.0 ||
			    eval->db + ATTEN_CAL_CORRECTION_MONOTONIC_EPS_DB < previous_db ||
			    (pass == 2U && i == 1U &&
			     fabs(eval->db - coeffs->max_calibrated_db) > ATTEN_CAL_CORRECTION_MONOTONIC_EPS_DB)) {
				return false;
			}
			previous_db = eval->db;
			if (atomic_get(&cal_fit_cancel)) return false;
		}
	}
	return true;
}

/**
 * Fit the optional empirical residual correction after the base model fit.
 *
 * The correction remains subordinate to the physical model. Refit successively
 * fewer leading Chebyshev terms when the solve or the final curve is invalid;
 * dropping every term is the last fallback. The six stored slots never change.
 * All candidates use the same measured prefix, including its above-limit anchor.
 * Each candidate's calibrated limit and continuation are established BEFORE
 * validation, so unused polynomial behavior in the tail cannot reject a good fit.
 * Numerical work only; one best-effort warning summarizes a reduced-order result.
 */
static int fit_correction_coeff(const struct atten_cal_fit_point *points,
				const struct atten_cal_record *records,
				uint8_t point_count,
				double max_atten_sigma_db,
				struct attenuator_model_coeffs *coeffs)
{
	double normal[ATTENUATOR_MODEL_CORRECTION_TERMS][ATTENUATOR_MODEL_CORRECTION_TERMS] = {0};
	double rhs[ATTENUATOR_MODEL_CORRECTION_TERMS] = {0};
	uint8_t used = 0U;
	int selected = -1;
	char first_failure[112] = "";

	memset(coeffs->correction_coeff, 0, sizeof(coeffs->correction_coeff));
	for (uint8_t i = 0U; i < point_count; ++i) {
		const struct atten_cal_fit_point *point = &points[i];
		const struct atten_cal_record *record = &records[point->record_index];
		struct atten_model_eval eval;
		double sigma_db;
		double basis[ATTENUATOR_MODEL_CORRECTION_TERMS];
		double residual_db;
		double weight;

		if (atomic_get(&cal_fit_cancel)) return -ECANCELED;
		if (!atten_model_eval(coeffs, record->sweep_mv, &eval) ||
		    !atten_model_db_sigma(&eval, (double)point->measured_db_err,
					  (double)ATTEN_CAL_DAC_SIGMA_MV,
					  max_atten_sigma_db, &sigma_db) ||
		    !atten_model_correction_basis(eval.db, coeffs->max_atten_db, basis)) {
			continue;
		}
		sigma_db = MAX(sigma_db, ATTENUATOR_FIT_MIN_SIGMA_DB);
		residual_db = (double)point->measured_db - eval.db;
		weight = 1.0 / (sigma_db * sigma_db);
		for (uint8_t row = 0U; row < ATTENUATOR_MODEL_CORRECTION_TERMS; ++row) {
			rhs[row] += basis[row] * residual_db * weight;
			for (uint8_t col = 0U; col < ATTENUATOR_MODEL_CORRECTION_TERMS; ++col) {
				normal[row][col] += basis[row] * basis[col] * weight;
			}
		}
		used++;
	}
	for (int terms = ATTENUATOR_MODEL_CORRECTION_TERMS; terms >= 0; --terms) {
		const struct atten_cal_fit_point *last = &points[point_count - 1U];
		float failed_mv = records[last->record_index].sweep_mv;
		struct atten_model_eval eval = {.db = NAN, .d_db_d_voltage_mv = NAN};
		const char *reason = "solve";
		int rc = 0;

		if (atomic_get(&cal_fit_cancel)) return -ECANCELED;
		/* Zero temporarily selects the raw polynomial while locating its endpoint. */
		coeffs->max_calibrated_db = 0.0;
		memset(coeffs->correction_coeff, 0, sizeof(coeffs->correction_coeff));
		if (terms > 0) {
			rc = used < terms ? -ERANGE :
				solve_correction_normal_equation(normal, rhs, (uint8_t)terms, coeffs->correction_coeff);
		}
		if (rc == 0) {
			reason = "endpoint";
			if (atten_model_eval(coeffs, failed_mv, &eval)) {
				coeffs->max_calibrated_db = MIN(ATTENUATOR_CALIBRATED_MAX_DB,
					MIN((double)last->measured_db, MIN(eval.db, coeffs->max_atten_db)));
				if (coeffs->max_calibrated_db > 0.0) {
					reason = "curve";
					if (fit_curve_valid(coeffs, points, records, point_count, &failed_mv, &eval)) {
						selected = terms;
						break;
					}
				}
			}
		}
		if (first_failure[0] == '\0') {
			snprintk(first_failure, sizeof(first_failure), "check=%s mv=%.3f db=%.6g slope=%.6g",
				 reason, (double)failed_mv, eval.db, eval.d_db_d_voltage_mv);
		}
	}
	/* Cancellation is not a rejected model and must not emit a fit warning. */
	if (atomic_get(&cal_fit_cancel)) return -ECANCELED;
	if (selected != ATTENUATOR_MODEL_CORRECTION_TERMS) {
		char context[160];
		const char *name = physical_name(records == cal.records[0] ? 0U : 1U);

		snprintk(context, sizeof(context), "%s terms=%d first_failure: %s", name, selected, first_failure);
		LOG_WRN("atten correction %s", context);
		coo_cmd_runtime_emit(command_runtime_get(), &(struct coo_cmd_runtime_emit_args){
			.type = COO_CMD_RUNTIME_EMIT_WARNING, .delivery = COO_CMD_RUNTIME_EMIT_BEST_EFFORT,
			.code = "atten_correction_rejected",
			.msg = selected < 0 ? "no valid attenuator fit" : (selected == 0 ?
				"all corrections rejected; retaining base fit" : "full correction rejected; refitted with fewer terms"),
			.context = context,
		});
	}
	return selected < 0 ? -ERANGE : 0;
}

/** Fit one physical FVOA's retained records to the firmware attenuator model. */
static int fit_one_physical(uint8_t physical,
			    struct attenuator_calibration_fit_metrics *out)
{
	struct attenuator snapshot;
	attenuator_snapshot(&attenuators[cal.attenuator_index], &snapshot);
	const struct attenuator *atten = &snapshot;
	const struct atten_cal_record *records = cal.records[physical];
	double gain = physical == 0U ? atten->coeff1.gain : atten->coeff2.gain;
	double fvoa_50pct_mv = 0.0;
	double slope_inv_fvoa_mv = 0.0;
	double max_atten_db = 0.0;
	double max_atten_sigma_db = 0.0;
	double min_tx = 1.0;
	double max_tx = 0.0;
	double min_x = (double)ATTENUATOR_DRIVE_MAX_MV * gain;
	double max_x = 0.0;
	double sum_sq_db = 0.0;
	double max_abs_db = 0.0;
	double sum_model = 0.0;
	double sum_measured = 0.0;
	double sum_model_model = 0.0;
	double sum_measured_measured = 0.0;
	double sum_model_measured = 0.0;
	uint8_t point_count = 0U;
	uint8_t scored_count = 0U;
	struct attenuator_model_coeffs coeffs;
	int rc;

	if (out == NULL || physical >= ATTENUATOR_PHYSICAL_COUNT || !(gain > 0.0)) {
		return -EINVAL;
	}
	memset(out, 0, sizeof(*out));

	rc = build_fit_points(physical, cal_fit_points, &point_count);
	if (rc != 0) {
		return rc;
	}
	rc = estimate_max_atten_db(cal_fit_points, point_count,
				   &max_atten_db, &max_atten_sigma_db);
	if (rc != 0) {
		return rc;
	}
	/* The full sweep supplies the leakage floor. Include the first measured
	 * point above the operating limit to anchor both fits across that boundary,
	 * rather than extrapolating from the last point below it. Keep all raw records.
	 */
	for (uint8_t i = 0U; i < point_count; ++i) {
		if ((double)cal_fit_points[i].measured_db > ATTENUATOR_CALIBRATED_MAX_DB) {
			point_count = i + 1U;
			break;
		}
	}
	if (point_count < ATTEN_CAL_MIN_FIT_POINTS) return -ERANGE;
	rc = fit_optimize_db(cal_fit_points, records, point_count, gain,
			     max_atten_db, max_atten_sigma_db,
			     &fvoa_50pct_mv, &slope_inv_fvoa_mv);
	if (rc != 0) {
		return rc;
	}

	coeffs = (struct attenuator_model_coeffs) {
		.fvoa_50pct_mv = fvoa_50pct_mv,
		.slope_inv_fvoa_mv = slope_inv_fvoa_mv,
		.max_atten_db = max_atten_db,
		.gain = gain,
	};
	rc = fit_correction_coeff(cal_fit_points, records, point_count,
					 max_atten_sigma_db, &coeffs);
	if (rc != 0) return rc;

	for (uint8_t i = 0U; i < point_count; ++i) {
		const struct atten_cal_fit_point *point = &cal_fit_points[i];
		const struct atten_cal_record *record = &records[point->record_index];
		struct atten_model_eval eval;
		double measured_db = (double)point->measured_db;
		double x = (double)record->sweep_mv * gain;
		double tx = pow(10.0, -measured_db / 10.0);

		/* Spans and point count describe all fitting support, including the anchor. */
		min_tx = MIN(min_tx, tx);
		max_tx = MAX(max_tx, tx);
		min_x = MIN(min_x, x);
		max_x = MAX(max_x, x);
		/* Score by MEASURED attenuation. A bad prediction above the limit must
		 * still increase RMS for a measurement inside the calibrated region.
		 */
		if (measured_db > coeffs.max_calibrated_db) continue;
		if (!atten_model_eval(&coeffs, record->sweep_mv, &eval)) return -ERANGE;
		double residual_db = eval.db - measured_db;
		if (!isfinite(residual_db)) return -ERANGE;
		scored_count++;
		sum_sq_db += residual_db * residual_db;
		max_abs_db = MAX(max_abs_db, fabs(residual_db));
		sum_model += eval.db;
		sum_measured += measured_db;
		sum_model_model += eval.db * eval.db;
		sum_measured_measured += measured_db * measured_db;
		sum_model_measured += eval.db * measured_db;
		if (atomic_get(&cal_fit_cancel)) return -ECANCELED;
	}

	/* The fit already met its support-count minimum. Correlation needs two
	 * scored points; excluding the boundary anchor must not raise that minimum.
	 */
	if (scored_count < 2U || !(max_x > min_x)) {
		return -ERANGE;
	}
	{
		double denom_model = (double)scored_count * sum_model_model -
				     sum_model * sum_model;
		double denom_measured = (double)scored_count * sum_measured_measured -
					sum_measured * sum_measured;

		if (!(denom_model > 0.0) || !(denom_measured > 0.0)) {
			return -ERANGE;
		}
		out->correlation = ((double)scored_count * sum_model_measured -
				    sum_model * sum_measured) /
				   sqrt(denom_model * denom_measured);
	}

	out->valid = true;
	out->points = point_count;
	out->fvoa_50pct_mv = fvoa_50pct_mv;
	out->slope_inv_fvoa_mv = slope_inv_fvoa_mv;
	out->max_atten_db = max_atten_db;
	out->max_calibrated_db = coeffs.max_calibrated_db;
	out->max_atten_sigma_db = max_atten_sigma_db;
	out->rms_db = sqrt(sum_sq_db / (double)scored_count);
	out->max_abs_db = max_abs_db;
	out->min_tx = min_tx;
	out->max_tx = max_tx;
	out->fvoa_span_mv = max_x - min_x;
	memcpy(out->correction_coeff, coeffs.correction_coeff,
	       sizeof(out->correction_coeff));
	out->accepted = isfinite(out->correlation) &&
			out->correlation >= ATTEN_CAL_MIN_FIT_CORR &&
			isfinite(out->fvoa_50pct_mv) &&
			out->fvoa_50pct_mv > 0.0 &&
			out->slope_inv_fvoa_mv > 0.0 &&
			isfinite(out->max_atten_db) &&
			out->max_atten_db > 0.0;
	return out->accepted ? 0 : -ERANGE;
}

/** Apply accepted pair fits and their final residual RMS together.
 * May block on DAC I/O and optional NVS persistence. Rejected fits leave both
 * the previous coefficients and their uncertainty untouched; no extra sweep.
 */
static int apply_fit_to_settings_locked(void)
{
	struct attenuator snapshot;
	attenuator_snapshot(&attenuators[cal.attenuator_index], &snapshot);
	const struct attenuator *atten = &snapshot;
	struct app_attenuator_channel_settings stored = {0};
	struct attenuator_model_coeffs physical[ATTENUATOR_PHYSICAL_COUNT] = {
		{
			.fvoa_50pct_mv = cal.fit[0].fvoa_50pct_mv,
			.slope_inv_fvoa_mv = cal.fit[0].slope_inv_fvoa_mv,
			.max_atten_db = cal.fit[0].max_atten_db,
			.max_calibrated_db = cal.fit[0].max_calibrated_db,
			.rms_db = cal.fit[0].rms_db,
			.gain = atten->coeff1.gain,
		},
		{
			.fvoa_50pct_mv = cal.fit[1].fvoa_50pct_mv,
			.slope_inv_fvoa_mv = cal.fit[1].slope_inv_fvoa_mv,
			.max_atten_db = cal.fit[1].max_atten_db,
			.max_calibrated_db = cal.fit[1].max_calibrated_db,
			.rms_db = cal.fit[1].rms_db,
			.gain = atten->coeff2.gain,
		},
	};

	/* Copy the complete basis; no terms may disappear when a fit is installed. */
	for (uint8_t i = 0; i < ATTENUATOR_PHYSICAL_COUNT; ++i) {
		memcpy(physical[i].correction_coeff, cal.fit[i].correction_coeff,
		       sizeof(physical[i].correction_coeff));
	}

	if (!cal.fit[0].accepted || !cal.fit[1].accepted ||
	    !attenuator_model_coefficients_valid(physical)) {
		return -EINVAL;
	}
	if (attenuator_apply_coefficients_preserve_db(&attenuators[cal.attenuator_index], physical) != 0) {
		return -EIO;
	}

	stored.physical[0].fvoa_50pct_mv = physical[0].fvoa_50pct_mv;
	stored.physical[0].slope_inv_fvoa_mv = physical[0].slope_inv_fvoa_mv;
	stored.physical[0].max_atten_db = physical[0].max_atten_db;
	stored.physical[0].max_calibrated_db = physical[0].max_calibrated_db;
	stored.physical[0].gain = physical[0].gain;
	stored.physical[0].rms_db = physical[0].rms_db;
	memcpy(stored.physical[0].correction_coeff, physical[0].correction_coeff,
	       sizeof(stored.physical[0].correction_coeff));
	stored.physical[1].fvoa_50pct_mv = physical[1].fvoa_50pct_mv;
	stored.physical[1].slope_inv_fvoa_mv = physical[1].slope_inv_fvoa_mv;
	stored.physical[1].max_atten_db = physical[1].max_atten_db;
	stored.physical[1].max_calibrated_db = physical[1].max_calibrated_db;
	stored.physical[1].gain = physical[1].gain;
	stored.physical[1].rms_db = physical[1].rms_db;
	memcpy(stored.physical[1].correction_coeff, physical[1].correction_coeff,
	       sizeof(stored.physical[1].correction_coeff));
	app_settings_update_attenuator_channel(cal.attenuator_index, &stored, cal.persistent);
	return 0;
}

/** Fit immutable records on the existing throughput thread, without cal_lock.
 * Start/stop commands cancel and join this calculation before changing its data.
 * Lowest application priority lets health, UART polling and logging preempt the
 * math. Change/restore priority with no mutex held, avoiding priority inheritance.
 */
static void auto_fit(void)
{
	int priority = k_thread_priority_get(k_current_get());
	int first_error = 0;
	bool all_accepted = true;

	k_thread_priority_set(k_current_get(), K_LOWEST_APPLICATION_THREAD_PRIO);
	for (uint8_t physical = 0U; physical < ATTENUATOR_PHYSICAL_COUNT; ++physical) {
		struct attenuator_calibration_fit_metrics fit = {0};
		if (atomic_get(&cal_fit_cancel)) break;
		int rc = fit_one_physical(physical, &fit);
		if (atomic_get(&cal_fit_cancel)) break;

		k_mutex_lock(&cal_lock, K_FOREVER);
		cal.fit[physical] = fit;
		publish_status_locked(NULL);
		k_mutex_unlock(&cal_lock);
		/* Only this owner mutates cal until fitting signals completion. */
		atten_cal_emit_fit(physical, &fit);
		if (rc != 0 && first_error == 0) {
			first_error = rc;
		}
		all_accepted = all_accepted && fit.accepted;
	}
	k_thread_priority_set(k_current_get(), priority);

	k_mutex_lock(&cal_lock, K_FOREVER);
	if (atomic_get(&cal_fit_cancel)) {
		cal.state = ATTEN_CAL_STATE_INACTIVE;
		atten_cal_emit_simple("stop");
	} else if (!all_accepted) {
		cal.last_error = first_error == 0 ? -ERANGE : first_error;
		cal.state = ATTEN_CAL_STATE_COMPLETE;
		atten_cal_emit_simple("complete");
	} else {
		/* Cancellation and installation serialize here. A later stop cannot
		 * undo coefficients that have already been installed.
		 */
		int rc = apply_fit_to_settings_locked();

		cal.last_error = rc;
		cal.state = rc == 0 ? ATTEN_CAL_STATE_COMPLETE : ATTEN_CAL_STATE_ERROR;
		atten_cal_emit_simple(rc == 0 ? "complete" : "error");
	}
	cal.phase = ATTEN_CAL_PHASE_NONE;
	publish_status_locked(NULL);
	/* Signal before unlocking so a new start cannot reset the semaphore first. */
	k_sem_give(&cal_fit_done);
	k_mutex_unlock(&cal_lock);
}

/** Advance calibration when the PD has filled its post-change window. */
static void auto_tick_locked(const struct photodiode_status *pd_status)
{
	const struct photodiode_window_result *window;
	struct atten_cal_measurement measurement = {0};

	if (cal.state != ATTEN_CAL_STATE_RUNNING || cal.mode != ATTEN_CAL_MODE_TIB_AUTO) {
		return;
	}

	switch (cal.phase) {
		case ATTEN_CAL_PHASE_WAIT_WINDOW:
			if (pd_status == NULL) {
				auto_error_locked(-EINVAL);
				return;
			}

			/* Source health and confirmed emission are acquisition preconditions,
			 * separate from the current-based numerical power estimate. An
			 * unconfirmed source permits passive streaming, not calibration.
			 * No hardware I/O here.
			 */
			bool powered;
			int power_rc = housekeeping_power_get_confirmed((enum housekeeping_power_output)cal.channel, &powered);
			if (power_rc != 0 || !powered) {
				auto_error_locked(power_rc != 0 ? power_rc : -EIO);
				return;
			}
			bool emitting;
			int source_rc = hispec_laser_output_status(cal.laser, &emitting);
			if (source_rc != 0 || !emitting) {
				auto_error_locked(source_rc != 0 ? source_rc : -EIO);
				return;
			}

			window = &pd_status->channel[cal.channel].configurable_window;
			if (window->end_ms <= cal.window_started_ms ||
			    window->sample_length < cal.dwell_ms / PHOTODIODE_SAMPLE_INTERVAL_MS) {
				return;
			}
			build_measurement_from_pd_window(window, &measurement);

			switch (cal.measure_kind) {
				case ATTEN_CAL_MEASURE_INITIAL_PROBE:
					auto_handle_initial_probe_locked(&measurement);
					break;
				case ATTEN_CAL_MEASURE_SWEEP:
					auto_handle_sweep_locked(&measurement);
					break;
				case ATTEN_CAL_MEASURE_BRIDGE_PROBE:
					auto_handle_bridge_probe_locked(&measurement);
					break;
				default:
					auto_error_locked(-EINVAL);
					break;
			}
			break;
		case ATTEN_CAL_PHASE_NONE:
		default:
				return;
	}
}

/** Cancel/join numerical work before a command changes its input. Called with
 * cal_lock held and returns with it held. Start/stop callers share the command
 * executor; the semaphore wait releases the lock so fitting can finish at low
 * priority without inheriting the command thread's priority.
 */
static void cancel_fit_locked(void)
{
	if (cal.phase != ATTEN_CAL_PHASE_FITTING) return;
	atomic_set(&cal_fit_cancel, 1);
	k_mutex_unlock(&cal_lock);
	(void)k_sem_take(&cal_fit_done, K_FOREVER);
	k_mutex_lock(&cal_lock, K_FOREVER);
}

/** Start automatic TIB calibration, replacing the retained dataset in place. */
int attenuator_calibration_start_auto(
	const struct attenuator_calibration_auto_request *request,
	struct attenuator_calibration_status *status)
{
	bool replacing;
	bool stop_failed = false;
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
	cancel_fit_locked();
	if (cal.shutdown_pending) {
		rc = hispec_laser_stop_output(cal.laser, false);
		if (rc != 0) {
			publish_status_locked(status);
			k_mutex_unlock(&cal_lock);
			return rc;
		}
		cal.shutdown_pending = false;
	}
	if (cal.state == ATTEN_CAL_STATE_RUNNING) {
		cal.phase = ATTEN_CAL_PHASE_NONE;
		/* A replacement on another laser must first release the old source.
		 * Retain that identity if shutdown fails; do not start a second source.
		 */
		if (cal.laser != request->laser) {
			rc = hispec_laser_stop_output(cal.laser, false);
			if (rc != 0) {
				cal.shutdown_pending = true;
				cal.state = ATTEN_CAL_STATE_ERROR;
				cal.last_error = rc;
				housekeeping_photodiode_autooff_inhibit((enum housekeeping_power_output)cal.channel, false);
				atten_cal_emit_simple("error");
				publish_status_locked(status);
				k_mutex_unlock(&cal_lock);
				return rc;
			}
		}
		if (cal.channel != request->channel) {
			housekeeping_photodiode_autooff_inhibit((enum housekeeping_power_output)cal.channel, false);
		}
	}
	/* The accepted start is the only dataset reset. Failed setup now belongs
	 * to this new run; stop/error cleanup never erases its retained records.
	 */
	memset(&cal, 0, sizeof(cal));
	cal.state = ATTEN_CAL_STATE_RUNNING;
	cal.mode = ATTEN_CAL_MODE_TIB_AUTO;
	cal.attenuator_index = request->attenuator_index;
	cal.dwell_ms = request->dwell_ms < ATTEN_CAL_MIN_DWELL_MS
		? ATTEN_CAL_DEFAULT_DWELL_MS : MIN(request->dwell_ms, ATTEN_CAL_MAX_DWELL_MS);
	cal.persistent = request->persist;
	cal.laser = request->laser;
	cal.channel = request->channel;
	cal.other_mv = ATTENUATOR_DRIVE_MAX_MV;
	publish_status_locked(NULL);
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
	rc = throughput_monitor_stop(PHOTODIODE_CHANNEL_COUNT, NULL);
	if (rc != 0) goto failed_start;
	/* Recheck power after taking ownership: stopping throughput may have
	 * released an old deadline. No acquisition begins unless PD is still on.
	 */
	housekeeping_photodiode_autooff_inhibit((enum housekeeping_power_output)request->channel, true);
	bool powered;
	rc = housekeeping_power_get((enum housekeeping_power_output)request->channel, &powered);
	if (rc != 0 || !powered) {
		rc = rc != 0 ? rc : -EIO;
		goto failed_start;
	}

	rc = mems_router_apply_named_route(&router, request->route_input, request->output, false, NULL, NULL);
	if (rc == 0) {
		rc = mems_router_apply_named_route(&router, request->pd_input, request->pd_output, false, NULL, NULL);
	}
	if (rc != 0) goto failed_start;

	if (!set_physical_pair(request->attenuator_index, 0U, 0, ATTENUATOR_DRIVE_MAX_MV)) {
		rc = -EIO;
		goto failed_start;
	}

	rc = hispec_laser_stop_output(request->laser, false);
	if (rc != 0) {
		stop_failed = true;
		goto failed_start;
	}

	k_mutex_lock(&cal_lock, K_FOREVER);
	atten_cal_emit_simple("start");
	auto_start_next_physical_locked();
	publish_status_locked(status);
	k_mutex_unlock(&cal_lock);
	return 0;

failed_start:
	housekeeping_photodiode_autooff_inhibit((enum housekeeping_power_output)request->channel, false);
	k_mutex_lock(&cal_lock, K_FOREVER);
	cal.state = ATTEN_CAL_STATE_ERROR;
	cal.phase = ATTEN_CAL_PHASE_NONE;
	cal.last_error = rc;
	/* No retry of a failed stop here. Earlier setup failures still release a
	 * possibly emitting source, then keep any failed shutdown for explicit retry.
	 */
	cal.shutdown_pending = stop_failed || hispec_laser_stop_output(request->laser, false) != 0;
	atten_cal_emit_simple("error");
	publish_status_locked(status);
	k_mutex_unlock(&cal_lock);
	return rc;
}

/** Cancel sequencing/fitting and stop its source, retaining records and fit results. */
int attenuator_calibration_stop(struct attenuator_calibration_status *status)
{
	k_mutex_lock(&cal_lock, K_FOREVER);
	cancel_fit_locked();
	if (cal.state == ATTEN_CAL_STATE_RUNNING || cal.shutdown_pending) {
		cal.phase = ATTEN_CAL_PHASE_NONE;
		housekeeping_photodiode_autooff_inhibit((enum housekeeping_power_output)cal.channel, false);
		int rc = hispec_laser_stop_output(cal.laser, false);
		cal.shutdown_pending = rc != 0;
		if (rc != 0) {
			cal.state = ATTEN_CAL_STATE_ERROR;
			cal.last_error = rc;
			atten_cal_emit_simple("error");
			publish_status_locked(status);
			k_mutex_unlock(&cal_lock);
			return rc;
		}
		cal.laser_percent = 0;
	}
	if (cal.state != ATTEN_CAL_STATE_INACTIVE) {
		atten_cal_emit_simple("stop");
	}
	cal.state = ATTEN_CAL_STATE_INACTIVE;
	cal.phase = ATTEN_CAL_PHASE_NONE;
	publish_status_locked(status);
	k_mutex_unlock(&cal_lock);
	return 0;
}

/** Copy the last completed owner update without waiting for acquisition or fitting. */
void attenuator_calibration_get_status(struct attenuator_calibration_status *status)
{
	if (status == NULL) {
		return;
	}
	k_mutex_lock(&cal_status_lock, K_FOREVER);
	*status = cal_status;
	k_mutex_unlock(&cal_status_lock);
}

/** Report whether calibration currently owns attenuator sequencing. */
bool attenuator_calibration_active(void)
{
	bool active;

	k_mutex_lock(&cal_status_lock, K_FOREVER);
	active = strcmp(cal_status.state, "running") == 0;
	k_mutex_unlock(&cal_status_lock);
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
		"\"max_atten_db\":%.12g,\"max_calibrated_db\":%.9g,\"max_atten_sigma_db\":%.6g,"
		"\"correction_coeff\":[%.9g,%.9g,%.9g,%.9g,%.9g,%.9g],"
		"\"corr\":%.6g,\"rms_db\":%.6g,\"max_abs_db\":%.6g}",
		fit->accepted ? "true" : "false", fit->points,
		fit->fvoa_50pct_mv, fit->slope_inv_fvoa_mv,
		fit->max_atten_db, fit->max_calibrated_db, fit->max_atten_sigma_db,
		(double)fit->correction_coeff[0],
		(double)fit->correction_coeff[1],
		(double)fit->correction_coeff[2],
		(double)fit->correction_coeff[3],
		(double)fit->correction_coeff[4],
		(double)fit->correction_coeff[5],
		fit->correlation, fit->rms_db, fit->max_abs_db);
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
		"\"mv\":%.3f,\"other_mv\":%.3f,\"error\":%d",
		status->state != NULL ? status->state : "inactive",
		status->mode != NULL ? status->mode : "none",
		status->physical != NULL ? status->physical : "dac1",
		status->fit != NULL ? status->fit : "none",
		status->point_count, status->dwell_ms, status->complete_pct,
		MIN(status->point_index + 1U, status->point_count),
		status->point_count,
		(double)status->current_mv, (double)status->other_mv, status->last_error) != 0 ||
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

/** Copy calibration dataset role metadata into the host binary format. */
int attenuator_calibration_write_data_metadata(void *payload,
					       size_t payload_len,
					       uint8_t physical_index,
					       size_t *written)
{
	uint8_t *bytes = payload;
	uint8_t total;
	uint8_t bridge_count;
	uint8_t chunk_count;
	size_t byte_count;

	if (payload == NULL || written == NULL ||
	    payload_len < ATTEN_CAL_DATA_METADATA_HEADER_SIZE ||
	    physical_index >= ATTENUATOR_PHYSICAL_COUNT) {
		return -EINVAL;
	}
	*written = 0U;

	k_mutex_lock(&cal_lock, K_FOREVER);
	total = cal.record_count[physical_index];
	bridge_count = cal.bridge_count[physical_index];
	chunk_count = (uint8_t)(((uint16_t)total + ATTEN_CAL_DATA_RECORDS_PER_CHUNK - 1U) /
				ATTEN_CAL_DATA_RECORDS_PER_CHUNK);
	byte_count = ATTEN_CAL_DATA_METADATA_HEADER_SIZE +
		     ((size_t)bridge_count * ATTEN_CAL_DATA_BRIDGE_ENTRY_SIZE);
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
	bytes[5] = ATTEN_CAL_DATA_KIND_METADATA;
	bytes[6] = physical_index;
	bytes[7] = (uint8_t)cal.state;
	bytes[8] = (uint8_t)cal.mode;
	bytes[9] = cal.fit[physical_index].valid ? 1U : 0U;
	bytes[10] = cal.fit[physical_index].accepted ? 1U : 0U;
	bytes[11] = cal.record_overflow[physical_index] ? 1U : 0U;
	bytes[12] = ATTEN_CAL_DATA_CHUNK_RECORD_SIZE;
	bytes[13] = (uint8_t)ATTEN_CAL_DATA_RECORDS_PER_CHUNK;
	bytes[14] = total;
	bytes[15] = chunk_count;
	bytes[16] = cal.reference_record_index_valid[physical_index] ? 1U : 0U;
	bytes[17] = cal.reference_record_index[physical_index];
	bytes[18] = bridge_count;
	for (uint8_t i = 0U; i < bridge_count; ++i) {
		const struct atten_cal_bridge *bridge = &cal.bridges[physical_index][i];
		size_t off = ATTEN_CAL_DATA_METADATA_HEADER_SIZE +
			     ((size_t)i * ATTEN_CAL_DATA_BRIDGE_ENTRY_SIZE);

		bytes[off] = bridge->before_record_index;
		bytes[off + 1U] = bridge->after_record_index;
	}
	*written = byte_count;
	k_mutex_unlock(&cal_lock);
	return 0;
}

/** Copy retained calibration records into one fixed host binary chunk. */
int attenuator_calibration_write_record_chunk(void *payload,
					      size_t payload_len,
					      uint8_t physical_index,
					      uint8_t chunk_index,
					      size_t *written)
{
	uint8_t *bytes = payload;
	uint8_t total;
	uint8_t start_index;
	uint8_t count;
	uint8_t chunk_count;
	size_t byte_count;

	if (payload == NULL || written == NULL ||
	    physical_index >= ATTENUATOR_PHYSICAL_COUNT) {
		return -EINVAL;
	}
	*written = 0U;

	k_mutex_lock(&cal_lock, K_FOREVER);
	total = cal.record_count[physical_index];
	chunk_count = (uint8_t)(((uint16_t)total + ATTEN_CAL_DATA_RECORDS_PER_CHUNK - 1U) /
				ATTEN_CAL_DATA_RECORDS_PER_CHUNK);
	if (chunk_index >= chunk_count) {
		k_mutex_unlock(&cal_lock);
		return -ERANGE;
	}
	start_index = (uint8_t)(chunk_index * ATTEN_CAL_DATA_RECORDS_PER_CHUNK);
	count = (uint8_t)MIN((uint16_t)ATTEN_CAL_DATA_RECORDS_PER_CHUNK,
			     (uint16_t)total - start_index);
	byte_count = (size_t)count * ATTEN_CAL_DATA_CHUNK_RECORD_SIZE;
	if (byte_count > payload_len) {
		k_mutex_unlock(&cal_lock);
		return -ENOSPC;
	}

	for (uint8_t i = 0U; i < count; ++i) {
		write_record_wire(bytes + ((size_t)i * ATTEN_CAL_DATA_CHUNK_RECORD_SIZE),
				  &cal.records[physical_index][start_index + i]);
	}
	*written = byte_count;
	k_mutex_unlock(&cal_lock);
	return 0;
}

/** Public tick hook called by the throughput monitor thread. */
void attenuator_calibration_tick(const struct photodiode_status *pd_status)
{
	k_mutex_lock(&cal_lock, K_FOREVER);
	auto_tick_locked(pd_status);
	bool fitting = cal.phase == ATTEN_CAL_PHASE_FITTING;
	publish_status_locked(NULL);
	k_mutex_unlock(&cal_lock);
	if (fitting) auto_fit();
}
