/**
 * @file attenuator.c
 * @brief DAC write/read helpers for paired physical attenuators.
 */

#include "attenuator.h"
#include "command.h"
#include "drivers/dac/dac7x78.h"

#include <errno.h>
#include <math.h>
#include <zephyr/sys/util.h>
#include <zsl/probability.h>
#include <zsl/zsl.h>

LOG_MODULE_REGISTER(attenuator, LOG_LEVEL_INF);

#define DAC_RESOLUTION_BITS 12
#define DAC_MAX_CODE        ((1 << DAC_RESOLUTION_BITS) - 1)
#define MODEL_ERF_SCALE     4.0
#define MODEL_MAX_DB        120.0
#define MODEL_MIN_TX        1.0e-300
#define ATTENUATOR_DB_EPSILON 1.0e-6
#define ATTENUATOR_VOLTAGE_WARN_EPS_MV 0.5
#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

static double attenuator_model_voltage_to_delta(const struct attenuator_model_coeffs *coeffs,
                                                double voltage)
{
    double fvoa_drive_mv = coeffs->gain * voltage;

    return coeffs->slope_inv_fvoa_mv * (fvoa_drive_mv - coeffs->fvoa_50pct_mv);
}

static double attenuator_model_raw_linear(const struct attenuator_model_coeffs *coeffs,
                                          double voltage)
{
    double transmission;

    if (coeffs == NULL) {
        return 0.0;
    }

    transmission = attenuator_model_b_to_linear(
        attenuator_model_voltage_to_delta(coeffs, voltage));
    if (!isfinite(transmission) || transmission <= 0.0) {
        return 0.0;
    }
    if (transmission > 1.0) {
        return 1.0;
    }
    return transmission;
}

static double attenuator_model_open_linear(const struct attenuator_model_coeffs *coeffs)
{
    return attenuator_model_raw_linear(coeffs, 0.0);
}

static double attenuator_model_relative_linear(const struct attenuator_model_coeffs *coeffs,
                                               double voltage)
{
    double open_tx = attenuator_model_open_linear(coeffs);
    double tx = attenuator_model_raw_linear(coeffs, voltage);
    double relative;

    if (!(open_tx > MODEL_MIN_TX) || !isfinite(open_tx)) {
        return 0.0;
    }

    relative = tx / open_tx;
    if (!isfinite(relative) || relative <= 0.0) {
        return 0.0;
    }
    if (relative > 1.0) {
        return 1.0;
    }
    return relative;
}

int attenuator_index_from_laser_id(enum hispec_laser_id laser, uint8_t *index)
{
    if (index == NULL || laser < 0 || laser >= HISPEC_LASER_COUNT) {
        return -EINVAL;
    }

    *index = (uint8_t)laser;
    return 0;
}

static void attenuator_cfg_init(struct attenuator_dac_cfg *dac_cfg,
                                const struct device *dev, uint8_t channel)
{
    dac_cfg->dev = dev;
    dac_cfg->ideal_full_scale_mv = ATTENUATOR_DRIVE_MAX_MV;
    dac_cfg->drive_limit_mv = ATTENUATOR_DRIVE_MAX_MV;
    dac_cfg->voltage = 0.0;
    dac_cfg->attenuation_db = 0.0;
    dac_cfg->cfg.channel_id = channel;
    dac_cfg->cfg.resolution = DAC_RESOLUTION_BITS;
#if defined(CONFIG_DAC_BUFFER_NOT_SUPPORT)
    dac_cfg->cfg.buffered = false;
#else
    dac_cfg->cfg.buffered = true;
#endif
}

