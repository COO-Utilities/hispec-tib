/**
 * @file photodiode.h
 * @brief TIB photodiode sampling, rolling status windows, and dark-calibration state.
 *
 * The sampler thread owns ADC reads and short averaging. Command handlers can
 * start/reset/query dark calibration but do not read the ADC or wait for an
 * entire measurement interval.
 */

#ifndef PHOTODIODE_H
#define PHOTODIODE_H

#include <zephyr/kernel.h>
#include <stdbool.h>
#include <stdint.h>

#define PUBLISH_INTERVAL_MS 20

#define PHOTODIODE_CHANNEL_COUNT 2

#define PHOTODIODE_DEFAULT_DARK_MV 0.0
#define PHOTODIODE_DEFAULT_LOWEST_DARK_MV 0.0
#define PHOTODIODE_DEFAULT_DARK_NOISE_RMS_MV 0.0
#define PHOTODIODE_DARK_MIN_MV -5000.0
#define PHOTODIODE_DARK_MAX_MV 5000.0
#define PHOTODIODE_NOISE_RMS_MIN_MV 0.0
#define PHOTODIODE_NOISE_RMS_MAX_MV 5000.0
#define PHOTODIODE_RESPONSIVITY_MIN_A_PER_W 0.000001
#define PHOTODIODE_RESPONSIVITY_MAX_A_PER_W 10.0
#define PHOTODIODE_TRANSIMPEDANCE_MIN_V_PER_A 1.0
#define PHOTODIODE_TRANSIMPEDANCE_MAX_V_PER_A 1.0e12
#define PHOTODIODE_YJ_DEFAULT_NOISE_WARN_RMS_MV 3.0
#define PHOTODIODE_HK_DEFAULT_NOISE_WARN_RMS_MV 1.0
#define PHOTODIODE_YJ_DEFAULT_RESPONSIVITY_A_PER_W 0.93
#define PHOTODIODE_HK_DEFAULT_RESPONSIVITY_A_PER_W 0.60971
#define PHOTODIODE_YJ_DEFAULT_TRANSIMPEDANCE_V_PER_A 5.0e10
#define PHOTODIODE_HK_DEFAULT_TRANSIMPEDANCE_V_PER_A 2.375e9

struct app_pd_channel_settings;

enum photodiode_channel {
	PHOTODIODE_CHANNEL_YJ = 0,
	PHOTODIODE_CHANNEL_HK = 1
};

enum photodiode_average_state {
	PHOTODIODE_AVERAGE_INACTIVE = 0,
	PHOTODIODE_AVERAGE_MEASURING,
	PHOTODIODE_AVERAGE_COMPLETE,
	PHOTODIODE_AVERAGE_ERROR
};

struct photodiode_channel_status {
	bool valid;
	int last_error;
	int16_t raw;
	double mv;
	double net_mv;
	double power_uw;
	double noise_rms_mv;
	double mean_mv_1s;
	double rms_mv_0p5s;
	double dark_mv;
	double lowest_dark_mv;
	bool lowest_dark_valid;
	enum photodiode_average_state average_state;
	uint32_t average_duration_ms;
	uint32_t average_samples;
	uint32_t average_target_samples;
	int average_last_error;
	uint32_t age_ms;
	uint32_t sample_count;
};

struct photodiode_status {
	struct photodiode_channel_status channel[PHOTODIODE_CHANNEL_COUNT];
	int64_t uptime_ms;
};

struct photodiode_average_result {
	enum photodiode_channel channel;
	uint32_t duration_ms;
	uint32_t samples;
	uint32_t target_samples;
	double mean_mv;
	double mean_net_mv;
	double rms_mv;
	double min_mv;
	double max_mv;
	int16_t max_raw;
};

struct photodiode_average_status {
	enum photodiode_channel channel;
	enum photodiode_average_state state;
	bool store_dark;
	int last_error;
	struct photodiode_average_result result;
};

/** Channel labels used in command replies and telemetry JSON. */
extern const char *const photodiode_channel_names[PHOTODIODE_CHANNEL_COUNT];
/** @brief Background sampler thread; blocks on ADC reads and periodic sleeps. */
void photodiode_thread(void *p1, void *p2, void *p3);

/** @brief Copy latest sample, calibration, and short-average progress. */
void photodiode_get_status(struct photodiode_status *out);

/** @brief Convert an average-measurement state enum to command JSON text. */
const char *photodiode_average_state_name(enum photodiode_average_state state);

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
 * @brief Convert dark-subtracted ADC millivolts to wavelength-corrected power.
 *
 * Uses the nearest nominal laser wavelength's photodiode correction coefficient
 * plus app-owned response settings. The current coefficient table is fixed in
 * firmware, performs no I/O, and uses unity coefficients until lab values are
 * installed.
 */
double photodiode_power_uw_from_mv_at_wavelength(
	double net_mv,
	double wavelength_nm,
	const struct app_pd_channel_settings *settings);

/**
 * @brief Convert dark-subtracted ADC millivolts to photon flux.
 *
 * Uses app-owned response settings plus the caller-provided wavelength and
 * nearest nominal-laser photodiode correction. This helper performs no I/O and
 * returns zero for non-positive signal or invalid wavelength/response settings.
 */
double photodiode_photon_flux_from_mv(double net_mv,
				      double wavelength_nm,
				      const struct app_pd_channel_settings *settings);

/** @brief Validate app-owned photodiode response/dark settings. Performs no I/O. */
bool photodiode_settings_valid(const struct app_pd_channel_settings *settings);

/**
 * @brief Start or restart a dark measurement on the sampling thread.
 *
 * @param channel Photodiode channel to measure.
 * @param duration_ms Requested measurement window in milliseconds. Zero uses
 * the firmware default. The implementation rounds to the nearest whole sample
 * and clamps to the short-average maximum duration.
 * @param store If true, update stored dark and lowest-dark when complete.
 * @param out Optional status populated immediately after the request is armed.
 *
 * This is a short average tagged to update dark calibration on completion when
 * @p store is true. A repeated dark request for the same channel discards the
 * previous in-progress accumulator and starts a fresh window. This call does
 * not wait for the measurement interval.
 *
 * @retval 0 Measurement was started.
 * @retval -EINVAL Bad channel.
 * @retval -ENODEV Photodiodes are unavailable or ADC is not ready.
 */
int photodiode_start_dark_measurement(enum photodiode_channel channel,
				      uint32_t duration_ms,
				      bool store,
				      struct photodiode_average_status *out);
/**
 * @brief Start a short non-persistent average on the sampling thread.
 *
 * The accumulator samples the same ADC snapshots as normal photodiode status
 * and reports raw and dark-subtracted means. It performs no persistence and
 * does not block for the requested window.
 */
int photodiode_start_average(enum photodiode_channel channel,
			     uint32_t duration_ms,
			     struct photodiode_average_status *out);
/** @brief Copy current or last short-average state for one channel. */
int photodiode_get_average_status(enum photodiode_channel channel,
				  struct photodiode_average_status *out);
/**
 * @brief Clear lowest-dark tracking for one channel.
 *
 * The current configured dark level is left unchanged. If @p persist is true,
 * only the selected channel's photodiode settings are saved.
 */
int photodiode_reset_lowest_dark(enum photodiode_channel channel, bool persist);

#endif //PHOTODIODE_H
