//
// Created by Jeb Bailey on 5/19/25.
//


#include <zephyr/kernel.h>             // k_sleep, thread declarations, etc.
#include <zephyr/device.h>             // DEVICE_DT_GET, device_is_ready
#include <zephyr/devicetree.h>         // DT_ALIAS, DT_NODELABEL, etc.
#include <zephyr/drivers/adc.h>        // ADC API
#include <zephyr/logging/log.h>        // LOG_ERR, LOG_WRN, etc.
#include <zephyr/net/sntp.h>
#include <stdint.h>                 // int16_t, int64_t, etc.
// #include <zephyr/posix/time.h>
// #include <limits.h>

#include "photodiode.h"
#include "command.h"
#include "devices.h"


LOG_MODULE_REGISTER(photodiode, LOG_LEVEL_INF);

//More ADC info:
// https://github.com/zephyrproject-rtos/zephyr/blob/main/samples/drivers/adc/adc_dt/src/main.c
// https://docs.zephyrproject.org/apidoc/latest/group__adc__interface.html
// https://github.com/zephyrproject-rtos/zephyr/blob/main/drivers/adc/adc_ads1x1x.c#L617

// static const struct device *adc_dev = DEVICE_DT_GET_OR_NULL(DT_NODELABEL(adc1115));

static const struct adc_channel_cfg yj_cfg_dt =
    ADC_CHANNEL_CFG_DT(DT_CHILD(DT_NODELABEL(adc1115), channel_0));

static const struct adc_channel_cfg hk_cfg_dt =
    ADC_CHANNEL_CFG_DT(DT_CHILD(DT_NODELABEL(adc1115), channel_1));

K_MSGQ_DEFINE(photodiode_queue, sizeof(struct OutMsg), 4, 4);


void photodiode_thread()
{
    int rc_yj, rc_hk;
    int16_t yj_sample, hk_sample;
    struct adc_sequence seq = {
        .channels = 0,
        .buffer = NULL,                        /* We will provide buffer later */
        .buffer_size = sizeof(int16_t),
        .resolution = 16,
        .oversampling=0,
        .calibrate=false,

    };

	k_sleep(K_MSEC(10));

	while(!device_is_ready(adc_dev)) {
        LOG_ERR("ADS1115 not ready");
		k_sleep(K_MSEC(10));
    }

    while (1) {

        int64_t start = k_uptime_get();

		yj_sample = INT16_MIN;
		hk_sample = INT16_MIN;

        rc_yj = adc_channel_setup(adc_dev, &yj_cfg_dt);
        if (rc_yj != 0) {
            LOG_ERR("ADC YJ channel setup failed (%d)", rc_yj);
        } else {
            seq.buffer = &yj_sample;
            rc_yj = adc_read(adc_dev, &seq);
            if (rc_yj != 0) {
                LOG_ERR("ADC YJ read failed (%d)", rc_yj);
            }
		}

        rc_hk = adc_channel_setup(adc_dev, &hk_cfg_dt);
        if (rc_hk != 0) {
            LOG_ERR("ADC HK channel setup failed  (%d)", rc_hk);
        } else {
            seq.buffer = &hk_sample;
            rc_hk = adc_read(adc_dev, &seq);
            if (rc_hk != 0) {
                LOG_ERR("ADC HK read failed (%d)", rc_hk);
            }
		}

        // Get timestamp
        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);

        struct OutMsg msg = {0};
        msg.qos = 0;
        snprintk(msg.topic, sizeof(msg.topic), "dt/hsfib-tib/photodiode");
        msg.payload_len = snprintk(msg.payload, sizeof(msg.payload),
                                  "{\"yj\":%hd, \"hk\":%hd, \"time\":%lld}",
                                  yj_sample, hk_sample, ts.tv_sec);

        while (k_msgq_put(&photodiode_queue, &msg, K_NO_WAIT) !=0) {
            /* photodiode_queue is full: purge old data & try again */
			LOG_WRN("ADC msgq full, purging");
            k_msgq_purge(&photodiode_queue);
        }

        int64_t elapsed = k_uptime_get() - start;  //overflow every 300M years
        int64_t remaining = PUBLISH_INTERVAL_MS - elapsed;

        if (remaining > 0) {
            k_sleep(K_MSEC(remaining));
        } else {
            LOG_WRN("ADC loop overran interval by %lld ms", -remaining);
        }

    }

}