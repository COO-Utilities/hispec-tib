/**
 * @file photodiode.h
 * @brief TIB photodiode sampling, rolling status windows, and dark-calibration state.
 *
 * The sampler thread owns ADC reads, per-channel moving windows, and dark
 * calibration snapshots. Command handlers can request internal configurable
 * window changes or dark captures; they do not read the ADC.
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
#define PHOTODIODE_YJ_DEFAULT_NOISE_WARN_RMS_MV 10.0
#define PHOTODIODE_HK_DEFAULT_NOISE_WARN_RMS_MV 10.0
#define PHOTODIODE_YJ_DEFAULT_RESPONSIVITY_A_PER_W 0.93
#define PHOTODIODE_HK_DEFAULT_RESPONSIVITY_A_PER_W 0.60971
#define PHOTODIODE_YJ_DEFAULT_TRANSIMPEDANCE_V_PER_A 5.0e10
#define PHOTODIODE_HK_DEFAULT_TRANSIMPEDANCE_V_PER_A 2.375e9
#define PHOTODIODE_FIXED_WINDOW_MS 500U
#define PHOTODIODE_INSTANT_ERR_MV 0.1875
#define PHOTODIODE_FORCED_DARK_RMS_DEFAULT_MV 1.5

struct app_pd_channel_settings;

enum photodiode_channel {
	PHOTODIODE_CHANNEL_YJ = 0,
	PHOTODIODE_CHANNEL_HK = 1
};

struct photodiode_window_result {
	bool valid;
	uint16_t sample_length;
	uint16_t failed_samples;
	int64_t end_ms;
	double mean_mv;
	double mean_net_mv;
	double rms_mv;
	double mean_net_err_mv;
	double min_mv;
	double max_mv;
	double power_uw;
	double power_err_uw;
	int16_t max_raw;
};

struct photodiode_channel_status {
	int16_t raw;
	double mv;
	double net_mv;
	double net_err_mv;
	double power_uw;
	double power_err_uw;
	uint32_t age_ms;
	struct photodiode_window_result configurable_window;
	struct photodiode_window_result last_configurable_window;
	struct photodiode_window_result fixed_window;
	struct photodiode_window_result last_fixed_window;
	struct photodiode_window_result dark_window;
	struct photodiode_window_result lowest_dark_window;
	bool dark_pending;
	uint32_t dark_duration_ms;
};

struct photodiode_status {
	struct photodiode_channel_status channel[PHOTODIODE_CHANNEL_COUNT];
};

/** Channel labels used in command replies and telemetry JSON. */
extern const char *const photodiode_channel_names[PHOTODIODE_CHANNEL_COUNT];
/** @brief Background sampler thread; blocks on ADC reads and periodic sleeps. */
void photodiode_thread(void *p1, void *p2, void *p3);

/** @brief Copy latest sample, calibration, and moving-window status. */
void photodiode_get_status(struct photodiode_status *out);

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
 * @brief Set the per-channel internal configurable moving-window duration.
 *
 * Zero requests the shortest supported one-sample window. The implementation
 * rounds to the nearest whole sample and clamps to the maximum dark/window
 * duration. Changing duration closes the current configurable window into
 * last_configurable_window and starts a fresh current window.
 */
int photodiode_set_configurable_window_duration(enum photodiode_channel channel,
						uint32_t duration_ms);

/**
 * @brief Start a sampler-owned dark capture using the configurable window.
 *
 * The command returns immediately after arming the capture. The sampler commits
 * the completed configurable window as dark when the requested sample count is
 * reached. No laser, route, or attenuator policy is checked here.
 */
int photodiode_start_dark_capture(enum photodiode_channel channel,
				  uint32_t duration_ms,
				  bool persist,
				  bool reset_lowest);

/** @brief Force one channel's dark result to a user-supplied value. */
int photodiode_force_dark(enum photodiode_channel channel,
			  double mean_mv,
			  double rms_mv,
			  bool persist,
			  bool reset_lowest);

/** @brief Reset the lowest-dark record to the active dark for one channel. */
int photodiode_reset_lowest_dark(enum photodiode_channel channel,
				 bool persist);

#endif //PHOTODIODE_H
