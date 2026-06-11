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
#define TP_ADC_USABLE_MV 5000.0
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
};

static const struct laser_pd_channel laser_pd_channels[] = {
	{HISPEC_LASER_1028_Y, PHOTODIODE_CHANNEL_YJ},
	{HISPEC_LASER_1270_J, PHOTODIODE_CHANNEL_YJ},
	{HISPEC_LASER_1430_YJ, PHOTODIODE_CHANNEL_YJ},
	{HISPEC_LASER_1430_HK, PHOTODIODE_CHANNEL_HK},
	{HISPEC_LASER_1510_H, PHOTODIODE_CHANNEL_HK},
	{HISPEC_LASER_2330_K, PHOTODIODE_CHANNEL_HK},
};

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

static void stop_locked(enum photodiode_channel channel)
{
	if (monitors[channel].active) {
		housekeeping_photodiode_autooff_inhibit(pd_power_output(channel), false);
	}
	memset(&monitors[channel], 0, sizeof(monitors[channel]));
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

static int autolevel_adjust(struct throughput_state *state,
			    const struct photodiode_channel_status *pd,
			    const struct attenuator_transmission_estimate *atten)
{
	double mean_net_mv = (double)pd->mean_mv_1s - (double)pd->dark_mv;
	bool low = mean_net_mv < (TP_ADC_USABLE_MV * TP_LOW_FRACTION);
	bool high = mean_net_mv > (TP_ADC_USABLE_MV * TP_HIGH_FRACTION);
	struct hispec_laser_flux_estimate laser_flux = {0};
	double emitted_flux = 0.0;
	double max_tx = 1.0;
	double next_tx;
	double next_percent;
	int rc;

	if (pd->raw > INT16_MAX - 1024 || mean_net_mv <= 0.0) {
		if (high || pd->raw > INT16_MAX - 1024) {
			state->high_count++;
		} else {
			state->low_count++;
		}
	} else {
		state->high_count = high ? state->high_count + 1U : 0U;
		state->low_count = low ? state->low_count + 1U : 0U;
	}

	if (!low && !high &&
	    state->high_count < TP_INSTANT_BAD_SAMPLES &&
	    state->low_count < TP_INSTANT_BAD_SAMPLES) {
		return 0;
	}

	if (low || state->low_count >= TP_INSTANT_BAD_SAMPLES) {
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
				return 0;
			}
			(void)attenuator_set_linear(&attenuators[state->attenuator_index], next_tx);
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
				return 0;
			}
			rc = hispec_laser_set_output_percent_autooff(state->laser,
								     next_percent, 0U);
			if (rc == 0) {
				state->level_percent = next_percent;
			}
		}
		state->low_count = 0U;
		return 0;
	}

	if (high || state->high_count >= TP_INSTANT_BAD_SAMPLES) {
		if (atten->linear > TP_MIN_ATTEN_TX) {
			next_tx = atten->linear / 3.0;
			if (next_tx < TP_MIN_ATTEN_TX) {
				next_tx = TP_MIN_ATTEN_TX;
			}
			(void)attenuator_set_linear(&attenuators[state->attenuator_index], next_tx);
		} else if (state->level_percent > 0.0) {
			next_percent = state->level_percent / 3.0;
			if (next_percent < 0.0) {
				next_percent = 0.0;
			}
			rc = hispec_laser_set_output_percent_autooff(state->laser,
								     next_percent, 0U);
			if (rc == 0) {
				state->level_percent = next_percent;
			}
		}
		state->high_count = 0U;
	}

	return 0;
}