static bool attenuator_channel_setup(struct attenuator_dac_cfg *dac_cfg)
{
    struct dac7x78_transfer transfer;
    int err;

    if (dac_cfg->dev == NULL || !device_is_ready(dac_cfg->dev)) {
        LOG_ERR("DAC device unavailable for channel %u",
                dac_cfg->cfg.channel_id);
        return false;
    }

    err = dac7x78_get_transfer(dac_cfg->dev, &transfer);
    if (err != 0 || transfer.ideal_full_scale_uv == 0U ||
        transfer.output_limit_uv == 0U) {
        LOG_ERR("DAC transfer setup failed for channel %u: %d",
                dac_cfg->cfg.channel_id, err);
        return false;
    }
    dac_cfg->ideal_full_scale_mv =
        (double)transfer.ideal_full_scale_uv / 1000.0;
    dac_cfg->drive_limit_mv =
        MIN(ATTENUATOR_DRIVE_MAX_MV,
            (double)transfer.output_limit_uv / 1000.0);

    /* dac_channel_setup() prepares the selected channel in the DAC driver and
     * may perform I2C transactions depending on the underlying implementation.
     */
    err = dac_channel_setup(dac_cfg->dev, &dac_cfg->cfg);
    if (err != 0) {
        LOG_ERR("DAC channel setup failed: %d", err);
        return false;
    }

    return true;
}

bool attenuator_init(struct attenuator *drv,
                     const struct device *dac1, uint8_t channel1,
                     const struct device *dac2, uint8_t channel2)
{
    if (drv == NULL) {
        return false;
    }

    attenuator_cfg_init(&drv->dac_cfg1, dac1, channel1);
    attenuator_cfg_init(&drv->dac_cfg2, dac2, channel2);
    drv->coeff1.fvoa_50pct_mv = 0.5 * ATTENUATOR_DRIVE_MAX_MV * ATTENUATOR_DEFAULT_GAIN;
    drv->coeff1.slope_inv_fvoa_mv = 8.0 / (ATTENUATOR_DRIVE_MAX_MV * ATTENUATOR_DEFAULT_GAIN);
    drv->coeff1.gain = ATTENUATOR_DEFAULT_GAIN;
    drv->coeff2.fvoa_50pct_mv = 0.5 * ATTENUATOR_DRIVE_MAX_MV * ATTENUATOR_DEFAULT_GAIN;
    drv->coeff2.slope_inv_fvoa_mv = 8.0 / (ATTENUATOR_DRIVE_MAX_MV * ATTENUATOR_DEFAULT_GAIN);
    drv->coeff2.gain = ATTENUATOR_DEFAULT_GAIN;
    drv->attenuation_db = 0.0;

    return attenuator_channel_setup(&drv->dac_cfg1) &&
           attenuator_channel_setup(&drv->dac_cfg2);
}

double attenuator_model_voltage_to_db(const struct attenuator_model_coeffs *coeffs,
                                      double voltage)
{
    double transmission;

    if (coeffs == NULL) {
        return 0.0;
    }

    transmission = attenuator_model_relative_linear(coeffs, voltage);

    if (transmission <= 0.0) {
        return MODEL_MAX_DB;
    }
    if (transmission >= 1.0) {
        return 0.0;
    }

    return -10.0 * (double)ZSL_LOG10((zsl_real_t)transmission);
}

bool attenuator_model_db_to_voltage(const struct attenuator_model_coeffs *coeffs,
                                    double attenuation_db, double *voltage)
{
    const zsl_real_t erf_scale = ZSL_ERF((zsl_real_t)MODEL_ERF_SCALE);
    double open_tx;
    double target_tx;
    zsl_real_t erf_arg;
    zsl_real_t inv;
    double delta;

    if (coeffs == NULL || voltage == NULL || coeffs->slope_inv_fvoa_mv == 0.0 ||
        coeffs->gain <= 0.0 ||
        attenuation_db < 0.0) {
        return false;
    }

    if (attenuation_db <= ATTENUATOR_DB_EPSILON) {
        *voltage = 0.0;
        return true;
    }

    open_tx = attenuator_model_open_linear(coeffs);
    if (!(open_tx > MODEL_MIN_TX) || !isfinite(open_tx)) {
        return false;
    }

    target_tx = open_tx * pow(10.0, -attenuation_db / 10.0);
    if (!(target_tx > MODEL_MIN_TX) || !isfinite(target_tx)) {
        return false;
    }
    if (target_tx > 1.0) {
        target_tx = 1.0;
    }

    erf_arg = erf_scale - ((zsl_real_t)2.0 * erf_scale * (zsl_real_t)target_tx);
    if (erf_arg <= (zsl_real_t)-1.0 || erf_arg >= (zsl_real_t)1.0) {
        return false;
    }

    inv = zsl_prob_erf_inv(&erf_arg);
    delta = (double)inv;
    *voltage = ((delta / coeffs->slope_inv_fvoa_mv) +
                coeffs->fvoa_50pct_mv) / coeffs->gain;

    return true;
}

