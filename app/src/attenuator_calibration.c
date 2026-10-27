/**
 * @file attenuator_calibration.c
 * @brief Bridge-normalized automatic FVOA attenuator calibration.
 *
 * This board-servicing routine runs on the existing throughput thread. It
 * sweeps the DUT from closed to 0 mV at fixed laser output. Below-SNR samples
 * are retained; the bright edge of the PD band triggers companion attenuation.
 * Raw peaks, not means, enforce the 2000 mV usable-input limit; the 1850 mV
 * search boundary leaves headroom without reclassifying usable measurements.
 *
 * Bridges hold a retained usable DUT point and close the companion until the
 * first measured usable probe. Before/after roles are indices into real records:
 * no confirmation windows, DUT backoff, copied records or assumed optical gains.
 * The accepted bridge-after record can anchor another bridge if no ordinary
 * point in the new segment is usable. Settling precedes each complete PD window.
 *
 * Source-off windows bracket the run. Postprocessing removes their midpoint
 * residual and carries their drift as uncertainty, without changing configured
 * PD dark or raw records. Normalization, robust peak/open selection and fitting
 * happen after acquisition; rejected fits retain the complete diagnostic data.
 * Operational open bounds the fitted branch, not engineering voltage access.
 */

//TODO go through entire file and see which saturated measuerments are retained.

#include "attenuator_calibration.h"
#include "calibration_fit.h"
#include "calibration_storage.h"
#include "laser_calibration.h"

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
#include "app_output.h"
#include "devices.h"
#include "housekeeping.h"
#include "mems_switching.h"
#include "throughput_monitor.h"

LOG_MODULE_REGISTER(attenuator_calibration, LOG_LEVEL_INF);

#define ATTEN_CAL_DEFAULT_DWELL_MS 400U
#define ATTEN_CAL_MIN_DWELL_MS 100U
#define ATTEN_CAL_MAX_DWELL_MS 2000U
#define ATTEN_CAL_SETTLE_MS 100U
#define ATTEN_CAL_DARK_SETTLE_MS 10000U
#define ATTEN_CAL_DARK_DWELL_MS 2000U
/* Peak target for reference/bridge searches, below the PD's 2000 mV usable limit. */
#define ATTEN_CAL_SEARCH_MAX_MV 1850.0f
/* Minimum bracket width for companion-FVOA binary searches. */
#define ATTEN_CAL_SEARCH_MIN_STEP_MV 5.0f
/* Fixed DUT-FVOA spacing across the complete closed-to-open sweep. */
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
#define ATTEN_CAL_FIT_MIN_SLOPE 1.0e-12
#define ATTEN_CAL_FIT_MAX_SLOPE 1.0
#define ATTEN_CAL_CORRECTION_PIVOT_EPS 1.0e-12
#define ATTEN_CAL_CORRECTION_MONOTONIC_EPS_DB 1.0e-4
#define ATTEN_CAL_TELEMETRY_TOPIC_SUFFIX "atten"
#define ATTEN_CAL_DATA_CHUNK_RECORD_SIZE 27U
#define ATTEN_CAL_DATA_METADATA_HEADER_SIZE 52U
#define ATTEN_CAL_DATA_BRIDGE_ENTRY_SIZE 2U
#define ATTEN_CAL_DATA_RECORDS_PER_CHUNK \
	(COO_CMD_PAYLOAD_MAX / ATTEN_CAL_DATA_CHUNK_RECORD_SIZE)
#define ATTEN_CAL_DATA_CHUNK_MAGIC0 'H'
#define ATTEN_CAL_DATA_CHUNK_MAGIC1 'A'
#define ATTEN_CAL_DATA_CHUNK_MAGIC2 'C'
#define ATTEN_CAL_DATA_CHUNK_MAGIC3 '4'
#define ATTEN_CAL_DATA_CHUNK_VERSION 4U
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
	ATTEN_CAL_PHASE_DARK_SETTLE,
	ATTEN_CAL_PHASE_WAIT_WINDOW,
	ATTEN_CAL_PHASE_FITTING,
};

enum atten_cal_measure_kind {
	ATTEN_CAL_MEASURE_SWEEP=0,
	ATTEN_CAL_MEASURE_BRIDGE_PROBE,
	ATTEN_CAL_MEASURE_DARK_BEFORE,
	ATTEN_CAL_MEASURE_DARK_AFTER,
};

enum atten_cal_record_event {
	ATTEN_CAL_EVENT_POINT = 0,
	ATTEN_CAL_EVENT_BRIDGE_PROBE = 2,
	ATTEN_CAL_EVENT_DARK_BEFORE,
	ATTEN_CAL_EVENT_DARK_AFTER,
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

BUILD_ASSERT(ATTEN_CAL_DATA_RECORDS_PER_CHUNK > 0U,
	     "calibration data chunk must carry at least one record");
BUILD_ASSERT(ATTEN_CAL_DATA_RECORDS_PER_CHUNK <= UINT8_MAX,
	     "calibration data records per chunk must fit in wire metadata");
BUILD_ASSERT(ATTENUATOR_CAL_RECORD_COUNT == CALIBRATION_RECORD_COUNT &&
	     ATTENUATOR_CAL_RECORD_COUNT <= UINT8_MAX,
	     "calibration storage and byte-sized record indices must agree");
BUILD_ASSERT(ATTEN_CAL_DATA_METADATA_HEADER_SIZE == 36U +
	     sizeof(((struct attenuator_calibration_fit_metrics *)0)->support),
	     "fit support bitmap must fit the metadata header");

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
	uint8_t laser_percent;
	/* Current commanded DAC pair for the active measurement. */
	float sweep_mv;
	float other_mv;
	/* Pending nominal grid point is independent of the held bridge anchor. */
	float pending_mv;
	float open_mv[2];
	bool range_failed[2];
	/* Companion search bracket. */
	float search_low_mv;
	float search_high_mv;
	uint8_t search_tries;
	/* Held-DUT bridge measurement selected from retained point records. */
	uint8_t bridge_before_index;
	/* Start of the source-off settling interval or the active PD window. */
	int64_t window_started_ms;
	int last_error;
	bool shutdown_pending; /* Keep laser identity after a failed fault shutdown. */
	uint8_t record_count[ATTENUATOR_PHYSICAL_COUNT];
	uint8_t bridge_count[ATTENUATOR_PHYSICAL_COUNT];
	bool record_overflow[ATTENUATOR_PHYSICAL_COUNT];
	struct attenuator_calibration_fit_metrics fit[ATTENUATOR_PHYSICAL_COUNT];
};

