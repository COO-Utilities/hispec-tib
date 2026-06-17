/**
 * @file attenuator_calibration.c
 * @brief Automatic and manual attenuator calibration.
 */

/* Automatic TIB calibration is a lab acquisition routine first and a model fit
 * second. Each logical attenuator has two physical FVOAs in series. The FVOA
 * range is much larger than the photodiode plus laser dynamic range, so one
 * FVOA and the laser level are used as range extenders while the other FVOA is
 * swept.
 *
 * Owner lab intent, retained until the physical model is validated:
 *   1. Put one FVOA open, one at maximum attenuation, laser at full power.
 *   2. Walk the swept FVOA down until the photodiode leaves saturation.
 *   3. If the whole range is saturated, reduce laser power and repeat.
 *   4. Once a usable corridor is found, densely probe the high-attenuation
 *      knee, then use the other FVOA as a range extender to keep exploring.
 *   5. Repeat for the other physical FVOA.
 *
 * The nominal sweep is the digitized datasheet dB/V curve converted to
 * DAC-side millivolts, plus a 5 V FVOA plateau point. It is deliberately dense
 * around the knee; do not replace it with evenly spaced DAC codes without
 * matching lab evidence.
 *
 * A failed fit is not a failed acquisition. The run completes, does not persist
 * coefficients, and leaves retained records for host-side analysis.
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
#define ATTEN_CAL_STEP_SETTLE_MS 50U
#define ATTEN_CAL_PD_POWER_SETTLE_MS 1000U
#define ATTEN_CAL_SAT_RAW (INT16_MAX - 1024)
#define ATTEN_CAL_MIN_STEP_MV 10.0
#define ATTEN_CAL_LINK_MIN_STEP_MV 20.0
#define ATTEN_CAL_MAX_ADJUST_TRIES 20U
#define ATTEN_CAL_MIN_FIT_POINTS ATTENUATOR_CAL_MIN_BATCH_POINTS
#define ATTEN_CAL_MIN_TX 1.0e-10
#define ATTEN_CAL_MAX_TX 0.999999
#define ATTEN_CAL_MIN_FIT_CORR 0.85
#define ATTEN_CAL_SNR_USABLE 5.0
#define ATTEN_CAL_SNR_FIT 5.0
#define ATTEN_CAL_ADC_LSB_MV 0.1875
#define ATTEN_CAL_ADC_CLIP_MV 5000.0
#define ATTEN_CAL_DAC_SIGMA_MV 3.0
#define ATTEN_CAL_TELEMETRY_TOPIC_SUFFIX "atten"
#define ATTEN_CAL_DATA_CHUNK_HEADER_SIZE 16U
#define ATTEN_CAL_DATA_CHUNK_RECORDS \
	((COO_CMD_PAYLOAD_MAX - ATTEN_CAL_DATA_CHUNK_HEADER_SIZE) / sizeof(struct atten_cal_record))
#define ATTEN_CAL_DATA_CHUNK_MAGIC0 'H'
#define ATTEN_CAL_DATA_CHUNK_MAGIC1 'A'
#define ATTEN_CAL_DATA_CHUNK_MAGIC2 'C'
#define ATTEN_CAL_DATA_CHUNK_MAGIC3 '2'
#define ATTEN_CAL_DATA_CHUNK_VERSION 1U

enum atten_cal_state {
	ATTEN_CAL_STATE_INACTIVE = 0,
	ATTEN_CAL_STATE_RUNNING,
	ATTEN_CAL_STATE_WAITING,
	ATTEN_CAL_STATE_COMPLETE,
	ATTEN_CAL_STATE_ERROR,
};

enum atten_cal_mode {
	ATTEN_CAL_MODE_NONE = 0,
	ATTEN_CAL_MODE_TIB_AUTO,
	ATTEN_CAL_MODE_MANUAL,
};

enum atten_cal_auto_phase {
	ATTEN_CAL_AUTO_NONE = 0,
	ATTEN_CAL_AUTO_PD_SETTLE,
	ATTEN_CAL_AUTO_MEASURE_SET,
	ATTEN_CAL_AUTO_MEASURE_SETTLE,
	ATTEN_CAL_AUTO_MEASURE_AVG,
	ATTEN_CAL_AUTO_FIT,
};

enum atten_cal_measure_kind {
	ATTEN_CAL_MEASURE_NONE = 0,
	ATTEN_CAL_MEASURE_POINT,
	ATTEN_CAL_MEASURE_ANCHOR_BEFORE,
	ATTEN_CAL_MEASURE_LINK_PROBE,
	ATTEN_CAL_MEASURE_ANCHOR_AFTER,
};

enum atten_cal_record_event {
	ATTEN_CAL_EVENT_POINT = 0,
	ATTEN_CAL_EVENT_ANCHOR_BEFORE,
	ATTEN_CAL_EVENT_LINK_PROBE,
	ATTEN_CAL_EVENT_ANCHOR_AFTER,
	ATTEN_CAL_EVENT_MANUAL,
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

struct atten_cal_point {
	double voltage_mv;
	double flux;
	bool valid;
	bool saturated;
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
	enum atten_cal_auto_phase phase;
	enum atten_cal_measure_kind measure_kind;
	uint8_t attenuator_index;
	uint8_t physical_index;
	uint8_t point_index;
	uint8_t schedule_index;
	uint32_t dwell_ms;
	bool persistent;
	enum hispec_laser_id laser;
	enum photodiode_channel channel;
	double other_mv;
	double laser_percent;
	double scale;
	double scale_rel_var;
	double sweep_mv;
	bool have_last_good;
	double last_good_mv;
	double last_good_signal_mv;
	double last_good_sigma_mv;
	bool have_low_signal;
	double low_signal_mv;
	bool have_sat;
	double sat_mv;
	double link_anchor_mv;
	double link_retry_mv;
	double link_before_signal_mv;
	double link_before_sigma_mv;
	double link_low_other_mv;
	double link_high_other_mv;
	int64_t wait_until_ms;
	uint8_t laser_level_index;
	uint8_t segment_id;
	uint8_t anchor_id;
	uint8_t adjust_tries;
	int last_error;
	struct atten_cal_point points[2][ATTENUATOR_CAL_POINT_COUNT];
	struct atten_cal_record records[2][ATTENUATOR_CAL_RECORD_COUNT];
	uint8_t record_count[2];
	bool record_overflow[2];
	struct attenuator_calibration_fit_metrics fit[2];
};

static const double voltage_schedule[ATTENUATOR_CAL_POINT_COUNT] = {
	3261.6, 2074.4, 2035.2, 1983.0, 1950.4,
	1917.8, 1885.2, 1852.6, 1820.0, 1787.3,
	1754.7, 1722.1, 1696.0, 1663.4, 1611.2,
	1572.1, 1513.4, 1454.7, 1232.9, 0.0,
};

static const double auto_laser_levels_pct[] = {100.0, 10.0, 2.0};

static struct atten_cal_state_data cal;
static K_MUTEX_DEFINE(cal_lock);
/* Calibration emit helpers are called with cal_lock held. Keep the response
 * buffer off the throughput thread stack while calibration runs from that
 * thread.
 */
static struct coo_cmd_response cal_telemetry_msg;

static uint8_t complete_percent_locked(void);
static bool sample_is_saturated(const struct photodiode_average_status *avg);
static bool point_valid_for_fit(double tx);
static double current_sweep_mv_locked(void);
static void auto_finish_physical_locked(void);
static void auto_error_locked(int error);

/* -------------------------------------------------------------------------- */
/* Names, telemetry, and compact operator-visible events.                     */

static const char *state_name(enum atten_cal_state state)
{
	switch (state) {
	case ATTEN_CAL_STATE_INACTIVE:
		return "inactive";
	case ATTEN_CAL_STATE_RUNNING:
		return "running";
	case ATTEN_CAL_STATE_WAITING:
		return "waiting";
	case ATTEN_CAL_STATE_COMPLETE:
		return "complete";
	case ATTEN_CAL_STATE_ERROR:
		return "error";
	default:
		return "unknown";
	}
}

static const char *mode_name(enum atten_cal_mode mode)
{
	switch (mode) {
	case ATTEN_CAL_MODE_TIB_AUTO:
		return "tib_auto";
	case ATTEN_CAL_MODE_MANUAL:
		return "manual";
	case ATTEN_CAL_MODE_NONE:
	default:
		return "none";
	}
}

static const char *physical_name(uint8_t physical_index)
{
	return physical_index == 0U ? "dac1" : "dac2";
}

static double current_sweep_mv_locked(void)
{
	if (cal.mode == ATTEN_CAL_MODE_TIB_AUTO) {
		return cal.sweep_mv;
	}
	return voltage_schedule[MIN(cal.point_index, ATTENUATOR_CAL_POINT_COUNT - 1U)];
}

static const char *record_event_name(uint8_t event)
{
	switch ((enum atten_cal_record_event)event) {
	case ATTEN_CAL_EVENT_POINT:
		return "point";
	case ATTEN_CAL_EVENT_ANCHOR_BEFORE:
		return "anchor_before";
	case ATTEN_CAL_EVENT_LINK_PROBE:
		return "link_probe";
	case ATTEN_CAL_EVENT_ANCHOR_AFTER:
		return "anchor_after";
	case ATTEN_CAL_EVENT_MANUAL:
		return "manual";
	default:
		return "unknown";
	}
}

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
	default:
		return "invalid";
	}
}