static bool attenuator_model_coeff_valid(const struct attenuator_model_coeffs *coeffs)
{
    double max_db;

    if (coeffs == NULL || !isfinite(coeffs->fvoa_50pct_mv) ||
        !isfinite(coeffs->slope_inv_fvoa_mv) || !isfinite(coeffs->gain) ||
        coeffs->slope_inv_fvoa_mv <= 0.0 || coeffs->gain <= 0.0) {
        return false;
    }

    max_db = attenuator_model_voltage_to_db(coeffs, ATTENUATOR_DRIVE_MAX_MV);
    return isfinite(max_db) && max_db > ATTENUATOR_DB_EPSILON;
}

bool attenuator_model_coefficients_valid(
    const struct attenuator_model_coeffs physical[ATTENUATOR_PHYSICAL_COUNT])
{
    if (physical == NULL) {
        return false;
    }

    for (uint8_t i = 0U; i < ATTENUATOR_PHYSICAL_COUNT; ++i) {
        if (!attenuator_model_coeff_valid(&physical[i])) {
            return false;
        }
    }

    return true;
}

static double attenuator_drive_limit_mv(const struct attenuator_dac_cfg *dac_cfg)
{
    if (dac_cfg == NULL || dac_cfg->drive_limit_mv <= 0.0) {
        return ATTENUATOR_DRIVE_MAX_MV;
    }

    return dac_cfg->drive_limit_mv;
}

static double attenuator_code_to_voltage(const struct attenuator_dac_cfg *dac_cfg,
                                         uint32_t code)
{
    double voltage;
    double drive_limit = attenuator_drive_limit_mv(dac_cfg);

    if (dac_cfg == NULL || dac_cfg->ideal_full_scale_mv <= 0.0) {
        return 0.0;
    }

    voltage = (dac_cfg->ideal_full_scale_mv /
               (double)(1 << DAC_RESOLUTION_BITS)) * (double)code;
    if (voltage > drive_limit) {
        return drive_limit;
    }

    return voltage;
}

static uint32_t attenuator_voltage_to_code(const struct attenuator_dac_cfg *dac_cfg,
                                           double voltage)
{
    double ideal_full_scale_mv;
    uint32_t code;

    if (dac_cfg == NULL || dac_cfg->ideal_full_scale_mv <= 0.0) {
        return 0U;
    }

    ideal_full_scale_mv = dac_cfg->ideal_full_scale_mv;
    code = (uint32_t)(((voltage / ideal_full_scale_mv) *
                       (double)(1 << DAC_RESOLUTION_BITS)) + 0.5);

    if (code > DAC_MAX_CODE) {
        return DAC_MAX_CODE;
    }

    return code;
}

