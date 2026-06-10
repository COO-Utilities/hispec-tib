/**
 * @file attenuator_calibration.h
 * @brief Attenuator calibration state machine shared by commands and throughput thread.
 *
 * Commands start/stop/manual-step calibration and format compact status
 * replies. The throughput monitor thread calls attenuator_calibration_tick()
 * so automatic TIB calibration reuses the existing photodiode polling thread
 * instead of creating another worker.
 */
#ifndef HISPEC_ATTENUATOR_CALIBRATION_H
#define HISPEC_ATTENUATOR_CALIBRATION_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "lasers.h"
#include "photodiode.h"

#define ATTENUATOR_CAL_POINT_COUNT 20U
#define ATTENUATOR_CAL_MIN_BATCH_POINTS 6U

struct attenuator_calibration_fit_metrics {
	bool valid;
	uint8_t points;
	double slope;
	double offset;
	double correlation;
	double rms_db;
	double max_abs_db;
	double min_tx;
	double max_tx;
	double voltage_span_mv;
};

struct attenuator_calibration_batch {
	double voltage_mv[ATTENUATOR_CAL_POINT_COUNT];
	double flux[ATTENUATOR_CAL_POINT_COUNT];
	size_t len;
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
	double current_mv;
	double other_mv;
	int last_error;
	bool include_voltage_schedule;
	double voltage_schedule_mv[ATTENUATOR_CAL_POINT_COUNT];
	struct attenuator_calibration_fit_metrics fit_metrics[2];
};

struct attenuator_calibration_auto_request {
	enum hispec_laser_id laser;
	const char *output;
	char fiber;
	uint32_t dwell_ms;
	bool persistent;
};

/** Start automatic TIB calibration for the laser's logical attenuator pair. */
int attenuator_calibration_start_auto(
	const struct attenuator_calibration_auto_request *request,
	struct attenuator_calibration_status *status);

/** Start manual stepping for one logical attenuator pair. */
int attenuator_calibration_start_manual(uint8_t attenuator_index,
					uint32_t dwell_ms,
					bool persistent,
					struct attenuator_calibration_status *status);

/** Advance manual stepping by one point; optional other_mv adjusts the held DAC. */
int attenuator_calibration_manual_continue(bool has_other_mv,
					   double other_mv,
					   struct attenuator_calibration_status *status);

/** Fit manual batch flux feedback for one logical attenuator pair. */
int attenuator_calibration_fit_manual(
	uint8_t attenuator_index,
	const struct attenuator_calibration_batch physical[2],
	bool persistent,
	struct attenuator_calibration_status *status);

/** Cancel calibration and return inactive state plus any schedule captured so far. */
int attenuator_calibration_stop(struct attenuator_calibration_status *status);

/** Copy current calibration status. */
void attenuator_calibration_get_status(struct attenuator_calibration_status *status);

/** Return true while calibration owns attenuator-control sequencing. */
bool attenuator_calibration_active(void);

/** Append the compact calibration status JSON to @p payload. */
int attenuator_calibration_format_status(
	char *payload, size_t payload_len,
	const struct attenuator_calibration_status *status);

/** Advance automatic calibration. Called only by the throughput monitor thread. */
void attenuator_calibration_tick(const struct photodiode_status *pd_status,
				 int64_t now_ms);

#endif /* HISPEC_ATTENUATOR_CALIBRATION_H */
