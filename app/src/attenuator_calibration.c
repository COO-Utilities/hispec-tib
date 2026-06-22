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
 * saturated low-DAC side and a more-attenuated high-DAC side, while separately
 * remembering the lowest usable companion DAC candidate.
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
#include <zephyr/sys/byteorder.h>
#include <zephyr/sys/util.h>

#include "app_settings.h"
#include "attenuator.h"
#include "command.h"
#include "devices.h"
#include "mems_switching.h"
#include "throughput_monitor.h"

LOG_MODULE_REGISTER(attenuator_calibration, LOG_LEVEL_INF);

#define ATTEN_CAL_DEFAULT_DWELL_MS 400U
#define ATTEN_CAL_MIN_DWELL_MS 100U
#define ATTEN_CAL_MAX_DWELL_MS 2000U
/* One ADC cycle is  under 3 ms; the pad prevents reading a partial window. */
#define ATTEN_CAL_ADC_SAMPLE_INTERVAL_PAD_MS 4
#define ATTEN_CAL_ADC_LSB_MV 0.1875f
#define ATTEN_CAL_ADC_CLIP_MV 5000.0f
/* Minimum bracket width for companion-FVOA binary searches. */
#define ATTEN_CAL_SEARCH_MIN_STEP_MV 5.0f
/* Fixed DUT-FVOA sweep spacing after the initial open-reference point. */
#define ATTEN_CAL_SWEEP_STEP_MV 50.0f
#define ATTEN_CAL_MAX_SEARCH_TRIES 16U
#define ATTEN_CAL_MIN_FIT_POINTS ATTENUATOR_CAL_MIN_FIT_POINTS
#define ATTEN_CAL_MIN_TX 1.0e-10
#define ATTEN_CAL_MAX_TX 0.999999
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
	int64_t wait_until_ms;
	int last_error;
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
static struct coo_cmd_response cal_telemetry_msg;
static struct atten_cal_fit_point cal_fit_points[ATTENUATOR_CAL_RECORD_COUNT];