static bool attenuator_write_voltage(struct attenuator_dac_cfg *dac_cfg,
                                     const struct attenuator_model_coeffs *coeffs,
                                     double voltage)
{
    double unclamped_voltage = voltage;
    bool report_clamp = false;
    uint32_t code;
    double applied_voltage;
    double drive_limit;
    int err;
    char context[64];

    if (dac_cfg == NULL || coeffs == NULL || dac_cfg->dev == NULL) {
        return false;
    }

    drive_limit = attenuator_drive_limit_mv(dac_cfg);
    if (voltage < 0.0) {
        report_clamp = voltage < -ATTENUATOR_VOLTAGE_WARN_EPS_MV;
        voltage = 0.0;
    } else if (voltage > drive_limit) {
        report_clamp = voltage > drive_limit + ATTENUATOR_VOLTAGE_WARN_EPS_MV;
        voltage = drive_limit;
    }

	if (report_clamp) {
		snprintk(context, sizeof(context),
			 "channel=%u requested=%.3f clamped=%.3f",
			 dac_cfg->cfg.channel_id, unclamped_voltage, voltage);
		coo_cmd_runtime_emit(command_runtime_get(),
				     &(const struct coo_cmd_runtime_emit_args){
					     .type = COO_CMD_RUNTIME_EMIT_WARNING,
					     .delivery = COO_CMD_RUNTIME_EMIT_BEST_EFFORT,
					     .code = "attenuator_clamped",
					     .msg = "attenuator command exceeded drive range and was clamped",
					     .context = context,
				     });
	}

    code = attenuator_voltage_to_code(dac_cfg, voltage);
    applied_voltage = attenuator_code_to_voltage(dac_cfg, code);
    /* dac_write_value() is the hardware side effect: it can block on I2C and
     * changes the DAC output voltage that feeds the FVOA-drive op amp.
     */
    err = dac_write_value(dac_cfg->dev, dac_cfg->cfg.channel_id, code);
    if (err != 0) {
        LOG_ERR("DAC write failed: %d", err);
        return false;
    }

    dac_cfg->voltage = applied_voltage;
    dac_cfg->attenuation_db = attenuator_model_voltage_to_db(coeffs, applied_voltage);

    return true;
}

static bool attenuator_set_physical_db(struct attenuator_dac_cfg *dac_cfg,
                                       const struct attenuator_model_coeffs *coeffs,
                                       double attenuation_db)
{
    double voltage;
    double max_db;

    if (dac_cfg == NULL || coeffs == NULL || attenuation_db < 0.0) {
        return false;
    }

    max_db = attenuator_model_voltage_to_db(coeffs,
                                            attenuator_drive_limit_mv(dac_cfg));
    if (attenuation_db <= ATTENUATOR_DB_EPSILON) {
        return attenuator_write_voltage(dac_cfg, coeffs, 0.0);
    }
    if (max_db > 0.0 && attenuation_db >= max_db - ATTENUATOR_DB_EPSILON) {
        return attenuator_write_voltage(dac_cfg, coeffs,
                                        attenuator_drive_limit_mv(dac_cfg));
    }

    if (!attenuator_model_db_to_voltage(coeffs, attenuation_db, &voltage)) {
        return false;
    }

    return attenuator_write_voltage(dac_cfg, coeffs, voltage);
}

bool attenuator_set_dac1_db(struct attenuator *drv, double attenuation_db)
{
    return drv != NULL &&
           attenuator_set_physical_db(&drv->dac_cfg1, &drv->coeff1,
                                      attenuation_db);
}

bool attenuator_set_dac2_db(struct attenuator *drv, double attenuation_db)
{
    return drv != NULL &&
           attenuator_set_physical_db(&drv->dac_cfg2, &drv->coeff2,
                                      attenuation_db);
}

bool attenuator_set_dac1_voltage(struct attenuator *drv, double voltage)
{
    return drv != NULL &&
           attenuator_write_voltage(&drv->dac_cfg1, &drv->coeff1, voltage);
}

bool attenuator_set_dac2_voltage(struct attenuator *drv, double voltage)
{
    return drv != NULL &&
           attenuator_write_voltage(&drv->dac_cfg2, &drv->coeff2, voltage);
}

bool attenuator_set_physical_voltage(struct attenuator *drv,
                                     uint8_t physical_index,
                                     double voltage)
{
    if (drv == NULL) {
        return false;
    }

    switch (physical_index) {
    case 0:
        return attenuator_set_dac1_voltage(drv, voltage);
    case 1:
        return attenuator_set_dac2_voltage(drv, voltage);
    default:
        return false;
    }
}

static double attenuator_physical_max_db(const struct attenuator_dac_cfg *dac_cfg,
                                         const struct attenuator_model_coeffs *coeffs)
{
    return attenuator_model_voltage_to_db(coeffs,
                                          attenuator_drive_limit_mv(dac_cfg));
}

