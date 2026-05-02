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

#define PHOTODIODE_CHANNEL_COUNT 2

enum photodiode_channel {
	PHOTODIODE_CHANNEL_YJ = 0,
	PHOTODIODE_CHANNEL_HK = 1
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
	uint32_t duration_ms;
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
/** Channel labels used in command replies and telemetry JSON. */
extern const char *const photodiode_channel_names[PHOTODIODE_CHANNEL_COUNT];
void photodiode_thread(void *p1, void *p2, void *p3);
void photodiode_get_status(struct photodiode_status *out);
/**
 * @brief Measure dark level using the regular photodiode sampling thread.
 *
 * @param channel Photodiode channel to measure.
 * @param duration_ms Requested measurement window in milliseconds. Zero uses
 * the firmware default. The implementation rounds to the nearest whole sample.
 * @param store If true, update the stored dark level and lowest-dark tracking.
 * @param out Result populated after the sampling thread latches the window.
 *
 * @retval 0 Measurement completed.
 * @retval -EINVAL Bad channel or output pointer.
 * @retval -ENODEV Photodiodes are unavailable or ADC is not ready.
 * @retval -EBUSY Another dark measurement is active.
 * @retval -ETIMEDOUT The sampling thread did not complete the window.
 */
int photodiode_measure_dark(enum photodiode_channel channel, uint32_t duration_ms,
			    bool store, struct photodiode_dark_result *out);
/**
 * @brief Clear lowest-dark tracking for one channel.
 *
 * The current configured dark level is left unchanged. If @p persist is true,
 * only the selected channel's photodiode settings are saved.
 */
int photodiode_reset_lowest_dark(enum photodiode_channel channel, bool persist);

#endif //PHOTODIODE_H
