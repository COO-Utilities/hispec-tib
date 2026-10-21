/** Laser calibration commands and the existing throughput-thread owner. */
#ifndef HISPEC_LASER_CALIBRATION_H
#define HISPEC_LASER_CALIBRATION_H

#include "lasers.h"
#include "photodiode.h"

enum laser_calibration_state {
	LASER_CAL_INACTIVE,
	LASER_CAL_RUNNING,
	LASER_CAL_FITTING,
	LASER_CAL_AWAITING_OUTPUT,
	LASER_CAL_COMPLETE,
	LASER_CAL_STOPPED,
	LASER_CAL_ERROR,
};

struct laser_calibration_request {
	enum hispec_laser_id laser;
	const char *output; /* Same-band named AO/FEI launch route. */
	char fiber; /* S or M selects the loopback return. */
	uint32_t dwell_ms;
	double loopback_loss_db; /* External cable/attenuators only; excludes the PD return. */
	bool determine_static_loss;
	bool update_noise;
	bool other_output;
	bool persist;
};

struct laser_calibration_fit {
	double threshold_ma;
	double efficiency_mw_per_ma;
	double floor_ma;
	double linear_max_ma; /* Diagnostic; never lowers configured maximum automatically. */
	double baseline_mv;
	double rms_fraction; /* Mean-curve residuals, separate from temporal noise. */
	double threshold_se_ma;
	double constant_noise_mw;
	double fractional_noise;
	int error;
	int noise_error;
	int persist_error;
	uint8_t points;
	/* Numerical availability, policy acceptance, RAM update and NVS success
	 * are distinct facts; a rejected fit may still have diagnostic estimates. */
	bool valid;
	bool accepted;
	bool applied;
	bool persisted;
	bool noise_valid;
	bool noise_applied;
	bool noise_persisted;
};

struct laser_calibration_route_fit {
	double combined_loss_db; /* Outbound plus configured return, excluding loopback/FVOAs. */
	double outbound_loss_db;
	int error;
	int persist_error;
	bool accepted;
	bool applied;
	bool persisted;
};

struct laser_calibration_status {
	enum laser_calibration_state state;
	enum hispec_laser_id laser;
	struct laser_calibration_fit fit;
	struct laser_calibration_route_fit route[2]; /* Requested output, then other output. */
	uint32_t dwell_ms;
	float current_ma;
	int error;
	uint8_t records;
	uint8_t output_index;
	bool first_fei;
	bool acquisition_complete; /* Completion of this output's acquisition, not fit acceptance. */
	bool overflow;
	bool shutdown_pending; /* Ownership remains until emission-off is confirmed. */
};

struct laser_calibration_restore_result {
	int laser_error;
	int route_error;
	int laser_persist_error;
	int route_persist_error;
	bool laser_applied;
	bool route_applied;
	bool laser_persisted;
	bool route_persisted;
};

/** Starts may cancel/join the previous calibration, stop throughput, and block
 * on hardware setup. Accepted starts replace the shared retained dataset. */
int laser_calibration_start(const struct laser_calibration_request *request,
				    struct laser_calibration_status *status);

/** Resume the paused other-output anchor. May block on hardware setup and queue
 * status; retains first-output results. No current sweep is repeated. */
int laser_calibration_continue(bool set_loopback, double loopback_loss_db,
				       struct laser_calibration_status *status);

/** Cancel/join numerical work and stop the owned source; retain raw records. */
int laser_calibration_stop(struct laser_calibration_status *status);

/** Snapshot under the status mutex, without hardware I/O. */
void laser_calibration_get_status(struct laser_calibration_status *status);

/** Ownership includes a recabling pause or unconfirmed shutdown; status lock only. */
bool laser_calibration_active(void);

/** Only the throughput thread calls tick. May sleep on actuation and run a
 * cancellable fit at lowest application priority, with no state mutex held.
 * Applies accepted settings, optionally writes NVS, and queues status. */
void laser_calibration_tick(const struct photodiode_status *pd);

/** Encode a coherent caller-owned snapshot; no locks or I/O. Returns -ENOSPC
 * rather than letting callers publish a truncated JSON response. */
int laser_calibration_format_status(char *payload, size_t size,
					    const struct laser_calibration_status *status);

/** Bounded copies, under the shared data lock. Metadata and records are binary;
 * chunk=-1 selects metadata. Returns -ENODATA after another calibration starts. */
int laser_calibration_write_data(void *payload, size_t size, int chunk, size_t *written);

/** Stops calibration/output and restores only calibration-owned properties and
 * this output's outbound loss. May block on hardware and optional NVS writes.
 * The required result reports independent RAM/persistence outcomes. */
int laser_calibration_restore_defaults(enum hispec_laser_id laser, const char *output,
				      bool persist, struct laser_calibration_restore_result *result);

#endif