static struct atten_cal_state_data cal;
#define cal_lock calibration_lock
static atomic_t cal_fit_cancel;
static K_SEM_DEFINE(cal_fit_done, 0, 1);
/* Readers must not wait for the owner to finish a numerical fit or hardware I/O. */
static K_MUTEX_DEFINE(cal_status_lock);
static struct attenuator_calibration_status cal_status = {
	.state = "inactive", .mode = "none", .physical = "dac1", .fit = "none",
	.point_count = ATTENUATOR_CAL_RECORD_COUNT,
};
#define cal_telemetry_msg calibration_telemetry
#define cal_fit_points calibration_scratch.atten.points

static void publish_status_locked(struct attenuator_calibration_status *status);
static void auto_schedule_measure_locked(enum atten_cal_measure_kind kind, float sweep_mv, float other_mv);
static void auto_begin_bridge_locked(void);
static void auto_close_bridge_locked(uint8_t index);
static const char *fit_reason_name(uint8_t reason);

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
	case ATTEN_CAL_EVENT_BRIDGE_PROBE:
		return "bridge_probe";
	case ATTEN_CAL_EVENT_DARK_BEFORE:
		return "dark_before";
	case ATTEN_CAL_EVENT_DARK_AFTER:
		return "dark_after";
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
	(void)app_output_emit(&(const struct coo_cmd_runtime_emit_args){
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
			    "\"accepted\":%s,\"points\":%u,\"reason\":\"%s\",\"open_mv\":%.6f,\"open_loss_db\":%.6f,"
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
			    fit_reason_name(fit != NULL ? fit->reason : 0U),
			    fit != NULL ? (double)fit->open_mv : 0.0, fit != NULL ? (double)fit->open_loss_db : 0.0,
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
	/* DAC1's first slot holds dark_before; reserve DAC2's last for dark_after. */
	uint8_t limit = ATTENUATOR_CAL_RECORD_COUNT -
		(physical == 1U && event != ATTEN_CAL_EVENT_DARK_AFTER ? 1U : 0U);
	if (cal.record_count[physical] >= limit) {
		cal.record_overflow[physical] = true;
		return false;
	}

	index = cal.record_count[physical]++;
	record = &calibration_data.atten.records[physical][index];
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
	bool first = attenuator_set_physical_voltage(atten, sweep_physical, sweep_mv);
	bool second = attenuator_set_physical_voltage(atten, sweep_physical == 0U ? 1U : 0U, other_mv);
	return first && second;
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
	cal.shutdown_pending = hispec_laser_stop_output(cal.laser, false, NULL, 0) != 0;
	(void)set_physical_pair(cal.attenuator_index, 0U, ATTENUATOR_DRIVE_MAX_MV, ATTENUATOR_DRIVE_MAX_MV);
	atten_cal_emit_simple("error");
}

/** Return the next midpoint in the bounded companion search. */
static float search_midpoint_locked(void)
{
	return (cal.search_low_mv + cal.search_high_mv) * 0.5f;
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

/** Arm a diagnostic window after confirmed STOP. Tick-based settling keeps STOP
 * responsive; the route, DACs, PD power and configured dark remain unchanged.
 */
static void auto_schedule_dark_locked(enum atten_cal_measure_kind kind)
{
	cal.measure_kind = kind;
	cal.laser_percent = 0U;
	cal.window_started_ms = k_uptime_get();
	cal.phase = ATTEN_CAL_PHASE_DARK_SETTLE;
}

/** Begin at closed DUT / operationally open companion, at fixed laser output. */
static void auto_start_next_physical_locked(void)
{
	cal.point_index = 0U;
	cal.segment_id = 0U;
	cal.search_tries = 0U;
	cal.laser_percent = 100U;
	if (hispec_laser_set_output_percent_autooff(cal.laser, 100U, 0U, true, NULL, 0) != 0) {
		auto_error_locked(-EIO);
		return;
	}
	auto_schedule_measure_locked(ATTEN_CAL_MEASURE_SWEEP, ATTENUATOR_DRIVE_MAX_MV,
		cal.open_mv[cal.physical_index == 0U ? 1U : 0U]);
	atten_cal_emit_simple("physical_start");
}

/** Finish the sweeps and stop the source; the final dark window precedes fitting. */
static void auto_finish_physical_locked(void)
{
	LOG_INF("atten cal physical complete physical=%s records=%u overflow=%d",
		physical_name(cal.physical_index),
		cal.record_count[cal.physical_index],
		cal.record_overflow[cal.physical_index] ? 1 : 0);

	if (!set_physical_pair(cal.attenuator_index, 0U, ATTENUATOR_DRIVE_MAX_MV, ATTENUATOR_DRIVE_MAX_MV)) {
		auto_error_locked(-EIO);
		return;
	}
	cal.sweep_mv = cal.other_mv = ATTENUATOR_DRIVE_MAX_MV;
	if (cal.physical_index == 0U) {
		cal.physical_index = 1U;
		auto_start_next_physical_locked();
		return;
	}
	int rc = hispec_laser_stop_output(cal.laser, false, NULL, 0);
	if (rc != 0) {
		/* Preserve the data and shutdown responsibility without retrying STOP. */
		housekeeping_photodiode_autooff_inhibit((enum housekeeping_power_output)cal.channel, false);
		cal.shutdown_pending = true;
		cal.last_error = rc;
		cal.state = ATTEN_CAL_STATE_ERROR;
		cal.phase = ATTEN_CAL_PHASE_NONE;
		atten_cal_emit_simple("error");
		return;
	}
	auto_schedule_dark_locked(ATTEN_CAL_MEASURE_DARK_AFTER);
}

/** Continue the pending nominal grid, or finish after its final point. */
static void auto_resume_sweep_locked(void)
{
	if (cal.pending_mv < 0.0f) {
		auto_finish_physical_locked();
	} else {
		auto_schedule_measure_locked(ATTEN_CAL_MEASURE_SWEEP, cal.pending_mv, cal.other_mv);
	}
}

/** Leave room for every remaining nominal point and the final source-off window. */
static bool bridge_has_room_locked(void)
{
	unsigned remaining = cal.pending_mv < 0.0f ? 0U : (unsigned)(cal.pending_mv / ATTEN_CAL_SWEEP_STEP_MV) + 1U;
	return cal.record_count[cal.physical_index] + remaining + (cal.physical_index == 1U ? 1U : 0U)
		< ATTENUATOR_CAL_RECORD_COUNT;
}

/** An unlinked range is a rejected fit, not lost acquisition. Park the companion
 * and retain the rest of the grid. Only I/O faults or STOP abort acquisition. */
static void auto_range_failed_locked(void)
{
	cal.range_failed[cal.physical_index] = true;
	cal.other_mv = ATTENUATOR_DRIVE_MAX_MV;
	app_output_emit(&(const struct coo_cmd_runtime_emit_args){
		.type = COO_CMD_RUNTIME_EMIT_WARNING, .delivery = COO_CMD_RUNTIME_EMIT_BEST_EFFORT,
		.code = "atten_bridge_failed", .msg = "no usable bridge; retaining remaining sweep with companion closed",
		.context = physical_name(cal.physical_index),
	});
	auto_resume_sweep_locked();
}

/** Retain every point. Bright points need a dimmer companion; a clipped point
 * is retried at the new gain, while a usable headroom trigger advances the grid. */
static void auto_handle_sweep_locked(const struct atten_cal_measurement *measurement)
{
	struct atten_cal_record *record;
	if (!append_record_locked(ATTEN_CAL_EVENT_POINT, measurement, &record)) {
		auto_error_locked(-ENOSPC);
		return;
	}
	if (record->classification == ATTEN_CAL_CLASSIFICATION_ADC_ERROR) {
		auto_error_locked(-EIO);
		return;
	}
	cal.pending_mv = cal.sweep_mv - ATTEN_CAL_SWEEP_STEP_MV;
	if (!cal.range_failed[cal.physical_index] && record->max_mv >= ATTEN_CAL_SEARCH_MAX_MV &&
	    (cal.sweep_mv > 0.0f || record->classification != ATTEN_CAL_CLASSIFICATION_OK)) {
		if (record->classification != ATTEN_CAL_CLASSIFICATION_OK) cal.pending_mv = cal.sweep_mv;
		auto_begin_bridge_locked();
	} else {
		auto_resume_sweep_locked();
	}
}

/** Hold the latest usable point, or the last accepted bridge-after if this
 * segment has no usable point. Never use an arbitrary search probe as an anchor.
 * These retained measurements replace the old pre/post confirmation windows. */
static void auto_begin_bridge_locked(void)
{
	uint8_t physical = cal.physical_index;
	uint8_t anchor_index = 0U;
	const struct atten_cal_record *anchor = NULL;
	if (cal.other_mv >= ATTENUATOR_DRIVE_MAX_MV - ATTEN_CAL_SEARCH_MIN_STEP_MV || !bridge_has_room_locked()) {
		auto_range_failed_locked();
		return;
	}
	for (uint8_t i = cal.record_count[physical]; i > 0U; --i) {
		const struct atten_cal_record *record = &calibration_data.atten.records[physical][i - 1U];
		if (record->segment == cal.segment_id && record->event == ATTEN_CAL_EVENT_POINT &&
		    record->classification == ATTEN_CAL_CLASSIFICATION_OK) {
			anchor = record;
			anchor_index = i - 1U;
			break;
		}
	}
	if (anchor == NULL && cal.bridge_count[physical] > 0U) {
		const struct atten_cal_bridge *bridge = &calibration_data.atten.bridges[physical][cal.bridge_count[physical] - 1U];
		const struct atten_cal_record *record = &calibration_data.atten.records[physical][bridge->after_record_index];
		if (record->classification == ATTEN_CAL_CLASSIFICATION_OK &&
		    record->segment == cal.segment_id && record->other_mv == cal.other_mv) {
			anchor = record;
			anchor_index = bridge->after_record_index;
		}
	}
	if (anchor == NULL) {
		auto_range_failed_locked();
		return;
	}
	cal.bridge_before_index = anchor_index;
	cal.search_low_mv = cal.other_mv;
	cal.search_high_mv = ATTENUATOR_DRIVE_MAX_MV;
	cal.search_tries = 0U;
	cal.segment_id++;
	auto_schedule_measure_locked(ATTEN_CAL_MEASURE_BRIDGE_PROBE, anchor->sweep_mv, search_midpoint_locked());
}

/** Keep the first measured usable reduction. The bracket is a voltage search,
 * not an assumed transmission curve or a requested fraction of the anchor. */
static void auto_handle_bridge_probe_locked(const struct atten_cal_measurement *measurement)
{
	struct atten_cal_record *record;
	if (!append_record_locked(ATTEN_CAL_EVENT_BRIDGE_PROBE, measurement, &record)) {
		auto_error_locked(-ENOSPC);
		return;
	}
	if (record->classification == ATTEN_CAL_CLASSIFICATION_ADC_ERROR) {
		auto_error_locked(-EIO);
		return;
	}
	cal.search_tries++;
	const struct atten_cal_record *before = &calibration_data.atten.records[cal.physical_index][cal.bridge_before_index];
	float ratio = record->signal_mv / before->signal_mv;
	if (record->classification == ATTEN_CAL_CLASSIFICATION_OK && record->max_mv < ATTEN_CAL_SEARCH_MAX_MV &&
	    isfinite(ratio) && ratio > 0.0f && ratio < 1.0f) {
		auto_close_bridge_locked(cal.point_index);
		return;
	}
	if (record->classification == ATTEN_CAL_CLASSIFICATION_BELOW_SNR && record->max_mv < ATTEN_CAL_SEARCH_MAX_MV) {
		cal.search_high_mv = cal.other_mv;
	} else {
		cal.search_low_mv = cal.other_mv;
	}
	if (cal.search_high_mv - cal.search_low_mv <= ATTEN_CAL_SEARCH_MIN_STEP_MV ||
	    cal.search_tries >= ATTEN_CAL_MAX_SEARCH_TRIES || !bridge_has_room_locked()) {
		auto_range_failed_locked();
		return;
	}
	auto_schedule_measure_locked(ATTEN_CAL_MEASURE_BRIDGE_PROBE, cal.sweep_mv, search_midpoint_locked());
}

/** Accept a real probe by index; resume the nominal grid without copying it. */
static void auto_close_bridge_locked(uint8_t index)
{
	struct atten_cal_bridge *bridge = &calibration_data.atten.bridges[cal.physical_index][cal.bridge_count[cal.physical_index]++];
	bridge->before_record_index = cal.bridge_before_index;
	bridge->after_record_index = index;
	auto_resume_sweep_locked();
}

enum atten_fit_reason {
	FIT_PENDING, FIT_BACKGROUND, FIT_BRIDGE, FIT_PEAK, FIT_SUPPORT,
	FIT_MODEL, FIT_RESIDUAL, FIT_ACCEPTED, FIT_RANGE,
};

/** Names explain rejection without discarding raw acquisition evidence. */
static const char *fit_reason_name(uint8_t reason)
{
	static const char *const names[] = {
		"pending", "background", "bridge", "peak", "support", "model", "residual", "accepted", "range",
	};
	return reason < ARRAY_SIZE(names) ? names[reason] : "unknown";
}

/** Correct only derived signals. The bracketing source-off span is uncertainty,
 * not a fitted time trend or an update to the configured photodiode dark. */
static int fit_background(struct attenuator_calibration_fit_metrics *fit)
{
	const struct atten_cal_record *before = NULL, *after = NULL;
	for (uint8_t p = 0; p < 2; ++p) {
		for (uint8_t i = 0; i < cal.record_count[p]; ++i) {
			const struct atten_cal_record *r = &calibration_data.atten.records[p][i];
			if (r->event == ATTEN_CAL_EVENT_DARK_BEFORE) before = r;
			if (r->event == ATTEN_CAL_EVENT_DARK_AFTER) after = r;
		}
	}
	if (before == NULL || after == NULL ||
	    before->classification == ATTEN_CAL_CLASSIFICATION_ADC_ERROR ||
	    after->classification == ATTEN_CAL_CLASSIFICATION_ADC_ERROR ||
	    (double)before->max_mv >= PHOTODIODE_ADC_USABLE_MV || (double)after->max_mv >= PHOTODIODE_ADC_USABLE_MV ||
	    !isfinite(before->signal_mv) || !isfinite(after->signal_mv) ||
	    !(before->signal_err_mv > 0.0f) || !(after->signal_err_mv > 0.0f)) return -ERANGE;
	fit->background_mv = 0.5f * (before->signal_mv + after->signal_mv);
	fit->background_err_mv = 0.5f * fabsf(after->signal_mv - before->signal_mv) +
		MAX(before->signal_err_mv, after->signal_err_mv);
	return 0;
}

/** Reassess measured signal against the source-off background; raw classifications
 * remain acquisition facts. A clipped window can never be recovered by subtraction. */
static bool corrected_measurement(const struct atten_cal_record *r,
		const struct attenuator_calibration_fit_metrics *fit, double *signal, double *error)
{
	*signal = (double)r->signal_mv - (double)fit->background_mv;
	*error = hypot((double)r->signal_err_mv, (double)fit->background_err_mv);
	return r->classification != ATTEN_CAL_CLASSIFICATION_ADC_ERROR &&
		r->classification != ATTEN_CAL_CLASSIFICATION_SATURATED &&
		(double)r->max_mv < PHOTODIODE_ADC_USABLE_MV && isfinite(*signal) &&
		isfinite(*error) && *error > 0.0 && *signal / *error >= (double)ATTEN_CAL_SNR_USABLE;
}

/** Derive cumulative gains from measured links only. This math works in either
 * direction; acquisition separately requires a reduction when closing a companion.
 * Reference-relative variance later includes only links between the two segments. */
static int build_segment_scales(uint8_t physical, double *scale, double *variance,
		const struct attenuator_calibration_fit_metrics *fit)
{
	for (uint8_t i = 0; i < ATTENUATOR_CAL_RECORD_COUNT; ++i) scale[i] = variance[i] = NAN;
	scale[0] = 1.0;
	variance[0] = 0.0;
	for (uint8_t i = 0; i < cal.bridge_count[physical]; ++i) {
		const struct atten_cal_bridge *b = &calibration_data.atten.bridges[physical][i];
		if (b->before_record_index >= cal.record_count[physical] || b->after_record_index >= cal.record_count[physical]) return -ERANGE;
		const struct atten_cal_record *before = &calibration_data.atten.records[physical][b->before_record_index];
		const struct atten_cal_record *after = &calibration_data.atten.records[physical][b->after_record_index];
		double bs, be, as, ae;
		if (before->segment >= ATTENUATOR_CAL_RECORD_COUNT || after->segment >= ATTENUATOR_CAL_RECORD_COUNT ||
		    !isfinite(scale[before->segment]) || before->sweep_mv != after->sweep_mv ||
		    !corrected_measurement(before, fit, &bs, &be) || !corrected_measurement(after, fit, &as, &ae)) return -ERANGE;
		double ratio = as / bs;
		if (!(ratio > 0.0) || !isfinite(ratio)) return -ERANGE;
		scale[after->segment] = scale[before->segment] * ratio;
		variance[after->segment] = variance[before->segment] + (be / bs) * (be / bs) + (ae / as) * (ae / as);
	}
	return 0;
}

/** Sort a fixed nine-sample statistic in place; bounded stack use, no allocation. */
static float median9(float values[9])
{
	for (unsigned i = 1; i < 9; ++i) {
		float value = values[i];
		unsigned j = i;
		while (j > 0 && values[j - 1] > value) { values[j] = values[j - 1]; --j; }
		values[j] = value;
	}
	return values[4];
}

/** A rolling median needs actual contiguous grid support, not interpolation over
 * clipped gaps. Return its real median record and robust local dB uncertainty. */
static bool peak_window(const struct atten_cal_fit_point *points, const struct atten_cal_record *records,
			uint8_t start, uint8_t *median_index, float *level, float *sigma)
{
	float values[9], errors[9];
	for (uint8_t j = 0; j < 9; ++j) {
		const struct atten_cal_fit_point *p = &points[start + j];
		if (j > 0 && fabsf(records[p->record_index].sweep_mv -
		    records[points[start + j - 1].record_index].sweep_mv - ATTEN_CAL_SWEEP_STEP_MV) > 1.0f) return false;
		values[j] = p->measured_db;
		errors[j] = p->measured_db_err;
	}
	*level = median9(values);
	*sigma = median9(errors);
	for (uint8_t j = 0; j < 9; ++j) {
		if (points[start + j].measured_db == *level) *median_index = start + j;
		values[j] = fabsf(points[start + j].measured_db - *level);
	}
	*sigma = MAX(*sigma, 1.4826f * median9(values));
	return true;
}

/** Select a stable closed-side open point without making it the 0 dB reference.
 * Nine-point medians suppress isolated bright outliers; three following medians
 * must confirm the crossing. Excess scatter or unresolved clipping rejects it. */
static int select_open(uint8_t physical, struct atten_cal_fit_point *points, uint8_t count,
		       struct attenuator_calibration_fit_metrics *fit)
{
	const struct atten_cal_record *records = calibration_data.atten.records[physical];
	if (count < 12U) return -ERANGE;
	for (uint8_t i = 0; i < cal.record_count[physical]; ++i) {
		if (records[i].event != ATTEN_CAL_EVENT_POINT || records[i].classification != ATTEN_CAL_CLASSIFICATION_SATURATED) continue;
		bool recovered = false;
		for (uint8_t j = 0; j < count; ++j) {
			if (records[points[j].record_index].sweep_mv == records[i].sweep_mv) recovered = true;
		}
		if (!recovered) return -ERANGE;
	}
	float peak = INFINITY, scatter = 0.0f;
	uint8_t peak_start = 0, reference = 0;
	for (uint8_t i = 0; i + 9U <= count; ++i) {
		uint8_t index; float level, sigma;
		if (peak_window(points, records, i, &index, &level, &sigma) && level <= peak) {
			peak = level; scatter = sigma; peak_start = i; reference = index;
		}
	}
	float loss = 0.2f + 3.0f * scatter;
	if (!isfinite(peak) || loss > 3.0f) return -ERANGE;
	float open = NAN;
	for (uint8_t i = peak_start + 1U; i + 12U <= count; ++i) {
		uint8_t index; float previous, current, sigma;
		if (!peak_window(points, records, i - 1U, &index, &previous, &sigma) ||
		    !peak_window(points, records, i, &index, &current, &sigma) ||
		    previous > peak + loss || current < peak + loss || !(current > previous)) continue;
		bool confirmed = true;
		for (uint8_t j = 1; j <= 3; ++j) {
			float level;
			if (!peak_window(points, records, i + j, &index, &level, &sigma) || level < peak + loss) confirmed = false;
		}
		if (confirmed) {
			float mv = records[points[i + 3U].record_index].sweep_mv;
			open = mv + ATTEN_CAL_SWEEP_STEP_MV * (peak + loss - previous) / (current - previous);
		}
	}
	if (!isfinite(open)) return -ERANGE;
	struct attenuator a;
	attenuator_snapshot(&attenuators[cal.attenuator_index], &a);
	struct attenuator_dac_cfg *cfg = physical == 0 ? &a.dac_cfg1 : &a.dac_cfg2;
	cfg->open_mv = open;
	fit->open_mv = attenuator_open_voltage(cfg);
	fit->open_loss_db = loss;
	fit->reference_record = points[reference].record_index;
	fit->reference_valid = true;
	return 0;
}

/** Normalize once, then select the operational branch. Raw acquisition order is
 * retained; only the small fit-index array is sorted into increasing voltage. */
static int build_fit_points(uint8_t physical, struct atten_cal_fit_point *points, uint8_t *count_out,
			    struct attenuator_calibration_fit_metrics *fit)
{
	double *scale = calibration_scratch.atten.segment_scale;
	double *variance = calibration_scratch.atten.segment_rel_var;
	const struct atten_cal_record *records = calibration_data.atten.records[physical];
	uint8_t count = 0;
	fit->reason = FIT_BACKGROUND;
	if (fit_background(fit) != 0) return -ERANGE;
	fit->reason = FIT_BRIDGE;
	if (build_segment_scales(physical, scale, variance, fit) != 0) return -ERANGE;
	for (uint8_t i = 0; i < cal.record_count[physical]; ++i) {
		const struct atten_cal_record *r = &records[i];
		double signal, error;
		if (r->event != ATTEN_CAL_EVENT_POINT || r->segment >= ATTENUATOR_CAL_RECORD_COUNT ||
		    !isfinite(scale[r->segment]) || !corrected_measurement(r, fit, &signal, &error)) continue;
		struct atten_cal_fit_point point = {
			.record_index = i,
			.measured_db = (float)(-10.0 * log10(signal / scale[r->segment])),
			.measured_db_err = (float)(10.0 / log(10.0) * error / signal),
		};
		uint8_t j = 0;
		while (j < count && records[points[j].record_index].sweep_mv < r->sweep_mv) ++j;
		if (j < count && records[points[j].record_index].sweep_mv == r->sweep_mv) {
			if (point.measured_db_err < points[j].measured_db_err) points[j] = point;
			continue;
		}
		memmove(&points[j + 1U], &points[j], (count - j) * sizeof(*points));
		points[j] = point; ++count;
	}
	fit->reason = FIT_PEAK;
	if (select_open(physical, points, count, fit) != 0) return -ERANGE;
	const struct atten_cal_record *ref = &records[fit->reference_record];
	double rs, re;
	if (!corrected_measurement(ref, fit, &rs, &re)) return -ERANGE;
	double ref_db = -10.0 * log10(rs / scale[ref->segment]);
	uint8_t selected = 0;
	for (uint8_t i = 0; i < count; ++i) {
		const struct atten_cal_record *r = &records[points[i].record_index];
		if (r->sweep_mv < fit->open_mv) continue;
		double signal, error;
		(void)corrected_measurement(r, fit, &signal, &error);
		double rel_var = (error / signal) * (error / signal) + (re / rs) * (re / rs) +
			fabs(variance[r->segment] - variance[ref->segment]);
		points[selected] = points[i];
		points[selected].measured_db -= (float)ref_db;
		points[selected].measured_db_err = (float)MAX(10.0 / log(10.0) * sqrt(rel_var), ATTEN_CAL_MIN_DB_ERR);
		/* Keep signed attenuation: excluding readings brighter than the reference
		 * selects dimmer noise and was deliberately removed in f8a9e54. */
		++selected;
	}
	fit->reason = FIT_SUPPORT;
	*count_out = selected;
	return selected >= ATTEN_CAL_MIN_FIT_POINTS ? 0 : -ERANGE;
}

/**
 * Estimate the physical FVOA leakage floor from the final retained fit points.
 *
 * Fit indices are ordered by increasing voltage, independent of sweep direction.
 * The highest-voltage usable points estimate the finite transmission floor only if the optical
 * path probes that far. Constrain the estimate to at least 65 dB so a dim path's
 * last usable readings cannot bend the floor into its measured region. This
 * constraint supplies no additional calibrated range. It is not a third
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

	/* An unprobed high-attenuation tail is not evidence for a low leakage
	 * floor. 65 dB is a model constraint, not additional measured support.
	 * Keep measured tail scatter for weighting; inspect its effect on fits. */
	*max_atten_db = MAX(mean, 65.0);
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

struct atten_fit_context {
	const struct atten_cal_fit_point *points;
	const struct atten_cal_record *records;
	uint8_t count;
	struct attenuator_model_coeffs coeffs;
	double max_atten_sigma_db;
};

/* Sum weighted squared residuals for a complete fit candidate, optionally
 * accumulating its normal equation. Keep point order, weighting and cancellation
 * here; only the two-variable damped step is shared with laser calibration. */
static int atten_fit_evaluate(void *context, const double p[2], bool derivatives,
			      struct calibration_normal *normal)
{
	struct atten_fit_context *fit = context;
	fit->coeffs.fvoa_50pct_mv = p[0];
	fit->coeffs.slope_inv_fvoa_mv = p[1];

	for (uint8_t i = 0; i < fit->count; ++i) {
		double r;
		double j0;
		double j1;

		if (atomic_get(&cal_fit_cancel)) {
			return -ECANCELED;
		}
		int rc = fit_point_weighted_eval(&fit->points[i], fit->records, &fit->coeffs,
			fit->max_atten_sigma_db, &r, derivatives ? &j0 : NULL, derivatives ? &j1 : NULL);
		if (rc != 0) {
			return rc;
		}

		normal->cost += r * r;
		if (derivatives) {
			normal->h00 += j0 * j0;
			normal->h01 += j0 * j1;
			normal->h11 += j1 * j1;
			normal->g0 += j0 * r;
			normal->g1 += j1 * r;
		}
	}
	return 0;
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
	double p[2];

	if (points == NULL || records == NULL || fvoa_50pct_mv == NULL ||
	    slope_inv_fvoa_mv == NULL || point_count < ATTEN_CAL_MIN_FIT_POINTS) {
		return -EINVAL;
	}

	/* Keep the physical model and uncertainty in this owner. The shared
	 * optimizer sees only p[0]=f50 in mV and p[1]=slope in inverse mV. */
	fit_initial_guess(points, records, point_count, gain, &p[0], &p[1]);
	struct atten_fit_context context = {
		.points = points,
		.records = records,
		.count = point_count,
		.coeffs = {
			.max_atten_db = max_atten_db,
			.gain = gain,
		},
		.max_atten_sigma_db = max_atten_sigma_db,
	};
	const double lower[2] = {1.0, ATTEN_CAL_FIT_MIN_SLOPE};
	const double upper[2] = {2.0 * (double)ATTENUATOR_DRIVE_MAX_MV * gain, ATTEN_CAL_FIT_MAX_SLOPE};
	const double tolerance[2] = {1.0e-6, 1.0e-12};

	int rc = calibration_fit_2(atten_fit_evaluate, &context, lower, upper, tolerance, p);
	if (rc == 0) {
		*fvoa_50pct_mv = p[0];
		*slope_inv_fvoa_mv = p[1];
	}
	return rc;
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

/** Check the final operational curve, above the selected open boundary. Sparse fit
 * points. A 1 mV grid checks the operational branch and slopes between samples; retained
 * voltages and the calibrated join are also checked. This is numerical validation,
 * not a proof between grid locations. No I/O; returns the first failing location.
 */
static bool fit_curve_valid(const struct attenuator_model_coeffs *coeffs,
                            const struct atten_cal_fit_point *points,
                            const struct atten_cal_record *records, uint8_t point_count,
                            float open_mv, float *failed_mv, struct atten_model_eval *eval)
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
			if (mv < open_mv) continue;
			*failed_mv = CLAMP(mv, open_mv, ATTENUATOR_DRIVE_MAX_MV);
			*eval = (struct atten_model_eval){.db = NAN, .d_db_d_voltage_mv = NAN};
			/* Validate the actual float control curve; fitting and metrics above
			 * still use the double model and its full coefficient derivatives.
			 */
			float db, slope;
			if (!atten_model_eval_runtime(coeffs, *failed_mv, &db, &slope)) return false;
			eval->db = (double)db;
			eval->d_db_d_voltage_mv = (double)slope;
			if (eval->d_db_d_voltage_mv < 0.0 ||
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
				double max_atten_sigma_db, float open_mv,
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
					if (fit_curve_valid(coeffs, points, records, point_count, open_mv, &failed_mv, &eval)) {
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
		const char *name = physical_name(records == calibration_data.atten.records[0] ? 0U : 1U);

		snprintk(context, sizeof(context), "%s terms=%d first_failure: %s", name, selected, first_failure);
		LOG_WRN("atten correction %s", context);
		app_output_emit(&(struct coo_cmd_runtime_emit_args){
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
	const struct atten_cal_record *records = calibration_data.atten.records[physical];
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

	out->reason = FIT_RANGE;
	if (cal.range_failed[physical]) return -ERANGE;
	rc = build_fit_points(physical, cal_fit_points, &point_count, out);
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
	out->points = point_count;
	for (uint8_t i = 0; i < point_count; ++i) {
		uint8_t index = cal_fit_points[i].record_index;
		out->support[index / 8U] |= BIT(index % 8U);
	}
	out->reason = FIT_MODEL;
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
					 max_atten_sigma_db, out->open_mv, &coeffs);
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
		if (measured_db > ATTENUATOR_CALIBRATED_MAX_DB) continue;
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
	struct app_attenuator_fit_policy policy;
	app_settings_get_attenuator_fit(cal.attenuator_index, &policy);
	out->accepted = max_abs_db <= policy.max_fit_residual_db[physical] &&
			isfinite(out->correlation) &&
			out->correlation >= ATTEN_CAL_MIN_FIT_CORR &&
			isfinite(out->fvoa_50pct_mv) &&
			out->fvoa_50pct_mv > 0.0 &&
			out->slope_inv_fvoa_mv > 0.0 &&
			isfinite(out->max_atten_db) &&
			out->max_atten_db > 0.0;
	out->reason = out->accepted ? FIT_ACCEPTED : FIT_RESIDUAL;
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
	struct app_attenuator_open_settings open = {.mv = {cal.fit[0].open_mv, cal.fit[1].open_mv}};
	(void)attenuator_set_open_voltages(&attenuators[cal.attenuator_index], open.mv);
	app_settings_update_attenuator_channel(cal.attenuator_index, &stored, cal.persistent);
	return app_settings_update_attenuator_open(cal.attenuator_index, &open, cal.persistent);
}

/** Fit immutable records on the existing throughput thread, without cal_lock.
 * Start/stop commands cancel and join this calculation before changing its data.
 * Lowest application priority lets health, UART polling and logging preempt the
 * math. Change/restore priority with no mutex held, avoiding priority inheritance.
 * Keep this out of line so acquisition does not carry the fitting stack frame.
 */
static __noinline void auto_fit(void)
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
		case ATTEN_CAL_PHASE_DARK_SETTLE:
		case ATTEN_CAL_PHASE_WAIT_WINDOW:
			if (pd_status == NULL) {
				auto_error_locked(-EINVAL);
				return;
			}

			/* Source health and the expected emission state are acquisition preconditions,
			 * separate from the current-based numerical power estimate. An
			 * unconfirmed source permits passive streaming, not calibration.
			 * Diagnostic darks follow a confirmed STOP and require continued non-emission.
			 * No hardware I/O here.
			 */
			bool powered;
			int power_rc = housekeeping_power_get_confirmed((enum housekeeping_power_output)cal.channel, &powered);
			if (power_rc != 0 || !powered) {
				auto_error_locked(power_rc != 0 ? power_rc : -EIO);
				return;
			}
			bool dark = cal.measure_kind == ATTEN_CAL_MEASURE_DARK_BEFORE ||
				cal.measure_kind == ATTEN_CAL_MEASURE_DARK_AFTER;
			bool emitting;
			int source_rc = hispec_laser_output_status(cal.laser, &emitting);
			if (source_rc != 0 || emitting == dark) {
				auto_error_locked(source_rc != 0 ? source_rc : -EIO);
				return;
			}
			if (cal.phase == ATTEN_CAL_PHASE_DARK_SETTLE) {
				if (k_uptime_get() - cal.window_started_ms < ATTEN_CAL_DARK_SETTLE_MS) return;
				int rc = photodiode_set_configurable_window_duration(cal.channel, ATTEN_CAL_DARK_DWELL_MS);
				if (rc < 0) {
					auto_error_locked(rc);
					return;
				}
				cal.window_started_ms = k_uptime_get();
				cal.phase = ATTEN_CAL_PHASE_WAIT_WINDOW;
				return;
			}

			window = &pd_status->channel[cal.channel].configurable_window;
			uint32_t dwell_ms = dark ? ATTEN_CAL_DARK_DWELL_MS : cal.dwell_ms;
			if (window->end_ms <= cal.window_started_ms ||
			    window->sample_length < dwell_ms / PHOTODIODE_SAMPLE_INTERVAL_MS) {
				return;
			}
			build_measurement_from_pd_window(window, &measurement);

			switch (cal.measure_kind) {
				case ATTEN_CAL_MEASURE_DARK_BEFORE:
				case ATTEN_CAL_MEASURE_DARK_AFTER: {
					struct atten_cal_record *record;
					bool before = cal.measure_kind == ATTEN_CAL_MEASURE_DARK_BEFORE;
					if (!append_record_locked(before ? ATTEN_CAL_EVENT_DARK_BEFORE :
								  ATTEN_CAL_EVENT_DARK_AFTER, &measurement, &record)) {
						auto_error_locked(-ENOSPC);
					} else if (measurement.classification == ATTEN_CAL_CLASSIFICATION_ADC_ERROR ||
						   measurement.classification == ATTEN_CAL_CLASSIFICATION_SATURATED) {
						auto_error_locked(measurement.classification == ATTEN_CAL_CLASSIFICATION_ADC_ERROR ? -EIO : -ERANGE);
					} else if (before) {
						/* Zero/negative offsets are expected; SNR is not a dark validity test. */
						auto_start_next_physical_locked();
					} else {
						housekeeping_photodiode_autooff_inhibit((enum housekeeping_power_output)cal.channel, false);
						atomic_clear(&cal_fit_cancel);
						k_sem_reset(&cal_fit_done);
						cal.phase = ATTEN_CAL_PHASE_FITTING;
					}
					break;
				}
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

	/* Both calibrations reuse the same data/scratch buffers. Join the laser
	 * owner and confirm its source stopped before replacing any retained data. */
	rc = laser_calibration_stop(NULL);
	if (rc != 0) {
		return rc;
	}

	k_mutex_lock(&cal_lock, K_FOREVER);
	replacing = cal.state == ATTEN_CAL_STATE_RUNNING;
	cancel_fit_locked();
	if (cal.shutdown_pending) {
		rc = hispec_laser_stop_output(cal.laser, false, NULL, 0);
		if (rc != 0) {
			publish_status_locked(status);
			k_mutex_unlock(&cal_lock);
			return rc;
		}
		cal.shutdown_pending = false;
	}
	if (cal.state == ATTEN_CAL_STATE_RUNNING) {
		cal.phase = ATTEN_CAL_PHASE_NONE;
		if (!set_physical_pair(cal.attenuator_index, 0U, ATTENUATOR_DRIVE_MAX_MV, ATTENUATOR_DRIVE_MAX_MV)) {
			auto_error_locked(-EIO);
			publish_status_locked(status);
			k_mutex_unlock(&cal_lock);
			return -EIO;
		}
		/* A replacement on another laser must first release the old source.
		 * Retain that identity if shutdown fails; do not start a second source.
		 */
		if (cal.laser != request->laser) {
			rc = hispec_laser_stop_output(cal.laser, false, NULL, 0);
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
	memset(&calibration_data, 0, sizeof(calibration_data));
	atomic_set(&calibration_data_kind, CALIBRATION_DATA_ATTEN);
	cal.state = ATTEN_CAL_STATE_RUNNING;
	cal.mode = ATTEN_CAL_MODE_TIB_AUTO;
	cal.attenuator_index = request->attenuator_index;
	cal.dwell_ms = request->dwell_ms < ATTEN_CAL_MIN_DWELL_MS
		? ATTEN_CAL_DEFAULT_DWELL_MS : MIN(request->dwell_ms, ATTEN_CAL_MAX_DWELL_MS);
	cal.persistent = request->persist;
	cal.laser = request->laser;
	cal.channel = request->channel;
	cal.other_mv = ATTENUATOR_DRIVE_MAX_MV;
	cal.sweep_mv = ATTENUATOR_DRIVE_MAX_MV;
	struct attenuator snapshot;
	attenuator_snapshot(&attenuators[cal.attenuator_index], &snapshot);
	cal.open_mv[0] = attenuator_open_voltage(&snapshot.dac_cfg1);
	cal.open_mv[1] = attenuator_open_voltage(&snapshot.dac_cfg2);
	publish_status_locked(NULL);
	k_mutex_unlock(&cal_lock);
	if (replacing) {
		app_output_emit(&(const struct coo_cmd_runtime_emit_args){
			.type = COO_CMD_RUNTIME_EMIT_WARNING,
			.delivery = COO_CMD_RUNTIME_EMIT_BEST_EFFORT,
			.code = "atten_calibration_restart",
			.msg = "restarting attenuator calibration",
		});
	}
	if (throughput_monitor_any_active()) {
		app_output_emit(&(const struct coo_cmd_runtime_emit_args){
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

	if (!set_physical_pair(request->attenuator_index, 0U, ATTENUATOR_DRIVE_MAX_MV, ATTENUATOR_DRIVE_MAX_MV)) {
		rc = -EIO;
		goto failed_start;
	}

	rc = hispec_laser_stop_output(request->laser, false, NULL, 0);
	if (rc != 0) {
		stop_failed = true;
		goto failed_start;
	}

	k_mutex_lock(&cal_lock, K_FOREVER);
	atten_cal_emit_simple("start");
	auto_schedule_dark_locked(ATTEN_CAL_MEASURE_DARK_BEFORE);
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
	cal.shutdown_pending = stop_failed || hispec_laser_stop_output(request->laser, false, NULL, 0) != 0;
	(void)set_physical_pair(cal.attenuator_index, 0U, ATTENUATOR_DRIVE_MAX_MV, ATTENUATOR_DRIVE_MAX_MV);
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
		int rc = hispec_laser_stop_output(cal.laser, false, NULL, 0);
		cal.shutdown_pending = rc != 0;
		bool parked = set_physical_pair(cal.attenuator_index, 0U, ATTENUATOR_DRIVE_MAX_MV, ATTENUATOR_DRIVE_MAX_MV);
		if (rc == 0 && !parked) rc = -EIO;
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
	if (coo_json_append(payload, payload_len, off, ",\"%s\":{\"reason\":\"%s\",\"open_mv\":%.6f,\"open_loss_db\":%.6f,",
		name, fit_reason_name(fit != NULL ? fit->reason : 0U),
		fit != NULL ? (double)fit->open_mv : 0.0, fit != NULL ? (double)fit->open_loss_db : 0.0) != 0) {
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
	if (atomic_get(&calibration_data_kind) != CALIBRATION_DATA_ATTEN) {
		k_mutex_unlock(&cal_lock);
		return -ENODATA;
	}
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
	const struct attenuator_calibration_fit_metrics *fit = &cal.fit[physical_index];
	bytes[16] = fit->reference_valid ? 1U : 0U;
	bytes[17] = fit->reference_record;
	bytes[18] = bridge_count;
	put_le_float(bytes + 19U, fit->background_mv);
	put_le_float(bytes + 23U, fit->background_err_mv);
	put_le_float(bytes + 27U, fit->open_mv);
	put_le_float(bytes + 31U, fit->open_loss_db);
	bytes[35] = fit->reason;
	memcpy(bytes + 36U, fit->support, sizeof(fit->support));
	for (uint8_t i = 0U; i < bridge_count; ++i) {
		const struct atten_cal_bridge *bridge = &calibration_data.atten.bridges[physical_index][i];
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
	if (atomic_get(&calibration_data_kind) != CALIBRATION_DATA_ATTEN) {
		k_mutex_unlock(&cal_lock);
		return -ENODATA;
	}
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
				  &calibration_data.atten.records[physical_index][start_index + i]);
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
