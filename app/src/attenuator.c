//
// Created by Jeb Bailey on 4/22/25.
//

//TODO The attenuators had a non-linear relationship between dB and voltage that we want to calibrate and then
// then set on dB, not voltage

#include "attenuator.h"
#include "devices.h"
LOG_MODULE_REGISTER(attenuator, LOG_LEVEL_INF);

//See
//https://docs.zephyrproject.org/apidoc/latest/group__dac__interface.html#gab8be77003ba8fd7225c0808f95602a56
//https://github.com/zephyrproject-rtos/zephyr/blob/main/samples/drivers/dac/src/main.c


#define DAC_RESOLUTION_BITS  12
#define DAC_MAX_CODE         ((1 << DAC_RESOLUTION_BITS) - 1)
#define MAX_VOLTAGE          4.096d

// static const struct device *dac_dev = DEVICE_DT_GET(DT_NODELABEL(dac7578));  //or DEVICE_DT_GET_OR_NULL

void attenuator_init(struct attenuator *drv, uint8_t channel) {
    drv->voltage = 0.0f;
    drv->cfg.channel_id = channel;
    drv->cfg.resolution = DAC_RESOLUTION_BITS;
#if defined(CONFIG_DAC_BUFFER_NOT_SUPPORT)
    drv->cfg.buffered = false;
#else
    drv->cfg.buffered = true;
#endif
}

bool attenuator_set(struct attenuator *drv, double value, bool raw) {
    /* Clamp voltage to [0, MAX_VOLTAGE] */
    double voltage;
    if (raw) {
        voltage = value;
    }
    else {
        voltage = drv->coeff_db_to_volt[0]+
            drv->coeff_db_to_volt[2]*value*value+
                drv->coeff_db_to_volt[1]*value;
    }

    if (voltage < 0.0d) {
        voltage = 0.0d;
    } else if (voltage > MAX_VOLTAGE) {
        voltage = MAX_VOLTAGE;
    }
    drv->voltage = voltage;

    if (!device_is_ready(dac_dev)) {
        LOG_ERR("DAC device %s not ready", dac_dev->name);
        return false;
    }

    int err = dac_channel_setup(dac_dev, &drv->cfg);
    if (err != 0) {
        LOG_ERR("DAC channel setup failed: %d", err);
        return false;
    }

    uint32_t code = (uint32_t)((drv->voltage / MAX_VOLTAGE) * DAC_MAX_CODE);
    err = dac_write_value(dac_dev, drv->cfg.channel_id, code);
    if (err != 0) {
        LOG_ERR("DAC write failed: %d", err);
        return false;
    }

    return true;
}

bool attenuator_get(struct attenuator *drv, double *value, bool raw) {
    if (raw) {
        *value = drv->voltage;
    } else {
        *value = drv->coeff_volt_to_db[0]+
            drv->voltage*drv->voltage*drv->coeff_volt_to_db[2]+
                drv->voltage*drv->coeff_volt_to_db[1];
    }

    return true;
}
