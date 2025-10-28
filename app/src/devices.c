//
// Created by Jeb Bailey on 5/19/25.
//
/*
 * Copyright (c) 2024 Analog Devices, Inc.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef __DEVICE_C__
#define __DEVICE_C__

#include "devices.h"
#include "mems_switching.h"
LOG_MODULE_REGISTER(devices, LOG_LEVEL_INF);


/* Devices */

#define USER_NODE DT_PATH(zephyr_user)

const struct gpio_dt_spec power_gpio = GPIO_DT_SPEC_GET(USER_NODE, power_gpios);
// const struct gpio_dt_spec mems0_A = GPIO_DT_SPEC_GET(USER_NODE, mems0_a_gpios);
// const struct gpio_dt_spec mems0_B = GPIO_DT_SPEC_GET(USER_NODE, mems0_b_gpios);
// const struct gpio_dt_spec mems1_A = GPIO_DT_SPEC_GET(USER_NODE, mems1_a_gpios);
// const struct gpio_dt_spec mems1_B = GPIO_DT_SPEC_GET(USER_NODE, mems1_b_gpios);
// const struct gpio_dt_spec mems2_A = GPIO_DT_SPEC_GET(USER_NODE, mems2_a_gpios);
// const struct gpio_dt_spec mems2_B = GPIO_DT_SPEC_GET(USER_NODE, mems2_b_gpios);
// const struct gpio_dt_spec mems3_A = GPIO_DT_SPEC_GET(USER_NODE, mems3_a_gpios);
// const struct gpio_dt_spec mems3_B = GPIO_DT_SPEC_GET(USER_NODE, mems3_b_gpios);
// const struct gpio_dt_spec mems4_A = GPIO_DT_SPEC_GET(USER_NODE, mems4_a_gpios);
// const struct gpio_dt_spec mems4_B = GPIO_DT_SPEC_GET(USER_NODE, mems4_b_gpios);
// const struct gpio_dt_spec mems5_A = GPIO_DT_SPEC_GET(USER_NODE, mems5_a_gpios);
// const struct gpio_dt_spec mems5_B = GPIO_DT_SPEC_GET(USER_NODE, mems5_b_gpios);
// const struct gpio_dt_spec mems6_A = GPIO_DT_SPEC_GET(USER_NODE, mems6_a_gpios);
// const struct gpio_dt_spec mems6_B = GPIO_DT_SPEC_GET(USER_NODE, mems6_b_gpios);
// const struct gpio_dt_spec mems7_A = GPIO_DT_SPEC_GET(USER_NODE, mems7_a_gpios);
// const struct gpio_dt_spec mems7_B = GPIO_DT_SPEC_GET(USER_NODE, mems7_b_gpios);


#define MODBUS_NODE DT_COMPAT_GET_ANY_STATUS_OKAY(zephyr_modbus_serial)
const char modbus_name[] = DEVICE_DT_NAME(MODBUS_NODE);
const struct device *adc_dev = DEVICE_DT_GET(DT_NODELABEL(adc1115));
const struct device *dac_dev = DEVICE_DT_GET(DT_NODELABEL(dac7578)); //or DEVICE_DT_GET_OR_NULL
const struct device *gpio_dev = DEVICE_DT_GET(DT_NODELABEL(pcal6416a));


struct attenuator attenuators[NUM_ATTENUATORS];
struct mems_switch mems_switches[MEMS_ROUTER_MAX_SWITCHES];
struct mems_router router;
struct mems_switch *mems_switch_ptrs[MEMS_ROUTER_MAX_SWITCHES];



bool setup_modbus_client(void) {

    struct modbus_iface_param modbus_cfg = {
        .mode = MODBUS_MODE_RTU,
        .serial = {
            .baud = 115200,
            .parity = UART_CFG_PARITY_NONE,
            .stop_bits = UART_CFG_STOP_BITS_1
        },
		.rx_timeout = MODBUS_RX_TIMEOUT_MS
    };

    int client_iface  = modbus_iface_get_by_name(modbus_name);
    if (modbus_init_client(client_iface, modbus_cfg)==0) {
        LOG_INF("Modbus client (RTU) initialized on UART1");
    } else {
        LOG_ERR("Modbus init failed");
		return false;
    }
	return true;
}