static void copy_status_locked(struct attenuator_calibration_status *status);
static void auto_schedule_measure_locked(enum atten_cal_measure_kind kind, float sweep_mv, float other_mv);
static void auto_begin_bridge_locked(void);
static void auto_close_bridge_locked(uint8_t index);
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
			    "\"max_atten_db\":%.12g,"
			    "\"max_atten_sigma_db\":%.12g,"
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
			    fit != NULL ? (double)fit->max_atten_sigma_db : (double)NAN,
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

	/**
	 * Decide whether a photodiode window is pinned against the ADC rail.
	 *
	 * Saturation is based on the mean, not the max excursion: a noisy rail sample is
	 * diagnostic, but a saturated diode has the whole averaging window at the wall.
	 */
	saturated = (float) window->mean_net_mv >= ATTEN_CAL_ADC_CLIP_MV;

	if (!(measurement->signal_err_mv > 0.0f) || !isfinite(measurement->signal_err_mv)) {
		measurement->signal_err_mv = ATTEN_CAL_ADC_LSB_MV;
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

/** Reset all calibration state while preserving the requested top-level state. */
static void reset_locked(enum atten_cal_state state)
{
	memset(&cal, 0, sizeof(cal));
	cal.state = state;
	cal.mode = ATTEN_CAL_MODE_NONE;
	cal.phase = ATTEN_CAL_PHASE_NONE;
	cal.other_mv = ATTENUATOR_DRIVE_MAX_MV;
	cal.laser_level_index = 0;
	cal.dwell_ms = ATTEN_CAL_DEFAULT_DWELL_MS;
	cal.laser_percent = initial_laser_levels_pct[0];
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
	status->dwell_ms = cal.dwell_ms - ATTEN_CAL_ADC_SAMPLE_INTERVAL_PAD_MS;
	status->complete_pct = complete_percent_locked();
	status->current_mv = cal.sweep_mv;
	status->other_mv = cal.other_mv;
	status->last_error = cal.last_error;
	status->laser_percent = cal.laser_percent;
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
 * low side of this bracket is therefore the too-bright/saturated side; the high
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
 * Saturated means the companion needs more attenuation, so drive voltage low side moves up.
 * Below-SNR means the companion is too attenuated, so the drive high voltage moves down.
 * A usable measurement becomes the current candidate and the search keeps going to lower attenuation
 * to find the brightest non-saturated point.
 */
static bool companion_search_note_measurement_locked(enum atten_cal_record_classification classification, uint8_t index)
{

	if (classification == ATTEN_CAL_CLASSIFICATION_SATURATED) {
		cal.search_low_mv = cal.other_mv;
	} else if (classification == ATTEN_CAL_CLASSIFICATION_OK) {
		cal.search_high_mv = cal.other_mv;
		cal.search_candidate_mv = cal.other_mv;
		cal.search_candidate_valid = true;
		cal.search_candidate_record_index = index;
	} else if (classification == ATTEN_CAL_CLASSIFICATION_BELOW_SNR) {
		cal.search_high_mv = cal.other_mv;
	} else {
		return false;
	}
	cal.search_tries++;
	return true;
}

/** Set DAC voltages for a measurement and wait one photodiode configurable window. */
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
	atten_cal_emit_set(event);
	cal.wait_until_ms = k_uptime_get() + cal.dwell_ms;
	cal.phase = ATTEN_CAL_PHASE_WAIT_WINDOW;  // From here execution resumes at auto_tick_locked()
}

/** Initialize acquisition state for the current physical FVOA and schedule its first probe. */
static void auto_start_next_physical_locked(void)
{
	uint8_t physical = cal.physical_index;

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

	cal.record_count[physical] = 0U;
	cal.bridge_count[physical] = 0U;
	cal.record_overflow[physical] = false;

	memset(cal.records[physical], 0, sizeof(cal.records[physical]));
	memset(cal.bridges[physical], 0, sizeof(cal.bridges[physical]));
	memset(&cal.fit[physical], 0, sizeof(cal.fit[physical]));

	if (!auto_set_laser_level_locked(0U)) {
		return;
	}
	/* The first scheduled window provides the settling delay after changing laser level. */
	auto_schedule_measure_locked(ATTEN_CAL_MEASURE_INITIAL_PROBE, 0.0f, ATTENUATOR_DRIVE_MAX_MV);
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

/** Handle companion-search probes used to find the initial usable open reference. */
static void auto_handle_initial_probe_locked(const struct atten_cal_measurement *measurement)
{
	struct atten_cal_record *record = NULL;

	(void)append_record_locked(ATTEN_CAL_EVENT_INITIAL_PROBE, measurement, &record);
	if (record == NULL) {
		auto_error_locked(-ENOSPC);
		return;
	}

	if (!companion_search_note_measurement_locked(record->classification, cal.point_index)) {
		auto_error_locked(record->classification == ATTEN_CAL_CLASSIFICATION_ADC_ERROR ? -EIO : -ERANGE);
		return;
	}

	if (record->classification == ATTEN_CAL_CLASSIFICATION_SATURATED &&
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

		/* The next scheduled window provides the settling delay after changing laser level. */
		companion_search_begin_locked(0.0f, ATTENUATOR_DRIVE_MAX_MV);
		auto_schedule_measure_locked(ATTEN_CAL_MEASURE_INITIAL_PROBE, 0.0f, ATTENUATOR_DRIVE_MAX_MV);

		return;
	}


	if (cal.search_high_mv - cal.search_low_mv <= ATTEN_CAL_SEARCH_MIN_STEP_MV ||
	    cal.search_tries >= ATTEN_CAL_MAX_SEARCH_TRIES) {

		/* The bracket is narrow enough; adopt the brightest retained usable initial probe. */
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

	bool all_below_snr = true;
	uint8_t count = 0;
	/* Find the latest usable DUT point in the current segment. */
	for (uint8_t i = cal.record_count[physical]; i > 0U; --i) {
		const struct atten_cal_record *record = &cal.records[physical][i - 1U];

		if (record->segment == cal.segment_id && record->event == ATTEN_CAL_EVENT_POINT) {
			count++;
			if (record->classification != ATTEN_CAL_CLASSIFICATION_BELOW_SNR) {
				all_below_snr = false;
			}
			if (record->classification == ATTEN_CAL_CLASSIFICATION_OK) {
				anchor = record;
				anchor_index = i - 1U;
				break;
			}
		}
	}

	if (anchor == NULL) {
		if (all_below_snr && count == 1U && cal.segment_id > 0U) {
			LOG_INF("No bridge anchor found in segment %u, only one faint point. Odd. assuming done.",
				cal.segment_id);
			auto_finish_physical_locked();
			return;
		}
		/** all sweep points in previous segment were saturated or below sn limit (and yet not at max drive) */
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

	if (!companion_search_note_measurement_locked(record->classification, cal.point_index)) {
		auto_error_locked(record->classification == ATTEN_CAL_CLASSIFICATION_ADC_ERROR ? -EIO : -ERANGE);
		return;
	}

	if (cal.search_high_mv - cal.search_low_mv <= ATTEN_CAL_SEARCH_MIN_STEP_MV ||
	    cal.search_tries >= ATTEN_CAL_MAX_SEARCH_TRIES) {

		/* The bracket is narrow enough; adopt the brightest retained usable bridge probe. */

		uint8_t bridge_index = cal.search_candidate_record_index;
		if (!cal.search_candidate_valid) {
			// Search again with a floor > cal.search_candidate_mv

			float search_floor = 0;
			for (uint8_t i = 1; i <= cal.point_index; ++i) {
				const struct atten_cal_record *candidate_record =
					&cal.records[cal.physical_index][cal.point_index - i];

				if (candidate_record->event == ATTEN_CAL_EVENT_BRIDGE_PROBE &&
				    candidate_record->segment == cal.segment_id) {
					if (candidate_record->classification == ATTEN_CAL_CLASSIFICATION_SATURATED) {
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
				// somehow no good bridge probe as all were saturated, try again with a higher attenuation floor.
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
static int build_segment_scales_locked(uint8_t physical,
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
static int build_fit_points_locked(uint8_t physical,
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

		/** Decide whether a normalized transmission lies in the invertible fit domain. */
		if (tx < ATTEN_CAL_MIN_TX || tx > ATTEN_CAL_MAX_TX) {
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
		sum += points[i].measured_db;
	}
	mean = sum / (double)count;
	for (uint8_t i = point_count - count; i < point_count; ++i) {
		double delta = points[i].measured_db - mean;

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

/** Evaluate the physical attenuator model in relative-attenuation dB. */
static double fit_model_db(double fvoa_50pct_mv, double slope_inv_fvoa_mv,
			   double max_atten_db, double gain, double dac_mv)
{
	const struct attenuator_model_coeffs coeffs = {
		.fvoa_50pct_mv = fvoa_50pct_mv,
		.slope_inv_fvoa_mv = slope_inv_fvoa_mv,
		.max_atten_db = max_atten_db,
		.gain = gain,
	};

	return attenuator_model_voltage_to_db(&coeffs, (float)dac_mv);
}

/** Estimate model dB sensitivity to DAC voltage for x-error propagation. */
static double fit_model_d_db_d_dac(double fvoa_50pct_mv, double slope_inv_fvoa_mv,
				   double max_atten_db, double gain, double dac_mv)
{
	double step = MAX((double)ATTEN_CAL_DAC_SIGMA_MV, 0.5);
	double lo = MAX(0.0, dac_mv - step);
	double hi = MIN((double)ATTENUATOR_DRIVE_MAX_MV, dac_mv + step);
	double db_lo;
	double db_hi;

	if (!(hi > lo)) {
		return 0.0;
	}
	db_lo = fit_model_db(fvoa_50pct_mv, slope_inv_fvoa_mv, max_atten_db, gain, lo);
	db_hi = fit_model_db(fvoa_50pct_mv, slope_inv_fvoa_mv, max_atten_db, gain, hi);
	if (!isfinite(db_lo) || !isfinite(db_hi)) {
		return 0.0;
	}
	return (db_hi - db_lo) / (hi - lo);
}

/** Estimate dB sensitivity to the fixed leakage-floor estimate. */
static double fit_model_d_db_d_max_atten(double fvoa_50pct_mv,
					 double slope_inv_fvoa_mv,
					 double max_atten_db,
					 double gain,
					 double dac_mv)
{
	double step = MAX(fabs(max_atten_db) * 1.0e-5, 0.01);
	double lo = MAX(ATTEN_CAL_MIN_DB_ERR, max_atten_db - step);
	double hi = max_atten_db + step;
	double db_lo;
	double db_hi;

	if (!(hi > lo)) {
		return 0.0;
	}
	db_lo = fit_model_db(fvoa_50pct_mv, slope_inv_fvoa_mv, lo, gain, dac_mv);
	db_hi = fit_model_db(fvoa_50pct_mv, slope_inv_fvoa_mv, hi, gain, dac_mv);
	if (!isfinite(db_lo) || !isfinite(db_hi)) {
		return 0.0;
	}
	return (db_hi - db_lo) / (hi - lo);
}

/** Return one weighted dB residual for the current fit parameters. */
static double fit_point_residual(const struct atten_cal_fit_point *point,
				 double fvoa_50pct_mv,
				 double slope_inv_fvoa_mv,
				 double max_atten_db,
				 double max_atten_sigma_db,
				 double gain)
{
	double model_db;
	double d_db_d_dac;
	double d_db_d_max;
	double sigma_db;

	if (point == NULL) {
		return NAN;
	}
	model_db = fit_model_db(fvoa_50pct_mv, slope_inv_fvoa_mv,
				max_atten_db, gain, point->dac_mv);
	d_db_d_dac = fit_model_d_db_d_dac(fvoa_50pct_mv, slope_inv_fvoa_mv,
					  max_atten_db, gain, point->dac_mv);
	d_db_d_max = fit_model_d_db_d_max_atten(fvoa_50pct_mv, slope_inv_fvoa_mv,
						max_atten_db, gain, point->dac_mv);
	sigma_db = sqrt(point->measured_db_err * point->measured_db_err +
			(d_db_d_dac * (double)ATTEN_CAL_DAC_SIGMA_MV) *
			(d_db_d_dac * (double)ATTEN_CAL_DAC_SIGMA_MV) +
			(d_db_d_max * max_atten_sigma_db) *
			(d_db_d_max * max_atten_sigma_db));
	if (!isfinite(model_db) || !(sigma_db > 0.0) || !isfinite(sigma_db)) {
		return NAN;
	}
	return (model_db - point->measured_db) /
	       MAX(sigma_db, ATTENUATOR_FIT_MIN_SIGMA_DB);
}

/** Sum weighted squared residuals for a complete fit candidate. */
static double fit_cost(const struct atten_cal_fit_point *points, uint8_t point_count,
		       double fvoa_50pct_mv, double slope_inv_fvoa_mv,
		       double max_atten_db, double max_atten_sigma_db,
		       double gain)
{
	double cost = 0.0;

	for (uint8_t i = 0U; i < point_count; ++i) {
		double residual = fit_point_residual(&points[i], fvoa_50pct_mv,
						     slope_inv_fvoa_mv,
						     max_atten_db,
						     max_atten_sigma_db,
						     gain);

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
	double max_fvoa_mv = (double)ATTENUATOR_DRIVE_MAX_MV * gain;
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
			   double max_atten_db, double max_atten_sigma_db,
			   double *fvoa_50pct_mv,
			   double *slope_inv_fvoa_mv)
{
	double max_fvoa_mv = (double)ATTENUATOR_DRIVE_MAX_MV * gain;
	double f50;
	double slope;
	double lambda = ATTEN_CAL_FIT_INITIAL_LAMBDA;
	double cost;

	if (fvoa_50pct_mv == NULL || slope_inv_fvoa_mv == NULL || point_count < ATTEN_CAL_MIN_FIT_POINTS) {
		return -EINVAL;
	}
	fit_initial_guess(points, point_count, gain, &f50, &slope);
	cost = fit_cost(points, point_count, f50, slope,
			max_atten_db, max_atten_sigma_db, gain);
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
			double r = fit_point_residual(&points[i], f50, slope,
						      max_atten_db,
						      max_atten_sigma_db,
						      gain);
			double r_f50 = fit_point_residual(&points[i],
							  f50 + step_f50,
							  slope,
							  max_atten_db,
							  max_atten_sigma_db,
							  gain);
			double r_slope = fit_point_residual(&points[i], f50,
							    slope + step_slope,
							    max_atten_db,
							    max_atten_sigma_db,
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
				      trial_slope, max_atten_db,
				      max_atten_sigma_db, gain);
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
	int rc;

	if (out == NULL || physical >= ATTENUATOR_PHYSICAL_COUNT || !(gain > 0.0)) {
		return -EINVAL;
	}
	memset(out, 0, sizeof(*out));

	rc = build_fit_points_locked(physical, cal_fit_points, &point_count);
	if (rc != 0) {
		return rc;
	}
	rc = estimate_max_atten_db(cal_fit_points, point_count,
				   &max_atten_db, &max_atten_sigma_db);
	if (rc != 0) {
		return rc;
	}
	rc = fit_optimize_db(cal_fit_points, point_count, gain,
			     max_atten_db, max_atten_sigma_db,
			     &fvoa_50pct_mv, &slope_inv_fvoa_mv);
	if (rc != 0) {
		return rc;
	}

	for (uint8_t i = 0U; i < point_count; ++i) {
		double model_db = fit_model_db(fvoa_50pct_mv, slope_inv_fvoa_mv,
					       max_atten_db, gain,
					       cal_fit_points[i].dac_mv);
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
	out->max_atten_db = max_atten_db;
	out->max_atten_sigma_db = max_atten_sigma_db;
	out->rms_db = sqrt(sum_sq_db / (double)point_count);
	out->max_abs_db = max_abs_db;
	out->min_tx = min_tx;
	out->max_tx = max_tx;
	out->fvoa_span_mv = max_x - min_x;
	out->accepted = isfinite(out->correlation) &&
			out->correlation >= ATTEN_CAL_MIN_FIT_CORR &&
			isfinite(out->fvoa_50pct_mv) &&
			out->fvoa_50pct_mv > 0.0 &&
			out->slope_inv_fvoa_mv > 0.0 &&
			isfinite(out->max_atten_db) &&
			out->max_atten_db > 0.0;
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
			.max_atten_db = cal.fit[0].max_atten_db,
			.gain = atten->coeff1.gain,
		},
		{
			.fvoa_50pct_mv = cal.fit[1].fvoa_50pct_mv,
			.slope_inv_fvoa_mv = cal.fit[1].slope_inv_fvoa_mv,
			.max_atten_db = cal.fit[1].max_atten_db,
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
	stored.physical[0].max_atten_db = physical[0].max_atten_db;
	stored.physical[0].gain = physical[0].gain;
	stored.physical[1].fvoa_50pct_mv = physical[1].fvoa_50pct_mv;
	stored.physical[1].slope_inv_fvoa_mv = physical[1].slope_inv_fvoa_mv;
	stored.physical[1].max_atten_db = physical[1].max_atten_db;
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

	if (cal.state != ATTEN_CAL_STATE_RUNNING || cal.mode != ATTEN_CAL_MODE_TIB_AUTO) {
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

	rc = mems_router_apply_named_route(&router, request->route_input, request->output, false, NULL, NULL);
	if (rc == 0) {
		rc = mems_router_apply_named_route(&router, request->pd_input, request->pd_output, false, NULL, NULL);
	}
	if (rc != 0) {
		return rc;
	}

	/** Clamp a requested dwell to the calibration-supported averaging interval. */
	uint32_t dwell_ms;
	dwell_ms = request->dwell_ms < ATTEN_CAL_MIN_DWELL_MS
		           ? ATTEN_CAL_DEFAULT_DWELL_MS
		           : MIN(request->dwell_ms, ATTEN_CAL_MAX_DWELL_MS);

	rc = photodiode_set_configurable_window_duration(request->channel, dwell_ms);
	if (rc != 0) {
		return rc;
	}

	if (!set_physical_pair(request->attenuator_index, 0U, 0, ATTENUATOR_DRIVE_MAX_MV)) {
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
	cal.dwell_ms = dwell_ms + ATTEN_CAL_ADC_SAMPLE_INTERVAL_PAD_MS;
	cal.persistent = request->persist;
	cal.laser = request->laser;
	cal.channel = request->channel;
	cal.laser_percent = 0;
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
		"\"max_atten_db\":%.12g,\"max_atten_sigma_db\":%.12g,"
		"\"corr\":%.12g,\"rms_db\":%.12g,\"max_abs_db\":%.12g,"
		"\"min_tx\":%.12g,\"max_tx\":%.12g,\"fvoa_span_mv\":%.6f}",
		fit->accepted ? "true" : "false", fit->points,
		fit->fvoa_50pct_mv, fit->slope_inv_fvoa_mv,
		fit->max_atten_db, fit->max_atten_sigma_db,
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
void attenuator_calibration_tick(const struct photodiode_status *pd_status,
				 int64_t now_ms)
{
	k_mutex_lock(&cal_lock, K_FOREVER);
	auto_tick_locked(pd_status, now_ms);
	k_mutex_unlock(&cal_lock);
}
