/** Laser calibration commands and the existing throughput-thread owner. */
#ifndef HISPEC_LASER_CALIBRATION_H
#define HISPEC_LASER_CALIBRATION_H

#include "lasers.h"
#include "photodiode.h"

enum laser_calibration_state {
	LASER_CAL_INACTIVE, LASER_CAL_RUNNING, LASER_CAL_FITTING,
	LASER_CAL_AWAITING_OUTPUT, LASER_CAL_COMPLETE, LASER_CAL_STOPPED, LASER_CAL_ERROR,
};

struct laser_calibration_request {
	enum hispec_laser_id laser;
	const char *output;
	char fiber;
	uint32_t dwell_ms;
	double loopback_loss_db;
	bool determine_static_loss, update_noise, other_output, persist;
};

struct laser_calibration_fit {
	double threshold_ma, efficiency_mw_per_ma, floor_ma, linear_max_ma;
	double baseline_mv, rms_fraction, threshold_se_ma;
	double constant_noise_mw, fractional_noise;
	int error, noise_error, persist_error;
	uint8_t points;
	bool valid, accepted, applied, persisted;
	bool noise_valid, noise_applied, noise_persisted;
};

struct laser_calibration_route_fit {
	double combined_loss_db, outbound_loss_db;
	int error, persist_error;
	bool accepted, applied, persisted;
};

struct laser_calibration_status {
	enum laser_calibration_state state;
	enum hispec_laser_id laser;
	struct laser_calibration_fit fit;
	struct laser_calibration_route_fit route[2];
	uint32_t dwell_ms;
	float current_ma;
	int error;
	uint8_t records, output_index;
	bool first_fei, acquisition_complete, overflow, shutdown_pending;
};

struct laser_calibration_restore_result {
	int laser_error, route_error, laser_persist_error, route_persist_error;
	bool laser_applied, route_applied, laser_persisted, route_persisted;
};

/* Starts may cancel/join the previous calibration, stop throughput, and block
 * on hardware setup. Accepted starts replace the shared retained dataset. */
int laser_calibration_start(const struct laser_calibration_request *request,
			    struct laser_calibration_status *status);
/* Resume the paused other-output anchor. No current sweep is repeated. */
int laser_calibration_continue(bool set_loopback, double loopback_loss_db,
			       struct laser_calibration_status *status);
/* Cancel/join numerical work and stop the owned source; retain raw records. */
int laser_calibration_stop(struct laser_calibration_status *status);
/* Non-I/O snapshot; active includes a recabling pause or unconfirmed shutdown. */
void laser_calibration_get_status(struct laser_calibration_status *status);
bool laser_calibration_active(void);
/* Only the throughput thread calls tick. May sleep on actuation and run a
 * cancellable fit at lowest application priority, with no state mutex held. */
void laser_calibration_tick(const struct photodiode_status *pd);
int laser_calibration_format_status(char *payload, size_t size,
				    const struct laser_calibration_status *status);
/* Bounded copies, under the shared data lock. Metadata and records are binary;
 * chunk=-1 selects metadata. Returns -ENODATA after another calibration starts. */
int laser_calibration_write_data(void *payload, size_t size, int chunk, size_t *written);
/* Stops calibration/output and restores only calibration-owned properties and
 * this output's outbound loss. May block on hardware and optional NVS writes. */
int laser_calibration_restore_defaults(enum hispec_laser_id laser, const char *output,
				      bool persist, struct laser_calibration_restore_result *result);

#endif
