/**
 * @file attenuator_calibration.c
 * @brief Automatic and manual attenuator calibration.
 */

#include "attenuator_calibration.h"

#include <errno.h>
#include <math.h>
#include <string.h>

#include <coo_commons/json_utils.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/util.h>
#include <zsl/statistics.h>

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
#define ATTEN_CAL_SIGNAL_MIN_MV 20.0
#define ATTEN_CAL_HIGH_MV 750.0
#define ATTEN_CAL_SAT_RAW (INT16_MAX - 1024)
#define ATTEN_CAL_OTHER_STEP_MV 512.0
#define ATTEN_CAL_MIN_FIT_POINTS ATTENUATOR_CAL_MIN_BATCH_POINTS
#define ATTEN_CAL_MIN_TX 1.0e-10
#define ATTEN_CAL_MAX_TX 0.999999
#define ATTEN_CAL_MIN_FIT_CORR 0.85
#define ATTEN_CAL_TELEMETRY_TOPIC_SUFFIX "atten"
#define ATTEN_CAL_DATA_PAGE_POINTS 5U

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
	ATTEN_CAL_AUTO_SIGNAL_SET,
	ATTEN_CAL_AUTO_SIGNAL_SETTLE,
	ATTEN_CAL_AUTO_SIGNAL_AVG,
	ATTEN_CAL_AUTO_POINT_SET,
	ATTEN_CAL_AUTO_POINT_SETTLE,
	ATTEN_CAL_AUTO_POINT_AVG,
	ATTEN_CAL_AUTO_ADJUST_SET,
	ATTEN_CAL_AUTO_ADJUST_SETTLE,
	ATTEN_CAL_AUTO_ADJUST_AVG,
	ATTEN_CAL_AUTO_FIT,
};

struct atten_cal_point {
	double voltage_mv;
	double flux;
	bool valid;
	bool saturated;
};

struct atten_cal_state_data {
	enum atten_cal_state state;
	enum atten_cal_mode mode;
	enum atten_cal_auto_phase phase;
	uint8_t attenuator_index;
	uint8_t physical_index;
	uint8_t point_index;
	uint32_t dwell_ms;
	bool persistent;
	enum hispec_laser_id laser;
	enum photodiode_channel channel;
	double other_mv;
	double laser_percent;
	double scale;
	double pending_before_mv;
	int64_t wait_until_ms;
	bool adjust_uses_laser;
	int last_error;
	struct atten_cal_point points[2][ATTENUATOR_CAL_POINT_COUNT];
	struct attenuator_calibration_fit_metrics fit[2];
};

static const double voltage_schedule[ATTENUATOR_CAL_POINT_COUNT] = {
	5000.0, 4500.0, 4000.0, 3700.0, 3500.0,
	3350.0, 3250.0, 3175.0, 3100.0, 3050.0,
	3000.0, 2950.0, 2875.0, 2800.0, 2650.0,
	2500.0, 2000.0, 1500.0, 750.0, 0.0,
};

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
static const char *fit_point_exclusion_reason(const struct atten_cal_point *point,
						      double max_flux, double *tx,
						      double *b);
static double fit_points_max_flux(
	const struct atten_cal_point points[ATTENUATOR_CAL_POINT_COUNT]);

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
		cal.point_index, ATTENUATOR_CAL_POINT_COUNT,
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
	size_t off;

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
		MIN(cal.point_index + 1U, ATTENUATOR_CAL_POINT_COUNT),
		ATTENUATOR_CAL_POINT_COUNT, sweep_mv, cal.other_mv,
		cal.laser_percent);
}

