/**
 * @file attenuator.c
 * @brief DAC write/read helpers for logical attenuator channels.
 */

//TODO The attenuators had a non-linear relationship between dB and voltage that we want to calibrate and then
// then set on dB, not voltage

#include "attenuator.h"
#include "app_warning.h"
#include "devices.h"
#include "drivers/dac/dac7578.h"
LOG_MODULE_REGISTER(attenuator, LOG_LEVEL_INF);

//See
//https://docs.zephyrproject.org/apidoc/latest/group__dac__interface.html#gab8be77003ba8fd7225c0808f95602a56
//https://github.com/zephyrproject-rtos/zephyr/blob/main/samples/drivers/dac/src/main.c


#define DAC_RESOLUTION_BITS  12
#define DAC_MAX_CODE         ((1 << DAC_RESOLUTION_BITS) - 1)
#define MAX_VOLTAGE          4096.0

// static const struct device *dac_dev = DEVICE_DT_GET(DT_NODELABEL(dac7578));  //or DEVICE_DT_GET_OR_NULL

bool attenuator_init(struct attenuator *drv, uint8_t channel) {
    drv->voltage = 0.0;
    drv->cfg.channel_id = channel;
    drv->cfg.resolution = DAC_RESOLUTION_BITS;
#if defined(CONFIG_DAC_BUFFER_NOT_SUPPORT)
    drv->cfg.buffered = false;
#else
    drv->cfg.buffered = true;
#endif

    if (!device_is_ready(dac_dev)) {
        LOG_ERR("DAC device %s not ready", dac_dev->name);
        return false;
    }

    /* dac_channel_setup() prepares the selected channel in the DAC driver and
     * may perform I2C transactions depending on the underlying implementation.
     */
    int err = dac_channel_setup(dac_dev, &drv->cfg);
    if (err != 0) {
        LOG_ERR("DAC channel setup failed: %d", err);
        return false;
    }
    return true;
}

bool attenuator_set(struct attenuator *drv, double value, bool raw) {
    /* Clamp voltage to [0, MAX_VOLTAGE] */
    double voltage;
    double unclamped_voltage;
    int err;
    char context[48];

    if (raw) {
        voltage = value;
    }
    else {
        voltage = drv->coeff_db_to_volt[0]+
            drv->coeff_db_to_volt[1]*value+
            drv->coeff_db_to_volt[2]*value*value;
    }
    unclamped_voltage = voltage;

    if (voltage < 0.0) {
        voltage = 0.0;
    } else if (voltage > MAX_VOLTAGE) {
        voltage = MAX_VOLTAGE;
    }
    if (voltage != unclamped_voltage) {
        snprintk(context, sizeof(context), "channel=%u requested=%.3f clamped=%.3f",
                 drv->cfg.channel_id, unclamped_voltage, voltage);
        app_warning_emit("attenuator_clamped",
                         "attenuator command exceeded DAC range and was clamped",
                         context);
    }

    drv->voltage = voltage;
    uint32_t code = (uint32_t)((drv->voltage / MAX_VOLTAGE) * DAC_MAX_CODE);
    /* dac_write_value() is the hardware side effect: it can block on I2C and
     * changes the analog attenuation control voltage.
     */
    err = dac_write_value(dac_dev, drv->cfg.channel_id, code);
    if (err != 0) {
        LOG_ERR("DAC write failed: %d", err);
        return false;
    }

    return true;
}

bool attenuator_get(struct attenuator *drv, double *value, bool raw) {
    //NB Reads the register, no checking for chip power down state
    uint32_t code;
    double volt;
    dac7578_read_value(dac_dev, drv->cfg.channel_id, &code);
    drv->voltage = ((double) MAX_VOLTAGE/(double) DAC_MAX_CODE)*(double) code;
    volt = drv->voltage;
    if (raw) {
        *value = volt;
    } else {
        *value = drv->coeff_volt_to_db[0] +
            volt*drv->coeff_volt_to_db[1] +
            volt*volt*drv->coeff_volt_to_db[2];
    }

    return true;
}
