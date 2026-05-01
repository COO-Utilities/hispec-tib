//
// Created by Jeb Bailey on 5/29/25.
//

#ifndef PHOTODIODE_H
#define PHOTODIODE_H

#include <zephyr/kernel.h>
#include <stdbool.h>
#include <stdint.h>

#define ADC_RESOLUTION 16  //TODO get this from zephyr,resolution = < 16 >; in the DT
#define PUBLISH_INTERVAL_MS 20

enum photodiode_channel {
	PHOTODIODE_CHANNEL_YJ = 0,
	PHOTODIODE_CHANNEL_HK = 1,
	PHOTODIODE_CHANNEL_COUNT = 2,
};

struct photodiode_channel_status {
	bool valid;
	int last_error;
	int16_t raw;
	float mv;
	float net_mv;
	float power_uw;
	float noise_rms_mv;
	float dark_mv;
	float lowest_dark_mv;
	bool lowest_dark_valid;
	uint32_t age_ms;
	uint32_t sample_count;
};

struct photodiode_status {
	struct photodiode_channel_status channel[PHOTODIODE_CHANNEL_COUNT];
	int64_t uptime_ms;
};

struct photodiode_dark_result {
	enum photodiode_channel channel;
	uint16_t samples;
	bool stored;
	float mean_mv;
	float rms_mv;
	float min_mv;
	float max_mv;
	float previous_dark_mv;
	float configured_dark_mv;
	float lowest_dark_mv;
	bool lowest_dark_valid;
};


extern struct k_msgq photodiode_queue;
void photodiode_thread(void *p1, void *p2, void *p3);
int photodiode_channel_from_name(const char *name, enum photodiode_channel *channel);
const char *photodiode_channel_name(enum photodiode_channel channel);
void photodiode_get_status(struct photodiode_status *out);
int photodiode_measure_dark(enum photodiode_channel channel, uint16_t samples,
			    bool store, struct photodiode_dark_result *out);
int photodiode_reset_lowest_dark(enum photodiode_channel channel, bool persist);

#endif //PHOTODIODE_H
