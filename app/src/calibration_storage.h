/* Internal typed storage shared by the two mutually exclusive calibrations.
 * Control/status remain with each domain. The shared lock protects reset and
 * record copies; numerical work owns immutable data until cancellation joins.
 */
#ifndef HISPEC_CALIBRATION_STORAGE_H
#define HISPEC_CALIBRATION_STORAGE_H

#include "attenuator.h"
#include "laser_calibration.h"
#include <coo_commons/command_dispatch.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/atomic.h>

#define CALIBRATION_RECORD_COUNT 128U
#define LASER_CAL_SEGMENTS 8U
#define LASER_CAL_GRID_COUNT 64U

enum calibration_data_kind { CALIBRATION_DATA_NONE, CALIBRATION_DATA_ATTEN, CALIBRATION_DATA_LASER };

struct atten_cal_record {
	float sweep_mv, other_mv;
	uint8_t laser_pct;
	float signal_mv, signal_err_mv, max_mv;
	uint8_t event, classification, segment;
};
struct atten_cal_bridge { uint8_t before_record_index, after_record_index; };
struct atten_cal_fit_point { uint8_t record_index; float measured_db, measured_db_err; };

/* Explicit binary encoding follows this field order; never transmit C padding. */
struct laser_cal_record {
	uint32_t elapsed_ms;
	float net_mv, rms_mv;
	uint16_t current_tenth_ma, dac_code[2];
	int16_t max_raw;
	uint8_t good, failed, segment, flags;
};

struct laser_cal_metadata {
	struct attenuator_model_coeffs coeff[2];
	double ref_mw, ref_ma, ref_temp_c, max_ma, nominal_ma, seed_threshold_ma;
	double current_cal_pct, pd_gain_mv_per_mw, dark_mv, dark_rms_mv;
	double return_tx, loopback_loss_db[2];
	double default_threshold_ma, default_efficiency_mw_per_ma;
	uint64_t started_ms;
	uint32_t run_id, dwell_ms;
	float dac_full_scale_mv[2], dac_limit_mv[2], temp_min_c, temp_max_c;
	uint8_t laser, channel, fiber, first_fei, options;
};

union calibration_data {
	struct {
		struct atten_cal_record records[2][CALIBRATION_RECORD_COUNT];
		struct atten_cal_bridge bridges[2][CALIBRATION_RECORD_COUNT];
	} atten;
	struct {
		struct laser_cal_metadata meta;
		struct laser_calibration_status result;
		struct laser_cal_record records[CALIBRATION_RECORD_COUNT];
		uint16_t grid[LASER_CAL_GRID_COUNT];
	} laser;
};

union calibration_scratch {
	struct {
		struct atten_cal_fit_point points[CALIBRATION_RECORD_COUNT];
		double segment_scale[CALIBRATION_RECORD_COUNT];
		double segment_rel_var[CALIBRATION_RECORD_COUNT];
	} atten;
	struct {
		/* S0,Sx,Sxx,Sy,Sxy in each fixed-attenuation segment. */
		double sums[LASER_CAL_SEGMENTS][5];
		double gain[LASER_CAL_SEGMENTS], dt[LASER_CAL_SEGMENTS], db[LASER_CAL_SEGMENTS];
		double dark_variance, baseline_weight, baseline_mean;
		uint8_t include[CALIBRATION_RECORD_COUNT], count[LASER_CAL_SEGMENTS];
	} laser;
};

extern union calibration_data calibration_data;
extern union calibration_scratch calibration_scratch;
extern struct k_mutex calibration_lock;
extern struct coo_cmd_response calibration_telemetry;
extern atomic_t calibration_data_kind;
extern uint32_t calibration_run_id; /* Updated under calibration_lock. */

#endif
