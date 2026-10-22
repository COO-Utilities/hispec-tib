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

enum calibration_data_kind {
	CALIBRATION_DATA_NONE,
	CALIBRATION_DATA_ATTEN,
	CALIBRATION_DATA_LASER,
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

struct atten_cal_bridge {
	uint8_t before_record_index;
	uint8_t after_record_index;
};

struct atten_cal_fit_point {
	uint8_t record_index;
	float measured_db;
	float measured_db_err;
};

/* Explicit binary encoding follows this field order; never transmit C padding. */
struct laser_cal_record {
	uint32_t elapsed_ms;
	float net_mv;
	float rms_mv; /* Population RMS of the surviving ADC samples. */
	uint16_t current_tenth_ma;
	uint16_t dac_code[2]; /* Confirmed physical pair, before optical model conversion. */
	int16_t max_raw;
	uint8_t good;
	uint8_t failed;
	uint8_t segment;
	uint8_t flags; /* Classification 0..1, event 2..4, other output 7. */
};

struct laser_cal_metadata {
	struct attenuator_model_coeffs coeff[2];
	/* Frozen acquisition assumptions, separate from the fitted result. */
	double ref_mw;
	double ref_ma;
	double ref_temp_c;
	double max_ma;
	double nominal_ma;
	double seed_threshold_ma;
	double current_cal_pct;
	double pd_gain_mv_per_mw;
	double dark_mv;
	double dark_rms_mv;
	double return_tx;
	double loopback_loss_db[2];
	double default_threshold_ma;
	double default_efficiency_mw_per_ma;
	double max_fit_residual_pct;
	double pd_target_mv;
	/* Published normalization, conditional on this run's dark and reference.
	 * Copied from fit scratch under calibration_lock, never reconstructed by host. */
	double segment_mv_per_mw[LASER_CAL_SEGMENTS];
	double baseline_mv;
	double dark_variance_mv2;
	uint8_t fit_support[CALIBRATION_RECORD_COUNT / 8U];
	uint64_t started_ms;
	uint32_t run_id;
	uint32_t dwell_ms;
	float dac_full_scale_mv[2];
	float dac_limit_mv[2];
	float temp_min_c;
	float temp_max_c;
	uint8_t laser;
	uint8_t channel;
	uint8_t fiber;
	uint8_t first_fei;
	uint8_t options;
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
		/* Record-indexed normalized observations; accumulate normal equations
		 * in double. This remains smaller than the attenuator scratch member. */
		float power_mw[CALIBRATION_RECORD_COUNT];
		float variance_mw2[CALIBRATION_RECORD_COUNT];
		uint16_t levels[CALIBRATION_RECORD_COUNT];
		double gain[LASER_CAL_SEGMENTS]; /* Measured mV/mW, including attenuation. */
		double dark_variance;
		double baseline_weight;
		double baseline_mean;
		uint8_t include[CALIBRATION_RECORD_COUNT];
	} laser;
};

BUILD_ASSERT(sizeof(struct laser_cal_record) == 24U);
BUILD_ASSERT(sizeof(union calibration_scratch) <= 3584U);

extern union calibration_data calibration_data;
extern union calibration_scratch calibration_scratch;
extern struct k_mutex calibration_lock;
extern struct coo_cmd_response calibration_telemetry;
extern atomic_t calibration_data_kind;
extern uint32_t calibration_run_id; /* Updated under calibration_lock. */

#endif