static void atten_cal_publish_telemetry(struct coo_cmd_response *msg)
{
	if (msg == NULL) {
		return;
	}

	msg->payload_len = strlen(msg->payload);
	(void)coo_cmd_runtime_emit(command_runtime_get(),
				   &(const struct coo_cmd_runtime_emit_args){
					   .type = COO_CMD_RUNTIME_EMIT_DATA,
					   .delivery = COO_CMD_RUNTIME_EMIT_BEST_EFFORT,
					   .suffix = ATTEN_CAL_TELEMETRY_TOPIC_SUFFIX,
					   .out = msg,
				   });
}

static struct coo_cmd_response *atten_cal_telemetry_begin(size_t *off,
							  const char *event)
{
	struct coo_cmd_response *msg = &cal_telemetry_msg;

	if (msg == NULL || off == NULL || event == NULL) {
		return NULL;
	}

	memset(msg, 0, sizeof(*msg));
	*off = 0U;
	if (coo_json_append(msg->payload, sizeof(msg->payload), off,
			"{\"event\":\"%s\",\"state\":\"%s\",\"mode\":\"%s\","
			"\"physical\":\"%s\",\"attenuator\":%u,\"point_index\":%u,"
			"\"point_count\":%u,\"complete_pct\":%u",
			event, state_name(cal.state), mode_name(cal.mode),
			physical_name(cal.physical_index), cal.attenuator_index,
			cal.point_index,
			cal.mode == ATTEN_CAL_MODE_TIB_AUTO ?
				ATTENUATOR_CAL_RECORD_COUNT : ATTENUATOR_CAL_POINT_COUNT,
			complete_percent_locked()) != 0) {
		return NULL;
	}
	return msg;
}

static void atten_cal_emit_simple(const char *event)
{
	struct coo_cmd_response *msg;
	size_t off;

	msg = atten_cal_telemetry_begin(&off, event);
	if (msg == NULL ||
	    coo_json_append(msg->payload, sizeof(msg->payload), &off,
			    ",\"dwell_ms\":%u,\"other_mv\":%.3f,"
			    "\"laser\":\"%s\",\"laser_pct\":%.3f,"
			    "\"pd_channel\":\"%s\",\"error\":%d}",
			    cal.dwell_ms, cal.other_mv,
			    hispec_laser_name(cal.laser), cal.laser_percent,
			    photodiode_channel_names[cal.channel],
			    cal.last_error) != 0) {
		LOG_WRN("atten calibration telemetry payload too large");
		return;
	}

	atten_cal_publish_telemetry(msg);
	LOG_INF("atten cal %s physical=%s point=%u/%u other_mv=%.1f error=%d",
		event, physical_name(cal.physical_index),
		MIN(cal.point_index + 1U, ATTENUATOR_CAL_POINT_COUNT),
		ATTENUATOR_CAL_POINT_COUNT, cal.other_mv, cal.last_error);
}

static void atten_cal_emit_point_set(const char *event, double sweep_mv)
{
	struct coo_cmd_response *msg;
	uint8_t display_index = MIN(cal.point_index + 1U, ATTENUATOR_CAL_POINT_COUNT);
	size_t off;

	if (cal.mode == ATTEN_CAL_MODE_TIB_AUTO &&
	    cal.measure_kind == ATTEN_CAL_MEASURE_POINT) {
		display_index = MIN(cal.schedule_index + 1U, ATTENUATOR_CAL_POINT_COUNT);
	}

	msg = atten_cal_telemetry_begin(&off, event);
	if (msg == NULL ||
	    coo_json_append(msg->payload, sizeof(msg->payload), &off,
			    ",\"sweep_mv\":%.3f,\"other_mv\":%.3f,"
			    "\"laser_pct\":%.3f}",
			    sweep_mv, cal.other_mv, cal.laser_percent) != 0) {
		LOG_WRN("atten calibration telemetry payload too large");
		return;
	}

	atten_cal_publish_telemetry(msg);
	LOG_INF("atten cal %s physical=%s point=%u/%u sweep_mv=%.1f other_mv=%.1f laser_pct=%.2f",
		event, physical_name(cal.physical_index),
		display_index, ATTENUATOR_CAL_POINT_COUNT, sweep_mv, cal.other_mv,
		cal.laser_percent);
}

static void atten_cal_emit_record(const struct atten_cal_record *record)
{
	struct coo_cmd_response *msg;
	size_t off;

	if (record == NULL) {
		return;
	}

	msg = atten_cal_telemetry_begin(&off, record_event_name(record->event));
	if (msg == NULL ||
	    coo_json_append(msg->payload, sizeof(msg->payload), &off,
			    ",\"i\":%u,\"reason\":\"%s\",\"segment\":%u,"
			    "\"sweep_mv\":%.3f,\"other_mv\":%.3f,"
			    "\"laser_pct\":%.3f,\"mean_mv\":%.6f,"
			    "\"signal_mv\":%.6f,\"rms_mv\":%.6f,"
			    "\"sigma_y_mv\":%.6f,\"sigma_x_mv\":%.6f,"
			    "\"snr\":%.6f,\"samples\":%u,\"max_raw\":%d,"
			    "\"saturated\":%s,\"usable\":%s,"
			    "\"fit_eligible\":%s,\"scale\":%.12g,"
			    "\"scale_sigma\":%.12g,\"flux\":%.12g,"
			    "\"flux_sigma\":%.12g}",
			    cal.record_count[cal.physical_index] == 0U ? 0U :
				    (uint8_t)(cal.record_count[cal.physical_index] - 1U),
			    record_reason_name(record->reason), record->segment,
			    (double)record->sweep_mv, (double)record->other_mv,
			    (double)record->laser_pct,
			    (double)record->mean_mv, (double)record->signal_mv,
			    (double)record->rms_mv,
			    (double)record->sigma_y_mv, (double)record->sigma_x_mv,
			    (double)record->snr,
			    record->samples, record->max_raw,
			    (record->flags & ATTEN_CAL_RECORD_SATURATED) != 0U ? "true" : "false",
			    (record->flags & ATTEN_CAL_RECORD_USABLE) != 0U ? "true" : "false",
			    (record->flags & ATTEN_CAL_RECORD_FIT_ELIGIBLE) != 0U ? "true" : "false",
			    (double)record->scale, (double)record->scale_sigma,
			    (double)record->flux,
			    (double)record->flux_sigma) != 0) {
		LOG_WRN("atten calibration telemetry payload too large");
		return;
	}

	atten_cal_publish_telemetry(msg);
	LOG_INF("atten cal %s physical=%s rec=%u sweep_mv=%.1f other_mv=%.1f signal_mv=%.4f snr=%.2f reason=%s flags=0x%02x",
		record_event_name(record->event), physical_name(cal.physical_index),
		cal.record_count[cal.physical_index] == 0U ? 0U :
			(uint8_t)(cal.record_count[cal.physical_index] - 1U),
		(double)record->sweep_mv, (double)record->other_mv,
		(double)record->signal_mv, (double)record->snr,
		record_reason_name(record->reason), record->flags);
}

static void atten_cal_emit_fit(uint8_t physical,
			       const struct attenuator_calibration_fit_metrics *fit)
{
	struct coo_cmd_response *msg;
	size_t off;

	if (fit == NULL) {
		return;
	}

	msg = atten_cal_telemetry_begin(&off, "fit");
	if (msg == NULL ||
	    coo_json_append(msg->payload, sizeof(msg->payload), &off,
			    ",\"fit_physical\":\"%s\",\"valid\":%s,"
			    "\"accepted\":%s,\"points\":%u,"
			    "\"slope\":%.12g,\"offset\":%.12g,"
			    "\"corr\":%.12g,\"rms_db\":%.12g,"
			    "\"max_abs_db\":%.12g,\"min_tx\":%.12g,"
			    "\"max_tx\":%.12g,\"voltage_span_mv\":%.6f}",
			    physical_name(physical), fit->valid ? "true" : "false",
			    fit->accepted ? "true" : "false", fit->points,
			    fit->slope, fit->offset,
			    fit->correlation, fit->rms_db, fit->max_abs_db,
			    fit->min_tx, fit->max_tx, fit->voltage_span_mv) != 0) {
		LOG_WRN("atten calibration telemetry payload too large");
		return;
	}

	atten_cal_publish_telemetry(msg);
	LOG_INF("atten cal fit physical=%s valid=%d accepted=%d points=%u slope=%.9g offset=%.9g corr=%.6f rms_db=%.6f max_abs_db=%.6f",
		physical_name(physical), fit->valid ? 1 : 0,
		fit->accepted ? 1 : 0, fit->points, fit->slope,
		fit->offset, fit->correlation, fit->rms_db, fit->max_abs_db);
}