bool attenuator_set_db(struct attenuator *drv, double attenuation_db)
{
    double max_db1;
    double max_total_db;
    double db1;
    double db2;
    char context[64];

    if (drv == NULL || attenuation_db < 0.0) {
        return false;
    }

    max_db1 = attenuator_physical_max_db(&drv->dac_cfg1, &drv->coeff1);
    max_total_db = max_db1 + attenuator_physical_max_db(&drv->dac_cfg2,
                                                        &drv->coeff2);

	if (attenuation_db > max_total_db) {
		snprintk(context, sizeof(context),
			 "requested=%.3f clamped=%.3f", attenuation_db, max_total_db);
		coo_cmd_runtime_emit(command_runtime_get(),
				     &(const struct coo_cmd_runtime_emit_args){
					     .type = COO_CMD_RUNTIME_EMIT_WARNING,
					     .delivery = COO_CMD_RUNTIME_EMIT_BEST_EFFORT,
					     .code = "attenuator_clamped",
					     .msg = "attenuator command exceeded modeled range and was clamped",
					     .context = context,
				     });
		attenuation_db = max_total_db;
	}

    db1 = attenuation_db < max_db1 ? attenuation_db : max_db1;
    db2 = attenuation_db - db1;
    if (db2 < ATTENUATOR_DB_EPSILON) {
        db2 = 0.0;
    }

    if (!attenuator_set_dac1_db(drv, db1)) {
        return false;
    }
    if (!attenuator_set_dac2_db(drv, db2)) {
        return false;
    }

    drv->attenuation_db = db1 + db2;

    return true;
}

bool attenuator_set_linear(struct attenuator *drv, double linear)
{
    double attenuation_db;

    if (linear <= 0.0 || linear > 1.0) {
        return false;
    }

    attenuation_db = -10.0 * (double)ZSL_LOG10((zsl_real_t)linear);

    return attenuator_set_db(drv, attenuation_db);
}

static bool attenuator_read_physical(struct attenuator_dac_cfg *dac_cfg,
                                     const struct attenuator_model_coeffs *coeffs)
{
    uint32_t code = 0U;
    int err;

    err = dac7x78_read_value(dac_cfg->dev, dac_cfg->cfg.channel_id, &code);
    if (err != 0) {
        LOG_ERR("DAC read failed: %d", err);
        return false;
    }

    dac_cfg->voltage = attenuator_code_to_voltage(dac_cfg, code);
    dac_cfg->attenuation_db =
        attenuator_model_voltage_to_db(coeffs, dac_cfg->voltage);

    return true;
}

bool attenuator_get(struct attenuator *drv, struct attenuator_status *out)
{
    double total_db;

    if (drv == NULL || out == NULL) {
        return false;
    }

    if (!attenuator_read_physical(&drv->dac_cfg1, &drv->coeff1) ||
        !attenuator_read_physical(&drv->dac_cfg2, &drv->coeff2)) {
        return false;
    }

    total_db = drv->dac_cfg1.attenuation_db + drv->dac_cfg2.attenuation_db;
    drv->attenuation_db = total_db;

    out->attenuation_db = total_db;
    out->linear = (double)ZSL_POW((zsl_real_t)10.0,
                                  (zsl_real_t)(-total_db / 10.0));
    out->voltage1 = drv->dac_cfg1.voltage;
    out->voltage2 = drv->dac_cfg2.voltage;
    out->attenuation_db1 = drv->dac_cfg1.attenuation_db;
    out->attenuation_db2 = drv->dac_cfg2.attenuation_db;

    return true;
}

double attenuator_model_b_to_linear(double b)
{
    const zsl_real_t erf_scale = ZSL_ERF((zsl_real_t)MODEL_ERF_SCALE);
    double transmission = (double)((erf_scale - ZSL_ERF((zsl_real_t)b)) /
                                   ((zsl_real_t)2.0 * erf_scale));

    if (!isfinite(transmission)) {
        return (isfinite(b) && b < 0.0) ? 1.0 : 0.0;
    }
    if (transmission < 0.0) {
        return 0.0;
    }
    if (transmission > 1.0) {
        return 1.0;
    }

    return transmission;
}

