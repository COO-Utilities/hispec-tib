/**
 * @file throughput_monitor.c
 * @brief Throughput streaming and autolevel control.
 */

#include "throughput_monitor.h"

#include <ctype.h>
#include <errno.h>
#include <math.h>
#include <string.h>
#include <time.h>

#include "app_settings.h"
#include "attenuator.h"
#include "attenuator_calibration.h"
#include "command.h"
#include "devices.h"
#include "housekeeping.h"

#include <coo_commons/json_utils.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/sys/clock.h>
#include <zephyr/sys/util.h>

LOG_MODULE_REGISTER(throughput_monitor, LOG_LEVEL_INF);

#define TP_INTERVAL_MS 100U
#define TP_LOW_FRACTION 0.20
#define TP_HIGH_FRACTION 0.80
#define TP_INSTANT_BAD_SAMPLES 5U
#define TP_MIN_ATTEN_TX 1.0e-9

BUILD_ASSERT((int)HOUSEKEEPING_POWER_YJ_PHOTODIODE == (int)PHOTODIODE_CHANNEL_YJ,
	     "YJ photodiode relay index must match photodiode channel");
BUILD_ASSERT((int)HOUSEKEEPING_POWER_HK_PHOTODIODE == (int)PHOTODIODE_CHANNEL_HK,
	     "HK photodiode relay index must match photodiode channel");
BUILD_ASSERT(IS_ENABLED(CONFIG_LITTLE_ENDIAN),
	     "throughput binary telemetry uses little-endian double layout");

struct laser_pd_channel {
	enum hispec_laser_id laser;
	enum photodiode_channel channel;
};

struct throughput_state {
	bool active;
	bool autolevel;
	bool binary;
	bool has_laser;
	enum hispec_laser_id laser;
	enum photodiode_channel channel;
	uint8_t attenuator_index;
	char fiber;
	double level_percent;
	int64_t started_ms;
	uint32_t off_in_s;
	double max_flux_ph_s;
	uint8_t high_count;
	uint8_t low_count;
	bool acquiring;
	int64_t input_changed_ms;
	/* Cached input estimate: refreshed after writes, never during publication. */
	struct attenuator_transmission_estimate atten;
	struct hispec_laser_flux_estimate laser_flux;
	double pd_route_tx;
	double laser_route_tx;
	double pd_flux_per_mv;
	double emitted_flux;
	double emitted_flux_err;
	/* Autolevel's laser must still stop after manual attenuation disables adjustments. */
	bool stop_laser;
};

static const struct laser_pd_channel laser_pd_channels[] = {
	{HISPEC_LASER_1028_Y, PHOTODIODE_CHANNEL_YJ},
	{HISPEC_LASER_1270_J, PHOTODIODE_CHANNEL_YJ},
	{HISPEC_LASER_1430_YJ, PHOTODIODE_CHANNEL_YJ},
	{HISPEC_LASER_1430_HK, PHOTODIODE_CHANNEL_HK},
	{HISPEC_LASER_1510_H, PHOTODIODE_CHANNEL_HK},
	{HISPEC_LASER_2330_K, PHOTODIODE_CHANNEL_HK},
};

/* Both loops are available for engineering use. Instrument light paths combine
 * outside this controller, so normal operation should use only one autolevel loop.
 */
static struct throughput_state monitors[PHOTODIODE_CHANNEL_COUNT];
static K_MUTEX_DEFINE(monitors_lock);
/* The throughput thread is the only user of these scratch objects. Keeping the
 * large snapshots and publish buffer in BSS leaves stack headroom for
 * calibration, autolevel, and formatting calls made from that thread.
 */
static struct photodiode_status throughput_pd_status;
static struct throughput_state throughput_local[PHOTODIODE_CHANNEL_COUNT];
static struct coo_cmd_response throughput_sample_msg;

static enum housekeeping_power_output pd_power_output(enum photodiode_channel channel)
{
	return (enum housekeeping_power_output)channel;
}

static int photodiode_channel_for_laser(enum hispec_laser_id laser,
					enum photodiode_channel *channel)
{
	if (channel == NULL) {
		return -EINVAL;
	}