static void atten_cal_emit_manual_batch_point(uint8_t physical,
					      size_t point_index,
					      double voltage_mv,
					      double flux)
{
	struct coo_cmd_response *msg;
	size_t off;

	msg = atten_cal_telemetry_begin(&off, "manual_point");
	if (msg == NULL ||
	    coo_json_append(msg->payload, sizeof(msg->payload), &off,
			    ",\"fit_physical\":\"%s\",\"batch_index\":%u,"
			    "\"sweep_mv\":%.3f,\"flux\":%.12g,"
			    "\"valid\":%s}",
			    physical_name(physical), (uint8_t)point_index,
			    voltage_mv, flux, flux > 0.0 ? "true" : "false") != 0) {
		LOG_WRN("atten calibration telemetry payload too large");
		return;
	}

	atten_cal_publish_telemetry(msg);
	LOG_INF("atten cal manual_point physical=%s point=%u sweep_mv=%.1f flux=%.6g",
		physical_name(physical), (uint8_t)(point_index + 1U),
		voltage_mv, flux);
}

/* -------------------------------------------------------------------------- */
/* Measurement classification and retained raw-record creation.               */

static uint32_t clamp_dwell(uint32_t dwell_ms)
{
	if (dwell_ms == 0U) {
		return ATTEN_CAL_DEFAULT_DWELL_MS;
	}
	if (dwell_ms > ATTEN_CAL_MAX_DWELL_MS) {
		return ATTEN_CAL_MAX_DWELL_MS;
	}
	return dwell_ms;
}

static bool sample_is_saturated(const struct photodiode_average_status *avg)
{
	if (avg == NULL) {
		return false;
	}
	return avg->result.max_raw >= ATTEN_CAL_SAT_RAW ||
	       avg->result.mean_mv >= ATTEN_CAL_ADC_CLIP_MV ||
	       avg->result.max_mv >= ATTEN_CAL_ADC_CLIP_MV;
}

static double average_mean_sigma_mv(const struct photodiode_average_status *avg)
{
	double samples;
	double rms_mean;
	double adc_mean;

	if (avg == NULL || avg->result.samples == 0U) {
		return ATTEN_CAL_ADC_LSB_MV;
	}

	samples = (double)avg->result.samples;
	rms_mean = avg->result.rms_mv / sqrt(samples);
	adc_mean = (ATTEN_CAL_ADC_LSB_MV / sqrt(12.0)) / sqrt(samples);
	return sqrt(rms_mean * rms_mean + adc_mean * adc_mean);
}

static void measurement_from_average(const struct photodiode_average_status *avg,
				     struct atten_cal_measurement *measurement)
{
	double avg_sigma;

	if (measurement == NULL) {
		return;
	}
	memset(measurement, 0, sizeof(*measurement));
	measurement->reason = ATTEN_CAL_REASON_INVALID;
	if (avg == NULL || avg->state != PHOTODIODE_AVERAGE_COMPLETE ||
	    avg->result.samples == 0U) {
		measurement->reason = ATTEN_CAL_REASON_ADC_ERROR;
		return;
	}

	avg_sigma = average_mean_sigma_mv(avg);
	measurement->mean_mv = avg->result.mean_mv;
	measurement->rms_mv = avg->result.rms_mv;
	measurement->samples = avg->result.samples;
	measurement->max_raw = avg->result.max_raw;
	measurement->saturated = sample_is_saturated(avg);
	measurement->signal_mv = avg->result.mean_net_mv;
	measurement->sigma_y_mv = avg_sigma;
	if (!(measurement->sigma_y_mv > 0.0)) {
		measurement->sigma_y_mv = ATTEN_CAL_ADC_LSB_MV;
	}
	if (measurement->saturated) {
		/* Clipped ADC samples are retained for the lab record but are not
		 * measurements with meaningful SNR.
		 */
		measurement->snr = NAN;
		measurement->reason = ATTEN_CAL_REASON_SATURATED;
		return;
	}
	measurement->snr = measurement->signal_mv / measurement->sigma_y_mv;
	if (measurement->signal_mv <= 0.0 ||
	    measurement->snr < ATTEN_CAL_SNR_USABLE) {
		measurement->reason = ATTEN_CAL_REASON_BELOW_SNR;
	} else {
		measurement->reason = ATTEN_CAL_REASON_OK;
		measurement->usable = true;
	}
}

static double current_scale_sigma_locked(void)
{
	return cal.scale * sqrt(MAX(cal.scale_rel_var, 0.0));
}

static bool append_record_locked(enum atten_cal_record_event event,
				 const struct atten_cal_measurement *measurement,
				 bool fit_eligible,
				 struct atten_cal_record **out)
{
	struct atten_cal_record *record;
	uint8_t physical = cal.physical_index;
	uint8_t index;
	double scale_sigma;
	double flux = 0.0;
	double flux_sigma = 0.0;

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
	if (measurement->usable) {
		flux = measurement->signal_mv * cal.scale;
		flux_sigma = sqrt((measurement->sigma_y_mv * cal.scale) *
				  (measurement->sigma_y_mv * cal.scale) +
				  (measurement->signal_mv * scale_sigma) *
				  (measurement->signal_mv * scale_sigma));
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
	record->flux = (float)flux;
	record->flux_sigma = (float)flux_sigma;
	record->scale = (float)cal.scale;
	record->scale_sigma = (float)scale_sigma;
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
	if (fit_eligible && measurement->usable && measurement->snr >= ATTEN_CAL_SNR_FIT) {
		record->flags |= ATTEN_CAL_RECORD_FIT_ELIGIBLE;
	}

	cal.point_index = index;
	atten_cal_emit_record(record);
	if (out != NULL) {
		*out = record;
	}
	return true;
}

static void copy_voltage_schedule(double out[ATTENUATOR_CAL_POINT_COUNT])
{
	memcpy(out, voltage_schedule, sizeof(voltage_schedule));
}

static uint8_t complete_percent_locked(void)
{
	uint16_t complete;

	if (cal.state == ATTEN_CAL_STATE_INACTIVE) {
		return 0U;
	}
	if (cal.state == ATTEN_CAL_STATE_COMPLETE) {
		return 100U;
	}
	if (cal.mode == ATTEN_CAL_MODE_MANUAL) {
		complete = (uint16_t)cal.physical_index * ATTENUATOR_CAL_POINT_COUNT +
			   cal.point_index;
		return (uint8_t)((complete * 100U) /
				 (ATTENUATOR_CAL_POINT_COUNT * 2U));
	}
	if (cal.mode == ATTEN_CAL_MODE_TIB_AUTO) {
		uint16_t records = cal.record_count[cal.physical_index];
		complete = (uint16_t)cal.physical_index * 50U + MIN(records, 49U);
		return (uint8_t)MIN(complete, 99U);
	}
	return 0U;
}

/* -------------------------------------------------------------------------- */
/* Shared state/status copying and route/DAC control helpers.                 */

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
	} else {
		status->fit = cal.last_error != 0 ? "failed" : "none";
	}
	status->attenuator_index = cal.attenuator_index;
	status->physical_index = cal.physical_index;
	status->point_index = cal.point_index;
	status->point_count = cal.mode == ATTEN_CAL_MODE_TIB_AUTO ?
			      ATTENUATOR_CAL_RECORD_COUNT : ATTENUATOR_CAL_POINT_COUNT;
	status->dwell_ms = cal.dwell_ms == 0U ? ATTEN_CAL_DEFAULT_DWELL_MS : cal.dwell_ms;
	status->complete_pct = complete_percent_locked();
	status->current_mv = current_sweep_mv_locked();
	status->other_mv = (cal.state == ATTEN_CAL_STATE_INACTIVE && cal.other_mv == 0.0) ?
			   ATTENUATOR_DRIVE_MAX_MV : cal.other_mv;
	status->last_error = cal.last_error;
	status->include_voltage_schedule =
		cal.mode == ATTEN_CAL_MODE_MANUAL &&
		(cal.state == ATTEN_CAL_STATE_WAITING ||
		 cal.state == ATTEN_CAL_STATE_COMPLETE ||
		 cal.state == ATTEN_CAL_STATE_INACTIVE);
	copy_voltage_schedule(status->voltage_schedule_mv);
	memcpy(status->fit_metrics, cal.fit, sizeof(status->fit_metrics));
}

static void reset_locked(enum atten_cal_state state)
{
	memset(&cal, 0, sizeof(cal));
	cal.state = state;
	cal.mode = ATTEN_CAL_MODE_NONE;
	cal.dwell_ms = ATTEN_CAL_DEFAULT_DWELL_MS;
	cal.other_mv = ATTENUATOR_DRIVE_MAX_MV;
	cal.laser_percent = 100.0;
	cal.scale = 1.0;
	cal.scale_rel_var = 0.0;
	cal.sweep_mv = voltage_schedule[0];
}

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

static int apply_route_pair(const char *input, const char *output)
{
	return mems_router_apply_named_route(&router, input, output, false, NULL, NULL);
}

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

/* -------------------------------------------------------------------------- */
/* Fit model and manual/auto fit extraction from retained records.            */

static bool point_valid_for_fit(double tx)
{
	return tx > ATTEN_CAL_MIN_TX && tx <= ATTEN_CAL_MAX_TX;
}

static double fit_points_max_flux(
	const struct atten_cal_point points[ATTENUATOR_CAL_POINT_COUNT])
{
	double max_flux = 0.0;

	if (points == NULL) {
		return 0.0;
	}

	for (uint8_t i = 0U; i < ATTENUATOR_CAL_POINT_COUNT; ++i) {
		if (points[i].valid && !points[i].saturated && points[i].flux > max_flux) {
			max_flux = points[i].flux;
		}
	}
	return max_flux;
}