static void atten_cal_emit_reading(const char *event,
				   const struct photodiode_average_status *avg,
				   double flux)
{
	struct coo_cmd_response *msg;
	size_t off;
	double sweep_mv;

	if (avg == NULL) {
		return;
	}

	sweep_mv = voltage_schedule[MIN(cal.point_index,
					ATTENUATOR_CAL_POINT_COUNT - 1U)];
	msg = atten_cal_telemetry_begin(&off, event);
	if (msg == NULL ||
	    coo_json_append(msg->payload, sizeof(msg->payload), &off,
			    ",\"sweep_mv\":%.3f,\"other_mv\":%.3f,"
			    "\"mean_mv\":%.6f,\"mean_net_mv\":%.6f,"
			    "\"rms_mv\":%.6f,\"min_mv\":%.6f,"
			    "\"max_mv\":%.6f,\"max_raw\":%d,"
			    "\"samples\":%u,\"target_samples\":%u,"
			    "\"duration_ms\":%u,\"saturated\":%s,"
			    "\"valid\":%s,\"scale\":%.12g,\"flux\":%.12g}",
			    sweep_mv, cal.other_mv,
			    avg->result.mean_mv, avg->result.mean_net_mv,
			    avg->result.rms_mv, avg->result.min_mv,
			    avg->result.max_mv, avg->result.max_raw,
			    avg->result.samples, avg->result.target_samples,
			    avg->result.duration_ms,
			    sample_is_saturated(avg) ? "true" : "false",
			    flux > 0.0 ? "true" : "false",
			    cal.scale, flux) != 0) {
		LOG_WRN("atten calibration telemetry payload too large");
		return;
	}

	atten_cal_publish_telemetry(msg);
	LOG_INF("atten cal %s physical=%s point=%u/%u sweep_mv=%.1f other_mv=%.1f mean_net_mv=%.4f flux=%.6g samples=%u saturated=%d",
		event, physical_name(cal.physical_index),
		MIN(cal.point_index + 1U, ATTENUATOR_CAL_POINT_COUNT),
		ATTENUATOR_CAL_POINT_COUNT, sweep_mv, cal.other_mv,
		avg->result.mean_net_mv, flux, avg->result.samples,
		sample_is_saturated(avg) ? 1 : 0);
}

static void atten_cal_emit_adjust(const char *kind,
				  double before,
				  double after,
				  double measured_mv)
{
	struct coo_cmd_response *msg;
	size_t off;

	msg = atten_cal_telemetry_begin(&off, "adjust");
	if (msg == NULL ||
	    coo_json_append(msg->payload, sizeof(msg->payload), &off,
			    ",\"kind\":\"%s\",\"before\":%.6f,"
			    "\"after\":%.6f,\"measured_mv\":%.6f,"
			    "\"sweep_mv\":%.3f,\"other_mv\":%.3f,"
			    "\"laser_pct\":%.3f}",
			    kind, before, after, measured_mv,
			    voltage_schedule[MIN(cal.point_index,
						 ATTENUATOR_CAL_POINT_COUNT - 1U)],
			    cal.other_mv, cal.laser_percent) != 0) {
		LOG_WRN("atten calibration telemetry payload too large");
		return;
	}

	atten_cal_publish_telemetry(msg);
	LOG_INF("atten cal adjust kind=%s physical=%s point=%u/%u before=%.3f after=%.3f measured_mv=%.3f",
		kind, physical_name(cal.physical_index),
		MIN(cal.point_index + 1U, ATTENUATOR_CAL_POINT_COUNT),
		ATTENUATOR_CAL_POINT_COUNT, before, after, measured_mv);
}

