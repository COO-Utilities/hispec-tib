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

#include <coo_commons/json_utils.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/sys/util.h>

LOG_MODULE_REGISTER(throughput_monitor, LOG_LEVEL_INF);

#define TP_LOW_FRACTION 0.20
#define TP_HIGH_FRACTION 0.80
#define TP_ATTEN_FIRST 0
#define TP_LASER_FIRST 1
/* Both policies brighten with attenuation first. Select only the dimming order. */
#ifndef TP_AUTOLEVEL_DIM_PRIORITY
#define TP_AUTOLEVEL_DIM_PRIORITY TP_ATTEN_FIRST
#endif
#if TP_AUTOLEVEL_DIM_PRIORITY != TP_ATTEN_FIRST && TP_AUTOLEVEL_DIM_PRIORITY != TP_LASER_FIRST
#error "TP_AUTOLEVEL_DIM_PRIORITY must be TP_ATTEN_FIRST or TP_LASER_FIRST"
#endif
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

/* Nominal input context belongs to the measurement, not the ADC. Keep only
 * the previous and current contexts around the last confirmed input change.
 * This associates delayed readings; it does not model physical filter settling.
 */
struct throughput_source_reference {
	double delivered_power_nw;
	double delivered_power_err_nw;
	double laser_output_power_uw;
	double laser_output_power_err_uw;
	double laser_current_ma;
	double atten_tx;
	double atten_db;
	double wavelength_nm;
};

/* Preparing retains the PD inhibition while command-owned routes change. */
enum throughput_phase { TP_INACTIVE, TP_PREPARING, TP_RUNNING };