static bool manual_point_to_b(const struct atten_cal_point *point,
			      double max_flux, double *tx_out, double *b_out)
{
	double tx;

	if (point == NULL || point->saturated || !point->valid ||
	    !(point->flux > 0.0) || !(max_flux > 0.0)) {
		return false;
	}

	tx = point->flux / max_flux;
	if (tx > ATTEN_CAL_MAX_TX) {
		tx = ATTEN_CAL_MAX_TX;
	}
	if (!point_valid_for_fit(tx) ||
	    !attenuator_model_linear_to_b(tx, b_out)) {
		return false;
	}
	if (tx_out != NULL) {
		*tx_out = tx;
	}
	return true;
}

static int fit_one_manual_physical(
	uint8_t physical_index,
	const struct atten_cal_point points[ATTENUATOR_CAL_POINT_COUNT],
	struct attenuator_calibration_fit_metrics *out)
{
	double x_data[ATTENUATOR_CAL_POINT_COUNT];
	double y_data[ATTENUATOR_CAL_POINT_COUNT];
	double max_flux = 0.0;
	double min_tx = 1.0;
	double max_tx = 0.0;
	double min_v = ATTENUATOR_DRIVE_MAX_MV;
	double max_v = 0.0;
	double sum_x = 0.0;
	double sum_y = 0.0;
	double sum_xx = 0.0;
	double sum_yy = 0.0;
	double sum_xy = 0.0;
	double denom_x;
	double denom_y;
	double slope;
	double intercept;
	double correlation;
	double sum_sq_db = 0.0;
	double max_abs_db = 0.0;
	size_t point_count = 0U;

	if (out == NULL || physical_index >= ATTENUATOR_PHYSICAL_COUNT) {
		return -EINVAL;
	}
	memset(out, 0, sizeof(*out));

	max_flux = fit_points_max_flux(points);
	if (!(max_flux > 0.0)) {
		return -ERANGE;
	}

	for (uint8_t i = 0U; i < ATTENUATOR_CAL_POINT_COUNT; ++i) {
		double tx;
		double b;

		if (!manual_point_to_b(&points[i], max_flux, &tx, &b)) {
			continue;
		}

		x_data[point_count] = points[i].voltage_mv;
		y_data[point_count] = b;
		sum_x += points[i].voltage_mv;
		sum_y += b;
		sum_xx += points[i].voltage_mv * points[i].voltage_mv;
		sum_yy += b * b;
		sum_xy += points[i].voltage_mv * b;
		point_count++;
		if (tx < min_tx) {
			min_tx = tx;
		}
		if (tx > max_tx) {
			max_tx = tx;
		}
		if (points[i].voltage_mv < min_v) {
			min_v = points[i].voltage_mv;
		}
		if (points[i].voltage_mv > max_v) {
			max_v = points[i].voltage_mv;
		}
	}

	if (point_count < ATTEN_CAL_MIN_FIT_POINTS || !(max_v > min_v)) {
		return -ERANGE;
	}

	denom_x = (double)point_count * sum_xx - sum_x * sum_x;
	denom_y = (double)point_count * sum_yy - sum_y * sum_y;
	if (!(denom_x > 0.0) || !(denom_y > 0.0)) {
		return -ERANGE;
	}
	slope = ((double)point_count * sum_xy - sum_x * sum_y) / denom_x;
	intercept = (sum_y - slope * sum_x) / (double)point_count;
	correlation = ((double)point_count * sum_xy - sum_x * sum_y) /
		      sqrt(denom_x * denom_y);
	if (!(slope > 0.0) || !isfinite(intercept) || !isfinite(correlation)) {
		return -ERANGE;
	}

	for (size_t i = 0U; i < point_count; ++i) {
		double measured_tx = attenuator_model_b_to_linear(y_data[i]);
		double predicted_tx =
			attenuator_model_b_to_linear(slope * x_data[i] + intercept);
		double residual_db;

		if (!(measured_tx > 0.0) || !(predicted_tx > 0.0)) {
			continue;
		}
		residual_db = 10.0 * log10(predicted_tx / measured_tx);
		sum_sq_db += residual_db * residual_db;
		if (fabs(residual_db) > max_abs_db) {
			max_abs_db = fabs(residual_db);
		}
	}

	out->valid = true;
	out->points = (uint8_t)point_count;
	out->slope = slope;
	out->offset = intercept;
	out->correlation = correlation;
	out->rms_db = sqrt(sum_sq_db / (double)point_count);
	out->max_abs_db = max_abs_db;
	out->min_tx = min_tx;
	out->max_tx = max_tx;
	out->voltage_span_mv = max_v - min_v;
	out->accepted = isfinite(out->correlation) &&
			out->correlation >= ATTEN_CAL_MIN_FIT_CORR;

	return out->accepted ? 0 : -ERANGE;
}

static bool record_is_fit_candidate(const struct atten_cal_record *record)
{
	return record != NULL &&
	       (record->flags & ATTEN_CAL_RECORD_FIT_ELIGIBLE) != 0U &&
	       record->flux > 0.0f &&
	       record->flux_sigma > 0.0f;
}

static bool find_open_reference(uint8_t physical, const struct atten_cal_record **record_out)
{
	const struct atten_cal_record *best = NULL;

	if (record_out == NULL || physical >= ATTENUATOR_PHYSICAL_COUNT) {
		return false;
	}
	for (uint8_t i = 0U; i < cal.record_count[physical]; ++i) {
		const struct atten_cal_record *record = &cal.records[physical][i];

		if (!record_is_fit_candidate(record)) {
			continue;
		}
		if ((double)record->sweep_mv > ATTEN_CAL_MIN_STEP_MV) {
			continue;
		}
		if (best == NULL || record->flux > best->flux) {
			best = record;
		}
	}
	*record_out = best;
	return best != NULL;
}

static bool record_to_b(const struct atten_cal_record *record,
			const struct atten_cal_record *open_record,
			double *tx_out, double *b_out, double *sigma_b_out)
{
	double tx;
	double rel_flux;
	double rel_open;
	double sigma_tx;
	double tx_lo;
	double tx_hi;
	double b;
	double b_lo;
	double b_hi;

	if (!record_is_fit_candidate(record) || open_record == NULL ||
	    !(open_record->flux > 0.0f) || !(open_record->flux_sigma > 0.0f)) {
		return false;
	}

	tx = (double)record->flux / (double)open_record->flux;
	if (tx >= ATTEN_CAL_MAX_TX || !point_valid_for_fit(tx)) {
		return false;
	}
	if (!attenuator_model_linear_to_b(tx, &b)) {
		return false;
	}

	rel_flux = (double)record->flux_sigma / (double)record->flux;
	rel_open = (double)open_record->flux_sigma / (double)open_record->flux;
	sigma_tx = tx * sqrt(rel_flux * rel_flux + rel_open * rel_open);
	if (!(sigma_tx > 0.0) || !isfinite(sigma_tx)) {
		sigma_tx = ATTEN_CAL_MIN_TX;
	}
	tx_lo = MAX(ATTEN_CAL_MIN_TX, tx - sigma_tx);
	tx_hi = MIN(ATTEN_CAL_MAX_TX, tx + sigma_tx);
	if (!attenuator_model_linear_to_b(tx_lo, &b_lo) ||
	    !attenuator_model_linear_to_b(tx_hi, &b_hi)) {
		return false;
	}

	if (tx_out != NULL) {
		*tx_out = tx;
	}
	if (b_out != NULL) {
		*b_out = b;
	}
	if (sigma_b_out != NULL) {
		*sigma_b_out = MAX(fabs(b_hi - b_lo) * 0.5, 1.0e-6);
	}
	return true;
}

static int fit_one_auto_physical(uint8_t physical,
				 struct attenuator_calibration_fit_metrics *out)
{
	const struct atten_cal_record *open_record = NULL;
	double min_tx = 1.0;
	double max_tx = 0.0;
	double min_v = ATTENUATOR_DRIVE_MAX_MV;
	double max_v = 0.0;
	double slope = 0.0;
	double intercept = 0.0;
	double correlation = 0.0;
	double sum_sq_db = 0.0;
	double max_abs_db = 0.0;
	uint8_t point_count = 0U;

	if (out == NULL || physical >= ATTENUATOR_PHYSICAL_COUNT) {
		return -EINVAL;
	}
	memset(out, 0, sizeof(*out));
	for (uint8_t i = 0U; i < cal.record_count[physical]; ++i) {
		cal.records[physical][i].flags &= (uint8_t)~ATTEN_CAL_RECORD_FIT_INCLUDED;
		cal.records[physical][i].tx = 0.0f;
		cal.records[physical][i].b = 0.0f;
		cal.records[physical][i].residual_db = NAN;
	}

