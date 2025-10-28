//
// Created by Jeb Bailey on 4/22/25.
//
#ifndef ATTENUATOR_H
#define ATTENUATOR_H

#include <zephyr/kernel.h>
#include <zephyr/drivers/dac.h>
#include <zephyr/logging/log.h>
#include <zephyr/devicetree.h>
#include <stdint.h>
#include <stdbool.h>

/**
 * Attenuator driver structure.
 */
struct attenuator {
    double  coeff_db_to_volt[3];
    double  coeff_volt_to_db[3];
    double  voltage;
    struct dac_channel_cfg cfg;
};

/**
 * Initialize attenuator driver for given DAC channel.
 * @param drv     Pointer to driver instance
 * @param channel DAC channel number
 */
void attenuator_init(struct attenuator *drv, uint8_t channel);

/**
 * Set output voltage on attenuator (clamped to DAC range).
 * @param drv      Pointer to driver instance
 * @param voltage  Desired voltage (0.0f to MAX_VOLTAGE)
 * @return true on success, false on error
 */
bool attenuator_set(struct attenuator *drv, double voltage, bool raw);

/**
 * Get last-set voltage value.
 * @param drv      Pointer to driver instance
 * @param voltage  Out parameter for voltage
 * @return true
 */
bool attenuator_get(struct attenuator *drv, double *voltage, bool raw);

#endif /* ATTENUATOR_H */