	for (uint8_t i = 0U; i < ARRAY_SIZE(laser_pd_channels); ++i) {
		if (laser_pd_channels[i].laser == laser) {
			*channel = laser_pd_channels[i].channel;
			return 0;
		}
	}

	return -ENOENT;
}

static void route_name_for_pd(char *buf, size_t buf_len,
			      enum photodiode_channel channel, char fiber)
{
	const char *prefix = channel == PHOTODIODE_CHANNEL_YJ ? "yj" : "hk";
	const char *kind = (fiber == 'M') ? "mm" : "sm";

	snprintk(buf, buf_len, "%s_%s_to_%s_pd", prefix, kind, prefix);
}

static void route_name_for_laser(char *buf, size_t buf_len,
				 const char *laser, char fiber)
{
	snprintk(buf, buf_len, "%s_to_%c", laser, fiber);
}

static void channel_fiber_name(char *buf, size_t buf_len,
			       enum photodiode_channel channel, char fiber)
{
	snprintk(buf, buf_len, "%s_%c", photodiode_channel_names[channel],
		 (char)tolower((unsigned char)fiber));
}

static uint64_t realtime_ms(void)
{
	struct timespec ts = {0};

	(void)sys_clock_gettime(SYS_CLOCK_REALTIME, &ts);
	return ((uint64_t)ts.tv_sec * 1000ULL) + ((uint64_t)ts.tv_nsec / 1000000ULL);
}

/* Relinquish ownership without changing a manual command's laser setting. */
static void release_locked(enum photodiode_channel channel)
{
	if (monitors[channel].active) {
		housekeeping_photodiode_autooff_inhibit(pd_power_output(channel), false);
	}
	photodiode_set_throughput_reference(channel,
		(struct photodiode_throughput_reference){0}, true);
	memset(&monitors[channel], 0, sizeof(monitors[channel]));
}

/* May block on Modbus. Retain the laser identity after failure for stop retry. */
static int stop_locked(enum photodiode_channel channel)
{
	struct throughput_state *state = &monitors[channel];

	if (state->active) {
		housekeeping_photodiode_autooff_inhibit(pd_power_output(channel), false);
	}
	photodiode_set_throughput_reference(channel,
		(struct photodiode_throughput_reference){0}, true);
	state->active = false;
	state->autolevel = false;
	if (state->stop_laser) {
		int rc = hispec_laser_stop_output(state->laser, false);

		if (rc != 0) {
			LOG_ERR("Throughput stopped; laser %s shutdown failed (%d), retry stop",
				hispec_laser_name(state->laser), rc);
			return rc;
		}
	}
	release_locked(channel);
	return 0;
}

static void put_bytes(uint8_t *payload, size_t payload_len, size_t *offset,
		      const void *src, size_t src_len)
{
	if (*offset + src_len > payload_len) {
		return;
	}

	memcpy(payload + *offset, src, src_len);
	*offset += src_len;
}

static void put_u64(uint8_t *payload, size_t payload_len, size_t *offset, uint64_t value)
{
	uint8_t encoded[sizeof(value)];

	sys_put_le64(value, encoded);
	put_bytes(payload, payload_len, offset, encoded, sizeof(encoded));
}

static void put_i16(uint8_t *payload, size_t payload_len, size_t *offset, int16_t value)
{
	uint8_t encoded[sizeof(value)];

	sys_put_le16((uint16_t)value, encoded);
	put_bytes(payload, payload_len, offset, encoded, sizeof(encoded));
}

static void put_f64(uint8_t *payload, size_t payload_len, size_t *offset, double value)
{
	/* STM32 binary telemetry is specified as little-endian IEEE-754. */
	put_bytes(payload, payload_len, offset, &value, sizeof(value));
}

/* Capture actual DAC state and cached laser state after an input write. May
 * block on DAC/settings locks, but runs in the monitor/command thread, never
 * the ADC sampler. Invalid source estimates disable normalization until a
 * usable reference is available; an input change alone does not clear history.
 */
