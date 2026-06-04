/**
 * @file laser_properties.h
 * @brief Fixed diode property table used for estimates and safety limits.
 *
 * These values are compile-time defaults. Maiman EEPROM owns driver-side
 * persisted parameters; app settings do not currently persist laser properties.
 */

#ifndef LASER_PROPERTIES_H
#define LASER_PROPERTIES_H

#include <math.h>
#include <stdint.h>

typedef struct {
    double min_c;
    double max_c;
} temp_range_c_t;

typedef struct {
    uint16_t kp;
    uint16_t ki;
    uint16_t kd;
} tec_pid_t;

typedef struct laserprops_t {
    const char *name;
    const char *model_number;
    double nominal_current_ma;
    double max_current_ma;
    double dne_current_ma;
    double threshold_current_ma;
    double efficiency_mw_per_ma;
    double wavelength_nm;
    double test_monitor_current_ua;
    temp_range_c_t operating_temp_range_c;
    double operating_temp_c;
    double thermistor_kohm;
    double isolation_db;
    double tec_max_current_a;
    tec_pid_t tec_pid;
    double ntc_t_coefficient_per_c;
    double dlambda_dT_nm_per_k;
    double dlambda_dA_nm_per_ma;
} laserprops_t;

#define LASERPROP_NA (NAN)

static const tec_pid_t TEC_PID_DEFAULT = {100, 1000, 0};
static const tec_pid_t TEC_PID_DFB = {20, 1000, 1000};

static const laserprops_t LASER_1028 = {
    .name = "1028",
    .model_number = "FLPD-1028-50-DFB-BTF",
    .threshold_current_ma = 14.5,
    .nominal_current_ma = 250.0,
    .max_current_ma = 250.0,
    .dne_current_ma = 275.0,
    .tec_max_current_a = 1.2,
    .tec_pid = TEC_PID_DFB,
    .wavelength_nm = 1028.01,
    .test_monitor_current_ua = 23.3,
    .efficiency_mw_per_ma = 0.185,
    .dlambda_dA_nm_per_ma = 0.015,
    .dlambda_dT_nm_per_k = 0.12,
    .operating_temp_range_c = {17.0, 38.0},
    .operating_temp_c = 25.0,
    .thermistor_kohm = 10.0,
    .isolation_db = 30.0,
    .ntc_t_coefficient_per_c = -0.044
};

static const laserprops_t LASER_2330 = {
    .name = "2330",
    .model_number = "FLPD-2330-03-DFB-BTF",
    .threshold_current_ma = 24.9,
    .nominal_current_ma = 120.0,
    .max_current_ma = 120.0,
    .dne_current_ma = 137.0,
    .tec_max_current_a = 1.2,
    .tec_pid = TEC_PID_DFB,
    .wavelength_nm = 2329.81,
    .test_monitor_current_ua = LASERPROP_NA,
    .efficiency_mw_per_ma = 0.031,
    .dlambda_dA_nm_per_ma = 0.015,
    .dlambda_dT_nm_per_k = 0.12,
    .operating_temp_range_c = {17.0, 38.0},
    .operating_temp_c = 25.0,
    .thermistor_kohm = 10.0,
    .isolation_db = 30.0,
    .ntc_t_coefficient_per_c = -0.044
};

static const laserprops_t LASER_1270 = {
    .name = "1270",
    .model_number = "1270LD-1-0-0",
    .threshold_current_ma = 8.0,
    .nominal_current_ma = 60.0,
    .max_current_ma = 60.0,
    .dne_current_ma = 75.0,
    .tec_max_current_a = 1.0,
    .tec_pid = TEC_PID_DEFAULT,
    .wavelength_nm = 1270.0,
    .test_monitor_current_ua = LASERPROP_NA,
    .efficiency_mw_per_ma = 0.166,
    .dlambda_dA_nm_per_ma = 0.003,
    .dlambda_dT_nm_per_k = 0.08,
    .operating_temp_range_c = {17.0, 38.0},
    .operating_temp_c = 25.0,
    .thermistor_kohm = 10.0,
    .isolation_db = 25.0,
    .ntc_t_coefficient_per_c = LASERPROP_NA
};

static const laserprops_t LASER_1430 = {
    .name = "1430",
    .model_number = "1430LD-1-0-0",
    .threshold_current_ma = 8.0,
    .nominal_current_ma = 60.0,
    .max_current_ma = 60.0,
    .dne_current_ma = 72.0,
    .tec_max_current_a = 1.0,
    .tec_pid = TEC_PID_DEFAULT,
    .wavelength_nm = 1430.0,
    .test_monitor_current_ua = LASERPROP_NA,
    .efficiency_mw_per_ma = 0.166,
    .dlambda_dA_nm_per_ma = 0.003,
    .dlambda_dT_nm_per_k = 0.08,
    .operating_temp_range_c = {17.0, 38.0},
    .operating_temp_c = 25.0,
    .thermistor_kohm = 10.0,
    .isolation_db = 25.0,
    .ntc_t_coefficient_per_c = LASERPROP_NA
};

static const laserprops_t LASER_1510 = {
    .name = "1510",
    .model_number = "1510LD-1-0-0",
    .threshold_current_ma = 8.0,
    .nominal_current_ma = 60.0,
    .max_current_ma = 60.0,
    .dne_current_ma = 72.0,
    .tec_max_current_a = 1.0,
    .tec_pid = TEC_PID_DEFAULT,
    .wavelength_nm = 1510.0,
    .test_monitor_current_ua = LASERPROP_NA,
    .efficiency_mw_per_ma = 0.166,
    .dlambda_dA_nm_per_ma = 0.003,
    .dlambda_dT_nm_per_k = 0.08,
    .operating_temp_range_c = {17.0, 38.0},
    .operating_temp_c = 25.0,
    .thermistor_kohm = 10.0,
    .isolation_db = 25.0,
    .ntc_t_coefficient_per_c = LASERPROP_NA
};

#endif // LASER_PROPERTIES_H