static void atten_cal_emit_adjust_result(const struct photodiode_average_status *avg,
					 double scale_before, bool scale_updated)
{
	struct coo_cmd_response *msg;
	size_t off;

	if (avg == NULL) {
		return;
	}

	msg = atten_cal_telemetry_begin(&off, "adjust_result");
	if (msg == NULL ||
	    coo_json_append(msg->payload, sizeof(msg->payload), &off,
			    ",\"measured_before_mv\":%.6f,"
			    "\"measured_after_mv\":%.6f,"
			    "\"scale_before\":%.12g,\"scale_after\":%.12g,"
			    "\"scale_updated\":%s,\"saturated\":%s,"
			    "\"other_mv\":%.3f,\"laser_pct\":%.3f}",
			    cal.pending_before_mv, avg->result.mean_net_mv,
			    scale_before, cal.scale,
			    scale_updated ? "true" : "false",
			    sample_is_saturated(avg) ? "true" : "false",
			    cal.other_mv, cal.laser_percent) != 0) {
		LOG_WRN("atten calibration telemetry payload too large");
		return;
	}

	atten_cal_publish_telemetry(msg);
	LOG_INF("atten cal adjust_result physical=%s point=%u/%u before_mv=%.3f after_mv=%.3f scale_before=%.9g scale_after=%.9g updated=%d saturated=%d",
		physical_name(cal.physical_index),
		MIN(cal.point_index + 1U, ATTENUATOR_CAL_POINT_COUNT),
		ATTENUATOR_CAL_POINT_COUNT, cal.pending_before_mv,
		avg->result.mean_net_mv, scale_before, cal.scale,
		scale_updated ? 1 : 0, sample_is_saturated(avg) ? 1 : 0);
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

static void fit_point_diagnostics(
	const struct atten_cal_point *point,
	double max_flux,
	const struct attenuator_calibration_fit_metrics *fit,
	const char **reason_out,
	bool *included_out,
	double *tx_out,
	double *b_out,
	double *residual_db_out)
{
	double tx = 0.0;
	double b = 0.0;
	double residual_db = NAN;
	const char *reason = fit_point_exclusion_reason(point, max_flux, &tx, &b);
	bool included = strcmp(reason, "included") == 0;

	if (included && fit != NULL && fit->valid) {
		double predicted_tx =
			attenuator_model_b_to_linear(fit->slope * point->voltage_mv +
						     fit->offset);

		if (predicted_tx > 0.0 && tx > 0.0) {
			residual_db = 10.0 * log10(predicted_tx / tx);
		}
	}

	if (reason_out != NULL) {
		*reason_out = reason;
	}
	if (included_out != NULL) {
		*included_out = included;
	}
	if (tx_out != NULL) {
		*tx_out = tx;
	}
	if (b_out != NULL) {
		*b_out = b;
	}
	if (residual_db_out != NULL) {
		*residual_db_out = residual_db;
	}
}

static void atten_cal_log_fit_dataset_page(
	uint8_t physical,
	const struct atten_cal_point points[ATTENUATOR_CAL_POINT_COUNT],
	double max_flux,
	const struct attenuator_calibration_fit_metrics *fit,
	uint8_t start)
{
	char line[384];
	size_t off = 0U;
	uint8_t end;

	if (points == NULL || fit == NULL || start >= ATTENUATOR_CAL_POINT_COUNT) {
		return;
	}

	end = MIN(start + ATTEN_CAL_DATA_PAGE_POINTS, ATTENUATOR_CAL_POINT_COUNT);

	if (snprintk(line, sizeof(line), "query=atten/calibrate/data/%s/%u",
		     physical_name(physical), start) < 0) {
		return;
	}
	off = strlen(line);

	for (uint8_t i = start; i < end; ++i) {
		const struct atten_cal_point *point = &points[i];
		double tx;
		double b;
		double residual_db;
		const char *reason;
		bool included;
		int written;

		fit_point_diagnostics(point, max_flux, fit, &reason, &included,
				      &tx, &b, &residual_db);
		written = snprintk(line + off, sizeof(line) - off,
				   " %u:v=%.0f f=%.6g %s %s tx=%.6g res=%.4g",
				   i, point->voltage_mv, point->flux,
				   point->saturated ? "sat" : "ok",
				   included ? "in" : reason, tx, residual_db);
		if (written < 0 || written >= (int)(sizeof(line) - off)) {
			return;
		}
		off += (size_t)written;
	}

	LOG_INF("atten cal fit_data physical=%s start=%u max_flux=%.6g %s",
		physical_name(physical), start, max_flux, line);
}

static void atten_cal_log_fit_dataset(
	uint8_t physical,
	const struct atten_cal_point points[ATTENUATOR_CAL_POINT_COUNT],
	double max_flux,
	const struct attenuator_calibration_fit_metrics *fit)
{
	uint8_t included = 0U;
	uint8_t saturated = 0U;
	uint8_t valid = 0U;

	if (points == NULL || fit == NULL) {
		return;
	}

	for (uint8_t i = 0U; i < ATTENUATOR_CAL_POINT_COUNT; ++i) {
		double tx;
		double b;
		const char *reason;

		if (points[i].valid) {
			valid++;
		}
		if (points[i].saturated) {
			saturated++;
		}
		reason = fit_point_exclusion_reason(&points[i], max_flux, &tx, &b);
		if (strcmp(reason, "included") == 0) {
			included++;
		}
	}

	LOG_INF("atten cal fit_dataset physical=%s valid=%u included=%u saturated=%u accepted=%d max_flux=%.6g pages=atten/calibrate/data/%s/{0,5,10,15}",
		physical_name(physical), valid, included, saturated,
		fit->accepted ? 1 : 0, max_flux, physical_name(physical));
	for (uint8_t start = 0U; start < ATTENUATOR_CAL_POINT_COUNT;
	     start += ATTEN_CAL_DATA_PAGE_POINTS) {
		atten_cal_log_fit_dataset_page(physical, points, max_flux, fit, start);
	}
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
	return avg->result.max_raw >= ATTEN_CAL_SAT_RAW;
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
		complete = (uint16_t)cal.physical_index * ATTENUATOR_CAL_POINT_COUNT +
			   cal.point_index;
		return (uint8_t)((complete * 100U) /
				 (ATTENUATOR_CAL_POINT_COUNT * 2U));
	}
	return 0U;
}