static void refresh_reference(struct throughput_state *state)
{
	struct app_photodiode_settings pd_settings;
	struct photodiode_throughput_reference reference = {0};
	char pd_route[APP_ROUTE_LOSS_ROUTE_MAX_LEN];
	char laser_route[APP_ROUTE_LOSS_ROUTE_MAX_LEN];

	state->atten = (struct attenuator_transmission_estimate){
		.linear = NAN, .linear_err = NAN, .attenuation_db = NAN};
	state->laser_flux = (struct hispec_laser_flux_estimate){0};
	state->pd_route_tx = 1.0;
	state->laser_route_tx = 1.0;
	state->pd_flux_per_mv = NAN;
	state->emitted_flux = NAN;
	state->emitted_flux_err = NAN;
	if (state->has_laser &&
	    attenuator_estimate_transmission(&attenuators[state->attenuator_index],
					     0.0, 0.0, &state->atten) &&
	    laser_estimate_flux(state->laser, 0.0, 0.0, &state->laser_flux) == 0) {
		const char *name = hispec_laser_name(state->laser);

		app_settings_get_photodiode(&pd_settings);
		route_name_for_pd(pd_route, sizeof(pd_route), state->channel, state->fiber);
		route_name_for_laser(laser_route, sizeof(laser_route), name, state->fiber);
		(void)app_settings_get_route_loss(pd_route, name, &state->pd_route_tx);
		(void)app_settings_get_route_loss(laser_route, name, &state->laser_route_tx);
		state->pd_flux_per_mv = photodiode_photon_flux_from_mv(1.0,
			state->laser_flux.wavelength_nm, &pd_settings.channel[state->channel]) /
			state->pd_route_tx;
		state->emitted_flux = state->laser_flux.flux_ph_s * state->atten.linear *
			state->laser_route_tx;
		state->emitted_flux_err = hypot(
			state->laser_flux.flux_err_ph_s * state->atten.linear,
			state->laser_flux.flux_ph_s * state->atten.linear_err) * state->laser_route_tx;
		if (isfinite(state->emitted_flux) && state->emitted_flux > 0.0) {
			reference.scale_per_mv = state->pd_flux_per_mv / state->emitted_flux;
			reference.source_relative_error = state->emitted_flux_err / state->emitted_flux;
		}
	}
	photodiode_set_throughput_reference(state->channel, reference, false);
}

/* Ordinary moves use a full process window since the last input change. Initial
 * acquisition and consecutive instantaneous out-of-range observations bypass
 * that gate. Bright backoff wins if the instantaneous and mean signals disagree.
 * Returns true after a hardware write attempt (including partial failure).
 */