struct throughput_state {
	enum throughput_phase phase;
	bool autolevel;
	bool binary;
	bool has_laser;
	enum hispec_laser_id laser;
	enum photodiode_channel channel;
	uint8_t attenuator_index;
	char fiber;
	int64_t started_ms;
	uint32_t off_in_s;
	double max_flux_ph_s;
	int64_t input_changed_ms;
	struct throughput_source_reference source;
	struct throughput_source_reference previous_source;
	int64_t last_sample_ms;
	int64_t next_gap_warning_ms;
	/* Route calibration is run configuration; driver state stays with its owner. */
	double pd_route_tx;
	double laser_route_tx;
	/* Autolevel's laser must still stop after manual level changes disable adjustments. */
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

static void channel_fiber_name(char *buf, size_t buf_len,
			       enum photodiode_channel channel, char fiber)
{
	snprintk(buf, buf_len, "%s_%c", photodiode_channel_names[channel],
		 (char)tolower((unsigned char)fiber));
}

/* Relinquish ownership without changing a manual command's laser setting. */
static void release_locked(enum photodiode_channel channel)
{
	if (monitors[channel].phase != TP_INACTIVE) {
		housekeeping_photodiode_autooff_inhibit(pd_power_output(channel), false);
	}
	memset(&monitors[channel], 0, sizeof(monitors[channel]));
}

/* May block on Modbus. Retain the laser identity after failure for stop retry. */
static int stop_locked(enum photodiode_channel channel)
{
	struct throughput_state *state = &monitors[channel];

	if (state->phase != TP_INACTIVE) {
		housekeeping_photodiode_autooff_inhibit(pd_power_output(channel), false);
	}
	state->phase = TP_INACTIVE;
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

/* Read confirmed owner state without hardware I/O. Retain the prior context
 * only when values change. One adjustment per new reading needs two contexts,
 * not a sample history; arbitrary rapid manual changes are not reconstructed.
 */
static int refresh_reference(struct throughput_state *state)
{
	struct throughput_source_reference ref = {
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
	if (rc == 0 && memcmp(&ref, &state->source, sizeof(ref)) != 0) {
		state->previous_source = state->source;
		state->source = ref;
		state->input_changed_ms = k_uptime_get();
	}
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
	const struct throughput_source_reference *source = &state->source;
	struct attenuator *atten = &attenuators[state->attenuator_index];
	bool high = pd->mv >= PHOTODIODE_ADC_USABLE_MV ||
		pd->net_mv > PHOTODIODE_ADC_USABLE_MV * TP_HIGH_FRACTION;
	bool low = !high && pd->net_mv < PHOTODIODE_ADC_USABLE_MV * TP_LOW_FRACTION;
	double max_tx = 1.0;
	double laser_flux = source->laser_output_power_uw * 1.0e-6 *
		(source->wavelength_nm * 1.0e-9) / (6.62607015e-34 * 299792458.0);
	struct app_laser_channel_settings settings;

	if ((!high && !low) || !isfinite(source->atten_tx)) {
		return 0;
	}
	int rc = hispec_laser_get_channel_settings(state->laser, &settings);
	if (rc != 0) {
		return rc;
	}
	/* Preserve the command's cap on flux AFTER dynamic attenuation but BEFORE
	 * static route losses. Optical power is used everywhere else in the stream.
	 */
	if (state->max_flux_ph_s > 0.0 && laser_flux > 0.0) {
		max_tx = MIN(1.0, state->max_flux_ph_s / laser_flux);
	}
	/* Try each actuator once; a clamped/quantized no-op yields to the other.
	 * The same actuator code serves both build-time priority choices.
	 */
	bool laser_first = high && TP_AUTOLEVEL_DIM_PRIORITY == TP_LASER_FIRST;
	for (unsigned pass = 0; pass < 2; ++pass) {
		if ((pass == 0) != laser_first) {
			double next_tx = low ? MIN(source->atten_tx * 3.0, max_tx) : source->atten_tx / 3.0;
			if (low && next_tx <= source->atten_tx) {
				continue;
			}
			if (!attenuator_set_linear(atten, next_tx, true)) {
				return -EIO;
			}
			struct attenuator_transmission_estimate applied;
			if (!attenuator_estimate_transmission(atten, &applied)) {
				return -EIO;
			}
			if (applied.attenuation_db != source->atten_db) {
				return 1;
			}
			continue;
		}
		const laserprops_t *props = &settings.properties;
		double excess_ma = source->laser_current_ma - props->threshold_current_ma;
		double max_ma = props->nominal_current_ma;
		if (low && state->max_flux_ph_s > 0.0 && laser_flux * source->atten_tx > 0.0) {
			max_ma = MIN(max_ma, props->threshold_current_ma + excess_ma *
				state->max_flux_ph_s / (laser_flux * source->atten_tx));
		}
		double next_ma = hispec_laser_quantize_current_ma(
			props->threshold_current_ma + (low ? excess_ma * 3.0 : excess_ma / 3.0),
			settings.min_autolevel_current_ma, max_ma);
		if (!isfinite(next_ma) || (low ? next_ma <= source->laser_current_ma :
					       next_ma >= source->laser_current_ma)) {
			continue;
		}
		double percent = MIN(100.0, 100.0 * (next_ma - props->threshold_current_ma) /
			(props->nominal_current_ma - props->threshold_current_ma));
		rc = hispec_laser_set_output_percent_autooff(state->laser, percent, 0U, false);
		if (rc != 0) {
			return rc;
		}
		struct hispec_laser_flux_estimate applied;
		rc = laser_estimate_flux(state->laser, &applied);
		if (rc != 0) {
			return rc;
		}
		if (applied.current_ma != source->laser_current_ma) {
			return 1;
		}
	}
	return 0;
}

/* Serialize a single acquisition, never a window mean. The same power ratio
 * drives both encodings. Its derivative form remains valid at zero power;
 * relative PD error would divide by zero there.
 */
static void publish_sample(const struct throughput_state *state,
			   const struct photodiode_channel_status *pd)
{
	/* Prefer the pre-change context for a conversion begun during the move.
	 * An actual optical transition within a conversion remains visible; neither
	 * context claims to deconvolve detector or PCB filtering.
	 */
	const struct throughput_source_reference *source = pd->sample_ms <= state->input_changed_ms ?
		&state->previous_source : &state->source;
	struct coo_cmd_response *msg = &throughput_sample_msg;
	const char *topic_suffix = state->channel == PHOTODIODE_CHANNEL_YJ ? "yj_tput" : "hk_tput";
	char channel_fiber[8] = {0};
	size_t off = 0U;
	bool overrange = pd->mv >= PHOTODIODE_ADC_USABLE_MV;
	uint8_t flags = (overrange ? TP_FLAG_OVERRANGE : 0U) | (state->autolevel ? TP_FLAG_AUTOLEVEL : 0U);
	double response = state->has_laser ? photodiode_wavelength_coefficient(source->wavelength_nm) : 1.0;
	double pd_power = pd->power_uw * response * 1000.0 / state->pd_route_tx;
	double pd_error = overrange ? (double)NAN : pd->power_err_uw * response * 1000.0 / state->pd_route_tx;
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

/* Report the measurement's stop even when it precedes the owner's background
 * timeout notice. Keep this on the existing console/MQTT warning path.
 */
static void warn_fault_stop(const struct throughput_state *state, const char *reason, int error)
{
	char context[96];
	snprintk(context, sizeof(context), "channel=%s laser=%s rc=%d",
		photodiode_channel_names[state->channel],
		state->has_laser ? hispec_laser_name(state->laser) : "none", error);
	coo_cmd_runtime_emit(command_runtime_get(), &(struct coo_cmd_runtime_emit_args){
		.type = COO_CMD_RUNTIME_EMIT_WARNING, .delivery = COO_CMD_RUNTIME_EMIT_BEST_EFFORT,
		.code = "throughput_stopped", .msg = reason, .context = context,
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
		attenuator_calibration_tick(&throughput_pd_status);
		for (uint8_t i = 0; i < PHOTODIODE_CHANNEL_COUNT; ++i) {
			struct throughput_state *state = &monitors[i];
			const struct photodiode_channel_status *pd = &throughput_pd_status.channel[i];
			bool pd_power = false;
			int rc = 0;

			/* Start/stop and control serialize; a completed stop cannot be undone
			 * by an adjustment selected using an older copy of monitor state.
			 */
			k_mutex_lock(&monitors_lock, K_FOREVER);
			if (state->phase != TP_RUNNING) {
				goto next;
			}
			rc = housekeeping_power_get_confirmed(pd_power_output(i), &pd_power);
			if ((state->off_in_s > 0 && now - state->started_ms >= (int64_t)state->off_in_s * 1000) ||
			    rc != 0 || !pd_power) {
				if (rc != 0 || !pd_power) warn_fault_stop(state, "photodiode power unavailable; stopping throughput", rc != 0 ? rc : -EIO);
				(void)stop_locked(i);
				goto next;
			}
			bool emitting = false;
			if (state->has_laser) rc = hispec_laser_output_status(state->laser, &emitting);
			if (rc == 0) rc = refresh_reference(state);
			if (rc != 0 || (state->has_laser && state->autolevel && !emitting)) {
				warn_fault_stop(state, "source unavailable; stopping throughput", rc != 0 ? rc : -EIO);
				(void)stop_locked(i);
				goto next;
			}
			if (pd->sample_ms <= state->started_ms || pd->sample_ms <= state->last_sample_ms) {
				goto next;
			}
			if (state->last_sample_ms > 0 && pd->sample_ms - state->last_sample_ms >
			    PHOTODIODE_SAMPLE_INTERVAL_MS * 3 / 2 && now >= state->next_gap_warning_ms) {
				LOG_WRN("Throughput %s acquisition gap: %lld ms", photodiode_channel_names[i],
					(long long)(pd->sample_ms - state->last_sample_ms));
				state->next_gap_warning_ms = now + 10000;
			}
			state->last_sample_ms = pd->sample_ms;
			publish_sample(state, pd);
			if (state->autolevel && pd->sample_ms > state->input_changed_ms) {
				rc = autolevel_adjust(state, pd);
				if (rc > 0) {
					rc = refresh_reference(state);
				}
				if (rc < 0) {
					warn_fault_stop(state, "input change failed; stopping throughput", rc);
					(void)stop_locked(i);
				}
			}
next:
			k_mutex_unlock(&monitors_lock);
		}
	}
}

int throughput_monitor_prepare_start(const struct throughput_monitor_request *request)
{
	enum photodiode_channel channel;
	struct app_photodiode_settings pd_settings;
	struct photodiode_status pd_status;
	int rc;

	if (request == NULL) {
		return -EINVAL;
	}

	if (request->fiber != 'M' && request->fiber != 'S') {
		return -EINVAL;
	}
	if (request->autolevel && (!isfinite(request->initial_level) ||
	    request->initial_level < 0.0 || request->initial_level > 1.0)) {
		return -EINVAL;
	}
	photodiode_get_status(&pd_status);
	if (attenuator_calibration_active() || pd_status.channel[0].dark_pending ||
	    pd_status.channel[1].dark_pending) {
		return -EBUSY;
	}
	if (request->has_laser) {
		if (photodiode_channel_for_laser(request->laser, &channel) != 0 ||
		    channel != request->channel) {
			return -EINVAL;
		}
	} else {
		if (request->autolevel ||
		    request->channel < 0 || request->channel >= PHOTODIODE_CHANNEL_COUNT) {
			return -EINVAL;
		}
		channel = request->channel;
	}

	app_settings_get_photodiode(&pd_settings);
	if (pd_settings.channel[channel].power == APP_PD_POWER_OVERRIDE_OFF) {
		return -EACCES;
	}

	k_mutex_lock(&monitors_lock, K_FOREVER);
	for (uint8_t i = 0; i < PHOTODIODE_CHANNEL_COUNT; ++i) {
		if (request->autolevel && i != channel && monitors[i].phase == TP_RUNNING && monitors[i].autolevel) {
			k_mutex_unlock(&monitors_lock);
			return -EBUSY;
		}
	}
	/* Finish the previous autolevel operation before replacing its source;
	 * also retry a failed shutdown before accepting a new operation.
	 * Passive monitoring never assumes control of a manual laser setting.
	 */
	bool stop_previous = monitors[channel].stop_laser &&
		(monitors[channel].phase != TP_RUNNING || !request->has_laser ||
		 monitors[channel].laser != request->laser);
	/* Hold the PD across replacement, including a different laser on the same
	 * channel. Preparing suppresses publishing/control until routes are ready.
	 */
	housekeeping_photodiode_autooff_inhibit(pd_power_output(channel), true);
	monitors[channel].phase = TP_PREPARING;
	monitors[channel].autolevel = false;
	if (stop_previous) {
		rc = hispec_laser_stop_output(monitors[channel].laser, false);
		if (rc != 0) {
			/* Keep failed shutdown responsibility for a later explicit stop. */
			housekeeping_photodiode_autooff_inhibit(pd_power_output(channel), false);
			monitors[channel].phase = TP_INACTIVE;
			k_mutex_unlock(&monitors_lock);
			return rc;
		}
		monitors[channel].stop_laser = false;
	}
	k_mutex_unlock(&monitors_lock);
	return 0;
}

int throughput_monitor_start(const struct throughput_monitor_request *request,
			     struct throughput_monitor_status *status)
{
	enum photodiode_channel channel = request->channel;
	enum housekeeping_power_output pd_power = pd_power_output(channel);
	uint8_t attenuator_index = 0U;
	struct throughput_state next = {0};
	int rc;

	if (request->has_laser) {
		rc = attenuator_index_from_laser_id(request->laser, &attenuator_index);
		if (rc != 0) {
			return rc;
		}
	}
	k_mutex_lock(&monitors_lock, K_FOREVER);

	/* Acquire before enabling; a queued auto-off cannot win between these calls. */
	housekeeping_photodiode_autooff_inhibit(pd_power, true);
	rc = housekeeping_power_set(pd_power, true);
	if (rc != 0) {
		goto failed;
	}
	/*
	 * Throughput owns this stream until stopped. Auto mode may still arm a
	 * deadline via pd queries, but it must not turn off a running monitor.
	 */

	/* The command resolved losses for both independent routes before starting. */
	next.pd_route_tx = request->pd_route_tx;
	next.laser_route_tx = request->laser_route_tx;

	next.phase = TP_PREPARING;
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

	if (request->has_laser && request->autolevel) {
		struct app_laser_channel_settings settings;
		rc = hispec_laser_get_channel_settings(request->laser, &settings);
		if (rc != 0) {
			goto failed;
		}
		const laserprops_t *props = &settings.properties;
		double range_ma = props->nominal_current_ma - props->threshold_current_ma;
		double initial_ma = hispec_laser_quantize_current_ma(
			props->threshold_current_ma + range_ma * request->initial_level,
			settings.min_autolevel_current_ma, props->nominal_current_ma);
		rc = attenuator_set_db(&attenuators[attenuator_index],
			2.0 * ATTENUATOR_CALIBRATED_MAX_DB, true) ? 0 : -EIO;
		if (rc == 0) {
			rc = hispec_laser_set_output_percent_autooff(request->laser,
				MIN(100.0, 100.0 * (initial_ma - props->threshold_current_ma) / range_ma), 0U, false);
		}
		if (rc != 0) {
			goto failed;
		}
	}

	rc = refresh_reference(&monitors[channel]);
	if (rc == 0 && request->has_laser) {
		bool emitting;
		rc = hispec_laser_output_status(request->laser, &emitting);
		if (rc == 0 && request->autolevel && !emitting) rc = -EIO;
	}
	if (rc != 0) {
		goto failed;
	}
	monitors[channel].previous_source = monitors[channel].source;
	/* The first reported acquisition must begin after startup/context installation. */
	monitors[channel].started_ms = k_uptime_get();
	monitors[channel].phase = TP_RUNNING;
	if (status != NULL) {
		status->active = true;
		status->channel = channel;
		status->laser_name = request->has_laser ? hispec_laser_name(request->laser) : "none";
		status->autolevel = request->autolevel;
	}
	k_mutex_unlock(&monitors_lock);
	return 0;

failed:
	/* Keep the identity for command-side stop, but never publish a failed start. */
	housekeeping_photodiode_autooff_inhibit(pd_power, false);
	monitors[channel].phase = TP_INACTIVE;
	monitors[channel].autolevel = false;
	k_mutex_unlock(&monitors_lock);
	return rc;
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
	active = monitors[PHOTODIODE_CHANNEL_YJ].phase != TP_INACTIVE ||
		 monitors[PHOTODIODE_CHANNEL_HK].phase != TP_INACTIVE;
	k_mutex_unlock(&monitors_lock);
	return active;
}

void throughput_monitor_note_attenuator_changed(uint8_t attenuator_index)
{
	k_mutex_lock(&monitors_lock, K_FOREVER);
	for (uint8_t i = 0U; i < PHOTODIODE_CHANNEL_COUNT; ++i) {
		if (monitors[i].phase == TP_RUNNING && monitors[i].has_laser && monitors[i].attenuator_index == attenuator_index) {
			monitors[i].autolevel = false;
			if (refresh_reference(&monitors[i]) != 0) {
				(void)stop_locked((enum photodiode_channel)i);
			}
		}
	}
	k_mutex_unlock(&monitors_lock);
}

void throughput_monitor_note_laser_changed(enum hispec_laser_id laser, bool stop_monitoring)
{
	k_mutex_lock(&monitors_lock, K_FOREVER);
	for (uint8_t i = 0U; i < PHOTODIODE_CHANNEL_COUNT; ++i) {
		if (monitors[i].has_laser && monitors[i].laser == laser) {
			if (stop_monitoring) {
				release_locked((enum photodiode_channel)i);
			} else {
				/* Disable control before the manual write. The sampling loop
				 * picks up confirmed setpoints after I/O; retain run ownership.
				 */
				monitors[i].autolevel = false;
			}
		}
	}
	k_mutex_unlock(&monitors_lock);
}

int throughput_monitor_update_laser_settings(enum hispec_laser_id laser,
	const struct app_laser_channel_settings *settings, bool persist)
{
	k_mutex_lock(&monitors_lock, K_FOREVER);
	for (uint8_t i = 0U; i < PHOTODIODE_CHANNEL_COUNT; ++i) {
		if (monitors[i].has_laser && monitors[i].laser == laser) {
			housekeeping_photodiode_autooff_inhibit(pd_power_output(i), false);
			monitors[i].phase = TP_INACTIVE;
			monitors[i].autolevel = false;
		}
	}
	int rc = hispec_laser_update_channel_settings(laser, settings, persist);
	if (rc == 0) {
		for (uint8_t i = 0U; i < PHOTODIODE_CHANNEL_COUNT; ++i) {
			if (monitors[i].has_laser && monitors[i].laser == laser) {
				release_locked(i);
			}
		}
	}
	k_mutex_unlock(&monitors_lock);
	return rc;
}