static void publish_sample(const struct throughput_state *state,
			   const struct photodiode_channel_status *pd,
			   uint64_t time_ms)
{
	struct app_photodiode_settings pd_settings;
	struct attenuator_transmission_estimate atten = {
		.linear = NAN,
		.linear_err = NAN,
		.attenuation_db = NAN,
	};
	struct hispec_laser_flux_estimate laser_flux = {0};
	struct coo_cmd_response *msg = &throughput_sample_msg;
	size_t off = 0U;
	char pd_route[APP_ROUTE_LOSS_ROUTE_MAX_LEN] = {0};
	char laser_route[APP_ROUTE_LOSS_ROUTE_MAX_LEN] = {0};
	const char *laser_name = state->has_laser ? hispec_laser_name(state->laser) : "none";
	const char *topic_suffix = state->channel == PHOTODIODE_CHANNEL_YJ ?
				   "yj_tput" : "hk_tput";
	char channel_fiber[8] = {0};
	double pd_route_tx = 1.0;
	double laser_route_tx = 1.0;
	double pd_flux = NAN;
	double pd_flux_err = NAN;
	double emitted_flux = NAN;
	double emitted_flux_err = NAN;
	double tp = NAN;
	double tp_err = NAN;
	double tp_rms_err = NAN;
	uint64_t pd_ontime_s;
	uint64_t laser_current_ontime_s;
	double pd_1s_mean_mv;

	if (laser_name == NULL) {
		return;
	}
	memset(msg, 0, sizeof(*msg));
	app_settings_get_photodiode(&pd_settings);

	channel_fiber_name(channel_fiber, sizeof(channel_fiber), state->channel, state->fiber);
	pd_ontime_s = (uint64_t)housekeeping_power_on_time_s(pd_power_output(state->channel));
	laser_current_ontime_s = state->has_laser ?
				  (uint64_t)hispec_laser_current_on_time_s(state->laser) : 0U;
	pd_1s_mean_mv = (double)pd->mean_mv_1s - (double)pd->dark_mv;
	route_name_for_pd(pd_route, sizeof(pd_route), state->channel, state->fiber);
	if (state->has_laser &&
	    attenuator_estimate_transmission(&attenuators[state->attenuator_index],
					     0.0, 0.0, &atten) &&
	    laser_estimate_flux(state->laser, 0.0, 0.0, &laser_flux) == 0) {
		route_name_for_laser(laser_route, sizeof(laser_route), laser_name, state->fiber);
		(void)app_settings_get_route_loss(pd_route, laser_name, &pd_route_tx);
		(void)app_settings_get_route_loss(laser_route, laser_name, &laser_route_tx);

		pd_flux = photodiode_photon_flux_from_mv(
			pd->net_mv, laser_flux.wavelength_nm,
			&pd_settings.channel[state->channel]) / pd_route_tx;
		pd_flux_err = photodiode_photon_flux_from_mv(
			pd->rms_mv_0p5s, laser_flux.wavelength_nm,
			&pd_settings.channel[state->channel]) / pd_route_tx;
		emitted_flux = laser_flux.flux_ph_s * atten.linear * laser_route_tx;
		emitted_flux_err = sqrt((laser_flux.flux_err_ph_s * atten.linear * laser_route_tx) *
					(laser_flux.flux_err_ph_s * atten.linear * laser_route_tx) +
					(laser_flux.flux_ph_s * atten.linear_err * laser_route_tx) *
					(laser_flux.flux_ph_s * atten.linear_err * laser_route_tx));

		if (emitted_flux > 0.0) {
			tp = pd_flux / emitted_flux;
			tp_rms_err = pd_flux_err / emitted_flux;
			if (pd_flux > 0.0) {
				tp_err = fabs(tp) *
					 sqrt((pd_flux_err / pd_flux) * (pd_flux_err / pd_flux) +
					      (emitted_flux_err / emitted_flux) *
					      (emitted_flux_err / emitted_flux));
			}
		}
	}

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
		put_f64((uint8_t *)msg->payload, sizeof(msg->payload), &off, pd_1s_mean_mv);
		put_f64((uint8_t *)msg->payload, sizeof(msg->payload), &off, pd->rms_mv_0p5s);
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
			"\"pd_1s_mean_mv\":%.4f,\"pd_0p5s_rms_mv\":%.4f",
			(double)pd->mv, (double)pd->net_mv,
			pd_1s_mean_mv, (double)pd->rms_mv_0p5s) != 0 ||
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

		k_mutex_lock(&monitors_lock, K_FOREVER);
		memcpy(local, monitors, sizeof(throughput_local));
		k_mutex_unlock(&monitors_lock);

		for (uint8_t i = 0U; i < PHOTODIODE_CHANNEL_COUNT; ++i) {
			bool pd_power = false;
			struct attenuator_transmission_estimate atten = {0};

			if (!local[i].active) {
				continue;
			}

			if (local[i].off_in_s > 0U &&
			    now - local[i].started_ms >= (int64_t)local[i].off_in_s * 1000) {
				k_mutex_lock(&monitors_lock, K_FOREVER);
				stop_locked((enum photodiode_channel)i);
				k_mutex_unlock(&monitors_lock);
				continue;
			}

			if (housekeeping_power_get(pd_power_output((enum photodiode_channel)i),
						   &pd_power) == 0 && !pd_power) {
				k_mutex_lock(&monitors_lock, K_FOREVER);
				stop_locked((enum photodiode_channel)i);
				k_mutex_unlock(&monitors_lock);
				continue;
			}

			if (local[i].has_laser && local[i].autolevel &&
			    attenuator_estimate_transmission(&attenuators[local[i].attenuator_index],
							     0.0, 0.0, &atten)) {
				(void)autolevel_adjust(&local[i], &pd_status->channel[i],
						       &atten);
				k_mutex_lock(&monitors_lock, K_FOREVER);
				if (monitors[i].active && monitors[i].laser == local[i].laser) {
					monitors[i].level_percent = local[i].level_percent;
					monitors[i].high_count = local[i].high_count;
					monitors[i].low_count = local[i].low_count;
				}
				k_mutex_unlock(&monitors_lock);
			}

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
	bool was_active;
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

	pd_power = pd_power_output(channel);
	rc = housekeeping_power_set(pd_power, true);
	if (rc != 0) {
		return rc;
	}

	k_mutex_lock(&monitors_lock, K_FOREVER);
	was_active = monitors[channel].active;
	k_mutex_unlock(&monitors_lock);

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

	if (request->has_laser && request->autolevel) {
		next.level_percent = 100.0;
		(void)attenuator_set_db(&attenuators[attenuator_index], 120.0);
		rc = hispec_laser_set_output_percent_autooff(request->laser,
							     next.level_percent, 0U);
		if (rc != 0) {
			if (!was_active) {
				housekeeping_photodiode_autooff_inhibit(pd_power, false);
			}
			return rc;
		}
	}

	k_mutex_lock(&monitors_lock, K_FOREVER);
	monitors[channel] = next;
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
	if (channel > PHOTODIODE_CHANNEL_COUNT) {
		return -EINVAL;
	}

	k_mutex_lock(&monitors_lock, K_FOREVER);
	if (channel == PHOTODIODE_CHANNEL_COUNT) {
		stop_locked(PHOTODIODE_CHANNEL_YJ);
		stop_locked(PHOTODIODE_CHANNEL_HK);
	} else {
		stop_locked((enum photodiode_channel)channel);
	}
	if (status != NULL) {
		memset(status, 0, sizeof(*status));
	}
	k_mutex_unlock(&monitors_lock);
	return 0;
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
		}
	}
	k_mutex_unlock(&monitors_lock);
}

void throughput_monitor_note_laser_changed(enum hispec_laser_id laser)
{
	k_mutex_lock(&monitors_lock, K_FOREVER);
	for (uint8_t i = 0U; i < PHOTODIODE_CHANNEL_COUNT; ++i) {
		if (monitors[i].active && monitors[i].has_laser && monitors[i].laser == laser) {
			stop_locked((enum photodiode_channel)i);
		}
	}
	k_mutex_unlock(&monitors_lock);
}