static bool autolevel_adjust(struct throughput_state *state,
			    const struct photodiode_channel_status *pd,
			    const struct attenuator_transmission_estimate *atten)
{
	double mean_net_mv = pd->fixed_window.valid ?
			     (double)pd->fixed_window.mean_net_mv :
			     (double)pd->net_mv;
	bool low = mean_net_mv < (PHOTODIODE_ADC_USABLE_MV * TP_LOW_FRACTION);
	bool high = mean_net_mv > (PHOTODIODE_ADC_USABLE_MV * TP_HIGH_FRACTION);
	struct hispec_laser_flux_estimate laser_flux = {0};
	double emitted_flux = 0.0;
	double max_tx = 1.0;
	double next_tx;
	double next_percent;
	int rc;
	bool changed = false;

	bool instant_high = pd->raw > INT16_MAX - 1024 ||
		pd->net_mv > PHOTODIODE_ADC_USABLE_MV * TP_HIGH_FRACTION;
	bool instant_low = pd->net_mv < PHOTODIODE_ADC_USABLE_MV * TP_LOW_FRACTION;

	state->high_count = instant_high ? MIN(state->high_count + 1U, TP_INSTANT_BAD_SAMPLES) : 0U;
	state->low_count = instant_low ? MIN(state->low_count + 1U, TP_INSTANT_BAD_SAMPLES) : 0U;
	if (!instant_low) {
		state->acquiring = false;
	}
	bool fast_high = state->high_count >= TP_INSTANT_BAD_SAMPLES;
	bool fast_low = state->low_count >= TP_INSTANT_BAD_SAMPLES ||
		(state->acquiring && instant_low);
	bool ordinary_ready = pd->fixed_window.valid &&
		pd->fixed_window.end_ms - state->input_changed_ms >= PHOTODIODE_FIXED_WINDOW_MS;

	/* Do not raise flux from a lagging low mean while the newest sample is bright. */
	high = fast_high || (ordinary_ready && high) || (fast_low && instant_high);
	low = !instant_high && (fast_low || (ordinary_ready && low));
	if (!high && !low) {
		return false;
	}

	if (low && !high) {
		if (state->max_flux_ph_s > 0.0 &&
		    laser_estimate_flux(state->laser, 0.0, 0.0, &laser_flux) == 0 &&
		    laser_flux.flux_ph_s > 0.0) {
			emitted_flux = laser_flux.flux_ph_s * atten->linear;
			max_tx = state->max_flux_ph_s / laser_flux.flux_ph_s;
			if (max_tx > 1.0) {
				max_tx = 1.0;
			}
		}
		if (atten->linear < 0.999) {
			next_tx = atten->linear * 3.0;
			if (next_tx > 1.0) {
				next_tx = 1.0;
			}
			if (state->max_flux_ph_s > 0.0 && next_tx > max_tx) {
				next_tx = max_tx;
			}
			if (next_tx <= atten->linear) {
				state->low_count = 0U;
				return false;
			}
			changed = true;
			if (!attenuator_set_linear(&attenuators[state->attenuator_index], next_tx)) {
				LOG_WRN("Throughput attenuator adjustment failed");
			}
		} else if (state->level_percent < 100.0) {
			next_percent = state->level_percent * 3.0;
			if (next_percent > 100.0) {
				next_percent = 100.0;
			}
			if (state->max_flux_ph_s > 0.0 && emitted_flux > 0.0) {
				double capped = (double)((double)state->level_percent *
						       state->max_flux_ph_s / emitted_flux);

				if (capped < next_percent) {
					next_percent = capped;
				}
			}
			if (next_percent <= state->level_percent) {
				state->low_count = 0U;
				return false;
			}
			changed = true;
			rc = hispec_laser_set_output_percent_autooff(state->laser,
								     next_percent, 0U);
			if (rc == 0) {
				state->level_percent = next_percent;
			}
		}
		state->low_count = 0U;
		return changed;
	}

	if (high) {
		if (atten->linear > TP_MIN_ATTEN_TX) {
			next_tx = atten->linear / 3.0;
			if (next_tx < TP_MIN_ATTEN_TX) {
				next_tx = TP_MIN_ATTEN_TX;
			}
			changed = true;
			if (!attenuator_set_linear(&attenuators[state->attenuator_index], next_tx)) {
				LOG_WRN("Throughput attenuator adjustment failed");
			}
		} else if (state->level_percent > 0.0) {
			next_percent = state->level_percent / 3.0;
			if (next_percent < 0.0) {
				next_percent = 0.0;
			}
			changed = true;
			rc = hispec_laser_set_output_percent_autooff(state->laser,
								     next_percent, 0U);
			if (rc == 0) {
				state->level_percent = next_percent;
			}
		}
		state->high_count = 0U;
	}

	return changed;
}

