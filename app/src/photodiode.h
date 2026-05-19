/**
 * @file photodiode.h
 * @brief TIB photodiode sampling, rolling status windows, and dark-calibration state.
 *
 * The sampler thread owns ADC reads and dark-measurement accumulation. Command
 * handlers can start/reset/query dark calibration but do not read the ADC or
 * wait for an entire measurement interval.
 */

#ifndef PHOTODIODE_H
#define PHOTODIODE_H

#include <zephyr/kernel.h>
#include <stdbool.h>
#include <stdint.h>

#define PUBLISH_INTERVAL_MS 20

#define PHOTODIODE_CHANNEL_COUNT 2

struct app_pd_channel_settings;

enum photodiode_channel {
	PHOTODIODE_CHANNEL_YJ = 0,
	PHOTODIODE_CHANNEL_HK = 1
};

enum photodiode_dark_state {
	PHOTODIODE_DARK_IDLE = 0,
	PHOTODIODE_DARK_MEASURING,
	PHOTODIODE_DARK_COMPLETE,
	PHOTODIODE_DARK_ERROR
};

struct photodiode_channel_status {
	bool valid;
	int last_error;
	int16_t raw;
	float mv;
	float net_mv;
	float power_uw;
	float noise_rms_mv;
	float mean_mv_1s;
	float rms_mv_0p5s;
	float dark_mv;
	float lowest_dark_mv;
	bool lowest_dark_valid;
	enum photodiode_dark_state dark_state;
	uint32_t dark_duration_ms;
	uint32_t dark_samples;
	uint32_t dark_target_samples;
	int dark_last_error;
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
	uint32_t samples;
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

struct photodiode_dark_status {
	enum photodiode_channel channel;
	enum photodiode_dark_state state;
	bool store;
	uint32_t duration_ms;
	uint32_t samples;
	uint32_t target_samples;
	int last_error;
	struct photodiode_dark_result result;
};

/** Channel labels used in command replies and telemetry JSON. */
extern const char *const photodiode_channel_names[PHOTODIODE_CHANNEL_COUNT];
/** @brief Background sampler thread; blocks on ADC reads and periodic sleeps. */
void photodiode_thread(void *p1, void *p2, void *p3);

/** @brief Copy the latest sample, calibration, and dark-measurement status. */
void photodiode_get_status(struct photodiode_status *out);

/** @brief Convert a dark-measurement state enum to command JSON text. */
const char *photodiode_dark_state_name(enum photodiode_dark_state state);

/**
 * @brief Convert dark-subtracted ADC millivolts to optical power in uW.
 *
 * Uses the app-owned photodiode responsivity and transimpedance settings. This
 * helper performs no I/O and returns zero for non-positive signal or invalid
 * response settings.
 */
double photodiode_power_uw_from_mv(double net_mv,
				   const struct app_pd_channel_settings *settings);

/**
 * @brief Convert dark-subtracted ADC millivolts to photon flux.
 *
 * Uses app-owned response settings plus the caller-provided wavelength. This
 * helper performs no I/O and returns zero for non-positive signal or invalid
 * wavelength/response settings.
 */
double photodiode_photon_flux_from_mv(double net_mv,
				      double wavelength_nm,
				      const struct app_pd_channel_settings *settings);
/**
 * @brief Start or restart a dark measurement on the sampling thread.
 *
 * @param channel Photodiode channel to measure.
 * @param duration_ms Requested measurement window in milliseconds. Zero uses
 * the firmware default. The implementation rounds to the nearest whole sample.
 * @param store If true, update stored dark and lowest-dark when complete.
 * @param out Optional status populated immediately after the request is armed.
 *
 * A repeated request for the same channel discards the previous in-progress
 * accumulator and starts a fresh window. This call does not wait for the
 * measurement interval.
 *
 * @retval 0 Measurement was started.
 * @retval -EINVAL Bad channel.
 * @retval -ENODEV Photodiodes are unavailable or ADC is not ready.
 */
int photodiode_start_dark_measurement(enum photodiode_channel channel,
				      uint32_t duration_ms,
				      bool store,
				      struct photodiode_dark_status *out);
/** @brief Copy current or last dark-measurement state for one channel. */
int photodiode_get_dark_status(enum photodiode_channel channel,
			       struct photodiode_dark_status *out);
/**
 * @brief Clear lowest-dark tracking for one channel.
 *
 * The current configured dark level is left unchanged. If @p persist is true,
 * only the selected channel's photodiode settings are saved.
 */
int photodiode_reset_lowest_dark(enum photodiode_channel channel, bool persist);

#endif //PHOTODIODE_H
