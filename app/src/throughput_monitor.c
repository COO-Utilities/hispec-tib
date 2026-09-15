/**
 * @file throughput_monitor.c
 * @brief Throughput streaming and autolevel control.
 */

#include "throughput_monitor.h"

#include <ctype.h>
#include <errno.h>
#include <math.h>
#include <string.h>

#include "app_settings.h"
#include "attenuator.h"
#include "attenuator_calibration.h"
#include "command.h"
#include "devices.h"
#include "housekeeping.h"
#include "mems_switching.h"

#include <coo_commons/json_utils.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/sys/util.h>

LOG_MODULE_REGISTER(throughput_monitor, LOG_LEVEL_INF);

#define TP_LOW_FRACTION 0.20
#define TP_HIGH_FRACTION 0.80
#define TP_MIN_ATTEN_TX 1.0e-9
#define TP_FLAG_OVERRANGE BIT(0)
#define TP_FLAG_AUTOLEVEL BIT(1)

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
	int64_t input_changed_ms;
	int64_t last_sample_ms;
	int64_t next_gap_warning_ms;
	/* Route calibration is run configuration; driver state stays with its owner. */
	double pd_route_tx;
	double laser_route_tx;
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

/* Both detectors can stream; their external light paths allow one autolevel owner. */
static struct throughput_state monitors[PHOTODIODE_CHANNEL_COUNT];
static K_MUTEX_DEFINE(monitors_lock);
/* The throughput thread is the only user of these scratch objects. Keeping the
 * large snapshots and publish buffer in BSS leaves stack headroom for
 * calibration, autolevel, and formatting calls made from that thread.
 */
static struct photodiode_status throughput_pd_status;
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

static void channel_fiber_name(char *buf, size_t buf_len,
			       enum photodiode_channel channel, char fiber)
{
	snprintk(buf, buf_len, "%s_%c", photodiode_channel_names[channel],
		 (char)tolower((unsigned char)fiber));
}

/* Relinquish ownership without changing a manual command's laser setting. */
static void release_locked(enum photodiode_channel channel)
{
	if (monitors[channel].active) {
		housekeeping_photodiode_autooff_inhibit(pd_power_output(channel), false);
	}
	photodiode_set_source_reference(channel, (struct photodiode_source_reference){0});
	memset(&monitors[channel], 0, sizeof(monitors[channel]));
}