static void publish_sample(const struct throughput_state *state,
			   const struct photodiode_channel_status *pd,
			   uint64_t time_ms)
{
	const struct attenuator_transmission_estimate atten = state->atten;
	const struct hispec_laser_flux_estimate laser_flux = state->laser_flux;
	struct coo_cmd_response *msg = &throughput_sample_msg;
	size_t off = 0U;
	const char *laser_name = state->has_laser ? hispec_laser_name(state->laser) : "none";
	const char *topic_suffix = state->channel == PHOTODIODE_CHANNEL_YJ ?
				   "yj_tput" : "hk_tput";
	char channel_fiber[8] = {0};
	double pd_route_tx = state->pd_route_tx;
	double laser_route_tx = state->laser_route_tx;
	double pd_flux = NAN;
	double pd_flux_err = NAN;
	double emitted_flux = state->emitted_flux;
	double emitted_flux_err = state->emitted_flux_err;
	double tp = pd->throughput.samples ? pd->throughput.mean : (double)NAN;
	double tp_err = pd->throughput.samples ? pd->throughput.error : (double)NAN;
	double tp_rms_err = pd->throughput.samples ? pd->throughput.pd_error : (double)NAN;
	uint64_t pd_ontime_s;
	uint64_t laser_current_ontime_s;
	double pd_mean_net_mv;
	double pd_mean_net_err_mv;

	if (laser_name == NULL) {
		return;
	}
	memset(msg, 0, sizeof(*msg));

	channel_fiber_name(channel_fiber, sizeof(channel_fiber), state->channel, state->fiber);
	pd_ontime_s = (uint64_t)housekeeping_power_on_time_s(pd_power_output(state->channel));
	laser_current_ontime_s = state->has_laser ?
				  (uint64_t)hispec_laser_current_on_time_s(state->laser) : 0U;
	pd_mean_net_mv = pd->fixed_window.valid ?
			 (double)pd->fixed_window.mean_net_mv : (double)pd->net_mv;
	pd_mean_net_err_mv = pd->fixed_window.valid ?
			     (double)pd->fixed_window.mean_net_err_mv :
			     (double)pd->net_err_mv;
	/* Raw-window diagnostic flux is separate from the mean of normalized samples;
	 * during input changes tp is intentionally not pd_flux / emitted_flux.
	 */
	pd_flux = MAX(pd_mean_net_mv, 0.0) * state->pd_flux_per_mv;
	pd_flux_err = pd_mean_net_err_mv * state->pd_flux_per_mv;

	if (state->binary) {
		put_bytes((uint8_t *)msg->payload, sizeof(msg->payload), &off,
			  channel_fiber, sizeof(channel_fiber));
		put_u64((uint8_t *)msg->payload, sizeof(msg->payload), &off, time_ms);
		put_f64((uint8_t *)msg->payload, sizeof(msg->payload), &off, tp);
		put_f64((uint8_t *)msg->payload, sizeof(msg->payload), &off, tp_err);
		put_f64((uint8_t *)msg->payload, sizeof(msg->payload), &off, tp_rms_err);
		put_f64((uint8_t *)msg->payload, sizeof(msg->payload), &off, pd_flux);
		put_f64((uint8_t *)msg->payload, sizeof(msg->payload), &off, pd_flux_err);
		put_f64((uint8_t *)msg->payload, sizeof(msg->payload), &off, emitted_flux);
		put_f64((uint8_t *)msg->payload, sizeof(msg->payload), &off, emitted_flux_err);
		put_f64((uint8_t *)msg->payload, sizeof(msg->payload), &off, pd_route_tx);
		put_f64((uint8_t *)msg->payload, sizeof(msg->payload), &off, laser_route_tx);
		put_f64((uint8_t *)msg->payload, sizeof(msg->payload), &off, atten.linear);
		put_i16((uint8_t *)msg->payload, sizeof(msg->payload), &off, pd->raw);
		put_f64((uint8_t *)msg->payload, sizeof(msg->payload), &off, pd->mv);
		put_f64((uint8_t *)msg->payload, sizeof(msg->payload), &off, pd->net_mv);
		put_f64((uint8_t *)msg->payload, sizeof(msg->payload), &off, pd_mean_net_mv);
		put_f64((uint8_t *)msg->payload, sizeof(msg->payload), &off,
			pd_mean_net_err_mv);
		put_f64((uint8_t *)msg->payload, sizeof(msg->payload), &off,
			laser_flux.current_ma);
		put_f64((uint8_t *)msg->payload, sizeof(msg->payload), &off,
			atten.attenuation_db);
		put_f64((uint8_t *)msg->payload, sizeof(msg->payload), &off,
			laser_flux.wavelength_nm);
		put_u64((uint8_t *)msg->payload, sizeof(msg->payload), &off,
			pd_ontime_s);
		put_u64((uint8_t *)msg->payload, sizeof(msg->payload), &off,
			laser_current_ontime_s);
		msg->payload_len = off;
		(void)coo_cmd_runtime_emit(
			command_runtime_get(),
			&(const struct coo_cmd_runtime_emit_args){
				.type = COO_CMD_RUNTIME_EMIT_DATA,
				.delivery = COO_CMD_RUNTIME_EMIT_BEST_EFFORT,
				.suffix = topic_suffix,
				.out = msg,
			});
		return;
	}

	if (coo_json_append(msg->payload, sizeof(msg->payload), &off,
			"{\"channel\":\"%s\",\"laser\":\"%s\","
			"\"autolevel\":%s,\"t_ms\":%llu,\"tp\":",
			channel_fiber, laser_name, state->autolevel ? "true" : "false",
			(unsigned long long)time_ms) != 0 ||
	    coo_json_append_float_or_null(msg->payload, sizeof(msg->payload), &off, tp, 6) != 0 ||
	    coo_json_append(msg->payload, sizeof(msg->payload), &off, ",\"tp_err\":") != 0 ||
	    coo_json_append_float_or_null(msg->payload, sizeof(msg->payload), &off, tp_err, 6) != 0 ||
	    coo_json_append(msg->payload, sizeof(msg->payload), &off, ",\"tp_rms_err\":") != 0 ||
	    coo_json_append_float_or_null(msg->payload, sizeof(msg->payload), &off, tp_rms_err, 6) != 0 ||
	    coo_json_append(msg->payload, sizeof(msg->payload), &off, ",\"pd_flux_ph_s\":") != 0 ||
	    coo_json_append_float_or_null(msg->payload, sizeof(msg->payload), &off, pd_flux, 9) != 0 ||
	    coo_json_append(msg->payload, sizeof(msg->payload), &off, ",\"pd_flux_err_ph_s\":") != 0 ||
	    coo_json_append_float_or_null(msg->payload, sizeof(msg->payload), &off, pd_flux_err, 9) != 0 ||
	    coo_json_append(msg->payload, sizeof(msg->payload), &off, ",\"laser_flux_ph_s\":") != 0 ||
	    coo_json_append_float_or_null(msg->payload, sizeof(msg->payload), &off, emitted_flux, 9) != 0 ||
	    coo_json_append(msg->payload, sizeof(msg->payload), &off, ",\"laser_flux_err_ph_s\":") != 0 ||
	    coo_json_append_float_or_null(msg->payload, sizeof(msg->payload), &off, emitted_flux_err, 9) != 0 ||
	    coo_json_append(msg->payload, sizeof(msg->payload), &off,
			",\"pd_route_tx\":%.9g,\"laser_route_tx\":%.9g,"
			"\"atten_tx\":%.12g,\"pd_raw\":%d",
			pd_route_tx, laser_route_tx, atten.linear, pd->raw) != 0 ||
	    coo_json_append(msg->payload, sizeof(msg->payload), &off,
			",\"pd_mv\":%.4f,\"pd_net_mv\":%.4f,"
			"\"pd_mean_net_mv\":%.4f,\"pd_mean_net_err_mv\":%.4f",
			(double)pd->mv, (double)pd->net_mv,
			pd_mean_net_mv, pd_mean_net_err_mv) != 0 ||
	    coo_json_append(msg->payload, sizeof(msg->payload), &off, ",\"laser_current_ma\":") != 0 ||
	    coo_json_append_float_or_null(msg->payload, sizeof(msg->payload), &off,
					  laser_flux.current_ma, 4) != 0 ||
	    coo_json_append(msg->payload, sizeof(msg->payload), &off, ",\"atten_db\":") != 0 ||
	    coo_json_append_float_or_null(msg->payload, sizeof(msg->payload), &off,
					  atten.attenuation_db, 6) != 0 ||
	    coo_json_append(msg->payload, sizeof(msg->payload), &off, ",\"wavelength_nm\":") != 0 ||
	    coo_json_append_float_or_null(msg->payload, sizeof(msg->payload), &off,
					  laser_flux.wavelength_nm, 4) != 0 ||
	    coo_json_append(msg->payload, sizeof(msg->payload), &off,
			",\"pd_ontime_s\":%llu,\"laser_current_ontime_s\":%llu,"
			"\"flags\":[]}",
			(unsigned long long)pd_ontime_s,
			(unsigned long long)laser_current_ontime_s) != 0) {
		LOG_WRN("throughput telemetry payload too large");
		return;
	}
	msg->payload_len = strlen(msg->payload);

	(void)coo_cmd_runtime_emit(
		command_runtime_get(),
		&(const struct coo_cmd_runtime_emit_args){
			.type = COO_CMD_RUNTIME_EMIT_DATA,
			.delivery = COO_CMD_RUNTIME_EMIT_BEST_EFFORT,
			.suffix = topic_suffix,
			.out = msg,
		});
}