static const char *switch_names[8] = {
    "yj_cal_laser", "hk_cal_laser",
    "yj_ao_fei",    "hk_ao_fei",
    "yj_forward_retro", "hk_forward_retro",
    "yj_mm_sm",     "hk_mm_sm"
};

// Example: adjacent pins assigned for each switch
const gpio_pin_t mems_switch_pin_pairs[8][2] = {
    {0, 1}, {2, 3}, {4, 5}, {6, 7},
    {8, 9}, {10, 11}, {12, 13}, {14, 15}
};


struct mems_route_step yj_1430_to_yj_ao[] = {
    {"yj_cal_laser", 'B'},
    {"yj_forward_retro", 'A'},
    {"yj_ao_fei", 'A'}
};


void setup_attenuators() {
    for (int i=0;i<6;++i) {
        attenuator_init(&attenuators[i], i);
    }
}


void setup_mems_switches_and_routes() {

    if (!device_is_ready(gpio_dev)) {
        printk("GPIO expander not ready!\n");
        return;
    }

    // Initialize switches and pointer table
    for (int i = 0; i < 8; ++i) {
        mems_switch_init(&mems_switches[i], gpio_dev, mems_switch_pin_pairs[i][0], mems_switch_pin_pairs[i][1], switch_names[i]);
        mems_switch_ptrs[i] = &mems_switches[i];
    }

    // Initialize router
    mems_router_init(&router, mems_switch_ptrs, 8);

    // ------ Define all routes -------
    // For each route, define its path and register

    // yj_1430 → yj_ao
    struct mems_route_step yj_1430_to_yj_ao[] = {
        {"yj_cal_laser", 'B'},
        {"yj_forward_retro", 'A'},
        {"yj_ao_fei", 'A'}
    };
    mems_router_define_route(&router, "yj_1430", "yj_ao", yj_1430_to_yj_ao, 3);

    // yj_1430 → yj_fei
    struct mems_route_step yj_1430_to_yj_fei[] = {
        {"yj_cal_laser", 'B'},
        {"yj_forward_retro", 'A'},
        {"yj_ao_fei", 'B'}
    };
    mems_router_define_route(&router, "yj_1430", "yj_fei", yj_1430_to_yj_fei, 3);

    // yj_cal → yj_ao
    struct mems_route_step yj_cal_to_yj_ao[] = {
        {"yj_cal_laser", 'A'},
        {"yj_ao_fei", 'A'}
    };
    mems_router_define_route(&router, "yj_cal", "yj_ao", yj_cal_to_yj_ao, 2);

    // yj_cal → yj_fei
    struct mems_route_step yj_cal_to_yj_fei[] = {
        {"yj_cal_laser", 'A'},
        {"yj_ao_fei", 'B'}
    };
    mems_router_define_route(&router, "yj_cal", "yj_fei", yj_cal_to_yj_fei, 2);

    // yj_laser → yj_ao
    struct mems_route_step yj_laser_to_yj_ao[] = {
        {"yj_cal_laser", 'B'},
        {"yj_ao_fei", 'A'}
    };
    mems_router_define_route(&router, "yj_laser", "yj_ao", yj_laser_to_yj_ao, 2);

    // yj_laser → yj_fei
    struct mems_route_step yj_laser_to_yj_fei[] = {
        {"yj_cal_laser", 'B'},
        {"yj_ao_fei", 'B'}
    };
    mems_router_define_route(&router, "yj_laser", "yj_fei", yj_laser_to_yj_fei, 2);

    // yj_mm → yj_pd
    struct mems_route_step yj_mm_to_yj_pd[] = {
        {"yj_mm_sm", 'A'}
    };
    mems_router_define_route(&router, "yj_mm", "yj_pd", yj_mm_to_yj_pd, 1);

    // yj_sm → yj_pd
    struct mems_route_step yj_sm_to_yj_pd[] = {
        {"yj_mm_sm", 'B'}
    };
    mems_router_define_route(&router, "yj_sm", "yj_pd", yj_sm_to_yj_pd, 1);

    // hk_1430 → hk_ao
    struct mems_route_step hk_1430_to_hk_ao[] = {
        {"hk_cal_laser", 'B'},
        {"hk_forward_retro", 'A'},
        {"hk_ao_fei", 'A'}
    };
    mems_router_define_route(&router, "hk_1430", "hk_ao", hk_1430_to_hk_ao, 3);

    // hk_1430 → hk_fei
    struct mems_route_step hk_1430_to_hk_fei[] = {
        {"hk_cal_laser", 'B'},
        {"hk_forward_retro", 'A'},
        {"hk_ao_fei", 'B'}
    };
    mems_router_define_route(&router, "hk_1430", "hk_fei", hk_1430_to_hk_fei, 3);

    // hk_cal → hk_ao
    struct mems_route_step hk_cal_to_hk_ao[] = {
        {"hk_cal_laser", 'A'},
        {"hk_ao_fei", 'A'}
    };
    mems_router_define_route(&router, "hk_cal", "hk_ao", hk_cal_to_hk_ao, 2);

    // hk_cal → hk_fei
    struct mems_route_step hk_cal_to_hk_fei[] = {
        {"hk_cal_laser", 'A'},
        {"hk_ao_fei", 'B'}
    };
    mems_router_define_route(&router, "hk_cal", "hk_fei", hk_cal_to_hk_fei, 2);

    // hk_laser → hk_ao
    struct mems_route_step hk_laser_to_hk_ao[] = {
        {"hk_cal_laser", 'B'},
        {"hk_ao_fei", 'A'}
    };
    mems_router_define_route(&router, "hk_laser", "hk_ao", hk_laser_to_hk_ao, 2);

    // hk_laser → hk_fei
    struct mems_route_step hk_laser_to_hk_fei[] = {
        {"hk_cal_laser", 'B'},
        {"hk_ao_fei", 'B'}
    };
    mems_router_define_route(&router, "hk_laser", "hk_fei", hk_laser_to_hk_fei, 2);

    // hk_mm → hk_pd
    struct mems_route_step hk_mm_to_hk_pd[] = {
        {"hk_mm_sm", 'A'}
    };
    mems_router_define_route(&router, "hk_mm", "hk_pd", hk_mm_to_hk_pd, 1);

    // hk_sm → hk_pd
    struct mems_route_step hk_sm_to_hk_pd[] = {
        {"hk_mm_sm", 'B'}
    };
    mems_router_define_route(&router, "hk_sm", "hk_pd", hk_sm_to_hk_pd, 1);
}