static void copy_status_locked(struct attenuator_calibration_status *status)
{
	if (status == NULL) {
		return;
	}

	memset(status, 0, sizeof(*status));
	status->state = state_name(cal.state);
	status->mode = mode_name(cal.mode);
	status->physical = physical_name(cal.physical_index);
	status->fit = (cal.last_error != 0) ? "failed" :
		      ((cal.fit[0].accepted && cal.fit[1].accepted) ? "ok" : "none");
	status->attenuator_index = cal.attenuator_index;
	status->physical_index = cal.physical_index;
	status->point_index = cal.point_index;
	status->point_count = ATTENUATOR_CAL_POINT_COUNT;
	status->dwell_ms = cal.dwell_ms == 0U ? ATTEN_CAL_DEFAULT_DWELL_MS : cal.dwell_ms;
	status->complete_pct = complete_percent_locked();
	status->current_mv = voltage_schedule[MIN(cal.point_index,
						  ATTENUATOR_CAL_POINT_COUNT - 1U)];
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

static const char *fit_point_exclusion_reason(const struct atten_cal_point *point,
					      double max_flux, double *tx,
					      double *b)
{
	if (tx != NULL) {
		*tx = 0.0;
	}
	if (b != NULL) {
		*b = 0.0;
	}
	if (point == NULL) {
		return "invalid";
	}
	if (point->saturated) {
		return "saturated";
	}
	if (!point->valid) {
		return "invalid";
	}
	if (point->flux <= 0.0) {
		return "nonpositive_flux";
	}
	if (!(max_flux > 0.0)) {
		return "no_max_flux";
	}

	if (tx != NULL) {
		*tx = point->flux / max_flux;
		if (*tx > ATTEN_CAL_MAX_TX) {
			*tx = ATTEN_CAL_MAX_TX;
		}
		if (!point_valid_for_fit(*tx)) {
			return "tx_range";
		}
		if (b == NULL || !attenuator_model_linear_to_b(*tx, b)) {
			return "b_range";
		}
		return "included";
	}

	return "invalid";
}

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

static int fit_one_physical(uint8_t attenuator_index,
			    uint8_t physical_index,
			    const struct atten_cal_point points[ATTENUATOR_CAL_POINT_COUNT],
			    bool persistent,
			    struct attenuator_calibration_fit_metrics *out)
{
	zsl_real_t x_data[ATTENUATOR_CAL_POINT_COUNT];
	zsl_real_t y_data[ATTENUATOR_CAL_POINT_COUNT];
	struct zsl_vec x = {
		.sz = 0U,
		.data = x_data,
	};
	struct zsl_vec y = {
		.sz = 0U,
		.data = y_data,
	};
	struct zsl_sta_linreg reg = {0};
	double max_flux = 0.0;
	double min_tx = 1.0;
	double max_tx = 0.0;
	double min_v = ATTENUATOR_DRIVE_MAX_MV;
	double max_v = 0.0;
	double sum_sq_db = 0.0;
	double max_abs_db = 0.0;
	int rc;

	if (out == NULL || attenuator_index >= NUM_ATTENUATORS ||
	    physical_index >= ATTENUATOR_PHYSICAL_COUNT) {
		return -EINVAL;
	}
	ARG_UNUSED(persistent);
	memset(out, 0, sizeof(*out));

	max_flux = fit_points_max_flux(points);
	if (!(max_flux > 0.0)) {
		return -ERANGE;
	}

	for (uint8_t i = 0U; i < ATTENUATOR_CAL_POINT_COUNT; ++i) {
		double tx;
		double b;
		const char *reason;

		reason = fit_point_exclusion_reason(&points[i], max_flux, &tx, &b);
		if (strcmp(reason, "included") != 0) {
			continue;
		}

		x_data[x.sz] = (zsl_real_t)points[i].voltage_mv;
		y_data[y.sz] = (zsl_real_t)b;
		x.sz++;
		y.sz++;
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

	if (x.sz < ATTEN_CAL_MIN_FIT_POINTS || !(max_v > min_v)) {
		return -ERANGE;
	}

	rc = zsl_sta_linear_reg(&x, &y, &reg);
	if (rc != 0 || !(reg.slope > 0.0)) {
		return rc != 0 ? rc : -ERANGE;
	}

	for (size_t i = 0U; i < x.sz; ++i) {
		double measured_tx = attenuator_model_b_to_linear((double)y.data[i]);
		double predicted_tx =
			attenuator_model_b_to_linear((double)reg.slope * (double)x.data[i] +
						     (double)reg.intercept);
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
	out->points = (uint8_t)x.sz;
	out->slope = (double)reg.slope;
	out->offset = (double)reg.intercept;
	out->correlation = (double)reg.correlation;
	out->rms_db = sqrt(sum_sq_db / (double)x.sz);
	out->max_abs_db = max_abs_db;
	out->min_tx = min_tx;
	out->max_tx = max_tx;
	out->voltage_span_mv = max_v - min_v;
	out->accepted = isfinite(out->correlation) &&
			out->correlation >= ATTEN_CAL_MIN_FIT_CORR;

	atten_cal_log_fit_dataset(physical_index, points, max_flux, out);

	return out->accepted ? 0 : -ERANGE;
}

static int apply_fit_to_settings(uint8_t attenuator_index,
				 const struct attenuator_calibration_fit_metrics fit[2],
				 bool persistent)
{
	struct app_attenuator_channel_settings stored = {0};
	struct attenuator_model_coeffs physical[ATTENUATOR_PHYSICAL_COUNT];

	if (attenuator_index >= NUM_ATTENUATORS ||
	    !fit[0].accepted || !fit[1].accepted) {
		return -EINVAL;
	}

	physical[0].slope = fit[0].slope;
	physical[0].offset = fit[0].offset;
	physical[1].slope = fit[1].slope;
	physical[1].offset = fit[1].offset;

	if (attenuator_apply_coefficients_preserve_db(&attenuators[attenuator_index],
						      physical) != 0) {
		return -EIO;
	}

	stored.physical[0].slope = (double)fit[0].slope;
	stored.physical[0].offset = (double)fit[0].offset;
	stored.physical[1].slope = (double)fit[1].slope;
	stored.physical[1].offset = (double)fit[1].offset;
	app_settings_update_attenuator_channel(attenuator_index, &stored, persistent);
	return 0;
}

static int fit_current_locked(bool apply_settings)
{
	int rc;
	int first_error = 0;

	for (uint8_t physical = 0U; physical < ATTENUATOR_PHYSICAL_COUNT; ++physical) {
		rc = fit_one_physical(cal.attenuator_index, physical,
				      cal.points[physical], cal.persistent,
				      &cal.fit[physical]);
		atten_cal_emit_fit(physical, &cal.fit[physical]);
		if (rc != 0) {
			first_error = first_error == 0 ? rc : first_error;
		}
	}
	if (first_error != 0) {
		cal.last_error = first_error;
		cal.state = ATTEN_CAL_STATE_ERROR;
		atten_cal_emit_simple("error");
		return first_error;
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

	cal.state = ATTEN_CAL_STATE_COMPLETE;
	cal.phase = ATTEN_CAL_AUTO_NONE;
	cal.point_index = ATTENUATOR_CAL_POINT_COUNT;
	atten_cal_emit_simple("complete");
	return 0;
}

static void start_next_physical_locked(void)
{
	cal.point_index = 0U;
	cal.other_mv = ATTENUATOR_DRIVE_MAX_MV;
	cal.scale = 1.0;
	cal.phase = ATTEN_CAL_AUTO_SIGNAL_SET;
	atten_cal_emit_simple("physical_start");
}

static void auto_error_locked(int error)
{
	cal.last_error = error;
	cal.state = ATTEN_CAL_STATE_ERROR;
	cal.phase = ATTEN_CAL_AUTO_NONE;
	atten_cal_emit_simple("error");
}

static void record_current_point_locked(const struct photodiode_average_status *avg)
{
	struct atten_cal_point *point;

	point = &cal.points[cal.physical_index][cal.point_index];
	point->voltage_mv = voltage_schedule[cal.point_index];
	point->saturated = sample_is_saturated(avg);
	point->valid = avg->state == PHOTODIODE_AVERAGE_COMPLETE &&
		       !point->saturated &&
		       avg->result.mean_net_mv > 0.0;
	point->flux = point->valid ? (double)avg->result.mean_net_mv * cal.scale : 0.0;
	atten_cal_emit_reading("point_reading", avg, point->flux);
}

static void auto_finish_point_locked(void)
{
	cal.point_index++;
	if (cal.point_index < ATTENUATOR_CAL_POINT_COUNT) {
		cal.phase = ATTEN_CAL_AUTO_POINT_SET;
		return;
	}

	if (cal.physical_index == 0U) {
		cal.physical_index = 1U;
		start_next_physical_locked();
		return;
	}

	cal.phase = ATTEN_CAL_AUTO_FIT;
}

static void auto_tick_locked(int64_t now_ms)
{
	struct photodiode_average_status avg = {0};
	double sweep_mv;
	int rc;

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
	case ATTEN_CAL_AUTO_SIGNAL_SET:
		sweep_mv = voltage_schedule[0];
		if (!set_physical_pair(cal.attenuator_index, cal.physical_index,
				       sweep_mv, cal.other_mv)) {
			auto_error_locked(-EIO);
			return;
		}
		atten_cal_emit_point_set("signal_set", sweep_mv);
		cal.wait_until_ms = now_ms + ATTEN_CAL_STEP_SETTLE_MS;
		cal.phase = ATTEN_CAL_AUTO_SIGNAL_SETTLE;
		return;
	case ATTEN_CAL_AUTO_SIGNAL_SETTLE:
		if (now_ms < cal.wait_until_ms) {
			return;
		}
		rc = photodiode_start_average(cal.channel, cal.dwell_ms, NULL);
		if (rc != 0) {
			auto_error_locked(rc);
			return;
		}
		cal.phase = ATTEN_CAL_AUTO_SIGNAL_AVG;
		return;
	case ATTEN_CAL_AUTO_SIGNAL_AVG:
		rc = photodiode_get_average_status(cal.channel, &avg);
		if (rc != 0) {
			auto_error_locked(rc);
			return;
		}
		if (avg.state == PHOTODIODE_AVERAGE_MEASURING) {
			return;
		}
		if (avg.state != PHOTODIODE_AVERAGE_COMPLETE) {
			auto_error_locked(avg.last_error == 0 ? -EIO : avg.last_error);
			return;
		}
		if ((sample_is_saturated(&avg) ||
		     (double)avg.result.mean_net_mv > ATTEN_CAL_HIGH_MV) &&
		    cal.laser_percent > 3.0) {
			const double before = cal.laser_percent;

			cal.laser_percent /= 3.0;
			if (cal.laser_percent < 1.0) {
				cal.laser_percent = 1.0;
			}
			rc = hispec_laser_set_output_percent_autooff(
				cal.laser, (double)cal.laser_percent, 0U);
			if (rc != 0) {
				auto_error_locked(rc);
				return;
			}
			atten_cal_emit_adjust("laser_pct", before,
					      cal.laser_percent,
					      avg.result.mean_net_mv);
			cal.phase = ATTEN_CAL_AUTO_SIGNAL_SET;
			return;
		}
		if (sample_is_saturated(&avg)) {
			auto_error_locked(-ERANGE);
			return;
		}
		if ((double)avg.result.mean_net_mv < ATTEN_CAL_SIGNAL_MIN_MV &&
		    cal.other_mv > 0.0) {
			const double before = cal.other_mv;

			cal.other_mv -= ATTEN_CAL_OTHER_STEP_MV;
			if (cal.other_mv < 0.0) {
				cal.other_mv = 0.0;
			}
			atten_cal_emit_adjust("other_mv", before, cal.other_mv,
					      avg.result.mean_net_mv);
			cal.phase = ATTEN_CAL_AUTO_SIGNAL_SET;
			return;
		}
		cal.phase = ATTEN_CAL_AUTO_POINT_SET;
		return;
	case ATTEN_CAL_AUTO_POINT_SET:
		sweep_mv = voltage_schedule[cal.point_index];
		if (!set_physical_pair(cal.attenuator_index, cal.physical_index,
				       sweep_mv, cal.other_mv)) {
			auto_error_locked(-EIO);
			return;
		}
		atten_cal_emit_point_set("point_set", sweep_mv);
		cal.wait_until_ms = now_ms + ATTEN_CAL_STEP_SETTLE_MS;
		cal.phase = ATTEN_CAL_AUTO_POINT_SETTLE;
		return;
	case ATTEN_CAL_AUTO_POINT_SETTLE:
		if (now_ms < cal.wait_until_ms) {
			return;
		}
		rc = photodiode_start_average(cal.channel, cal.dwell_ms, NULL);
		if (rc != 0) {
			auto_error_locked(rc);
			return;
		}
		cal.phase = ATTEN_CAL_AUTO_POINT_AVG;
		return;
	case ATTEN_CAL_AUTO_POINT_AVG:
		rc = photodiode_get_average_status(cal.channel, &avg);
		if (rc != 0) {
			auto_error_locked(rc);
			return;
		}
		if (avg.state == PHOTODIODE_AVERAGE_MEASURING) {
			return;
		}
		if (avg.state != PHOTODIODE_AVERAGE_COMPLETE) {
			auto_error_locked(avg.last_error == 0 ? -EIO : avg.last_error);
			return;
		}
		if (sample_is_saturated(&avg)) {
			auto_error_locked(-ERANGE);
			return;
		}
		if ((double)avg.result.mean_net_mv > ATTEN_CAL_HIGH_MV &&
		    (cal.other_mv < ATTENUATOR_DRIVE_MAX_MV ||
		     cal.laser_percent > 3.0)) {
			cal.pending_before_mv = (double)avg.result.mean_net_mv;
			cal.adjust_uses_laser = cal.other_mv >= ATTENUATOR_DRIVE_MAX_MV;
			cal.phase = ATTEN_CAL_AUTO_ADJUST_SET;
			return;
		}
		record_current_point_locked(&avg);
		auto_finish_point_locked();
		return;
	case ATTEN_CAL_AUTO_ADJUST_SET:
		if (cal.adjust_uses_laser) {
			const double before = cal.laser_percent;

			cal.laser_percent /= 3.0;
			if (cal.laser_percent < 1.0) {
				cal.laser_percent = 1.0;
			}
			rc = hispec_laser_set_output_percent_autooff(cal.laser,
								     (double)cal.laser_percent,
								     0U);
			if (rc != 0) {
				auto_error_locked(rc);
				return;
			}
			atten_cal_emit_adjust("laser_pct", before, cal.laser_percent,
					      cal.pending_before_mv);
		} else {
			const double before = cal.other_mv;

			cal.other_mv += ATTEN_CAL_OTHER_STEP_MV;
			if (cal.other_mv > ATTENUATOR_DRIVE_MAX_MV) {
				cal.other_mv = ATTENUATOR_DRIVE_MAX_MV;
			}
			if (!set_physical_pair(cal.attenuator_index, cal.physical_index,
					       voltage_schedule[cal.point_index],
					       cal.other_mv)) {
				auto_error_locked(-EIO);
				return;
			}
			atten_cal_emit_adjust("other_mv", before, cal.other_mv,
					      cal.pending_before_mv);
		}
		cal.wait_until_ms = now_ms + ATTEN_CAL_STEP_SETTLE_MS;
		cal.phase = ATTEN_CAL_AUTO_ADJUST_SETTLE;
		return;
	case ATTEN_CAL_AUTO_ADJUST_SETTLE:
		if (now_ms < cal.wait_until_ms) {
			return;
		}
		rc = photodiode_start_average(cal.channel, cal.dwell_ms, NULL);
		if (rc != 0) {
			auto_error_locked(rc);
			return;
		}
		cal.phase = ATTEN_CAL_AUTO_ADJUST_AVG;
		return;
	case ATTEN_CAL_AUTO_ADJUST_AVG:
		rc = photodiode_get_average_status(cal.channel, &avg);
		if (rc != 0) {
			auto_error_locked(rc);
			return;
		}
		if (avg.state == PHOTODIODE_AVERAGE_MEASURING) {
			return;
		}
		if (avg.state != PHOTODIODE_AVERAGE_COMPLETE) {
			auto_error_locked(avg.last_error == 0 ? -EIO : avg.last_error);
			return;
		}
		{
			double scale_before = cal.scale;
			bool scale_updated = false;

			if (avg.result.mean_net_mv > 0.0 && !sample_is_saturated(&avg)) {
				cal.scale *= cal.pending_before_mv /
					     (double)avg.result.mean_net_mv;
				scale_updated = true;
			}
			atten_cal_emit_adjust_result(&avg, scale_before, scale_updated);
		}
		record_current_point_locked(&avg);
		auto_finish_point_locked();
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
	uint8_t attenuator_index;
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

	k_mutex_lock(&cal_lock, K_FOREVER);
	replacing = cal.state == ATTEN_CAL_STATE_RUNNING ||
		    cal.state == ATTEN_CAL_STATE_WAITING;
	k_mutex_unlock(&cal_lock);
	if (replacing) {
		coo_cmd_runtime_emit(command_runtime_get(),
				     &(const struct coo_cmd_runtime_emit_args){
					     .type = COO_CMD_RUNTIME_EMIT_WARNING,
					     .delivery = COO_CMD_RUNTIME_EMIT_BEST_EFFORT,
					     .code = "atten_calibration_replaced",
					     .msg = "attenuator calibration was replaced",
				     });
	}

	if (throughput_monitor_any_active()) {
		coo_cmd_runtime_emit(command_runtime_get(),
				     &(const struct coo_cmd_runtime_emit_args){
					     .type = COO_CMD_RUNTIME_EMIT_WARNING,
					     .delivery = COO_CMD_RUNTIME_EMIT_BEST_EFFORT,
					     .code = "throughput_stopped",
					     .msg = "throughput monitoring was stopped for attenuator calibration",
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
	rc = hispec_laser_set_output_percent_autooff(request->laser, 100.0, 0U);
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
	cal.wait_until_ms = k_uptime_get() + ATTEN_CAL_PD_POWER_SETTLE_MS;
	atten_cal_emit_simple("start");
	copy_status_locked(status);
	k_mutex_unlock(&cal_lock);
	return 0;
}

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

int attenuator_calibration_format_data_page(char *payload,
					    size_t payload_len,
					    uint8_t physical_index,
					    uint8_t start_index)
{
	const struct attenuator_calibration_fit_metrics *fit;
	const struct atten_cal_point *points;
	double max_flux;
	uint8_t count;
	size_t off = 0U;
	int rc = 0;

	if (payload == NULL || physical_index >= ATTENUATOR_PHYSICAL_COUNT ||
	    start_index >= ATTENUATOR_CAL_POINT_COUNT) {
		return -EINVAL;
	}

	k_mutex_lock(&cal_lock, K_FOREVER);
	points = cal.points[physical_index];
	fit = &cal.fit[physical_index];
	max_flux = fit_points_max_flux(points);
	count = MIN(ATTEN_CAL_DATA_PAGE_POINTS,
		    ATTENUATOR_CAL_POINT_COUNT - start_index);

	if (coo_json_append(payload, payload_len, &off,
			    "{\"state\":\"%s\",\"mode\":\"%s\","
			    "\"physical\":\"%s\",\"start\":%u,"
			    "\"count\":%u,\"point_count\":%u,\"next\":",
			    state_name(cal.state), mode_name(cal.mode),
			    physical_name(physical_index), start_index,
			    count, ATTENUATOR_CAL_POINT_COUNT) != 0) {
		rc = -ENOSPC;
		goto out;
	}
	if (start_index + count < ATTENUATOR_CAL_POINT_COUNT) {
		if (coo_json_append(payload, payload_len, &off, "%u",
				    start_index + count) != 0) {
			rc = -ENOSPC;
			goto out;
		}
	} else if (coo_json_append(payload, payload_len, &off, "null") != 0) {
		rc = -ENOSPC;
		goto out;
	}
	if (coo_json_append(payload, payload_len, &off,
			    ",\"fit_valid\":%s,\"fit_accepted\":%s,"
			    "\"max_flux\":%.12g,\"points\":[",
			    fit->valid ? "true" : "false",
			    fit->accepted ? "true" : "false",
			    max_flux) != 0) {
		rc = -ENOSPC;
		goto out;
	}

	for (uint8_t idx = start_index; idx < start_index + count; ++idx) {
		const struct atten_cal_point *point = &points[idx];
		double tx;
		double b;
		double residual_db;
		double b_json;
		const char *reason;
		bool included;

		fit_point_diagnostics(point, max_flux, fit, &reason, &included,
				      &tx, &b, &residual_db);
		b_json = included ? b : nan("");
		if (idx > start_index &&
		    coo_json_append(payload, payload_len, &off, ",") != 0) {
			rc = -ENOSPC;
			goto out;
		}
		if (coo_json_append(payload, payload_len, &off,
				    "{\"i\":%u,\"v\":%.3f,\"f\":%.12g,"
				    "\"valid\":%s,\"sat\":%s,\"in\":%s,"
				    "\"r\":\"%s\",\"tx\":%.12g,\"b\":",
				    idx, point->voltage_mv, point->flux,
				    point->valid ? "true" : "false",
				    point->saturated ? "true" : "false",
				    included ? "true" : "false", reason, tx) != 0 ||
		    coo_json_append_float_or_null(payload, payload_len, &off, b_json, 9) != 0 ||
		    coo_json_append(payload, payload_len, &off, ",\"res\":") != 0 ||
		    coo_json_append_float_or_null(payload, payload_len, &off,
						  residual_db, 6) != 0 ||
		    coo_json_append(payload, payload_len, &off, "}") != 0) {
			rc = -ENOSPC;
			goto out;
		}
	}

	if (coo_json_append(payload, payload_len, &off, "]}") != 0) {
		rc = -ENOSPC;
	}

out:
	k_mutex_unlock(&cal_lock);
	return rc;
}

void attenuator_calibration_tick(const struct photodiode_status *pd_status,
				 int64_t now_ms)
{
	ARG_UNUSED(pd_status);

	k_mutex_lock(&cal_lock, K_FOREVER);
	auto_tick_locked(now_ms);
	k_mutex_unlock(&cal_lock);
}
