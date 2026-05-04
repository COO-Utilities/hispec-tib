/**
 * @file attenuator.h
 * @brief DAC7578-backed logical attenuator channel helpers.
 *
 * Each logical attenuator stores runtime polynomial coefficients and the last
 * read/write voltage. Persistence is owned by app_settings; this module only
 * applies coefficients and performs DAC I/O.
 */
#ifndef ATTENUATOR_H
#define ATTENUATOR_H

#include <zephyr/kernel.h>
#include <zephyr/drivers/dac.h>
#include <zephyr/logging/log.h>
#include <zephyr/devicetree.h>
#include <stdint.h>
#include <stdbool.h>

/** Number of quadratic coefficients in each attenuator calibration polynomial. */
#define ATTENUATOR_COEFF_COUNT 3

/**
 * Attenuator driver structure.
 */
struct attenuator {
    double  coeff_db_to_volt[ATTENUATOR_COEFF_COUNT];
    double  coeff_volt_to_db[ATTENUATOR_COEFF_COUNT];
    double  voltage;
    struct dac_channel_cfg cfg;
};

/** Initialize a DAC channel. May block on I2C through the DAC driver. */
bool attenuator_init(struct attenuator *drv, uint8_t channel);

/**
 * @brief Set attenuation by raw millivolts or calibrated dB.
 *
 * If @p raw is false, @p voltage is interpreted as dB and converted using the
 * runtime `coeff_db_to_volt` polynomial. The final voltage is clamped to the
 * DAC range, may enqueue a warning, and is written over I2C.
 */
bool attenuator_set(struct attenuator *drv, double voltage, bool raw);

/**
 * @brief Read back the DAC register and return raw millivolts or estimated dB.
 *
 * The read may block on I2C. It does not verify that the attenuator hardware is
 * powered or optically calibrated.
 */
bool attenuator_get(struct attenuator *drv, double *voltage, bool raw);

#endif /* ATTENUATOR_H */