bool devices_ready(void)
{
    bool rc = true;

    if (!gpio_is_ready_dt(&power_gpio)) {
        LOG_ERR("Error: power gpio is not ready");

    }
    else {
        rc = gpio_pin_configure_dt(&power_gpio, GPIO_OUTPUT_INACTIVE);
        if (rc != 0) {
            LOG_ERR("Failed to configure power gpio");
        }
    }

    /* Check readiness only if a real sensor device is present */
    if (setup_modbus_client() != 0) {
        LOG_ERR("Failed to setup modbus");
        rc = false;
    } else {
        LOG_INF("Modbus online");
    }

    if (dac_dev != NULL) {
        if (!device_is_ready(dac_dev)) {
            LOG_ERR("Device %s is not ready", dac_dev->name);
            rc = false;
        } else {
            LOG_INF("Device %s is ready", dac_dev->name);
        }
    }

    if (adc_dev != NULL) {
        if (!device_is_ready(adc_dev)) {
            LOG_ERR("Device %s is not ready", adc_dev->name);
            rc = false;
        } else {
            LOG_INF("Device %s is ready", adc_dev->name);
        }
    }

    if (gpio_dev != NULL) {
        if (!device_is_ready(gpio_dev)) {
            LOG_ERR("GPIO expander not ready!\n");
            rc = false;
        } else {
            LOG_INF("GIPO expander ready");
        }
    }

    return rc;
}


#endif /* __DEVICE_C__ */