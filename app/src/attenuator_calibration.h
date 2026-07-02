/**
 * @file attenuator_calibration.h
 * @brief Attenuator calibration state machine shared by commands and throughput thread.
 *
 * Commands start/stop calibration and format compact status replies. The
 * throughput monitor thread calls attenuator_calibration_tick() so automatic
 * TIB calibration reuses the existing photodiode polling thread instead of
 * creating another worker.
 */
#ifndef HISPEC_ATTENUATOR_CALIBRATION_H
#define HISPEC_ATTENUATOR_CALIBRATION_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "attenuator.h"
#include "lasers.h"
#include "photodiode.h"

#define ATTENUATOR_CAL_RECORD_COUNT 128U
#define ATTENUATOR_CAL_MIN_FIT_POINTS 6U

struct attenuator_calibration_fit_metrics {
	bool valid;
	bool accepted;
	uint8_t points;
	double fvoa_50pct_mv;
	double slope_inv_fvoa_mv;
	double max_atten_db;
	double max_atten_sigma_db;
	double correlation;
	double rms_db;
	double max_abs_db;
	double min_tx;
	double max_tx;
	double fvoa_span_mv;
	float correction_coeff[ATTENUATOR_MODEL_CORRECTION_TERMS];
};

struct attenuator_calibration_status {
	const char *state;
	const char *mode;
	const char *physical;
	const char *fit;
	uint8_t attenuator_index;
	uint8_t physical_index;
	uint8_t point_index;
	uint8_t point_count;
	uint32_t dwell_ms;
	uint8_t complete_pct;
	float current_mv;
	float other_mv;
	int last_error;
	uint8_t laser_percent;
	struct attenuator_calibration_fit_metrics fit_metrics[2];
};

struct attenuator_calibration_auto_request {
	enum hispec_laser_id laser;
	uint8_t attenuator_index;
	enum photodiode_channel channel;
	const char *route_input;
	const char *output;
	const char *pd_input;
	const char *pd_output;
	uint32_t dwell_ms;
	bool persist;
};

/** Start automatic TIB calibration for the laser's logical attenuator pair. */
int attenuator_calibration_start_auto(
	const struct attenuator_calibration_auto_request *request,
	struct attenuator_calibration_status *status);

/** Cancel calibration and return inactive state. */
int attenuator_calibration_stop(struct attenuator_calibration_status *status);

/** Copy current calibration status. */
void attenuator_calibration_get_status(struct attenuator_calibration_status *status);

/** Return true while calibration owns attenuator-control sequencing. */
bool attenuator_calibration_active(void);

/** Append the compact calibration status JSON to @p payload. */
int attenuator_calibration_format_status(
	char *payload, size_t payload_len,
	const struct attenuator_calibration_status *status);

/** Write calibration dataset metadata into @p payload. */
int attenuator_calibration_write_data_metadata(void *payload,
					       size_t payload_len,
					       uint8_t physical_index,
					       size_t *written);

/** Write one fixed retained-record binary chunk into @p payload. */
int attenuator_calibration_write_record_chunk(void *payload,
					      size_t payload_len,
					      uint8_t physical_index,
					      uint8_t chunk_index,
					      size_t *written);

/** Advance automatic calibration. Called only by the throughput monitor thread. */
void attenuator_calibration_tick(const struct photodiode_status *pd_status,
				 int64_t now_ms);

#endif /* HISPEC_ATTENUATOR_CALIBRATION_H */
