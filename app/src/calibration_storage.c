#include "calibration_storage.h"

/* One retained dataset and one numerical workspace: starts join the previous
 * owner before selecting another union member. No per-calibration duplicate RAM. */
union calibration_data calibration_data;
union calibration_scratch calibration_scratch;
K_MUTEX_DEFINE(calibration_lock);
struct coo_cmd_response calibration_telemetry;
atomic_t calibration_data_kind;
uint32_t calibration_run_id;

/* Keep the original attenuator RAM envelope. Laser wire encoding is explicit,
 * but its in-memory records must also remain 24 bytes to meet the point budget. */
BUILD_ASSERT(sizeof(struct laser_cal_record) == 24);
BUILD_ASSERT(sizeof(struct laser_cal_metadata) <= 512);
BUILD_ASSERT(sizeof(calibration_data) == 7680);
BUILD_ASSERT(sizeof(calibration_scratch) == 3584);
/* Laser normalization reuses the larger attenuator workspace; no extra RAM. */
BUILD_ASSERT(sizeof(calibration_scratch.laser) <= sizeof(calibration_scratch.atten));
