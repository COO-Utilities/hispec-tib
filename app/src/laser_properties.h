#ifndef LASER_PROPERTIES_H
#define LASER_PROPERTIES_H

#include <math.h>
#include <stdint.h>

typedef struct {
    float min_c;
    float max_c;
} temp_range_c_t;

typedef struct {
    uint16_t kp;
    uint16_t ki;
    uint16_t kd;
} tec_pid_t;

typedef struct laserprops_t {
    const char *name;
    const char *model_number;
    float nominal_current_ma;
    float max_current_ma;
    float dne_current_ma;
    float threshold_current_ma;
    float efficiency_mw_per_ma;
    float wavelength_nm;
    float test_monitor_current_ua;
    temp_range_c_t operating_temp_range_c;
    float operating_temp_c;
    float thermistor_kohm;
    float isolation_db;
    float tec_max_current_a;
    tec_pid_t tec_pid;
    float ntc_t_coefficient_per_c;
    float dlambda_dT_nm_per_k;
    float dlambda_dA_nm_per_ma;
} laserprops_t;

#define LASERPROP_NA (NAN)

static const tec_pid_t TEC_PID_DEFAULT = {100, 1000, 0};
static const tec_pid_t TEC_PID_DFB = {20, 1000, 1000};

static const laserprops_t LASER_1028 = {
    .name = "1028",
    .model_number = "FLPD-1028-50-DFB-BTF",
    .threshold_current_ma = 14.5f,
    .nominal_current_ma = 250.0f,
    .max_current_ma = 250.0f,
    .dne_current_ma = 275.0f,
    .tec_max_current_a = 1.2f,
    .tec_pid = TEC_PID_DFB,
    .wavelength_nm = 1028.01f,
    .test_monitor_current_ua = 23.3f,
    .efficiency_mw_per_ma = 0.185f,
    .dlambda_dA_nm_per_ma = 0.015f,
    .dlambda_dT_nm_per_k = 0.12f,
    .operating_temp_range_c = {17.0f, 38.0f},
    .operating_temp_c = 25.0f,
    .thermistor_kohm = 10.0f,
    .isolation_db = 30.0f,
    .ntc_t_coefficient_per_c = -0.044f
};

static const laserprops_t LASER_2330 = {
    .name = "2330",
    .model_number = "FLPD-2330-03-DFB-BTF",
    .threshold_current_ma = 24.9f,
    .nominal_current_ma = 120.0f,
    .max_current_ma = 120.0f,
    .dne_current_ma = 137.0f,
    .tec_max_current_a = 1.2f,
    .tec_pid = TEC_PID_DFB,
    .wavelength_nm = 2329.81f,
    .test_monitor_current_ua = LASERPROP_NA,
    .efficiency_mw_per_ma = 0.031f,
    .dlambda_dA_nm_per_ma = 0.015f,
    .dlambda_dT_nm_per_k = 0.12f,
    .operating_temp_range_c = {17.0f, 38.0f},
    .operating_temp_c = 25.0f,
    .thermistor_kohm = 10.0f,
    .isolation_db = 30.0f,
    .ntc_t_coefficient_per_c = -0.044f
};

static const laserprops_t LASER_1270 = {
    .name = "1270",
    .model_number = "1270LD-1-0-0",
    .threshold_current_ma = 8.0f,
    .nominal_current_ma = 60.0f,
    .max_current_ma = 60.0f,
    .dne_current_ma = 75.0f,
    .tec_max_current_a = 1.0f,
    .tec_pid = TEC_PID_DEFAULT,
    .wavelength_nm = 1270.0f,
    .test_monitor_current_ua = LASERPROP_NA,
    .efficiency_mw_per_ma = 0.166f,
    .dlambda_dA_nm_per_ma = 0.003f,
    .dlambda_dT_nm_per_k = 0.08f,
    .operating_temp_range_c = {17.0f, 38.0f},
    .operating_temp_c = 25.0f,
    .thermistor_kohm = 10.0f,
    .isolation_db = 25.0f,
    .ntc_t_coefficient_per_c = LASERPROP_NA
};

static const laserprops_t LASER_1430 = {
    .name = "1430",
    .model_number = "1430LD-1-0-0",
    .threshold_current_ma = 8.0f,
    .nominal_current_ma = 60.0f,
    .max_current_ma = 60.0f,
    .dne_current_ma = 72.0f,
    .tec_max_current_a = 1.0f,
    .tec_pid = TEC_PID_DEFAULT,
    .wavelength_nm = 1430.0f,
    .test_monitor_current_ua = LASERPROP_NA,
    .efficiency_mw_per_ma = 0.166f,
    .dlambda_dA_nm_per_ma = 0.003f,
    .dlambda_dT_nm_per_k = 0.08f,
    .operating_temp_range_c = {17.0f, 38.0f},
    .operating_temp_c = 25.0f,
    .thermistor_kohm = 10.0f,
    .isolation_db = 25.0f,
    .ntc_t_coefficient_per_c = LASERPROP_NA
};

static const laserprops_t LASER_1510 = {
    .name = "1510",
    .model_number = "1510LD-1-0-0",
    .threshold_current_ma = 8.0f,
    .nominal_current_ma = 60.0f,
    .max_current_ma = 60.0f,
    .dne_current_ma = 72.0f,
    .tec_max_current_a = 1.0f,
    .tec_pid = TEC_PID_DEFAULT,
    .wavelength_nm = 1510.0f,
    .test_monitor_current_ua = LASERPROP_NA,
    .efficiency_mw_per_ma = 0.166f,
    .dlambda_dA_nm_per_ma = 0.003f,
    .dlambda_dT_nm_per_k = 0.08f,
    .operating_temp_range_c = {17.0f, 38.0f},
    .operating_temp_c = 25.0f,
    .thermistor_kohm = 10.0f,
    .isolation_db = 25.0f,
    .ntc_t_coefficient_per_c = LASERPROP_NA
};

#endif // LASER_PROPERTIES_H
