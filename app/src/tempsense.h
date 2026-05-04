/**
 * @file tempsense.h
 * @brief DS18B20 ambient temperature sampling cache.
 */

#ifndef APP_TEMPSENSE_H
#define APP_TEMPSENSE_H


#include <zephyr/kernel.h>
#include <stdbool.h>
#include <stdint.h>


struct tempsense_status {
    float ambient_c;
    uint32_t age_ms;
    int last_error;
    bool valid;
};

/* Latest DS18B20 ambient reading cache; use tempsense_get_status() for a stable copy. */
extern struct tempsense_status tempsense;

/** Copy the current temperature cache and compute age from Zephyr uptime. */
void tempsense_get_status(struct tempsense_status *out);

/** Background sampler thread. Reads the Zephyr sensor API once per second. */
void tempsensor_thread(void *p1, void *p2, void *p3);





#endif //APP_TEMPSENSE_H
