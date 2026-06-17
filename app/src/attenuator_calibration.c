/**
 * @file attenuator_calibration.c
 * @brief Bridge-normalized automatic FVOA attenuator calibration.
 *
 * This is a lab acquisition routine driven by the throughput monitor thread.
 * It uses the configured photodiode dark value, never measures a private dark,
 * and retains raw records even when the final fit is not accepted. The routine
 * deliberately does not use a datasheet voltage schedule: it finds usable
 * regions by binary searches against photodiode saturation and SNR.
 */

#include "attenuator_calibration.h"

#include <errno.h>
#include <math.h>
#include <string.h>

#include <coo_commons/json_utils.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/util.h>

#include "app_settings.h"
#include "attenuator.h"
#include "command.h"
#include "devices.h"
#include "housekeeping.h"
#include "mems_switching.h"
#include "throughput_monitor.h"

LOG_MODULE_REGISTER(attenuator_calibration, LOG_LEVEL_INF);

#define ATTEN_CAL_DEFAULT_DWELL_MS 300U
#define ATTEN_CAL_MAX_DWELL_MS 2000U
#define ATTEN_CAL_ADC_LSB_MV 0.1875
#define ATTEN_CAL_ADC_CLIP_MV 5000.0
#define ATTEN_CAL_SAT_RAW (INT16_MAX - 1024)
#define ATTEN_CAL_SEARCH_STEP_MV 5.0
#define ATTEN_CAL_MAX_SEARCH_TRIES 16U
#define ATTEN_CAL_MIN_FIT_POINTS ATTENUATOR_CAL_MIN_FIT_POINTS
#define ATTEN_CAL_MIN_TX 1.0e-10
#define ATTEN_CAL_MAX_TX 0.999999
#define ATTEN_CAL_MIN_FIT_CORR 0.85
#define ATTEN_CAL_SNR_USABLE 5.0
#define ATTEN_CAL_DAC_SIGMA_MV 3.0
#define ATTEN_CAL_TELEMETRY_TOPIC_SUFFIX "atten"
#define ATTEN_CAL_DATA_CHUNK_HEADER_SIZE 16U
#define ATTEN_CAL_DATA_CHUNK_RECORDS \
	((COO_CMD_PAYLOAD_MAX - ATTEN_CAL_DATA_CHUNK_HEADER_SIZE) / sizeof(struct atten_cal_record))
#define ATTEN_CAL_DATA_CHUNK_MAGIC0 'H'
#define ATTEN_CAL_DATA_CHUNK_MAGIC1 'A'
#define ATTEN_CAL_DATA_CHUNK_MAGIC2 'C'
#define ATTEN_CAL_DATA_CHUNK_MAGIC3 '3'
#define ATTEN_CAL_DATA_CHUNK_VERSION 1U

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
	ATTEN_CAL_EVENT_BRIDGE_BEFORE,
	ATTEN_CAL_EVENT_BRIDGE_PROBE,
	ATTEN_CAL_EVENT_BRIDGE_AFTER,
};

enum atten_cal_record_reason {
	ATTEN_CAL_REASON_OK = 0,
	ATTEN_CAL_REASON_SATURATED,
	ATTEN_CAL_REASON_BELOW_SNR,
	ATTEN_CAL_REASON_ADC_ERROR,
	ATTEN_CAL_REASON_INVALID,
};

enum atten_cal_record_flags {
	ATTEN_CAL_RECORD_SATURATED = BIT(0),
	ATTEN_CAL_RECORD_USABLE = BIT(1),
	ATTEN_CAL_RECORD_FIT_ELIGIBLE = BIT(2),
	ATTEN_CAL_RECORD_FIT_INCLUDED = BIT(3),
};

struct atten_cal_measurement {
	double mean_mv;
	double signal_mv;
	double rms_mv;
	double sigma_y_mv;
	double snr;
	uint16_t samples;
	int16_t max_raw;
	bool saturated;
	bool usable;
	enum atten_cal_record_reason reason;
};

struct atten_cal_record {
	float sweep_mv;
	float other_mv;
	float laser_pct;
	float mean_mv;
	float signal_mv;
	float rms_mv;
	float sigma_y_mv;
	float sigma_x_mv;
	float snr;
	float flux;
	float flux_sigma;
	float scale;
	float scale_sigma;
	float tx;
	float b;
	float residual_db;
	int16_t max_raw;
	uint16_t samples;
	uint8_t event;
	uint8_t reason;
	uint8_t segment;
	uint8_t flags;
};

