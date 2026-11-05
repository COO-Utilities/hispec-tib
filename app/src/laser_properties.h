/**
 * @file laser_properties.h
 * @brief Fixed diode property table used for estimates and safety limits.
 *
 * Unit-specific diode values come from manufacturer reports. Wavelengths are
 * referenced to nominal current and operating temperature; inferred values are
 * identified locally. NTC defaults use vendor thermistor specifications.
 * App-owned coefficients are translated to internal LD Maiman B
 * during existing profile preparation, without boot-time programming.
 * These values are compile-time defaults. Maiman EEPROM owns driver-side
 * persisted parameters; app settings persist accepted property overrides and
 * optical-power uncertainty. Noise defaults are separate in lasers.h.
 * Power defaults fit only the digitized vendor traces through ref_ma, with
 * current-spacing weights and ref_mw fixed at ref_ma (November 4, 2026).
 * Threshold is the fitted extrapolated zero, not a measured turn-on current.
 * Reproduction and residuals: commissioning notebook and laser_calibration.md.
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
    const char *ld_serial; /* Manufacturer diode serial, distinct from Maiman driver serial. */
    /* P = x*(efficiency + q*(I-efficiency_ref_ma)), x=I-threshold.
     * Calibration and compiled defaults anchor efficiency at ref_ma; the
     * correction is zero there, preserving the independent vendor power scale.
     * Placing the float here uses the MCU's
     * alignment gap before the doubles instead of enlarging every profile. */
    float efficiency_mw_per_ma2;
    double nominal_current_ma;
    double max_current_ma;
    double dne_current_ma;
    double threshold_current_ma;
    double efficiency_mw_per_ma;
    double efficiency_ref_ma; /* Curve coordinate, not a commanded current. */
    /* Independent vendor anchor for calibration; never used by the active curve. */
    double ref_mw;
    double ref_ma;
    double ref_temp_c;
    /* Independent wavelength measurement; these coordinates are not setpoints.
     * In particular, the 2330 report uses 40 C outside its operating envelope. */
    double wave_ref_nm;
    double wave_ref_ma;
    double wave_ref_temp_c;
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
    .ld_serial = "BF69016",
    .threshold_current_ma = 16.6083487737,
    .nominal_current_ma = 250.0,
    .max_current_ma = 250.0,
    .dne_current_ma = 275.0,
    .tec_curr_lim_a = 1.2,
    .tec_pid = TEC_PID_DFB,
    // BF69016 test report: wavelength measured at 20 C and 125 mA.
    .wave_ref_nm = 1028.01,
    .wave_ref_ma = 125.0,
    .wave_ref_temp_c = 20.0,
    .test_monitor_current_ua = 23.3,
    .efficiency_mw_per_ma = 0.1876673812875,
    .efficiency_mw_per_ma2 = -6.06411690252e-5f,
    .ref_mw = 43.8,
    .ref_ma = 250,
    .efficiency_ref_ma = 250,
    .ref_temp_c = 25.0,
    .dlambda_dA_nm_per_ma = 0.015,
    .dlambda_dT_nm_per_k = 0.12,
    .operating_temp_range_c = {17.0, 38.0},
    .operating_temp_c = 25.0,
    .thermistor_kohm = 10.0,
    .isolation_db = 30.0,
    // Sheet -4.4 %/C. Test report has no thermistor slope measurement.
    .ntc_coeff_pct_per_c = -4.4,
};

static const laserprops_t LASER_2330 = {
    .name = "2330",
    .model_number = "FLPD-2330-03-DFB-BTF",
    .ld_serial = "D4520009",
    .threshold_current_ma = 20.60959567841,
    .nominal_current_ma = 120.0,
    .max_current_ma = 120.0,
    .dne_current_ma = 137.0,
    .tec_curr_lim_a = 1.2,
    .tec_pid = TEC_PID_DFB,
    // D4520009 test report: wavelength measured at 40 C and 100 mA.
    .wave_ref_nm = 2329.81,
    .wave_ref_ma = 100.0,
    .wave_ref_temp_c = 40.0,
    .test_monitor_current_ua = LASERPROP_NA,
    .efficiency_mw_per_ma = 0.03199504038348,
    .efficiency_mw_per_ma2 = -8.911153979743e-5f,
    .ref_mw = 3.18,
    .ref_ma = 120,
    .efficiency_ref_ma = 120,
    .ref_temp_c = 25.0,
    .dlambda_dA_nm_per_ma = 0.015,
    .dlambda_dT_nm_per_k = 0.12,
    .operating_temp_range_c = {17.0, 38.0},
    .operating_temp_c = 25.0,
    .thermistor_kohm = 10.0,
    .isolation_db = 30.0,
    // Sheet -4.4 (%/C inferred, printed 1/C). Test report has no thermistor slope measurement.
    .ntc_coeff_pct_per_c = -4.4,
};