void throughput_monitor_thread(void *p1, void *p2, void *p3)
{
	ARG_UNUSED(p1);
	ARG_UNUSED(p2);
	ARG_UNUSED(p3);

	while (1) {
		struct photodiode_status *pd_status = &throughput_pd_status;
		struct throughput_state *local = throughput_local;
		int64_t now = k_uptime_get();
		uint64_t time_ms = realtime_ms();

		photodiode_get_status(pd_status);
		attenuator_calibration_tick(pd_status, now);

		for (uint8_t i = 0U; i < PHOTODIODE_CHANNEL_COUNT; ++i) {
			bool pd_power = false;
			struct throughput_state *state = &monitors[i];

			/* Serialize hardware adjustments with start/stop. A copied state
			 * must never restore laser output after a completed stop.
			 */
			k_mutex_lock(&monitors_lock, K_FOREVER);
			if (!state->active) {
				k_mutex_unlock(&monitors_lock);
				continue;
			}

			if (state->off_in_s > 0U &&
			    now - state->started_ms >= (int64_t)state->off_in_s * 1000) {
				(void)stop_locked((enum photodiode_channel)i);
				k_mutex_unlock(&monitors_lock);
				continue;
			}

			if (housekeeping_power_get(pd_power_output((enum photodiode_channel)i),
						   &pd_power) == 0 && !pd_power) {
				(void)stop_locked((enum photodiode_channel)i);
				k_mutex_unlock(&monitors_lock);
				continue;
			}

			if (state->has_laser && !isfinite(state->atten.linear)) {
				refresh_reference(state);
			}
			/* A command may have replaced this measurement since the calibration
			 * tick's earlier snapshot. Capture PD status under the monitor lock.
			 */
			photodiode_get_status(pd_status);
			/* Capture the source snapshot before selecting the NEXT process input. */
			local[i] = *state;
			if (isfinite(pd_status->channel[i].mv) && state->has_laser &&
			    state->autolevel && isfinite(state->atten.linear) &&
			    autolevel_adjust(state, &pd_status->channel[i], &state->atten)) {
				state->input_changed_ms = k_uptime_get();
				refresh_reference(state);
			}
			k_mutex_unlock(&monitors_lock);

			publish_sample(&local[i], &pd_status->channel[i], time_ms);
		}

		k_sleep(K_MSEC(TP_INTERVAL_MS));
	}
}