	if (!find_open_reference(physical, &open_record)) {
		return -ERANGE;
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
			double tx;
			double b;
			double sigma_b;
			double var;
			double w;
			double x;

			if (!record_to_b(record, open_record, &tx, &b, &sigma_b)) {
				continue;
			}
			x = record->sweep_mv;
			var = sigma_b * sigma_b +
			      slope * slope * (double)record->sigma_x_mv *
				      (double)record->sigma_x_mv;
			w = 1.0 / MAX(var, 1.0e-12);
			sum_w += w;
			sum_x += w * x;
			sum_y += w * b;
			sum_xx += w * x * x;
			sum_xy += w * x * b;
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
		double denom_x;
		double denom_y;
		uint8_t included = 0U;

		for (uint8_t i = 0U; i < cal.record_count[physical]; ++i) {
			struct atten_cal_record *record = &cal.records[physical][i];
			double tx;
			double b;
			double sigma_b;
			double predicted_tx;
			double residual_db;
			double x;

			if (!record_to_b(record, open_record, &tx, &b, &sigma_b)) {
				ARG_UNUSED(sigma_b);
				continue;
			}
			x = record->sweep_mv;
			predicted_tx = attenuator_model_b_to_linear(slope * x + intercept);
			if (!(predicted_tx > 0.0)) {
				continue;
			}
			residual_db = 10.0 * log10(predicted_tx / tx);
			record->flags |= ATTEN_CAL_RECORD_FIT_INCLUDED;
			record->tx = (float)tx;
			record->b = (float)b;
			record->residual_db = (float)residual_db;
			sum_sq_db += residual_db * residual_db;
			max_abs_db = MAX(max_abs_db, fabs(residual_db));
			min_tx = MIN(min_tx, tx);
			max_tx = MAX(max_tx, tx);
			min_v = MIN(min_v, x);
			max_v = MAX(max_v, x);
			sum_x += x;
			sum_y += b;
			sum_xx += x * x;
			sum_yy += b * b;
			sum_xy += x * b;
			included++;
		}

		if (included < ATTEN_CAL_MIN_FIT_POINTS || !(max_v > min_v)) {
			return -ERANGE;
		}
		denom_x = (double)included * sum_xx - sum_x * sum_x;
		denom_y = (double)included * sum_yy - sum_y * sum_y;
		if (!(denom_x > 0.0) || !(denom_y > 0.0)) {
			return -ERANGE;
		}
		correlation = ((double)included * sum_xy - sum_x * sum_y) /
			      sqrt(denom_x * denom_y);
		point_count = included;
	}

	out->valid = true;
	out->points = point_count;
	out->slope = slope;
	out->offset = intercept;
	out->correlation = correlation;
	out->rms_db = sqrt(sum_sq_db / (double)point_count);
	out->max_abs_db = max_abs_db;
	out->min_tx = min_tx;
	out->max_tx = max_tx;
	out->voltage_span_mv = max_v - min_v;
	out->accepted = isfinite(out->correlation) &&
			out->correlation >= ATTEN_CAL_MIN_FIT_CORR;

	LOG_INF("atten cal fit_records physical=%s records=%u included=%u open_flux=%.6g accepted=%d",
		physical_name(physical), cal.record_count[physical],
		point_count, (double)open_record->flux, out->accepted ? 1 : 0);
	return out->accepted ? 0 : -ERANGE;
}

static int apply_fit_to_settings(uint8_t attenuator_index,
				 const struct attenuator_calibration_fit_metrics fit[2],
				 bool persistent)
{
	struct app_attenuator_channel_settings stored = {0};
	struct attenuator_model_coeffs physical[ATTENUATOR_PHYSICAL_COUNT];
	struct attenuator *atten;

	if (attenuator_index >= NUM_ATTENUATORS || fit == NULL) {
		return -EINVAL;
	}
	if (!fit[0].accepted || !fit[1].accepted) {
		return -EINVAL;
	}
	atten = &attenuators[attenuator_index];

	/* Calibration fits measured b against DAC millivolts. Runtime keeps
	 * op-amp gain as a separate coefficient term, so normalize accepted fits
	 * by the configured gain before storing slope/offset.
	 */
	physical[0] = (struct attenuator_model_coeffs){
		.slope = fit[0].slope / atten->coeff1.gain,
		.offset = fit[0].offset / atten->coeff1.gain,
		.gain = atten->coeff1.gain,
	};
	physical[1] = (struct attenuator_model_coeffs){
		.slope = fit[1].slope / atten->coeff2.gain,
		.offset = fit[1].offset / atten->coeff2.gain,
		.gain = atten->coeff2.gain,
	};

	if (attenuator_apply_coefficients_preserve_db(atten, physical) != 0) {
		return -EIO;
	}

	stored.physical[0].slope = physical[0].slope;
	stored.physical[0].offset = physical[0].offset;
	stored.physical[0].gain = physical[0].gain;
	stored.physical[1].slope = physical[1].slope;
	stored.physical[1].offset = physical[1].offset;
	stored.physical[1].gain = physical[1].gain;
	app_settings_update_attenuator_channel(attenuator_index, &stored, persistent);
	return 0;
}

static int fit_current_locked(bool apply_settings)
{
	int rc;
	int first_error = 0;
	bool all_accepted = true;

	for (uint8_t physical = 0U; physical < ATTENUATOR_PHYSICAL_COUNT; ++physical) {
		if (cal.mode == ATTEN_CAL_MODE_TIB_AUTO) {
			rc = fit_one_auto_physical(physical, &cal.fit[physical]);
		} else {
			rc = fit_one_manual_physical(physical, cal.points[physical],
						     &cal.fit[physical]);
		}
		atten_cal_emit_fit(physical, &cal.fit[physical]);
		if (rc != 0) {
			first_error = first_error == 0 ? rc : first_error;
		}
		all_accepted = all_accepted && cal.fit[physical].accepted;
	}
	if (!all_accepted) {
		cal.last_error = first_error == 0 ? -ERANGE : first_error;
		if (cal.mode == ATTEN_CAL_MODE_TIB_AUTO) {
			cal.state = ATTEN_CAL_STATE_COMPLETE;
			cal.phase = ATTEN_CAL_AUTO_NONE;
			LOG_WRN("atten calibration acquisition complete with failed fit (%d)",
				cal.last_error);
			atten_cal_emit_simple("complete");
		} else {
			cal.state = ATTEN_CAL_STATE_ERROR;
			atten_cal_emit_simple("error");
		}
		return cal.last_error;
	}

	if (apply_settings) {
		rc = apply_fit_to_settings(cal.attenuator_index, cal.fit, cal.persistent);
		if (rc != 0) {
			cal.last_error = rc;
			cal.state = ATTEN_CAL_STATE_ERROR;
			atten_cal_emit_simple("error");
			return rc;
		}
	}

	cal.last_error = 0;
	cal.state = ATTEN_CAL_STATE_COMPLETE;
	cal.phase = ATTEN_CAL_AUTO_NONE;
	cal.point_index = cal.mode == ATTEN_CAL_MODE_TIB_AUTO ?
			  cal.record_count[ATTENUATOR_PHYSICAL_COUNT - 1U] :
			  ATTENUATOR_CAL_POINT_COUNT;
	atten_cal_emit_simple("complete");
	return 0;
}

/* -------------------------------------------------------------------------- */
/* Automatic lab acquisition flow.                                            */

static void auto_error_locked(int error)
{
	cal.last_error = error;
	cal.state = ATTEN_CAL_STATE_ERROR;
	cal.phase = ATTEN_CAL_AUTO_NONE;
	atten_cal_emit_simple("error");
}

static void auto_schedule_measure_locked(enum atten_cal_measure_kind kind)
{
	cal.measure_kind = kind;
	cal.phase = ATTEN_CAL_AUTO_MEASURE_SET;
}

static bool auto_set_laser_level_locked(uint8_t level_index)
{
	if (level_index >= ARRAY_SIZE(auto_laser_levels_pct)) {
		return false;
	}
	cal.laser_level_index = level_index;
	cal.laser_percent = auto_laser_levels_pct[level_index];
	if (hispec_laser_set_output_percent_autooff(cal.laser, cal.laser_percent, 0U) != 0) {
		auto_error_locked(-EIO);
		return false;
	}
	return true;
}

static void reset_current_physical_locked(void)
{
	cal.point_index = 0U;
	cal.schedule_index = 0U;
	cal.other_mv = 0.0;
	cal.scale = 1.0;
	cal.scale_rel_var = 0.0;
	cal.sweep_mv = voltage_schedule[0];
	cal.have_last_good = false;
	cal.last_good_mv = 0.0;
	cal.last_good_signal_mv = 0.0;
	cal.last_good_sigma_mv = 0.0;
	cal.have_low_signal = false;
	cal.low_signal_mv = ATTENUATOR_DRIVE_MAX_MV;
	cal.have_sat = false;
	cal.sat_mv = 0.0;
	cal.link_anchor_mv = ATTENUATOR_DRIVE_MAX_MV;
	cal.link_retry_mv = ATTENUATOR_DRIVE_MAX_MV;
	cal.link_before_signal_mv = 0.0;
	cal.link_before_sigma_mv = 0.0;
	cal.link_low_other_mv = 0.0;
	cal.link_high_other_mv = ATTENUATOR_DRIVE_MAX_MV;
	cal.laser_level_index = 0U;
	cal.laser_percent = auto_laser_levels_pct[0];
	cal.segment_id = 0U;
	cal.anchor_id = 0U;
	cal.adjust_tries = 0U;
}