/* May block on Modbus. Retain the laser identity after failure for stop retry. */
static int stop_locked(enum photodiode_channel channel)
{
	struct throughput_state *state = &monitors[channel];

	if (state->active) {
		housekeeping_photodiode_autooff_inhibit(pd_power_output(channel), false);
	}
	photodiode_set_source_reference(channel, (struct photodiode_source_reference){0});
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

/* Refresh only after source changes. The owners supply confirmed setpoints and
 * calibration; the ADC latches this compact context before each conversion.
 * Can block on DAC I/O, but never holds the PD mutex while doing hardware I/O.
 */
static int refresh_reference(struct throughput_state *state)
{
	struct photodiode_source_reference ref = {
		.delivered_power_nw = NAN, .delivered_power_err_nw = NAN,
		.laser_output_power_uw = NAN, .laser_output_power_err_uw = NAN,
		.laser_current_ma = NAN, .atten_tx = NAN, .atten_db = NAN, .wavelength_nm = NAN,
	};
	struct attenuator_transmission_estimate atten;
	struct hispec_laser_flux_estimate laser;
	int rc = 0;

	if (state->has_laser) {
		rc = laser_estimate_flux(state->laser, &laser);
		if (rc == 0 && !attenuator_estimate_transmission(
			&attenuators[state->attenuator_index], &atten)) {
			rc = -EIO;
		}
		if (rc == 0) {
			ref.laser_output_power_uw = laser.power_mw * 1.0e3;
			ref.laser_output_power_err_uw = laser.power_err_mw * 1.0e3;
			ref.delivered_power_nw = laser.power_mw * 1.0e6 * atten.linear * state->laser_route_tx;
			ref.delivered_power_err_nw = hypot(laser.power_err_mw * atten.linear,
				laser.power_mw * atten.linear_err) * 1.0e6 * state->laser_route_tx;
			ref.laser_current_ma = laser.current_ma;
			ref.atten_tx = atten.linear;
			ref.atten_db = atten.attenuation_db;
			ref.wavelength_nm = laser.wavelength_nm;
		}
	}
	photodiode_set_source_reference(state->channel, ref);
	return rc;
}

/* One decision per fresh reading. A dark-subtracted low reading cannot override
 * a raw overrange reading. Startup and ordinary operation use the same 50 ms
 * path: no rolling-window gate, observation counter, or settling holdoff.
 * Returns 1 after an input change, 0 without a change, or a hardware error.
 */
static int autolevel_adjust(struct throughput_state *state,
			   const struct photodiode_channel_status *pd)
{
	const struct photodiode_source_reference *source = &pd->source;
	struct attenuator *atten = &attenuators[state->attenuator_index];
	bool high = pd->mv >= PHOTODIODE_ADC_USABLE_MV ||
		pd->net_mv > PHOTODIODE_ADC_USABLE_MV * TP_HIGH_FRACTION;
	bool low = !high && pd->net_mv < PHOTODIODE_ADC_USABLE_MV * TP_LOW_FRACTION;
	double max_tx = 1.0;
	double laser_flux = source->laser_output_power_uw * 1.0e-6 *
		(source->wavelength_nm * 1.0e-9) / (6.62607015e-34 * 299792458.0);
	double next_percent;
	int rc;

	if ((!high && !low) || !isfinite(source->atten_tx)) {
		return 0;
	}
	/* Preserve the command's cap on flux AFTER dynamic attenuation but BEFORE
	 * static route losses. Optical power is used everywhere else in the stream.
	 */
	if (state->max_flux_ph_s > 0.0 && laser_flux > 0.0) {
		max_tx = MIN(1.0, state->max_flux_ph_s / laser_flux);
	}
	if ((low && source->atten_tx < 0.999) || (high && source->atten_tx > TP_MIN_ATTEN_TX)) {
		double next_tx = low ? MIN(source->atten_tx * 3.0, max_tx) :
			MAX(source->atten_tx / 3.0, TP_MIN_ATTEN_TX);
		if (low && next_tx <= source->atten_tx) {
			return 0;
		}
		if (!attenuator_set_linear(atten, next_tx)) {
			return -EIO;
		}
		if (atten->attenuation_db != source->atten_db) {
			return 1;
		}
		/* A clamped pair already at its limit must yield to laser adjustment. */
	}
	next_percent = low ? MIN(state->level_percent * 3.0, 100.0) : state->level_percent / 3.0;
	if (low && state->max_flux_ph_s > 0.0 && laser_flux * source->atten_tx > 0.0) {
		next_percent = MIN(next_percent, state->level_percent *
			state->max_flux_ph_s / (laser_flux * source->atten_tx));
	}
	if (next_percent == state->level_percent || (low && next_percent <= state->level_percent)) {
		return 0;
	}
	rc = hispec_laser_set_output_percent_autooff(state->laser, next_percent, 0U);
	if (rc != 0) {
		return rc;
	}
	state->level_percent = next_percent;
	return 1;
}

/* Serialize a single acquisition, never a window mean. The same power ratio
 * drives both encodings. Its derivative form remains valid at zero/negative
 * net signal; relative PD error would divide by zero there.
 */
static void publish_sample(const struct throughput_state *state,
			   const struct photodiode_channel_status *pd)
{
	const struct photodiode_source_reference *source = &pd->source;
	struct coo_cmd_response *msg = &throughput_sample_msg;
	const char *topic_suffix = state->channel == PHOTODIODE_CHANNEL_YJ ? "yj_tput" : "hk_tput";
	char channel_fiber[8] = {0};
	size_t off = 0U;
	bool overrange = pd->mv >= PHOTODIODE_ADC_USABLE_MV;
	uint8_t flags = (overrange ? TP_FLAG_OVERRANGE : 0U) | (state->autolevel ? TP_FLAG_AUTOLEVEL : 0U);
	double pd_power = pd->power_uw * 1000.0 / state->pd_route_tx;
	double pd_error = overrange ? (double)NAN : pd->power_err_uw * 1000.0 / state->pd_route_tx;
	double tp = NAN, tp_pd_err = NAN, tp_err = NAN;
	uint64_t pd_ontime = housekeeping_power_on_time_s(pd_power_output(state->channel));
	uint64_t laser_ontime = state->has_laser ? hispec_laser_current_on_time_s(state->laser) : 0U;

	if (isfinite(source->delivered_power_nw) && source->delivered_power_nw > 0.0) {
		tp = pd_power / source->delivered_power_nw;
		tp_pd_err = pd_error / source->delivered_power_nw;
		tp_err = hypot(tp_pd_err, tp * source->delivered_power_err_nw / source->delivered_power_nw);
	}
	/* This fixed list is the wire order, shared by JSON and binary. Field names
	 * carry units; %.12g preserves small finite uncertainties in JSON.
	 */
	const struct { const char *name; double value; } fields[] = {
		{"tp", tp}, {"tp_err", tp_err}, {"tp_pd_err", tp_pd_err},
		{"pd_power_nw", pd_power}, {"pd_power_err_nw", pd_error},
		{"delivered_power_nw", source->delivered_power_nw},
		{"delivered_power_err_nw", source->delivered_power_err_nw},
		{"laser_output_power_uw", source->laser_output_power_uw},
		{"laser_output_power_err_uw", source->laser_output_power_err_uw},
		{"pd_route_tx", state->pd_route_tx}, {"laser_route_tx", state->laser_route_tx},
		{"atten_tx", source->atten_tx}, {"pd_mv", pd->mv}, {"pd_net_mv", pd->net_mv},
		{"pd_net_err_mv", overrange ? (double)NAN : pd->net_err_mv},
		{"laser_current_ma", source->laser_current_ma}, {"atten_db", source->atten_db},
		{"wavelength_nm", source->wavelength_nm},
	};
	memset(msg, 0, sizeof(*msg));
	channel_fiber_name(channel_fiber, sizeof(channel_fiber), state->channel, state->fiber);
	if (state->binary) {
		put_bytes((uint8_t *)msg->payload, sizeof(msg->payload), &off, channel_fiber, sizeof(channel_fiber));
		put_u64((uint8_t *)msg->payload, sizeof(msg->payload), &off, pd->t_ms);
		for (size_t i = 0; i < ARRAY_SIZE(fields); ++i) {
			put_f64((uint8_t *)msg->payload, sizeof(msg->payload), &off, fields[i].value);
		}
		put_i16((uint8_t *)msg->payload, sizeof(msg->payload), &off, pd->raw);
		put_u64((uint8_t *)msg->payload, sizeof(msg->payload), &off, pd_ontime);
		put_u64((uint8_t *)msg->payload, sizeof(msg->payload), &off, laser_ontime);
		put_bytes((uint8_t *)msg->payload, sizeof(msg->payload), &off, &flags, sizeof(flags));
	} else {
		if (coo_json_append(msg->payload, sizeof(msg->payload), &off,
			"{\"channel\":\"%s\",\"laser\":\"%s\",\"autolevel\":%s,\"t_ms\":%llu",
			channel_fiber, state->has_laser ? hispec_laser_name(state->laser) : "none",
			state->autolevel ? "true" : "false", (unsigned long long)pd->t_ms) != 0) {
			return;
		}
		for (size_t i = 0; i < ARRAY_SIZE(fields); ++i) {
			if (coo_json_append(msg->payload, sizeof(msg->payload), &off,
				",\"%s\":", fields[i].name) != 0 ||
			    (isfinite(fields[i].value) ?
				coo_json_append(msg->payload, sizeof(msg->payload), &off, "%.12g", fields[i].value) :
				coo_json_append(msg->payload, sizeof(msg->payload), &off, "null")) != 0) {
				return;
			}
		}
		if (coo_json_append(msg->payload, sizeof(msg->payload), &off,
			",\"pd_raw\":%d,\"pd_ontime_s\":%llu,\"laser_current_ontime_s\":%llu,\"flags\":[%s]}",
			pd->raw, (unsigned long long)pd_ontime, (unsigned long long)laser_ontime,
			overrange ? "\"overrange\"" : "") != 0) {
			return;
		}
	}
	msg->payload_len = off;
	(void)coo_cmd_runtime_emit(command_runtime_get(), &(const struct coo_cmd_runtime_emit_args){
		.type = COO_CMD_RUNTIME_EMIT_DATA, .delivery = COO_CMD_RUNTIME_EMIT_BEST_EFFORT,
		.suffix = topic_suffix, .out = msg,
	});
}

void throughput_monitor_thread(void *p1, void *p2, void *p3)
{
	ARG_UNUSED(p1);
	ARG_UNUSED(p2);
	ARG_UNUSED(p3);

	while (1) {
		/* Completion establishes phase; timeout still services stop/expiry and
		 * calibration if the ADC cannot run. No second periodic sleep or queue.
		 */
		(void)photodiode_wait_for_sample(K_MSEC(PHOTODIODE_SAMPLE_INTERVAL_MS));
		int64_t now = k_uptime_get();
		photodiode_get_status(&throughput_pd_status);
		attenuator_calibration_tick(&throughput_pd_status, now);
		for (uint8_t i = 0; i < PHOTODIODE_CHANNEL_COUNT; ++i) {
			struct throughput_state *state = &monitors[i];
			const struct photodiode_channel_status *pd = &throughput_pd_status.channel[i];
			bool pd_power = false;
			int rc = 0;

			/* Start/stop and control serialize; a completed stop cannot be undone
			 * by an adjustment selected using an older copy of monitor state.
			 */
			k_mutex_lock(&monitors_lock, K_FOREVER);
			if (!state->active) {
				goto next;
			}
			if ((state->off_in_s > 0 && now - state->started_ms >= (int64_t)state->off_in_s * 1000) ||
			    (housekeeping_power_get(pd_power_output(i), &pd_power) == 0 && !pd_power)) {
				(void)stop_locked(i);
				goto next;
			}
			if (state->has_laser) {
				struct hispec_laser_flux_estimate laser;
				rc = laser_estimate_flux(state->laser, &laser);
				if ((rc != 0 && rc != -EBUSY) ||
				    (rc == 0 && state->autolevel && laser.current_ma <= 0.0)) {
					LOG_WRN("Throughput laser estimate invalid; stopping %s", hispec_laser_name(state->laser));
					(void)stop_locked(i);
					goto next;
				}
				/* An owner-side change (e.g. auto-off) can occur without a manual
				 * command callback. Refresh FUTURE acquisition context, never
				 * replace the reference already attached to this reading.
				 */
				if (rc == 0 && (laser.current_ma != pd->source.laser_current_ma ||
				    laser.wavelength_nm != pd->source.wavelength_nm)) {
					if (refresh_reference(state) != 0) {
						(void)stop_locked(i);
						goto next;
					}
					state->input_changed_ms = k_uptime_get();
				}
			}
			if (pd->acquired_ms <= state->started_ms || pd->acquired_ms <= state->last_sample_ms) {
				goto next;
			}
			if (state->last_sample_ms > 0 && pd->acquired_ms - state->last_sample_ms >
			    PHOTODIODE_SAMPLE_INTERVAL_MS * 3 / 2 && now >= state->next_gap_warning_ms) {
				LOG_WRN("Throughput %s acquisition gap: %lld ms", photodiode_channel_names[i],
					(long long)(pd->acquired_ms - state->last_sample_ms));
				state->next_gap_warning_ms = now + 10000;
			}
			state->last_sample_ms = pd->acquired_ms;
			publish_sample(state, pd);
			if (rc != -EBUSY && state->autolevel && pd->acquired_ms > state->input_changed_ms) {
				rc = autolevel_adjust(state, pd);
				if (rc > 0) {
					rc = refresh_reference(state);
					/* Also exclude acquisitions begun before the new reference was installed. */
					state->input_changed_ms = k_uptime_get();
				}
				if (rc < 0) {
					LOG_WRN("Throughput input change failed (%d); stopping", rc);
					(void)stop_locked(i);
				}
			}
next:
			k_mutex_unlock(&monitors_lock);
		}
	}
}

int throughput_monitor_start(const struct throughput_monitor_request *request,
			     struct throughput_monitor_status *status)
{
	enum photodiode_channel channel;
	enum housekeeping_power_output pd_power;
	uint8_t attenuator_index;
	struct app_photodiode_settings pd_settings;
	struct photodiode_status pd_status;
	struct throughput_state next = {0};
	int rc;

	if (request == NULL) {
		return -EINVAL;
	}

	if (request->fiber != 'M' && request->fiber != 'S') {
		return -EINVAL;
	}
	photodiode_get_status(&pd_status);
	if (attenuator_calibration_active() || pd_status.channel[0].dark_pending ||
	    pd_status.channel[1].dark_pending) {
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
	for (uint8_t i = 0; i < PHOTODIODE_CHANNEL_COUNT; ++i) {
		if (request->autolevel && i != channel && monitors[i].active && monitors[i].autolevel) {
			k_mutex_unlock(&monitors_lock);
			return -EBUSY;
		}
	}
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
	const char *failed_switch = NULL;
	char failed_state = '\0';
	rc = mems_router_apply_named_route(&router, request->input, request->output, false,
		&failed_switch, &failed_state);
	if (rc != 0) {
		(void)stop_locked(channel);
		k_mutex_unlock(&monitors_lock);
		return rc;
	}
	pd_power = pd_power_output(channel);
	rc = housekeeping_power_set(pd_power, true);
	if (rc != 0) {
		(void)stop_locked(channel);
		k_mutex_unlock(&monitors_lock);
		return rc;
	}
	/*
	 * Throughput owns this stream until stopped. Auto mode may still arm a
	 * deadline via pd queries, but it must not turn off a running monitor.
	 */
	housekeeping_photodiode_autooff_inhibit(pd_power, true);

	/* The monitor has applied this route. Capture its calibration once; dynamic
	 * drive estimates reuse these losses until the next start request.
	 */
	next.pd_route_tx = 1.0;
	next.laser_route_tx = 1.0;
	if (request->has_laser) {
		char route[APP_ROUTE_LOSS_ROUTE_MAX_LEN];
		const char *name = hispec_laser_name(request->laser);

		route_name_for_pd(route, sizeof(route), channel, request->fiber);
		(void)app_settings_get_route_loss(route, name, &next.pd_route_tx);
		snprintk(route, sizeof(route), "%s_to_%s", request->input, request->output);
		(void)app_settings_get_route_loss(route, name, &next.laser_route_tx);
	}

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
	/* Continuing the same source with adjustments disabled retains its shutdown. */
	next.stop_laser = request->autolevel || monitors[channel].stop_laser;
	monitors[channel] = next;
	photodiode_set_source_reference(channel, (struct photodiode_source_reference){0});

	if (request->has_laser && request->autolevel) {
		monitors[channel].level_percent = 100.0;
		rc = attenuator_set_db(&attenuators[attenuator_index], 120.0) ? 0 : -EIO;
		if (rc == 0) {
			rc = hispec_laser_set_output_percent_autooff(request->laser,
				monitors[channel].level_percent, 0U);
		}
		if (rc != 0) {
			(void)stop_locked(channel);
			k_mutex_unlock(&monitors_lock);
			return rc;
		}
	}

	monitors[channel].input_changed_ms = k_uptime_get();
	rc = refresh_reference(&monitors[channel]);
	if (rc != 0) {
		(void)stop_locked(channel);
		k_mutex_unlock(&monitors_lock);
		return rc;
	}
	/* The first reported acquisition must begin after startup/reference installation. */
	monitors[channel].started_ms = k_uptime_get();
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

void throughput_monitor_note_attenuator_changed(uint8_t attenuator_index)
{
	k_mutex_lock(&monitors_lock, K_FOREVER);
	for (uint8_t i = 0U; i < PHOTODIODE_CHANNEL_COUNT; ++i) {
		if (monitors[i].active && monitors[i].attenuator_index == attenuator_index) {
			monitors[i].autolevel = false;
			if (refresh_reference(&monitors[i]) != 0) {
				(void)stop_locked((enum photodiode_channel)i);
			}
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
