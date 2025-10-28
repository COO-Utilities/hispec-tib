//
// Created by Jeb Bailey on 5/29/25.
//

#ifndef PHOTODIODE_H
#define PHOTODIODE_H

#include <zephyr/kernel.h>

#define ADC_RESOLUTION 16  //TODO get this from zephyr,resolution = < 16 >; in the DT
#define PUBLISH_INTERVAL_MS 20


extern struct k_msgq photodiode_queue;
void photodiode_thread();

#endif //PHOTODIODE_H
