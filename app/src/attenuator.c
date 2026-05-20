/**
 * @file attenuator.c
 * @brief DAC write/read helpers for paired physical attenuators.
 */

#include "attenuator.h"
#include "command.h"
#include "drivers/dac/dac7578.h"

#include <errno.h>
#include <math.h>
#include <zsl/probability.h>
#include <zsl/zsl.h>

LOG_MODULE_REGISTER(attenuator, LOG_LEVEL_INF);

#define DAC_RESOLUTION_BITS 12
#define DAC_MAX_CODE        ((1 << DAC_RESOLUTION_BITS) - 1)
#define MODEL_ERF_SCALE     4.0
#define MODEL_MAX_DB        120.0
#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

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
    int err;

    if (dac_cfg->dev == NULL || !device_is_ready(dac_cfg->dev)) {
        LOG_ERR("DAC device unavailable for channel %u",
                dac_cfg->cfg.channel_id);
        return false;
    }

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
    drv->attenuation_db = 0.0;

    return attenuator_channel_setup(&drv->dac_cfg1) &&
           attenuator_channel_setup(&drv->dac_cfg2);
}

double attenuator_model_voltage_to_db(const struct attenuator_model_coeffs *coeffs,
                                      double voltage)
{
    double b;
    double transmission;

    if (coeffs == NULL) {
        return 0.0;
    }

    b = coeffs->slope * voltage + coeffs->offset;
    transmission = attenuator_model_b_to_linear(b);

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
    zsl_real_t transmission;
    zsl_real_t erf_arg;
    zsl_real_t inv;
    double b;

    if (coeffs == NULL || voltage == NULL || coeffs->slope == 0.0 ||
        attenuation_db < 0.0) {
        return false;
    }

    transmission = ZSL_POW((zsl_real_t)10.0,
                           (zsl_real_t)(-attenuation_db / 10.0));
    erf_arg = ((zsl_real_t)2.0 * erf_scale * transmission) - erf_scale;
    if (erf_arg <= (zsl_real_t)-1.0 || erf_arg >= (zsl_real_t)1.0) {
        return false;
    }

    inv = zsl_prob_erf_inv(&erf_arg);
    b = MODEL_ERF_SCALE - (double)inv;
    *voltage = (b - coeffs->offset) / coeffs->slope;

    return true;
}

static bool attenuator_write_voltage(struct attenuator_dac_cfg *dac_cfg,
                                     const struct attenuator_model_coeffs *coeffs,
                                     double voltage)
{
    double unclamped_voltage = voltage;
    uint32_t code;
    int err;
    char context[64];

    if (dac_cfg == NULL || coeffs == NULL || dac_cfg->dev == NULL) {
        return false;
    }

    if (voltage < 0.0) {
        voltage = 0.0;
    } else if (voltage > ATTENUATOR_DAC_MAX_MV) {
        voltage = ATTENUATOR_DAC_MAX_MV;
    }

    if (voltage != unclamped_voltage) {
        snprintk(context, sizeof(context),
                 "channel=%u requested=%.3f clamped=%.3f",
                 dac_cfg->cfg.channel_id, unclamped_voltage, voltage);
        coo_cmd_runtime_warning_emit(command_runtime_get(), "attenuator_clamped",
                         "attenuator command exceeded DAC range and was clamped",
                         context);
    }

    code = (uint32_t)((voltage / ATTENUATOR_DAC_MAX_MV) * DAC_MAX_CODE);
    /* dac_write_value() is the hardware side effect: it can block on I2C and
     * changes the analog attenuation control voltage.
     */
    err = dac_write_value(dac_cfg->dev, dac_cfg->cfg.channel_id, code);
    if (err != 0) {
        LOG_ERR("DAC write failed: %d", err);
        return false;
    }

    dac_cfg->voltage = voltage;
    dac_cfg->attenuation_db = attenuator_model_voltage_to_db(coeffs, voltage);

    return true;
}

static bool attenuator_set_physical_db(struct attenuator_dac_cfg *dac_cfg,
                                       const struct attenuator_model_coeffs *coeffs,
                                       double attenuation_db)
{
    double voltage;

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

static double attenuator_physical_max_db(const struct attenuator_model_coeffs *coeffs)
{
    return attenuator_model_voltage_to_db(coeffs, ATTENUATOR_DAC_MAX_MV);
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

    max_db1 = attenuator_physical_max_db(&drv->coeff1);
    max_total_db = max_db1 + attenuator_physical_max_db(&drv->coeff2);

    if (attenuation_db > max_total_db) {
        snprintk(context, sizeof(context),
                 "requested=%.3f clamped=%.3f", attenuation_db, max_total_db);
        coo_cmd_runtime_warning_emit(command_runtime_get(), "attenuator_clamped",
                         "attenuator command exceeded modeled range and was clamped",
                         context);
        attenuation_db = max_total_db;
    }

    db1 = attenuation_db < max_db1 ? attenuation_db : max_db1;
    db2 = attenuation_db - db1;

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

    err = dac7578_read_value(dac_cfg->dev, dac_cfg->cfg.channel_id, &code);
    if (err != 0) {
        LOG_ERR("DAC read failed: %d", err);
        return false;
    }

    dac_cfg->voltage = (ATTENUATOR_DAC_MAX_MV / (double)DAC_MAX_CODE) * (double)code;
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
    double transmission = (double)((erf_scale +
                                    ZSL_ERF((zsl_real_t)MODEL_ERF_SCALE -
                                            (zsl_real_t)b)) /
                                   ((zsl_real_t)2.0 * erf_scale));

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

    erf_arg = ((zsl_real_t)2.0 * erf_scale * (zsl_real_t)linear) - erf_scale;
    if (erf_arg <= (zsl_real_t)-1.0 || erf_arg >= (zsl_real_t)1.0) {
        return false;
    }

    inv = zsl_prob_erf_inv(&erf_arg);
    *b = MODEL_ERF_SCALE - (double)inv;
    return true;
}

static double attenuator_model_dlinear_db(double b)
{
    const double erf_scale = erf(MODEL_ERF_SCALE);

    return -exp(-((MODEL_ERF_SCALE - b) * (MODEL_ERF_SCALE - b))) /
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
    double dtx1_db;
    double dtx2_db;
    double var;

    if (drv == NULL || out == NULL || sigma_b1 < 0.0 || sigma_b2 < 0.0) {
        return false;
    }

    if (!attenuator_get(drv, &status)) {
        return false;
    }

    b1 = drv->coeff1.slope * status.voltage1 + drv->coeff1.offset;
    b2 = drv->coeff2.slope * status.voltage2 + drv->coeff2.offset;
    tx1 = attenuator_model_b_to_linear(b1);
    tx2 = attenuator_model_b_to_linear(b2);
    dtx1_db = attenuator_model_dlinear_db(b1);
    dtx2_db = attenuator_model_dlinear_db(b2);
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

    if (drv == NULL || physical == NULL) {
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