static const laserprops_t LASER_1270 = {
    .name = "1270",
    .model_number = "1270LD-1-0-0-1",
    .ld_serial = "8881AFR",
    .threshold_current_ma = 9.784031814486,
    .nominal_current_ma = 52.0,
    .max_current_ma = 65.0,
    .dne_current_ma = 75.0,
    .tec_curr_lim_a = 1.0,
    .tec_pid = TEC_PID_DEFAULT,
    .wave_ref_nm = 1270.52,
    .wave_ref_ma = 52.0,
    .wave_ref_temp_c = 25.0,
    .test_monitor_current_ua = 90,
    .efficiency_mw_per_ma = 0.2368771919681,
    .efficiency_mw_per_ma2 = -4.188985980908e-5f,
    .ref_mw = 10,
    .ref_ma = 52,
    .efficiency_ref_ma = 52,
    .ref_temp_c = 25.0,
    .dlambda_dA_nm_per_ma = 0.003,
    .dlambda_dT_nm_per_k = 0.08,
    .operating_temp_range_c = {17.0, 38.0},
    .operating_temp_c = 25.0,
    .thermistor_kohm = 10.0,
    .isolation_db = 25.0,
    // Vendor thermistor B=3900 K; resistance coefficient at 25 C.
    .ntc_coeff_pct_per_c = -100.0 * 3900.0 / (298.15 * 298.15),
};

static const laserprops_t LASER_1430YJ = {
    .name = "1430yj",
    .model_number = "1430LD-1-0-0-1",
    .ld_serial = "8897ACR",
    .threshold_current_ma = 3.592432600386, // Fitted zero; report threshold: 8897ACR=8mA, 8895ACR=7mA.
    .nominal_current_ma = 49.0, // 10 mW at 25 C in the unit report.
    .max_current_ma = 60.0,
    .dne_current_ma = 72.0,
    .tec_curr_lim_a = 1.0,
    .tec_pid = TEC_PID_DEFAULT,
    .wave_ref_nm = 1429.64, //8897ACR=1429.64 8895ACR=1429.56
    .wave_ref_ma = 49.0,
    .wave_ref_temp_c = 25.0,
    .test_monitor_current_ua = 143, //8897ACR=143 8895ACR=159
    .efficiency_mw_per_ma = 0.2202276090237,
    .efficiency_mw_per_ma2 = -7.876910936396e-5f,
    .ref_mw = 10,
    .ref_ma = 49,
    .efficiency_ref_ma = 49,
    .ref_temp_c = 25.0,
    .dlambda_dA_nm_per_ma = 0.003,
    .dlambda_dT_nm_per_k = 0.08,
    .operating_temp_range_c = {17.0, 38.0},
    .operating_temp_c = 25.0,
    .thermistor_kohm = 10.0,
    .isolation_db = 25.0,
    // Vendor thermistor B=3900 K; resistance coefficient at 25 C.
    .ntc_coeff_pct_per_c = -100.0 * 3900.0 / (298.15 * 298.15),
};

static const laserprops_t LASER_1430HK = {
    .name = "1430hk",
    .model_number = "1430LD-1-0-0-1",
    .ld_serial = "8895ACR",
    .threshold_current_ma = 3.43502055322, // Fitted zero; report threshold: 8897ACR=8mA, 8895ACR=7mA.
    .nominal_current_ma = 49,
    .max_current_ma = 60.0,
    .dne_current_ma = 72.0,
    .tec_curr_lim_a = 1.0,
    .tec_pid = TEC_PID_DEFAULT,
    .wave_ref_nm = 1429.56, //8897ACR=1429.64 8895ACR=1429.56
    .wave_ref_ma = 49.0,
    .wave_ref_temp_c = 25.0,
    .test_monitor_current_ua = 159, //8897ACR=143 8895ACR=159
    .efficiency_mw_per_ma = 0.2194667949248,
    .efficiency_mw_per_ma2 = -2.581978567103e-4f,
    .ref_mw = 10,
    .ref_ma = 49,
    .efficiency_ref_ma = 49,
    .ref_temp_c = 25.0,
    .dlambda_dA_nm_per_ma = 0.003,
    .dlambda_dT_nm_per_k = 0.08,
    .operating_temp_range_c = {17.0, 38.0},
    .operating_temp_c = 25.0,
    .thermistor_kohm = 10.0,
    .isolation_db = 25.0,
    // Vendor thermistor B=3900 K; resistance coefficient at 25 C.
    .ntc_coeff_pct_per_c = -100.0 * 3900.0 / (298.15 * 298.15),
};

static const laserprops_t LASER_1510 = {
    .name = "1510",
    .model_number = "1510LD-1-0-0-1",
    .ld_serial = "8882ACT",
    .threshold_current_ma = 4.095546033222,
    .nominal_current_ma = 47.0,
    .max_current_ma = 60.0,
    .dne_current_ma = 72.0,
    .tec_curr_lim_a = 1.0,
    .tec_pid = TEC_PID_DEFAULT,
    .wave_ref_nm = 1509.33,
    .wave_ref_ma = 47.0,
    .wave_ref_temp_c = 25.0,
    .test_monitor_current_ua = 475,
    .efficiency_mw_per_ma = 0.233076034664,
    .efficiency_mw_per_ma2 = -2.424936267747e-4f,
    .ref_mw = 10,
    .ref_ma = 47,
    .efficiency_ref_ma = 47,
    .ref_temp_c = 25.0,
    .dlambda_dA_nm_per_ma = 0.003,
    .dlambda_dT_nm_per_k = 0.08,
    .operating_temp_range_c = {17.0, 38.0},
    .operating_temp_c = 25.0,
    .thermistor_kohm = 10.0,
    .isolation_db = 25.0,
    // Vendor thermistor B=3900 K; resistance coefficient at 25 C.
    .ntc_coeff_pct_per_c = -100.0 * 3900.0 / (298.15 * 298.15),
};

#endif // LASER_PROPERTIES_H