int throughput_monitor_start(const struct throughput_monitor_request *request,
			     struct throughput_monitor_status *status)
{
	enum photodiode_channel channel;
	enum housekeeping_power_output pd_power;
	uint8_t attenuator_index;
	struct app_photodiode_settings pd_settings;
	struct throughput_state next = {0};
	int rc;

	if (request == NULL) {
		return -EINVAL;
	}

	if (request->fiber != 'M' && request->fiber != 'S') {
		return -EINVAL;
	}
	if (request->autolevel && attenuator_calibration_active()) {
		return -EBUSY;
	}
	if (request->has_laser) {
		if (photodiode_channel_for_laser(request->laser, &channel) != 0) {
			return -EINVAL;
		}
		rc = attenuator_index_from_laser_id(request->laser, &attenuator_index);
		if (rc != 0) {
			return rc;
		}
	} else {
		if (request->autolevel ||
		    request->channel < 0 || request->channel >= PHOTODIODE_CHANNEL_COUNT) {
			return -EINVAL;
		}
		channel = request->channel;
		attenuator_index = 0U;
	}

	app_settings_get_photodiode(&pd_settings);
	if (pd_settings.channel[channel].power == APP_PD_POWER_OVERRIDE_OFF) {
		return -EACCES;
	}

	k_mutex_lock(&monitors_lock, K_FOREVER);
	/* Finish the previous autolevel operation before replacing its source;
	 * also retry a failed shutdown before accepting a new operation.
	 * Passive monitoring never assumes control of a manual laser setting.
	 */
	if (monitors[channel].stop_laser &&
	    (!monitors[channel].active || !request->has_laser ||
	     monitors[channel].laser != request->laser)) {
		rc = stop_locked(channel);
		if (rc != 0) {
			k_mutex_unlock(&monitors_lock);
			return rc;
		}
	}
	pd_power = pd_power_output(channel);
	rc = housekeeping_power_set(pd_power, true);
	if (rc != 0) {
		k_mutex_unlock(&monitors_lock);
		return rc;
	}
	/*
	 * Throughput owns this stream until stopped. Auto mode may still arm a
	 * deadline via pd queries, but it must not turn off a running monitor.
	 */
	housekeeping_photodiode_autooff_inhibit(pd_power, true);

