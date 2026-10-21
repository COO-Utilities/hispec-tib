#include "calibration_storage.h"

union calibration_data calibration_data;
union calibration_scratch calibration_scratch;
K_MUTEX_DEFINE(calibration_lock);
struct coo_cmd_response calibration_telemetry;
atomic_t calibration_data_kind;
uint32_t calibration_run_id;

BUILD_ASSERT(sizeof(struct laser_cal_record) == 24);
BUILD_ASSERT(sizeof(struct laser_cal_metadata) <= 512);
BUILD_ASSERT(sizeof(calibration_data) == 7680);
BUILD_ASSERT(sizeof(calibration_scratch) == 3584);
BUILD_ASSERT(sizeof(calibration_scratch.laser) <= 1024);