static void start_next_physical_locked(void)
{
	reset_current_physical_locked();
	if (!auto_set_laser_level_locked(0U)) {
		return;
	}
	auto_schedule_measure_locked(ATTEN_CAL_MEASURE_POINT);
	atten_cal_emit_simple("physical_start");
}

static void auto_finish_physical_locked(void)
{
	if (cal.physical_index == 0U) {
		cal.physical_index = 1U;
		start_next_physical_locked();
		return;
	}

	cal.phase = ATTEN_CAL_AUTO_FIT;
}

static void auto_skip_physical_locked(const char *reason)
{
	LOG_WRN("atten calibration finishing physical=%s reason=%s records=%u overflow=%d",
		physical_name(cal.physical_index), reason == NULL ? "range" : reason,
		cal.record_count[cal.physical_index],
		cal.record_overflow[cal.physical_index] ? 1 : 0);
	auto_finish_physical_locked();
}

static void auto_update_last_good_locked(const struct atten_cal_measurement *measurement)
{
	if (measurement == NULL || !measurement->usable) {
		return;
	}
	cal.have_last_good = true;
	cal.last_good_mv = cal.sweep_mv;
	cal.last_good_signal_mv = measurement->signal_mv;
	cal.last_good_sigma_mv = measurement->sigma_y_mv;
}

static bool auto_next_laser_reset_locked(const char *reason)
{
	if (cal.laser_level_index + 1U >= ARRAY_SIZE(auto_laser_levels_pct)) {
		return false;
	}
	if (!auto_set_laser_level_locked((uint8_t)(cal.laser_level_index + 1U))) {
		return false;
	}
	cal.other_mv = 0.0;
	cal.scale = 1.0;
	cal.scale_rel_var = 0.0;
	cal.have_last_good = false;
	cal.have_low_signal = false;
	cal.have_sat = false;
	cal.schedule_index = 0U;
	cal.sweep_mv = voltage_schedule[0];
	cal.segment_id++;
	cal.adjust_tries = 0U;
	LOG_INF("atten cal laser_reset physical=%s reason=%s laser_pct=%.3f",
		physical_name(cal.physical_index), reason == NULL ? "range" : reason,
		cal.laser_percent);
	auto_schedule_measure_locked(ATTEN_CAL_MEASURE_POINT);
	return true;
}

static bool auto_reduce_laser_with_anchor_locked(const char *reason)
{
	if (cal.laser_level_index + 1U >= ARRAY_SIZE(auto_laser_levels_pct)) {
		return false;
	}
	if (!auto_set_laser_level_locked((uint8_t)(cal.laser_level_index + 1U))) {
		return false;
	}
	cal.other_mv = 0.0;
	cal.link_low_other_mv = 0.0;
	cal.link_high_other_mv = ATTENUATOR_DRIVE_MAX_MV;
	cal.adjust_tries = 0U;
	cal.sweep_mv = cal.link_anchor_mv;
	LOG_INF("atten cal link_laser_anchor physical=%s reason=%s laser_pct=%.3f",
		physical_name(cal.physical_index), reason == NULL ? "range" : reason,
		cal.laser_percent);
	auto_schedule_measure_locked(ATTEN_CAL_MEASURE_ANCHOR_AFTER);
	return true;
}

static bool auto_next_sweep_lower_locked(void)
{
	while (cal.schedule_index + 1U < ATTENUATOR_CAL_POINT_COUNT) {
		double next_mv;

		cal.schedule_index++;
		next_mv = voltage_schedule[cal.schedule_index];
		if (next_mv < cal.sweep_mv) {
			cal.sweep_mv = next_mv;
			return true;
		}
	}

	return false;
}

static void auto_begin_link_locked(void)
{
	/* Range extension: anchor a usable point, move the other FVOA or laser
	 * level until this photodiode is usable again, then scale the next
	 * segment back through anchor_before/anchor_after records.
	 */
	if (!cal.have_last_good ||
	    cal.record_count[cal.physical_index] >= ATTENUATOR_CAL_RECORD_COUNT) {
		auto_skip_physical_locked("no_link_anchor");
		return;
	}

	cal.link_anchor_mv = cal.last_good_mv;
	cal.link_retry_mv = cal.have_sat ? cal.sat_mv :
			    MAX(cal.last_good_mv - ATTEN_CAL_MIN_STEP_MV, 0.0);
	cal.sweep_mv = cal.link_anchor_mv;
	cal.adjust_tries = 0U;
	auto_schedule_measure_locked(ATTEN_CAL_MEASURE_ANCHOR_BEFORE);
}

static bool auto_set_pair_and_wait_locked(const char *event, int64_t now_ms)
{
	if (!set_physical_pair(cal.attenuator_index, cal.physical_index,
			       cal.sweep_mv, cal.other_mv)) {
		auto_error_locked(-EIO);
		return false;
	}
	atten_cal_emit_point_set(event, cal.sweep_mv);
	cal.wait_until_ms = now_ms + ATTEN_CAL_STEP_SETTLE_MS;
	cal.phase = ATTEN_CAL_AUTO_MEASURE_SETTLE;
	return true;
}

static bool auto_start_average_locked(void)
{
	int rc = photodiode_start_average(cal.channel, cal.dwell_ms, NULL);

	if (rc != 0) {
		auto_error_locked(rc);
		return false;
	}
	cal.phase = ATTEN_CAL_AUTO_MEASURE_AVG;
	return true;
}

static bool auto_average_complete_locked(struct photodiode_average_status *avg)
{
	int rc;

	if (avg == NULL) {
		auto_error_locked(-EINVAL);
		return false;
	}
	rc = photodiode_get_average_status(cal.channel, avg);
	if (rc != 0) {
		auto_error_locked(rc);
		return false;
	}
	if (avg->state == PHOTODIODE_AVERAGE_MEASURING) {
		return false;
	}
	if (avg->state != PHOTODIODE_AVERAGE_COMPLETE) {
		auto_error_locked(avg->last_error == 0 ? -EIO : avg->last_error);
		return false;
	}
	return true;
}

static void auto_record_and_finish_if_full_locked(enum atten_cal_record_event event,
						 const struct atten_cal_measurement *measurement,
						 bool fit_eligible,
						 struct atten_cal_record **record)
{
	if (!append_record_locked(event, measurement, fit_eligible, record)) {
		auto_skip_physical_locked("record_limit");
	}
}

static void auto_handle_point_locked(const struct atten_cal_measurement *measurement)
{
	struct atten_cal_record *record = NULL;
	bool had_sat = cal.have_sat;

	auto_record_and_finish_if_full_locked(ATTEN_CAL_EVENT_POINT, measurement,
					      measurement != NULL && measurement->usable,
					      &record);
	if (record == NULL || cal.phase == ATTEN_CAL_AUTO_FIT) {
		return;
	}

	if (measurement->usable) {
		auto_update_last_good_locked(measurement);
		cal.have_low_signal = false;
		cal.adjust_tries = 0U;
		if (cal.sweep_mv <= 0.0) {
			auto_finish_physical_locked();
			return;
		}
		if (had_sat) {
			if (cal.last_good_mv - cal.sat_mv <= ATTEN_CAL_MIN_STEP_MV) {
				auto_begin_link_locked();
				return;
			}
			cal.sweep_mv = (cal.last_good_mv + cal.sat_mv) * 0.5;
		} else {
			cal.have_sat = false;
			if (!auto_next_sweep_lower_locked()) {
				auto_finish_physical_locked();
				return;
			}
		}
		auto_schedule_measure_locked(ATTEN_CAL_MEASURE_POINT);
		return;
	}

	if (measurement->saturated) {
		cal.have_sat = true;
		cal.sat_mv = cal.sweep_mv;
		if (cal.have_last_good) {
			if (cal.last_good_mv - cal.sat_mv <= ATTEN_CAL_MIN_STEP_MV) {
				auto_begin_link_locked();
				return;
			}
			cal.sweep_mv = (cal.last_good_mv + cal.sat_mv) * 0.5;
			auto_schedule_measure_locked(ATTEN_CAL_MEASURE_POINT);
			return;
		}
		if (cal.have_low_signal) {
			if (cal.low_signal_mv - cal.sat_mv <= ATTEN_CAL_MIN_STEP_MV) {
				if (!auto_next_laser_reset_locked("initial_transition")) {
					auto_skip_physical_locked("initial_transition");
				}
				return;
			}
			cal.sweep_mv = (cal.low_signal_mv + cal.sat_mv) * 0.5;
			auto_schedule_measure_locked(ATTEN_CAL_MEASURE_POINT);
			return;
		}
		if (!auto_next_laser_reset_locked("saturated_without_bracket")) {
			auto_skip_physical_locked("saturated_without_bracket");
		}
		return;
	}

	cal.have_low_signal = true;
	cal.low_signal_mv = cal.sweep_mv;
	if (cal.have_sat) {
		if (cal.low_signal_mv - cal.sat_mv <= ATTEN_CAL_MIN_STEP_MV) {
			if (!auto_next_laser_reset_locked("no_snr_between_brackets")) {
				auto_skip_physical_locked("no_snr_between_brackets");
			}
			return;
		}
		cal.sweep_mv = (cal.low_signal_mv + cal.sat_mv) * 0.5;
	} else if (!auto_next_sweep_lower_locked()) {
		auto_finish_physical_locked();
		return;
	}
	auto_schedule_measure_locked(ATTEN_CAL_MEASURE_POINT);
}