BUILD_ASSERT(sizeof(struct atten_cal_record) == 72U,
	     "host calibration record decoder expects 72-byte records");
BUILD_ASSERT(ATTEN_CAL_DATA_CHUNK_RECORDS > 0U,
	     "calibration data chunk must carry at least one record");

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
	double sweep_mv;
	double other_mv;
	double search_low_mv;
	double search_high_mv;
	uint8_t search_tries;
	double sweep_low_mv;
	double sweep_high_mv;
	double open_signal_mv;
	double open_sigma_mv;
	double segment_scale;
	double segment_scale_rel_var;
	double bridge_before_signal_mv;
	double bridge_before_sigma_mv;
	double bridge_start_other_mv;
	int64_t wait_until_ms;
	int last_error;
	struct atten_cal_record records[ATTENUATOR_PHYSICAL_COUNT][ATTENUATOR_CAL_RECORD_COUNT];
	uint8_t record_count[ATTENUATOR_PHYSICAL_COUNT];
	bool record_overflow[ATTENUATOR_PHYSICAL_COUNT];
	struct attenuator_calibration_fit_metrics fit[ATTENUATOR_PHYSICAL_COUNT];
};

static const double initial_laser_levels_pct[] = {100.0, 50.0, 5.0};

static struct atten_cal_state_data cal;
static K_MUTEX_DEFINE(cal_lock);
static struct coo_cmd_response cal_telemetry_msg;

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

