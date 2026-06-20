/**
 * @file attenuator.h
 * @brief DAC7x78-backed logical attenuator channel helpers.
 *
 * Each logical attenuator owns two physical DAC-backed FVOAs. Persistence is
 * owned by app_settings; this module only applies model coefficients and
 * performs DAC I/O.
 */
#ifndef ATTENUATOR_H
#define ATTENUATOR_H

#include <zephyr/kernel.h>
#include <zephyr/drivers/dac.h>
#include <zephyr/logging/log.h>
#include <zephyr/devicetree.h>
#include <stdint.h>
#include <stdbool.h>

#include "lasers.h"

#define ATTENUATOR_PHYSICAL_COUNT 2
/* Firmware command/model span for DAC output before the FVOA-drive op amp. */
#define ATTENUATOR_DRIVE_MAX_MV 3300.0f
#define ATTENUATOR_DEFAULT_GAIN 1.533

struct attenuator_model_coeffs {
    double fvoa_50pct_mv;
    double slope_inv_fvoa_mv;
    double gain;
};

struct attenuator_dac_cfg {
    const struct device *dev;
    struct dac_channel_cfg cfg;
    float ideal_full_scale_mv;
    float drive_limit_mv;
    float voltage;
    double attenuation_db;
};

struct attenuator_status {
    double attenuation_db;
    double linear;
    float voltage1;
    float voltage2;
    double attenuation_db1;
    double attenuation_db2;
};

struct attenuator_transmission_estimate {
    double linear;
    double linear_err;
    double attenuation_db;
    double attenuation_db1;
    double attenuation_db2;
    float voltage1;
    float voltage2;
};

/**
 * Attenuator driver structure.
 */
struct attenuator {
    struct attenuator_model_coeffs coeff1;
    struct attenuator_model_coeffs coeff2;
    struct attenuator_dac_cfg dac_cfg1;
    struct attenuator_dac_cfg dac_cfg2;
    double attenuation_db;
};

/** Initialize a DAC channel. May block on I2C through the DAC driver. */
bool attenuator_init(struct attenuator *drv,
                     const struct device *dac1, uint8_t channel1,
                     const struct device *dac2, uint8_t channel2);

/** @brief Map a laser-bank channel to the matching logical attenuator index. */
int attenuator_index_from_laser_id(enum hispec_laser_id laser, uint8_t *index);

/**
 * @brief Convert a physical attenuator voltage to modeled attenuation in dB.
 *
 * The model is transmission = (erf(4) - erf(delta)) / (2 * erf(4)), where
 * delta = slope_inv_fvoa_mv * (gain * voltage - fvoa_50pct_mv). The voltage is
 * DAC output millivolts before the external FVOA-drive op amp. This helper does
 * not perform DAC I/O.
 */
double attenuator_model_voltage_to_db(const struct attenuator_model_coeffs *coeffs,
                                      float voltage);

/**
 * @brief Convert the model erf-delta coordinate to linear transmission.
 *
 * This helper performs no I/O. It is used by fit/residual code that needs the
 * same physical model as normal attenuator control.
 */
double attenuator_model_b_to_linear(double b);

/**
 * @brief Convert linear transmission to the model erf-delta coordinate.
 *
 * Returns false when @p linear is outside the invertible open interval.
 */
bool attenuator_model_linear_to_b(double linear, double *b);

/**
 * @brief Convert modeled attenuation in dB to a physical attenuator voltage.
 *
 * This is the inverse of attenuator_model_voltage_to_db(). Returns false when
 * the inverse slope is zero or the requested attenuation is outside the model
 * domain.
 */
bool attenuator_model_db_to_voltage(const struct attenuator_model_coeffs *coeffs,
                                    double attenuation_db, float *voltage);

/**
 * @brief Validate both physical attenuator model coefficient records.
 *
 * Coefficients must be finite, have positive slope, positive drive gain, and
 * produce a positive modeled attenuation change over the fixed DAC drive span.
 * This performs no DAC I/O and is used before applying or restoring persisted
 * coefficients.
 */
bool attenuator_model_coefficients_valid(
    const struct attenuator_model_coeffs physical[ATTENUATOR_PHYSICAL_COUNT]);

/**
 * @brief Set one physical attenuator by modeled dB. May block on I2C.
 *
 * @p physical_index is zero for dac1 and one for dac2.
 */
bool attenuator_set_physical_db(struct attenuator *drv, uint8_t physical_index, double attenuation_db);


/**
 * @brief Set one physical attenuator by raw DAC-output millivolts. May block on I2C.
 *
 * @p physical_index is zero for dac1 and one for dac2. This bypasses the
 * calibration model and is intended for calibration sweeps.
 */
bool attenuator_set_physical_voltage(struct attenuator *drv, uint8_t physical_index, float voltage);

/**
 * @brief Set total logical attenuation in dB.
 *
 * The first physical attenuator is driven to its modeled maximum before the
 * second attenuator is used. This overrides any prior individual physical
 * attenuator set point and may enqueue a warning if the requested attenuation
 * exceeds the modeled drive range.
 */
bool attenuator_set_db(struct attenuator *drv, double attenuation_db);

/**
 * @brief Set total logical transmission as a linear fraction in (0, 1].
 *
 * This converts the requested transmission to dB attenuation and delegates to
 * attenuator_set_db().
 */
bool attenuator_set_linear(struct attenuator *drv, double linear);

/**
 * @brief Read back both DAC registers and return logical/physical state.
 *
 * The read may block on I2C. It does not verify that the attenuator hardware is
 * powered or optically calibrated.
 */
bool attenuator_get(struct attenuator *drv, struct attenuator_status *out);

/**
 * @brief Read logical transmission and propagate physical FVOA b uncertainty.
 *
 * This may block on I2C through attenuator_get(). The uncertainty inputs are
 * standard deviations in the model coordinate b for physical attenuator 1 and
 * 2. A zero uncertainty reports the nominal transmission with zero error.
 */
bool attenuator_estimate_transmission(struct attenuator *drv,
                                      double sigma_b1, double sigma_b2,
                                      struct attenuator_transmission_estimate *out);

/**
 * @brief Replace both physical model coefficients and preserve logical dB.
 *
 * Reads the current logical attenuation, installs the supplied coefficients,
 * and reapplies that logical attenuation using the new model. This may block on
 * DAC I2C through attenuator_get() and attenuator_set_db(). On failure while
 * applying the new model, the previous coefficients are restored in RAM.
 *
 * @retval 0 Coefficients were applied and logical attenuation was reissued.
 * @retval -EINVAL Bad arguments.
 * @retval -EIO DAC read or write failed.
 */
int attenuator_apply_coefficients_preserve_db(
    struct attenuator *drv,
    const struct attenuator_model_coeffs physical[ATTENUATOR_PHYSICAL_COUNT]);

#endif /* ATTENUATOR_H */