	next.active = true;
	next.autolevel = request->autolevel;
	next.binary = request->binary;
	next.has_laser = request->has_laser;
	next.laser = request->laser;
	next.channel = channel;
	next.attenuator_index = attenuator_index;
	next.fiber = request->fiber;
	next.off_in_s = request->off_in_s;
	next.max_flux_ph_s = request->max_flux_ph_s;
	next.started_ms = k_uptime_get();
	next.acquiring = request->autolevel;
	/* Continuing the same source with adjustments disabled retains its shutdown. */
	next.stop_laser = request->autolevel || monitors[channel].stop_laser;
	monitors[channel] = next;
	photodiode_set_throughput_reference(channel,
		(struct photodiode_throughput_reference){0}, true);

	if (request->has_laser && request->autolevel) {
		monitors[channel].level_percent = 100.0;
		(void)attenuator_set_db(&attenuators[attenuator_index], 120.0);
		rc = hispec_laser_set_output_percent_autooff(request->laser,
							     monitors[channel].level_percent, 0U);
		if (rc != 0) {
			(void)stop_locked(channel);
			k_mutex_unlock(&monitors_lock);
			return rc;
		}
	}

	monitors[channel].input_changed_ms = k_uptime_get();
	refresh_reference(&monitors[channel]);
	if (status != NULL) {
		status->active = true;
		status->channel = channel;
		status->laser_name = request->has_laser ? hispec_laser_name(request->laser) : "none";
		status->autolevel = request->autolevel;
	}
	k_mutex_unlock(&monitors_lock);
	return 0;
}

int throughput_monitor_stop(uint8_t channel, struct throughput_monitor_status *status)
{
	int rc = 0;

	if (channel > PHOTODIODE_CHANNEL_COUNT) {
		return -EINVAL;
	}

	k_mutex_lock(&monitors_lock, K_FOREVER);
	for (uint8_t i = 0U; i < PHOTODIODE_CHANNEL_COUNT; ++i) {
		if (channel == i || channel == PHOTODIODE_CHANNEL_COUNT) {
			int stop_rc = stop_locked((enum photodiode_channel)i);

			if (rc == 0) {
				rc = stop_rc;
			}
		}
	}
	if (status != NULL) {
		memset(status, 0, sizeof(*status));
	}
	k_mutex_unlock(&monitors_lock);
	return rc;
}

bool throughput_monitor_any_active(void)
{
	bool active;

	k_mutex_lock(&monitors_lock, K_FOREVER);
	active = monitors[PHOTODIODE_CHANNEL_YJ].active ||
		 monitors[PHOTODIODE_CHANNEL_HK].active;
	k_mutex_unlock(&monitors_lock);
	return active;
}

bool throughput_monitor_autolevel_active(enum photodiode_channel channel)
{
	bool active = false;

	if (channel < 0 || channel >= PHOTODIODE_CHANNEL_COUNT) {
		return false;
	}

	k_mutex_lock(&monitors_lock, K_FOREVER);
	active = monitors[channel].active && monitors[channel].autolevel;
	k_mutex_unlock(&monitors_lock);
	return active;
}

void throughput_monitor_note_attenuator_changed(uint8_t attenuator_index)
{
	k_mutex_lock(&monitors_lock, K_FOREVER);
	for (uint8_t i = 0U; i < PHOTODIODE_CHANNEL_COUNT; ++i) {
		if (monitors[i].active && monitors[i].attenuator_index == attenuator_index) {
			monitors[i].autolevel = false;
			refresh_reference(&monitors[i]);
		}
	}
	k_mutex_unlock(&monitors_lock);
}

void throughput_monitor_note_laser_changed(enum hispec_laser_id laser)
{
	k_mutex_lock(&monitors_lock, K_FOREVER);
	for (uint8_t i = 0U; i < PHOTODIODE_CHANNEL_COUNT; ++i) {
		if (monitors[i].has_laser && monitors[i].laser == laser) {
			release_locked((enum photodiode_channel)i);
		}
	}
	k_mutex_unlock(&monitors_lock);
}