/** Return the retained-record and telemetry reason spelling for a reason id. */
static const char *record_reason_name(uint8_t reason)
{
	switch ((enum atten_cal_record_reason)reason) {
	case ATTEN_CAL_REASON_OK:
		return "ok";
	case ATTEN_CAL_REASON_SATURATED:
		return "saturated";
	case ATTEN_CAL_REASON_BELOW_SNR:
		return "below_snr";
	case ATTEN_CAL_REASON_ADC_ERROR:
		return "adc_error";
	case ATTEN_CAL_REASON_INVALID:
		return "invalid";
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

/** Return the absolute uncertainty of the current bridge normalization scale. */
static double current_scale_sigma_locked(void)
{
	return cal.segment_scale * sqrt(MAX(cal.segment_scale_rel_var, 0.0));
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
			    ",\"i\":%u,\"reason\":\"%s\","
			    "\"mean_mv\":%.6f,\"signal_mv\":%.6f,"
			    "\"rms_mv\":%.6f,\"sigma_y_mv\":%.6f,"
			    "\"snr\":%.6f,\"samples\":%u,\"max_raw\":%d,"
			    "\"saturated\":%s,\"usable\":%s,"
			    "\"fit_eligible\":%s,\"scale\":%.9g,"
			    "\"scale_sigma\":%.9g,\"relative_tx\":%.9g,"
			    "\"relative_signal_mv\":%.9g,"
			    "\"relative_signal_sigma_mv\":%.9g}",
			    cal.point_index, record_reason_name(record->reason),
			    (double)record->mean_mv, (double)record->signal_mv,
			    (double)record->rms_mv, (double)record->sigma_y_mv,
			    (double)record->snr, record->samples, record->max_raw,
			    (record->flags & ATTEN_CAL_RECORD_SATURATED) != 0U ? "true" : "false",
			    (record->flags & ATTEN_CAL_RECORD_USABLE) != 0U ? "true" : "false",
			    (record->flags & ATTEN_CAL_RECORD_FIT_ELIGIBLE) != 0U ? "true" : "false",
			    (double)record->scale, (double)record->scale_sigma,
			    (double)record->tx, (double)record->flux,
			    (double)record->flux_sigma) != 0) {
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

/** Decide whether a photodiode window clipped the ADC path. */
static bool sample_is_saturated(const struct photodiode_window_result *window)
{
	return window == NULL ||
	       window->max_raw >= ATTEN_CAL_SAT_RAW ||
	       window->mean_mv >= ATTEN_CAL_ADC_CLIP_MV ||
	       window->max_mv >= ATTEN_CAL_ADC_CLIP_MV;
}

/** Convert the current photodiode configurable window into the calibration measurement model. */
static void measurement_from_window(const struct photodiode_window_result *window,
				    struct atten_cal_measurement *measurement)
{
	if (measurement == NULL) {
		return;
	}
	memset(measurement, 0, sizeof(*measurement));
	measurement->reason = ATTEN_CAL_REASON_INVALID;

	if (window == NULL || !window->valid ||
	    window->sample_length == window->failed_samples) {
		measurement->reason = ATTEN_CAL_REASON_ADC_ERROR;
		return;
	}

	measurement->mean_mv = window->mean_mv;
	measurement->signal_mv = window->mean_net_mv;
	measurement->rms_mv = window->rms_mv;
	measurement->sigma_y_mv = window->mean_net_err_mv;
	measurement->samples = window->sample_length - window->failed_samples;
	measurement->max_raw = window->max_raw;
	measurement->saturated = sample_is_saturated(window);
	if (!(measurement->sigma_y_mv > 0.0) || !isfinite(measurement->sigma_y_mv)) {
		measurement->sigma_y_mv = ATTEN_CAL_ADC_LSB_MV;
	}

	if (measurement->saturated) {
		measurement->snr = NAN;
		measurement->reason = ATTEN_CAL_REASON_SATURATED;
		return;
	}

	measurement->snr = measurement->signal_mv / measurement->sigma_y_mv;
	if (measurement->signal_mv <= 0.0 ||
	    !isfinite(measurement->snr) ||
	    measurement->snr < ATTEN_CAL_SNR_USABLE) {
		measurement->reason = ATTEN_CAL_REASON_BELOW_SNR;
		return;
	}

	measurement->reason = ATTEN_CAL_REASON_OK;
	measurement->usable = true;
}

/** Retain one measurement, compute normalized fields, and optionally mark it fit-eligible. */
static bool append_record_locked(enum atten_cal_record_event event,
				 const struct atten_cal_measurement *measurement,
				 bool fit_candidate,
				 struct atten_cal_record **out)
{
	struct atten_cal_record *record;
	uint8_t physical = cal.physical_index;
	uint8_t index;
	double scale_sigma;
	double normalized_signal = 0.0;
	double normalized_sigma = 0.0;
	double tx = 0.0;
	double tx_sigma = 0.0;
	double rel_var = 0.0;

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

	scale_sigma = current_scale_sigma_locked();
	if (measurement->usable &&
	    cal.segment_scale > 0.0 &&
	    cal.open_signal_mv > 0.0) {
		normalized_signal = measurement->signal_mv / cal.segment_scale;
		rel_var = (measurement->sigma_y_mv / measurement->signal_mv) *
			  (measurement->sigma_y_mv / measurement->signal_mv) +
			  (scale_sigma / cal.segment_scale) *
			  (scale_sigma / cal.segment_scale);
		normalized_sigma = fabs(normalized_signal) * sqrt(rel_var);
		tx = measurement->signal_mv /
		     (cal.open_signal_mv * cal.segment_scale);
		rel_var += (cal.open_sigma_mv / cal.open_signal_mv) *
			   (cal.open_sigma_mv / cal.open_signal_mv);
		tx_sigma = fabs(tx) * sqrt(rel_var);
		ARG_UNUSED(tx_sigma);
	}

	record->sweep_mv = (float)cal.sweep_mv;
	record->other_mv = (float)cal.other_mv;
	record->laser_pct = (float)cal.laser_percent;
	record->mean_mv = (float)measurement->mean_mv;
	record->signal_mv = (float)measurement->signal_mv;
	record->rms_mv = (float)measurement->rms_mv;
	record->sigma_y_mv = (float)measurement->sigma_y_mv;
	record->sigma_x_mv = (float)ATTEN_CAL_DAC_SIGMA_MV;
	record->snr = (float)measurement->snr;
	record->flux = (float)normalized_signal;
	record->flux_sigma = (float)normalized_sigma;
	record->scale = (float)cal.segment_scale;
	record->scale_sigma = (float)scale_sigma;
	record->tx = (float)tx;
	record->b = NAN;
	record->residual_db = NAN;
	record->max_raw = measurement->max_raw;
	record->samples = measurement->samples;
	record->event = (uint8_t)event;
	record->reason = (uint8_t)measurement->reason;
	record->segment = cal.segment_id;
	if (measurement->saturated) {
		record->flags |= ATTEN_CAL_RECORD_SATURATED;
	}
	if (measurement->usable) {
		record->flags |= ATTEN_CAL_RECORD_USABLE;
	}
	if (fit_candidate && measurement->usable && point_valid_for_fit(tx)) {
		record->flags |= ATTEN_CAL_RECORD_FIT_ELIGIBLE;
	}

	cal.point_index = index;
	atten_cal_emit_record(record);
	if (out != NULL) {
		*out = record;
	}
	return true;
}

/** Map a laser id to the MEMS input route used for calibration launch. */
static int route_input_for_laser(enum hispec_laser_id laser, char *out, size_t out_len)
{
	const char *name;

	if (out == NULL || out_len == 0U) {
		return -EINVAL;
	}

	switch (laser) {
	case HISPEC_LASER_1430_YJ:
		name = "yj_1430";
		break;
	case HISPEC_LASER_1430_HK:
		name = "hk_1430";
		break;
	case HISPEC_LASER_1028_Y:
	case HISPEC_LASER_1270_J:
		name = "yj_laser";
		break;
	case HISPEC_LASER_1510_H:
	case HISPEC_LASER_2330_K:
		name = "hk_laser";
		break;
	default:
		return -EINVAL;
	}

	if (snprintk(out, out_len, "%s", name) >= out_len) {
		return -ENOSPC;
	}
	return 0;
}

/** Choose the photodiode channel and loopback route for automatic TIB calibration. */
static int pd_route_for_auto(enum hispec_laser_id laser, char fiber,
			     enum photodiode_channel *channel,
			     char *input, size_t input_len,
			     char *output, size_t output_len)
{
	const char *prefix;
	const char *kind;

	if (channel == NULL || input == NULL || output == NULL ||
	    input_len == 0U || output_len == 0U ||
	    (fiber != 'M' && fiber != 'S')) {
		return -EINVAL;
	}

	switch (laser) {
	case HISPEC_LASER_1028_Y:
	case HISPEC_LASER_1270_J:
	case HISPEC_LASER_1430_YJ:
		*channel = PHOTODIODE_CHANNEL_YJ;
		prefix = "yj";
		break;
	case HISPEC_LASER_1430_HK:
	case HISPEC_LASER_1510_H:
	case HISPEC_LASER_2330_K:
		*channel = PHOTODIODE_CHANNEL_HK;
		prefix = "hk";
		break;
	default:
		return -EINVAL;
	}

	kind = fiber == 'M' ? "mm" : "sm";
	if (snprintk(input, input_len, "%s_%s", prefix, kind) >= input_len ||
	    snprintk(output, output_len, "%s_pd", prefix) >= output_len) {
		return -ENOSPC;
	}
	return 0;
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
	cal.segment_scale = 1.0;
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
	cal.search_low_mv = 0.0;
	cal.search_high_mv = ATTENUATOR_DRIVE_MAX_MV;
	cal.sweep_low_mv = 0.0;
	cal.sweep_high_mv = ATTENUATOR_DRIVE_MAX_MV;
	cal.open_signal_mv = 0.0;
	cal.open_sigma_mv = 0.0;
	cal.segment_scale = 1.0;
	cal.segment_scale_rel_var = 0.0;
	cal.bridge_before_signal_mv = 0.0;
	cal.bridge_before_sigma_mv = 0.0;
	cal.bridge_start_other_mv = cal.other_mv;
	cal.laser_level_index = 0U;
	cal.laser_percent = initial_laser_levels_pct[0];
	cal.record_count[physical] = 0U;
	cal.record_overflow[physical] = false;
	memset(cal.records[physical], 0, sizeof(cal.records[physical]));
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

/** Start a bridge-normalization operation from the last usable DUT point. */
static void auto_begin_bridge_locked(void)
{
	if (cal.other_mv <= ATTEN_CAL_SEARCH_STEP_MV) {
		auto_finish_physical_locked();
		return;
	}
	cal.sweep_mv = cal.sweep_low_mv;
	cal.bridge_start_other_mv = cal.other_mv;
	auto_schedule_measure_locked(ATTEN_CAL_MEASURE_BRIDGE_BEFORE,
				     cal.sweep_low_mv, cal.other_mv);
}

/** Handle companion-search probes used to find a non-saturated open reference. */
static void auto_handle_initial_probe_locked(const struct atten_cal_measurement *measurement)
{
	struct atten_cal_record *record = NULL;

	(void)append_record_locked(ATTEN_CAL_EVENT_INITIAL_PROBE, measurement, false, &record);
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
		cal.search_low_mv = 0.0;
		cal.search_high_mv = ATTENUATOR_DRIVE_MAX_MV;
		cal.search_tries = 0U;
		auto_schedule_measure_locked(ATTEN_CAL_MEASURE_INITIAL_PROBE,
					     0.0, ATTENUATOR_DRIVE_MAX_MV);
		return;
	}

	if (measurement->saturated) {
		cal.search_low_mv = cal.other_mv;
	} else {
		cal.search_high_mv = cal.other_mv;
	}
	cal.search_tries++;

	if (cal.search_high_mv - cal.search_low_mv <= ATTEN_CAL_SEARCH_STEP_MV ||
	    cal.search_tries >= ATTEN_CAL_MAX_SEARCH_TRIES) {
		auto_schedule_measure_locked(ATTEN_CAL_MEASURE_REFERENCE,
					     0.0, cal.search_high_mv);
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
		(void)append_record_locked(ATTEN_CAL_EVENT_POINT, measurement, false, NULL);
		auto_error_locked(-ERANGE);
		return;
	}

	cal.open_signal_mv = measurement->signal_mv;
	cal.open_sigma_mv = measurement->sigma_y_mv;
	cal.segment_scale = 1.0;
	cal.segment_scale_rel_var = 0.0;
	(void)append_record_locked(ATTEN_CAL_EVENT_POINT, measurement, true, &record);
	if (record == NULL) {
		auto_error_locked(-ENOSPC);
		return;
	}

	cal.sweep_low_mv = 0.0;
	cal.sweep_high_mv = ATTENUATOR_DRIVE_MAX_MV;
	auto_schedule_measure_locked(ATTEN_CAL_MEASURE_SWEEP,
				     sweep_midpoint_locked(), cal.other_mv);
}

/** Handle ordinary DUT sweep measurements and decide whether to continue or bridge. */
static void auto_handle_sweep_locked(const struct atten_cal_measurement *measurement)
{
	struct atten_cal_record *record = NULL;

	(void)append_record_locked(ATTEN_CAL_EVENT_POINT, measurement,
				   measurement != NULL && measurement->usable, &record);
	if (record == NULL) {
		auto_finish_physical_locked();
		return;
	}

	if (measurement->usable) {
		cal.sweep_low_mv = cal.sweep_mv;
		if (ATTENUATOR_DRIVE_MAX_MV - cal.sweep_low_mv <= ATTEN_CAL_SEARCH_STEP_MV) {
			auto_finish_physical_locked();
			return;
		}
	} else {
		cal.sweep_high_mv = cal.sweep_mv;
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

/** Record the low-signal side of a bridge and begin opening the companion FVOA. */
static void auto_handle_bridge_before_locked(const struct atten_cal_measurement *measurement)
{
	struct atten_cal_record *record = NULL;

	(void)append_record_locked(ATTEN_CAL_EVENT_BRIDGE_BEFORE, measurement,
				   measurement != NULL && measurement->usable, &record);
	if (record == NULL) {
		auto_finish_physical_locked();
		return;
	}
	if (measurement == NULL || !measurement->usable || !(measurement->signal_mv > 0.0)) {
		auto_finish_physical_locked();
		return;
	}

	cal.bridge_before_signal_mv = measurement->signal_mv;
	cal.bridge_before_sigma_mv = measurement->sigma_y_mv;
	cal.search_low_mv = 0.0;
	cal.search_high_mv = cal.other_mv;
	cal.search_tries = 0U;
	auto_schedule_measure_locked(ATTEN_CAL_MEASURE_BRIDGE_PROBE,
				     cal.sweep_low_mv, search_midpoint_locked());
}

/** Handle companion-FVOA bridge probes during the non-saturated binary search. */
static void auto_handle_bridge_probe_locked(const struct atten_cal_measurement *measurement)
{
	struct atten_cal_record *record = NULL;

	(void)append_record_locked(ATTEN_CAL_EVENT_BRIDGE_PROBE, measurement, false, &record);
	if (record == NULL) {
		auto_finish_physical_locked();
		return;
	}

	if (measurement->saturated) {
		cal.search_low_mv = cal.other_mv;
	} else {
		cal.search_high_mv = cal.other_mv;
	}
	cal.search_tries++;

	if (cal.search_high_mv - cal.search_low_mv <= ATTEN_CAL_SEARCH_STEP_MV ||
	    cal.search_tries >= ATTEN_CAL_MAX_SEARCH_TRIES) {
		if (cal.search_high_mv >= cal.bridge_start_other_mv - ATTEN_CAL_SEARCH_STEP_MV) {
			auto_finish_physical_locked();
			return;
		}
		auto_schedule_measure_locked(ATTEN_CAL_MEASURE_BRIDGE_AFTER,
					     cal.sweep_low_mv, cal.search_high_mv);
		return;
	}

	auto_schedule_measure_locked(ATTEN_CAL_MEASURE_BRIDGE_PROBE,
				     cal.sweep_low_mv, search_midpoint_locked());
}

/** Accept bridge normalization, update scale variance, and resume DUT sweeping. */
static void auto_handle_bridge_after_locked(const struct atten_cal_measurement *measurement)
{
	struct atten_cal_record *record = NULL;
	double ratio;

	if (measurement == NULL || !measurement->usable ||
	    !(cal.bridge_before_signal_mv > 0.0) ||
	    !(measurement->signal_mv > 0.0)) {
		(void)append_record_locked(ATTEN_CAL_EVENT_BRIDGE_AFTER, measurement, false, NULL);
		auto_finish_physical_locked();
		return;
	}

	ratio = measurement->signal_mv / cal.bridge_before_signal_mv;
	if (!(ratio > 1.0) || !isfinite(ratio)) {
		(void)append_record_locked(ATTEN_CAL_EVENT_BRIDGE_AFTER, measurement, false, NULL);
		auto_finish_physical_locked();
		return;
	}

	cal.segment_scale *= ratio;
	cal.segment_scale_rel_var +=
		(cal.bridge_before_sigma_mv / cal.bridge_before_signal_mv) *
		(cal.bridge_before_sigma_mv / cal.bridge_before_signal_mv) +
		(measurement->sigma_y_mv / measurement->signal_mv) *
		(measurement->sigma_y_mv / measurement->signal_mv);
	cal.segment_id++;

	(void)append_record_locked(ATTEN_CAL_EVENT_BRIDGE_AFTER, measurement, true, &record);
	if (record == NULL) {
		auto_finish_physical_locked();
		return;
	}

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

/** Convert one retained record into weighted linear-fit coordinates. */
static bool record_to_fit_point(const struct atten_cal_record *record,
				double gain, double *x_out,
				double *delta_out, double *sigma_delta_out)
{
	double tx;
	double sigma_tx;
	double tx_lo;
	double tx_hi;
	double delta;
	double delta_lo;
	double delta_hi;
	double signal_rel;

	if (record == NULL ||
	    (record->flags & ATTEN_CAL_RECORD_FIT_ELIGIBLE) == 0U ||
	    !(record->flux > 0.0f) ||
	    !(record->flux_sigma > 0.0f)) {
		return false;
	}

	tx = (double)record->tx;
	if (!(tx > ATTEN_CAL_MIN_TX) || !(tx < ATTEN_CAL_MAX_TX)) {
		return false;
	}
	if (!attenuator_model_linear_to_b(tx, &delta)) {
		return false;
	}

	signal_rel = (double)record->flux_sigma / (double)record->flux;
	sigma_tx = tx * signal_rel;
	if (!(sigma_tx > 0.0) || !isfinite(sigma_tx)) {
		sigma_tx = ATTEN_CAL_MIN_TX;
	}
	tx_lo = MAX(ATTEN_CAL_MIN_TX, tx - sigma_tx);
	tx_hi = MIN(ATTEN_CAL_MAX_TX, tx + sigma_tx);
	if (!attenuator_model_linear_to_b(tx_lo, &delta_lo) ||
	    !attenuator_model_linear_to_b(tx_hi, &delta_hi)) {
		return false;
	}

	*x_out = (double)record->sweep_mv * gain;
	*delta_out = delta;
	*sigma_delta_out = MAX(fabs(delta_hi - delta_lo) * 0.5, 1.0e-6);
	return true;
}

/** Fit one physical FVOA's retained records to the firmware attenuator model. */
static int fit_one_physical_locked(uint8_t physical,
				   struct attenuator_calibration_fit_metrics *out)
{
	struct attenuator *atten = &attenuators[cal.attenuator_index];
	double gain = physical == 0U ? atten->coeff1.gain : atten->coeff2.gain;
	double slope = 0.0;
	double intercept = 0.0;
	double min_tx = 1.0;
	double max_tx = 0.0;
	double min_x = ATTENUATOR_DRIVE_MAX_MV * ATTENUATOR_DEFAULT_GAIN;
	double max_x = 0.0;
	double sum_sq_db = 0.0;
	double max_abs_db = 0.0;
	uint8_t point_count = 0U;

	if (out == NULL || physical >= ATTENUATOR_PHYSICAL_COUNT || !(gain > 0.0)) {
		return -EINVAL;
	}
	memset(out, 0, sizeof(*out));

	for (uint8_t i = 0U; i < cal.record_count[physical]; ++i) {
		cal.records[physical][i].flags &= (uint8_t)~ATTEN_CAL_RECORD_FIT_INCLUDED;
		cal.records[physical][i].b = NAN;
		cal.records[physical][i].residual_db = NAN;
	}

	for (uint8_t iter = 0U; iter < 3U; ++iter) {
		double sum_w = 0.0;
		double sum_x = 0.0;
		double sum_y = 0.0;
		double sum_xx = 0.0;
		double sum_xy = 0.0;

		point_count = 0U;
		for (uint8_t i = 0U; i < cal.record_count[physical]; ++i) {
			struct atten_cal_record *record = &cal.records[physical][i];
			double x;
			double delta;
			double sigma_delta;
			double var;
			double w;

			if (!record_to_fit_point(record, gain, &x, &delta, &sigma_delta)) {
				continue;
			}
			var = sigma_delta * sigma_delta +
			      slope * slope * (gain * (double)record->sigma_x_mv) *
				      (gain * (double)record->sigma_x_mv);
			w = 1.0 / MAX(var, 1.0e-12);
			sum_w += w;
			sum_x += w * x;
			sum_y += w * delta;
			sum_xx += w * x * x;
			sum_xy += w * x * delta;
			point_count++;
		}

		if (point_count < ATTEN_CAL_MIN_FIT_POINTS ||
		    !(sum_w > 0.0) ||
		    !(sum_w * sum_xx - sum_x * sum_x > 0.0)) {
			return -ERANGE;
		}
		slope = (sum_w * sum_xy - sum_x * sum_y) /
			(sum_w * sum_xx - sum_x * sum_x);
		intercept = (sum_y - slope * sum_x) / sum_w;
		if (!(slope > 0.0) || !isfinite(intercept)) {
			return -ERANGE;
		}
	}

	{
		double sum_x = 0.0;
		double sum_y = 0.0;
		double sum_xx = 0.0;
		double sum_yy = 0.0;
		double sum_xy = 0.0;
		uint8_t included = 0U;

		for (uint8_t i = 0U; i < cal.record_count[physical]; ++i) {
			struct atten_cal_record *record = &cal.records[physical][i];
			double x;
			double delta;
			double sigma_delta;
			double predicted_tx;
			double residual_db;

			if (!record_to_fit_point(record, gain, &x, &delta, &sigma_delta)) {
				ARG_UNUSED(sigma_delta);
				continue;
			}
			predicted_tx = attenuator_model_b_to_linear(slope * x + intercept);
			if (!(predicted_tx > 0.0)) {
				continue;
			}
			residual_db = 10.0 * log10(predicted_tx / (double)record->tx);
			record->flags |= ATTEN_CAL_RECORD_FIT_INCLUDED;
			record->b = (float)delta;
			record->residual_db = (float)residual_db;
			sum_sq_db += residual_db * residual_db;
			max_abs_db = MAX(max_abs_db, fabs(residual_db));
			min_tx = MIN(min_tx, (double)record->tx);
			max_tx = MAX(max_tx, (double)record->tx);
			min_x = MIN(min_x, x);
			max_x = MAX(max_x, x);
			sum_x += x;
			sum_y += delta;
			sum_xx += x * x;
			sum_yy += delta * delta;
			sum_xy += x * delta;
			included++;
		}

		if (included < ATTEN_CAL_MIN_FIT_POINTS || !(max_x > min_x)) {
			return -ERANGE;
		}
		{
			double denom_x = (double)included * sum_xx - sum_x * sum_x;
			double denom_y = (double)included * sum_yy - sum_y * sum_y;

			if (!(denom_x > 0.0) || !(denom_y > 0.0)) {
				return -ERANGE;
			}
			out->correlation = ((double)included * sum_xy - sum_x * sum_y) /
					   sqrt(denom_x * denom_y);
		}
		point_count = included;
	}

	out->valid = true;
	out->points = point_count;
	out->slope_inv_fvoa_mv = slope;
	out->fvoa_50pct_mv = -intercept / slope;
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
	char route_input[MEMS_SOURCEDEST_MAX_LEN] = {0};
	char pd_input[MEMS_SOURCEDEST_MAX_LEN] = {0};
	char pd_output[MEMS_SOURCEDEST_MAX_LEN] = {0};
	enum photodiode_channel channel;
	struct app_photodiode_settings pd_settings;
	struct photodiode_status pd_status;
	uint8_t attenuator_index;
	double dark_mv;
	bool pd_power = false;
	bool replacing;
	int rc;

	if (devices_board_type() != HISPEC_BOARD_TIB) {
		return -ENODEV;
	}
	if (request == NULL || request->output == NULL ||
	    request->output[0] == '\0' ||
	    attenuator_index_from_laser_id(request->laser, &attenuator_index) != 0 ||
	    !devices_attenuator_channel_available(attenuator_index)) {
		return -EINVAL;
	}

	rc = route_input_for_laser(request->laser, route_input, sizeof(route_input));
	if (rc != 0) {
		return rc;
	}
	rc = pd_route_for_auto(request->laser, request->fiber, &channel,
			       pd_input, sizeof(pd_input),
			       pd_output, sizeof(pd_output));
	if (rc != 0) {
		return rc;
	}

	app_settings_get_photodiode(&pd_settings);
	dark_mv = pd_settings.channel[channel].dark.mean_mv;
	if (!isfinite(dark_mv) ||
	    dark_mv < PHOTODIODE_DARK_MIN_MV ||
	    dark_mv > PHOTODIODE_DARK_MAX_MV) {
		return -EINVAL;
	}
	rc = housekeeping_power_get((enum housekeeping_power_output)channel, &pd_power);
	if (rc != 0) {
		return rc;
	}
	if (!pd_power) {
		return -EACCES;
	}
	photodiode_get_status(&pd_status);
	if (!isfinite(pd_status.channel[channel].mv)) {
		return -ENODATA;
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

	rc = mems_router_apply_named_route(&router, route_input, request->output,
					   false, NULL, NULL);
	if (rc == 0) {
		rc = mems_router_apply_named_route(&router, pd_input, pd_output,
						   false, NULL, NULL);
	}
	if (rc != 0) {
		return rc;
	}
	rc = photodiode_set_configurable_window_duration(channel,
							 clamp_dwell(request->dwell_ms));
	if (rc != 0) {
		return rc;
	}
	if (!set_physical_pair(attenuator_index, 0U,
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
	cal.attenuator_index = attenuator_index;
	cal.physical_index = 0U;
	cal.dwell_ms = clamp_dwell(request->dwell_ms);
	cal.persistent = request->persist;
	cal.laser = request->laser;
	cal.channel = channel;
	cal.laser_percent = 0.0;
	LOG_INF("atten cal using configured photodiode dark channel=%s dark_mv=%.6f",
		photodiode_channel_names[channel], dark_mv);
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
		     ((size_t)count * sizeof(struct atten_cal_record));
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
	bytes[15] = (uint8_t)sizeof(struct atten_cal_record);
	if (count > 0U) {
		memcpy(bytes + ATTEN_CAL_DATA_CHUNK_HEADER_SIZE,
		       &cal.records[physical_index][start_index],
		       (size_t)count * sizeof(struct atten_cal_record));
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