static void auto_prepare_link_probe_locked(void)
{
	if (cal.adjust_tries >= ATTEN_CAL_MAX_ADJUST_TRIES) {
		auto_skip_physical_locked("link_search_limit");
		return;
	}
	if (cal.link_high_other_mv - cal.link_low_other_mv <= ATTEN_CAL_LINK_MIN_STEP_MV) {
		auto_skip_physical_locked("link_no_other_range");
		return;
	}
	cal.adjust_tries++;
	cal.other_mv = (cal.link_low_other_mv + cal.link_high_other_mv) * 0.5;
	cal.sweep_mv = cal.link_retry_mv;
	auto_schedule_measure_locked(ATTEN_CAL_MEASURE_LINK_PROBE);
}

static void auto_handle_anchor_before_locked(const struct atten_cal_measurement *measurement)
{
	struct atten_cal_record *record = NULL;

	auto_record_and_finish_if_full_locked(ATTEN_CAL_EVENT_ANCHOR_BEFORE, measurement,
					      measurement != NULL && measurement->usable,
					      &record);
	if (record == NULL || cal.phase == ATTEN_CAL_AUTO_FIT) {
		return;
	}
	if (!measurement->usable || !(measurement->signal_mv > 0.0)) {
		auto_skip_physical_locked("anchor_before_unusable");
		return;
	}

	cal.link_before_signal_mv = measurement->signal_mv;
	cal.link_before_sigma_mv = measurement->sigma_y_mv;
	cal.link_low_other_mv = 0.0;
	cal.link_high_other_mv = ATTENUATOR_DRIVE_MAX_MV;
	cal.adjust_tries = 0U;
	cal.other_mv = 0.0;
	cal.sweep_mv = cal.link_retry_mv;
	auto_schedule_measure_locked(ATTEN_CAL_MEASURE_LINK_PROBE);
}

static void auto_handle_link_probe_locked(const struct atten_cal_measurement *measurement)
{
	struct atten_cal_record *record = NULL;

	auto_record_and_finish_if_full_locked(ATTEN_CAL_EVENT_LINK_PROBE, measurement,
					      false, &record);
	if (record == NULL || cal.phase == ATTEN_CAL_AUTO_FIT) {
		return;
	}

	if (measurement->usable) {
		cal.sweep_mv = cal.link_anchor_mv;
		auto_schedule_measure_locked(ATTEN_CAL_MEASURE_ANCHOR_AFTER);
		return;
	}

	if (measurement->saturated) {
		cal.link_low_other_mv = cal.other_mv;
		if (cal.link_high_other_mv - cal.link_low_other_mv <=
		    ATTEN_CAL_LINK_MIN_STEP_MV) {
			if (!auto_reduce_laser_with_anchor_locked("link_saturated_at_limit")) {
				auto_skip_physical_locked("link_saturated_at_limit");
			}
			return;
		}
	} else {
		cal.link_high_other_mv = cal.other_mv;
		if (cal.link_high_other_mv <= ATTEN_CAL_LINK_MIN_STEP_MV ||
		    cal.link_high_other_mv - cal.link_low_other_mv <=
			    ATTEN_CAL_LINK_MIN_STEP_MV) {
			auto_skip_physical_locked("link_below_snr_at_limit");
			return;
		}
	}
	auto_prepare_link_probe_locked();
}

static void auto_handle_anchor_after_locked(const struct atten_cal_measurement *measurement)
{
	struct atten_cal_record *record = NULL;

	if (measurement != NULL && measurement->usable &&
	    cal.link_before_signal_mv > 0.0 && measurement->signal_mv > 0.0) {
		double before = cal.link_before_signal_mv;
		double after = measurement->signal_mv;
		double before_rel = cal.link_before_sigma_mv / before;
		double after_rel = measurement->sigma_y_mv / after;

		cal.scale *= before / after;
		cal.scale_rel_var += before_rel * before_rel + after_rel * after_rel;
		cal.segment_id++;
		cal.anchor_id++;
	}

	auto_record_and_finish_if_full_locked(ATTEN_CAL_EVENT_ANCHOR_AFTER, measurement,
					      measurement != NULL && measurement->usable,
					      &record);
	if (record == NULL || cal.phase == ATTEN_CAL_AUTO_FIT) {
		return;
	}
	if (!measurement->usable) {
		auto_skip_physical_locked("anchor_after_unusable");
		return;
	}

	auto_update_last_good_locked(measurement);
	cal.have_low_signal = false;
	cal.have_sat = false;
	cal.sweep_mv = cal.link_retry_mv;
	auto_schedule_measure_locked(ATTEN_CAL_MEASURE_POINT);
}

static const char *measure_set_event_name(enum atten_cal_measure_kind kind)
{
	switch (kind) {
	case ATTEN_CAL_MEASURE_POINT:
		return "point_set";
	case ATTEN_CAL_MEASURE_ANCHOR_BEFORE:
		return "anchor_before_set";
	case ATTEN_CAL_MEASURE_LINK_PROBE:
		return "link_probe_set";
	case ATTEN_CAL_MEASURE_ANCHOR_AFTER:
		return "anchor_after_set";
	case ATTEN_CAL_MEASURE_NONE:
	default:
		return "measure_set";
	}
}

static void auto_handle_measurement_locked(const struct atten_cal_measurement *measurement)
{
	switch (cal.measure_kind) {
	case ATTEN_CAL_MEASURE_POINT:
		auto_handle_point_locked(measurement);
		return;
	case ATTEN_CAL_MEASURE_ANCHOR_BEFORE:
		auto_handle_anchor_before_locked(measurement);
		return;
	case ATTEN_CAL_MEASURE_LINK_PROBE:
		auto_handle_link_probe_locked(measurement);
		return;
	case ATTEN_CAL_MEASURE_ANCHOR_AFTER:
		auto_handle_anchor_after_locked(measurement);
		return;
	case ATTEN_CAL_MEASURE_NONE:
	default:
		auto_error_locked(-EINVAL);
		return;
	}
}

static void auto_tick_locked(int64_t now_ms)
{
	struct photodiode_average_status avg = {0};
	struct atten_cal_measurement measurement = {0};

	if (cal.state != ATTEN_CAL_STATE_RUNNING ||
	    cal.mode != ATTEN_CAL_MODE_TIB_AUTO) {
		return;
	}

	switch (cal.phase) {
	case ATTEN_CAL_AUTO_PD_SETTLE:
		if (now_ms < cal.wait_until_ms) {
			return;
		}
		start_next_physical_locked();
		return;
	case ATTEN_CAL_AUTO_MEASURE_SET:
		(void)auto_set_pair_and_wait_locked(
			measure_set_event_name(cal.measure_kind), now_ms);
		return;
	case ATTEN_CAL_AUTO_MEASURE_SETTLE:
		if (now_ms < cal.wait_until_ms) {
			return;
		}
		(void)auto_start_average_locked();
		return;
	case ATTEN_CAL_AUTO_MEASURE_AVG:
		if (!auto_average_complete_locked(&avg)) {
			return;
		}
		measurement_from_average(&avg, &measurement);
		auto_handle_measurement_locked(&measurement);
		return;
	case ATTEN_CAL_AUTO_FIT:
		(void)fit_current_locked(true);
		return;
	case ATTEN_CAL_AUTO_NONE:
	default:
		return;
	}
}

int attenuator_calibration_start_auto(
	const struct attenuator_calibration_auto_request *request,
	struct attenuator_calibration_status *status)
{
	char route_input[MEMS_SOURCEDEST_MAX_LEN] = {0};
	char pd_input[MEMS_SOURCEDEST_MAX_LEN] = {0};
	char pd_output[MEMS_SOURCEDEST_MAX_LEN] = {0};
	enum photodiode_channel channel;
	struct app_photodiode_settings pd_settings;
	uint8_t attenuator_index;
	double dark_mv;
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
	dark_mv = pd_settings.channel[channel].dark_mv;
	if (!isfinite(dark_mv) ||
	    dark_mv < PHOTODIODE_DARK_MIN_MV ||
	    dark_mv > PHOTODIODE_DARK_MAX_MV) {
		return -EINVAL;
	}

