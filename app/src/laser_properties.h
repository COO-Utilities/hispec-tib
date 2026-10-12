/**
 * @file laser_properties.h
 * @brief Fixed diode property table used for estimates and safety limits.
 *
 * NTC defaults match internal LD register B=3988 K on all six drivers, read
 * 2026-10-09. These are controller settings, not measured thermistor slopes.
 * App-owned coefficients are translated to internal LD Maiman B
 * during existing profile preparation, without boot-time programming.
 * These values are compile-time defaults. Maiman EEPROM owns driver-side
 * persisted parameters; app settings persist accepted property overrides and
 * optical-power uncertainty. Noise-floor defaults always use this fixed table.
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
    const char *serial_number;
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
    double tec_curr_lim_a;
    tec_pid_t tec_pid;
    double ntc_coeff_pct_per_c; /* Resistance coefficient, %/C at 25 C. */
    double dlambda_dT_nm_per_k;
    double dlambda_dA_nm_per_ma;
} laserprops_t;

#define LASERPROP_NA (NAN)

static const tec_pid_t TEC_PID_DEFAULT = {100, 1000, 0};
static const tec_pid_t TEC_PID_DFB = {20, 1000, 1000};

static const laserprops_t LASER_1028 = {
    .name = "1028",
    .model_number = "FLPD-1028-50-DFB-BTF",
    .serial_number = "BF69016",
    .threshold_current_ma = 14.5,
    .nominal_current_ma = 250.0,
    .max_current_ma = 250.0,
    .dne_current_ma = 275.0,
    .tec_curr_lim_a = 1.2,
    .tec_pid = TEC_PID_DFB,
    .wavelength_nm = 1028.01,
    .test_monitor_current_ua = 23.3,
    .efficiency_mw_per_ma = 0.186,
    .dlambda_dA_nm_per_ma = 0.015,
    .dlambda_dT_nm_per_k = 0.12,
    .operating_temp_range_c = {17.0, 38.0},
    .operating_temp_c = 25.0,
    .thermistor_kohm = 10.0,
    .isolation_db = 30.0,
    // sheet -4.4 (%/C inferred, printed 1/C). Test report has no slope measurement.
    .ntc_coeff_pct_per_c = -4.4,
};

static const laserprops_t LASER_2330 = {
    .name = "2330",
    .model_number = "FLPD-2330-03-DFB-BTF",
    .serial_number = "D4520009",
    .threshold_current_ma = 24.9,
    .nominal_current_ma = 120.0,
    .max_current_ma = 120.0,
    .dne_current_ma = 137.0,
    .tec_curr_lim_a = 1.2,
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
    // sheet -4.4 (%/C inferred, printed 1/C). Test report has no slope measurement.
    .ntc_coeff_pct_per_c = -4.4,
};

static const laserprops_t LASER_1270 = {
    .name = "1270",
    .model_number = "1270LD-1-0-0",
    .serial_number = "8881AFR",
    .threshold_current_ma = 13.0,
    .nominal_current_ma = 52.0,
    .max_current_ma = 65.0,
    .dne_current_ma = 75.0,
    .tec_curr_lim_a = 1.0,
    .tec_pid = TEC_PID_DEFAULT,
    .wavelength_nm = 1270.52,
    .test_monitor_current_ua = 90,
    .efficiency_mw_per_ma = 0.26,
    .dlambda_dA_nm_per_ma = 0.003,
    .dlambda_dT_nm_per_k = 0.08,
    .operating_temp_range_c = {17.0, 38.0},
    .operating_temp_c = 25.0,
    .thermistor_kohm = 10.0,
    .isolation_db = 25.0,
    // Internal B=3900 K on both 1430 drivers; per email communication
    .ntc_coeff_pct_per_c = -100.0 * 3900.0 / (298.15*298.15)
};

static const laserprops_t LASER_1430YJ = {
    .name = "1430",
    .model_number = "1430LD-1-0-0",
    .serial_number = "8897ACR",
    .threshold_current_ma = 8.0, //8897ACR=8mA 8895ACR=7mA
    .nominal_current_ma = 49.0,  //8897ACR=8mA 8895ACR=7mA
    .max_current_ma = 60.0,
    .dne_current_ma = 72.0,
    .tec_curr_lim_a = 1.0,
    .tec_pid = TEC_PID_DEFAULT,
    .wavelength_nm = 1429.64, //8897ACR=1429.64 8895ACR=1429.56
    .test_monitor_current_ua = 143, //8897ACR=143 8895ACR=159
    .efficiency_mw_per_ma = 0.24,
    .dlambda_dA_nm_per_ma = 0.003,
    .dlambda_dT_nm_per_k = 0.08,
    .operating_temp_range_c = {17.0, 38.0},
    .operating_temp_c = 25.0,
    .thermistor_kohm = 10.0,
    .isolation_db = 25.0,
    // Internal B=3900 K on both 1430 drivers; per email communication
    .ntc_coeff_pct_per_c = -100.0 * 3900.0 / (298.15*298.15)
};

static const laserprops_t LASER_1430HK = {
    .name = "1430",
    .model_number = "1430LD-1-0-0",
    .serial_number = "8895ACR",
    .threshold_current_ma = 7.0, //8897ACR=8mA 8895ACR=7mA
    .nominal_current_ma = 49,
    .max_current_ma = 60.0,
    .dne_current_ma = 72.0,
    .tec_curr_lim_a = 1.0,
    .tec_pid = TEC_PID_DEFAULT,
    .wavelength_nm = 1429.56, //8897ACR=1429.64 8895ACR=1429.56
    .test_monitor_current_ua = 159, //8897ACR=143 8895ACR=159
    .efficiency_mw_per_ma = 0.24,
    .dlambda_dA_nm_per_ma = 0.003,
    .dlambda_dT_nm_per_k = 0.08,
    .operating_temp_range_c = {17.0, 38.0},
    .operating_temp_c = 25.0,
    .thermistor_kohm = 10.0,
    .isolation_db = 25.0,
    // Internal B=3900 K on both 1430 drivers; per email communication
    .ntc_coeff_pct_per_c = -100.0 * 3900.0 / (298.15*298.15)
};

static const laserprops_t LASER_1510 = {
    .name = "1510",
    .model_number = "1510LD-1-0-0",
    .serial_number = "8882ACT",
    .threshold_current_ma = 6.0,
    .nominal_current_ma = 47.0,
    .max_current_ma = 60.0,
    .dne_current_ma = 72.0,
    .tec_curr_lim_a = 1.0,
    .tec_pid = TEC_PID_DEFAULT,
    .wavelength_nm = 1509.33,
    .test_monitor_current_ua = 475,
    .efficiency_mw_per_ma = 0.24,
    .dlambda_dA_nm_per_ma = 0.003,
    .dlambda_dT_nm_per_k = 0.08,
    .operating_temp_range_c = {17.0, 38.0},
    .operating_temp_c = 25.0,
    .thermistor_kohm = 10.0,
    .isolation_db = 25.0,
    // Internal B=3900 K on both 1430 drivers; per email communication
    .ntc_coeff_pct_per_c = -100.0 * 3900.0 / (298.15*298.15)
};

#endif // LASER_PROPERTIES_H