bool attenuator_model_linear_to_b(double linear, double *b)
{
    const zsl_real_t erf_scale = ZSL_ERF((zsl_real_t)MODEL_ERF_SCALE);
    zsl_real_t erf_arg;
    zsl_real_t inv;

    if (b == NULL || linear <= 0.0 || linear >= 1.0) {
        return false;
    }

    erf_arg = erf_scale - ((zsl_real_t)2.0 * erf_scale * (zsl_real_t)linear);
    if (erf_arg <= (zsl_real_t)-1.0 || erf_arg >= (zsl_real_t)1.0) {
        return false;
    }

    inv = zsl_prob_erf_inv(&erf_arg);
    *b = (double)inv;
    return true;
}

static double attenuator_model_dlinear_db(double b)
{
    const double erf_scale = erf(MODEL_ERF_SCALE);

    return -exp(-(b * b)) /
           (sqrt(M_PI) * erf_scale);
}

bool attenuator_estimate_transmission(struct attenuator *drv,
                                      double sigma_b1, double sigma_b2,
                                      struct attenuator_transmission_estimate *out)
{
    struct attenuator_status status;
    double b1;
    double b2;
    double tx1;
    double tx2;
    double open_tx1;
    double open_tx2;
    double dtx1_db;
    double dtx2_db;
    double var;

    if (drv == NULL || out == NULL || sigma_b1 < 0.0 || sigma_b2 < 0.0) {
        return false;
    }

    if (!attenuator_get(drv, &status)) {
        return false;
    }

    b1 = attenuator_model_voltage_to_delta(&drv->coeff1, status.voltage1);
    b2 = attenuator_model_voltage_to_delta(&drv->coeff2, status.voltage2);
    open_tx1 = attenuator_model_open_linear(&drv->coeff1);
    open_tx2 = attenuator_model_open_linear(&drv->coeff2);
    if (!(open_tx1 > MODEL_MIN_TX) || !(open_tx2 > MODEL_MIN_TX)) {
        return false;
    }

    tx1 = attenuator_model_b_to_linear(b1) / open_tx1;
    tx2 = attenuator_model_b_to_linear(b2) / open_tx2;
    if (tx1 > 1.0) {
        tx1 = 1.0;
    }
    if (tx2 > 1.0) {
        tx2 = 1.0;
    }
    dtx1_db = attenuator_model_dlinear_db(b1) / open_tx1;
    dtx2_db = attenuator_model_dlinear_db(b2) / open_tx2;
    var = (tx2 * dtx1_db * sigma_b1) * (tx2 * dtx1_db * sigma_b1) +
          (tx1 * dtx2_db * sigma_b2) * (tx1 * dtx2_db * sigma_b2);

    out->linear = tx1 * tx2;
    out->linear_err = sqrt(var);
    out->attenuation_db = status.attenuation_db;
    out->attenuation_db1 = status.attenuation_db1;
    out->attenuation_db2 = status.attenuation_db2;
    out->voltage1 = status.voltage1;
    out->voltage2 = status.voltage2;

    return true;
}

int attenuator_apply_coefficients_preserve_db(
    struct attenuator *drv,
    const struct attenuator_model_coeffs physical[ATTENUATOR_PHYSICAL_COUNT])
{
    struct attenuator_model_coeffs old_coeff1;
    struct attenuator_model_coeffs old_coeff2;
    struct attenuator_status status = {0};

    if (drv == NULL || !attenuator_model_coefficients_valid(physical)) {
        return -EINVAL;
    }

    if (!attenuator_get(drv, &status)) {
        return -EIO;
    }

    old_coeff1 = drv->coeff1;
    old_coeff2 = drv->coeff2;
    drv->coeff1 = physical[0];
    drv->coeff2 = physical[1];

    if (!attenuator_set_db(drv, status.attenuation_db)) {
        drv->coeff1 = old_coeff1;
        drv->coeff2 = old_coeff2;
        return -EIO;
    }

    return 0;
}