	k_mutex_lock(&cal_lock, K_FOREVER);
	replacing = cal.state == ATTEN_CAL_STATE_RUNNING || cal.state == ATTEN_CAL_STATE_WAITING;
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
					     .msg = "stopping auto throughput for attenuator calibration",
				     });
	}
	(void)throughput_monitor_stop(PHOTODIODE_CHANNEL_COUNT, NULL);
	rc = apply_route_pair(route_input, request->output);
	if (rc == 0) {
		rc = apply_route_pair(pd_input, pd_output);
	}
	if (rc != 0) {
		return rc;
	}
	rc = housekeeping_power_set((enum housekeeping_power_output)channel, true);
	if (rc != 0) {
		return rc;
	}

	if (!set_physical_pair(attenuator_index, 0U,
			       ATTENUATOR_DRIVE_MAX_MV, ATTENUATOR_DRIVE_MAX_MV)) {
		return -EIO;
	}
	throughput_monitor_note_attenuator_changed(attenuator_index);
	rc = hispec_laser_stop_output(request->laser, false);
	if (rc != 0) {
		return rc;
	}

	k_mutex_lock(&cal_lock, K_FOREVER);
	reset_locked(ATTEN_CAL_STATE_RUNNING);
	cal.mode = ATTEN_CAL_MODE_TIB_AUTO;
	cal.phase = ATTEN_CAL_AUTO_PD_SETTLE;
	cal.attenuator_index = attenuator_index;
	cal.physical_index = 0U;
	cal.point_index = 0U;
	cal.dwell_ms = clamp_dwell(request->dwell_ms);
	cal.persistent = request->persistent;
	cal.laser = request->laser;
	cal.channel = channel;
	cal.laser_percent = 0.0;
	cal.measure_kind = ATTEN_CAL_MEASURE_NONE;
	cal.wait_until_ms = k_uptime_get() + ATTEN_CAL_PD_POWER_SETTLE_MS;
	LOG_INF("atten cal using configured photodiode dark channel=%s dark_mv=%.6f",
		photodiode_channel_names[channel], dark_mv);
	atten_cal_emit_simple("start");
	copy_status_locked(status);
	k_mutex_unlock(&cal_lock);
	return 0;
}

/* -------------------------------------------------------------------------- */
/* Manual command path.                                                       */

static int manual_apply_current_locked(void)
{
	if (!set_physical_pair(cal.attenuator_index, cal.physical_index,
			       voltage_schedule[cal.point_index], cal.other_mv)) {
		return -EIO;
	}
	return 0;
}

int attenuator_calibration_start_manual(uint8_t attenuator_index,
					uint32_t dwell_ms,
					bool persistent,
					struct attenuator_calibration_status *status)
{
	if (!devices_attenuator_channel_available(attenuator_index)) {
		return -ENODEV;
	}

	throughput_monitor_note_attenuator_changed(attenuator_index);

	k_mutex_lock(&cal_lock, K_FOREVER);
	reset_locked(ATTEN_CAL_STATE_WAITING);
	cal.mode = ATTEN_CAL_MODE_MANUAL;
	cal.attenuator_index = attenuator_index;
	cal.physical_index = 0U;
	cal.point_index = 0U;
	cal.dwell_ms = clamp_dwell(dwell_ms);
	cal.persistent = persistent;
	cal.other_mv = ATTENUATOR_DRIVE_MAX_MV;
	cal.last_error = manual_apply_current_locked();
	if (cal.last_error != 0) {
		cal.state = ATTEN_CAL_STATE_ERROR;
		atten_cal_emit_simple("error");
	} else {
		atten_cal_emit_simple("start");
		atten_cal_emit_point_set("manual_point_set",
					 voltage_schedule[cal.point_index]);
	}
	copy_status_locked(status);
	k_mutex_unlock(&cal_lock);
	return status != NULL && status->last_error != 0 ? status->last_error : 0;
}

int attenuator_calibration_manual_continue(bool has_other_mv,
					   double other_mv,
					   struct attenuator_calibration_status *status)
{
	int rc = 0;

	k_mutex_lock(&cal_lock, K_FOREVER);
	if (cal.mode != ATTEN_CAL_MODE_MANUAL ||
	    cal.state != ATTEN_CAL_STATE_WAITING) {
		rc = -EINVAL;
		goto out;
	}

	if (has_other_mv) {
		if (other_mv < 0.0) {
			other_mv = 0.0;
		} else if (other_mv > ATTENUATOR_DRIVE_MAX_MV) {
			other_mv = ATTENUATOR_DRIVE_MAX_MV;
		}
		cal.other_mv = other_mv;
	}

	cal.point_index++;
	if (cal.point_index >= ATTENUATOR_CAL_POINT_COUNT) {
		if (cal.physical_index == 0U) {
			cal.physical_index = 1U;
			cal.point_index = 0U;
		} else {
			cal.state = ATTEN_CAL_STATE_COMPLETE;
			cal.point_index = ATTENUATOR_CAL_POINT_COUNT;
			atten_cal_emit_simple("complete");
			goto out;
		}
	}

	cal.last_error = manual_apply_current_locked();
	if (cal.last_error != 0) {
		cal.state = ATTEN_CAL_STATE_ERROR;
		atten_cal_emit_simple("error");
		rc = cal.last_error;
	} else {
		atten_cal_emit_point_set("manual_point_set",
					 voltage_schedule[cal.point_index]);
	}

out:
	copy_status_locked(status);
	k_mutex_unlock(&cal_lock);
	return rc;
}

int attenuator_calibration_fit_manual(
	uint8_t attenuator_index,
	const struct attenuator_calibration_batch physical[2],
	bool persistent,
	struct attenuator_calibration_status *status)
{
	int rc = 0;

	if (physical == NULL || !devices_attenuator_channel_available(attenuator_index)) {
		return -EINVAL;
	}

	k_mutex_lock(&cal_lock, K_FOREVER);
	reset_locked(ATTEN_CAL_STATE_RUNNING);
	cal.mode = ATTEN_CAL_MODE_MANUAL;
	cal.attenuator_index = attenuator_index;
	cal.persistent = persistent;

	for (uint8_t p = 0U; p < ATTENUATOR_PHYSICAL_COUNT; ++p) {
		if (physical[p].len > ATTENUATOR_CAL_POINT_COUNT) {
			rc = -EINVAL;
			break;
		}
		for (size_t i = 0U; i < physical[p].len; ++i) {
			cal.points[p][i].voltage_mv = physical[p].voltage_mv[i];
			cal.points[p][i].flux = physical[p].flux[i];
			cal.points[p][i].valid = physical[p].flux[i] > 0.0;
			atten_cal_emit_manual_batch_point(p, i,
							  physical[p].voltage_mv[i],
							  physical[p].flux[i]);
		}
	}
	if (rc == 0) {
		rc = fit_current_locked(true);
	}
	if (rc != 0) {
		cal.last_error = rc;
		cal.state = ATTEN_CAL_STATE_ERROR;
	}
	copy_status_locked(status);
	k_mutex_unlock(&cal_lock);
	return rc;
}

/* -------------------------------------------------------------------------- */
/* Public status, record retrieval, and throughput-thread tick entry points.  */

int attenuator_calibration_stop(struct attenuator_calibration_status *status)
{
	enum atten_cal_mode old_mode;

	k_mutex_lock(&cal_lock, K_FOREVER);
	old_mode = cal.mode;
	if (cal.state != ATTEN_CAL_STATE_INACTIVE) {
		atten_cal_emit_simple("stop");
	}
	reset_locked(ATTEN_CAL_STATE_INACTIVE);
	copy_status_locked(status);
	k_mutex_unlock(&cal_lock);
	if (status != NULL && old_mode == ATTEN_CAL_MODE_MANUAL) {
		status->mode = "manual";
		status->include_voltage_schedule = true;
		copy_voltage_schedule(status->voltage_schedule_mv);
	}
	return 0;
}

void attenuator_calibration_get_status(struct attenuator_calibration_status *status)
{
	k_mutex_lock(&cal_lock, K_FOREVER);
	copy_status_locked(status);
	k_mutex_unlock(&cal_lock);
}

bool attenuator_calibration_active(void)
{
	bool active;

	k_mutex_lock(&cal_lock, K_FOREVER);
	active = cal.state == ATTEN_CAL_STATE_RUNNING ||
		 cal.state == ATTEN_CAL_STATE_WAITING;
	k_mutex_unlock(&cal_lock);

	return active;
}

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
		"\"valid\":true,\"accepted\":%s,\"points\":%u,\"slope\":%.12g,"
		"\"offset\":%.12g,\"corr\":%.12g,\"rms_db\":%.12g,"
		"\"max_abs_db\":%.12g,\"min_tx\":%.12g,"
		"\"max_tx\":%.12g,\"voltage_span_mv\":%.6f}",
		fit->accepted ? "true" : "false", fit->points,
		fit->slope, fit->offset, fit->correlation, fit->rms_db,
		fit->max_abs_db, fit->min_tx, fit->max_tx, fit->voltage_span_mv);
}

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
			    &status->fit_metrics[1]) != 0) {
		return -ENOSPC;
	}

	if (status->include_voltage_schedule) {
		if (coo_json_append(payload, payload_len, &off, ",\"v_mV\":[") != 0) {
			return -ENOSPC;
		}
		for (uint8_t i = 0U; i < ATTENUATOR_CAL_POINT_COUNT; ++i) {
			if (coo_json_append(payload, payload_len, &off,
					    "%s%.6f", i == 0U ? "" : ",",
					    status->voltage_schedule_mv[i]) != 0) {
				return -ENOSPC;
			}
		}
		if (coo_json_append(payload, payload_len, &off, "]") != 0) {
			return -ENOSPC;
		}
	}

	if (coo_json_append(payload, payload_len, &off, "}") != 0) {
		return -ENOSPC;
	}
	return 0;
}

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

void attenuator_calibration_tick(const struct photodiode_status *pd_status,
				 int64_t now_ms)
{
	ARG_UNUSED(pd_status);

	k_mutex_lock(&cal_lock, K_FOREVER);
	auto_tick_locked(now_ms);
	k_mutex_unlock(&cal_lock);
}
