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
#include "app_identity.h"
#include "attenuator.h"
#include "command.h"
#include "devices.h"
#include "housekeeping.h"

#include <coo_commons/json_utils.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/sys/clock.h>
#include <zephyr/sys/util.h>

LOG_MODULE_REGISTER(throughput_monitor, LOG_LEVEL_INF);

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
	     "throughput binary telemetry uses little-endian float layout");

struct laser_pd_channel {
	enum hispec_laser_id laser;
	enum photodiode_channel channel;
};

struct throughput_state {
	bool active;
	bool autolevel;
	bool binary;
	enum hispec_laser_id laser;
	enum photodiode_channel channel;
	uint8_t attenuator_index;
	char fiber;
	float level_percent;
	int64_t started_ms;
	uint32_t stopafter_s;
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

static void put_f32(uint8_t *payload, size_t payload_len, size_t *offset, float value)
{
	/* STM32 binary telemetry is specified as little-endian IEEE-754. */
	put_bytes(payload, payload_len, offset, &value, sizeof(value));
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
	double next_tx;
	float next_percent;
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
		if (atten->linear < 0.999) {
			next_tx = atten->linear * 3.0;
			if (next_tx > 1.0) {
				next_tx = 1.0;
			}
			(void)attenuator_set_linear(&attenuators[state->attenuator_index], next_tx);
		} else if (state->level_percent < 100.0f) {
			next_percent = state->level_percent * 3.0f;
			if (next_percent > 100.0f) {
				next_percent = 100.0f;
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
		} else if (state->level_percent > 0.0f) {
			next_percent = state->level_percent / 3.0f;
			if (next_percent < 0.0f) {
				next_percent = 0.0f;
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
	struct attenuator_transmission_estimate atten = {0};
	struct hispec_laser_flux_estimate laser_flux = {0};
	struct coo_cmd_response msg = {0};
	size_t off = 0U;
	char pd_route[APP_ROUTE_LOSS_ROUTE_MAX_LEN] = {0};
	char laser_route[APP_ROUTE_LOSS_ROUTE_MAX_LEN] = {0};
	const char *laser_name = hispec_laser_name(state->laser);
	char channel_fiber[8] = {0};
	double pd_route_tx = 1.0;
	double laser_route_tx = 1.0;
	double pd_flux;
	double pd_flux_err;
	double emitted_flux;
	double emitted_flux_err;
	double tp = NAN;
	double tp_err = NAN;
	double tp_rms_err = NAN;
	double pd_ontime_s;
	double laser_current_ontime_s;

	if (laser_name == NULL ||
	    !attenuator_estimate_transmission(&attenuators[state->attenuator_index],
					      0.0, 0.0, &atten) ||
	    laser_estimate_flux(state->laser, 0.0f, 0.0f, &laser_flux) != 0) {
		return;
	}
	app_settings_get_photodiode(&pd_settings);

	channel_fiber_name(channel_fiber, sizeof(channel_fiber), state->channel, state->fiber);
	pd_ontime_s = housekeeping_power_on_time_s((enum housekeeping_power_output)state->channel);
	laser_current_ontime_s = hispec_laser_current_on_time_s(state->laser);
	route_name_for_pd(pd_route, sizeof(pd_route), state->channel, state->fiber);
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
			tp_err = fabs(tp) * sqrt((pd_flux_err / pd_flux) * (pd_flux_err / pd_flux) +
						 (emitted_flux_err / emitted_flux) *
						 (emitted_flux_err / emitted_flux));
		}
	}

	msg.target = COO_CMD_OUT_MQTT_BEST_EFFORT;
	msg.qos = 0;
	(void)coo_cmd_format_data_topic(app_mqtt_device_id(),
					state->channel == PHOTODIODE_CHANNEL_YJ ?
					"yj_tput" : "hk_tput",
					msg.topic, sizeof(msg.topic));

	if (state->binary) {
		put_bytes((uint8_t *)msg.payload, sizeof(msg.payload), &off,
			  channel_fiber, sizeof(channel_fiber));
		put_u64((uint8_t *)msg.payload, sizeof(msg.payload), &off, time_ms);
		put_f64((uint8_t *)msg.payload, sizeof(msg.payload), &off, tp);
		put_f64((uint8_t *)msg.payload, sizeof(msg.payload), &off, tp_err);
		put_f64((uint8_t *)msg.payload, sizeof(msg.payload), &off, tp_rms_err);
		put_f64((uint8_t *)msg.payload, sizeof(msg.payload), &off, pd_flux);
		put_f64((uint8_t *)msg.payload, sizeof(msg.payload), &off, pd_flux_err);
		put_f64((uint8_t *)msg.payload, sizeof(msg.payload), &off, emitted_flux);
		put_f64((uint8_t *)msg.payload, sizeof(msg.payload), &off, emitted_flux_err);
		put_f64((uint8_t *)msg.payload, sizeof(msg.payload), &off, pd_route_tx);
		put_f64((uint8_t *)msg.payload, sizeof(msg.payload), &off, laser_route_tx);
		put_f64((uint8_t *)msg.payload, sizeof(msg.payload), &off, atten.linear);
		put_i16((uint8_t *)msg.payload, sizeof(msg.payload), &off, pd->raw);
		put_f32((uint8_t *)msg.payload, sizeof(msg.payload), &off, pd->mv);
		put_f32((uint8_t *)msg.payload, sizeof(msg.payload), &off, pd->net_mv);
		put_f32((uint8_t *)msg.payload, sizeof(msg.payload), &off, pd->mean_mv_1s);
		put_f32((uint8_t *)msg.payload, sizeof(msg.payload), &off, pd->rms_mv_0p5s);
		put_f32((uint8_t *)msg.payload, sizeof(msg.payload), &off,
			(float)laser_flux.current_ma);
		put_f32((uint8_t *)msg.payload, sizeof(msg.payload), &off,
			(float)atten.attenuation_db);
		put_f32((uint8_t *)msg.payload, sizeof(msg.payload), &off,
			(float)laser_flux.wavelength_nm);
		put_f32((uint8_t *)msg.payload, sizeof(msg.payload), &off,
			(float)pd_ontime_s);
		put_f32((uint8_t *)msg.payload, sizeof(msg.payload), &off,
			(float)laser_current_ontime_s);
		msg.payload_len = off;
		(void)k_msgq_put(&outbound_queue, &msg, K_NO_WAIT);
		return;
	}

	if (coo_json_append(msg.payload, sizeof(msg.payload), &off,
			"{\"channel\":\"%s\",\"laser\":\"%s\","
			"\"autolevel\":%s,\"time\":%llu,\"tp\":%.6f,"
			"\"tp_err\":%.6f,\"tp_rms_err\":%.6f",
			channel_fiber, laser_name, state->autolevel ? "true" : "false",
			(unsigned long long)time_ms, tp, tp_err, tp_rms_err) != 0 ||
	    coo_json_append(msg.payload, sizeof(msg.payload), &off,
			",\"pd_flux_ph_s\":%.3e,\"pd_flux_err_ph_s\":%.3e,"
			"\"laser_flux_ph_s\":%.3e,\"laser_flux_err_ph_s\":%.3e",
			pd_flux, pd_flux_err, emitted_flux, emitted_flux_err) != 0 ||
	    coo_json_append(msg.payload, sizeof(msg.payload), &off,
			",\"pd_route_tx\":%.6f,\"laser_route_tx\":%.6f,"
			"\"atten_tx\":%.6f,\"pd_raw\":%d",
			pd_route_tx, laser_route_tx, atten.linear, pd->raw) != 0 ||
	    coo_json_append(msg.payload, sizeof(msg.payload), &off,
			",\"pd_mv\":%.2f,\"pd_net_mv\":%.2f,"
			"\"pd_mean_mv_1s\":%.2f,\"pd_rms_mv_0p5s\":%.2f",
			(double)pd->mv, (double)pd->net_mv,
			(double)pd->mean_mv_1s, (double)pd->rms_mv_0p5s) != 0 ||
	    coo_json_append(msg.payload, sizeof(msg.payload), &off,
			",\"laser_current_ma\":%.2f,\"atten_db\":%.3f,"
			"\"wavelength_nm\":%.1f,\"pd_ontime_s\":%.2f,"
			"\"laser_current_ontime_s\":%.2f,\"flags\":[]}",
			laser_flux.current_ma, atten.attenuation_db,
			laser_flux.wavelength_nm, pd_ontime_s,
			laser_current_ontime_s) != 0) {
		LOG_WRN("throughput telemetry payload too large");
		return;
	}
	msg.payload_len = strlen(msg.payload);

	(void)k_msgq_put(&outbound_queue, &msg, K_NO_WAIT);
}

void throughput_monitor_run_once(void)
{
	struct photodiode_status pd_status;
	struct throughput_state local[PHOTODIODE_CHANNEL_COUNT];
	int64_t now = k_uptime_get();
	uint64_t time_ms = realtime_ms();

	if (!devices_board_type_checked() || devices_board_type() != HISPEC_BOARD_TIB) {
		return;
	}

	photodiode_get_status(&pd_status);

	k_mutex_lock(&monitors_lock, K_FOREVER);
	memcpy(local, monitors, sizeof(local));
	k_mutex_unlock(&monitors_lock);

	for (uint8_t i = 0U; i < PHOTODIODE_CHANNEL_COUNT; ++i) {
		bool pd_power = false;
		struct attenuator_transmission_estimate atten = {0};

		if (!local[i].active) {
			continue;
		}

		if (local[i].stopafter_s > 0U &&
		    now - local[i].started_ms >= (int64_t)local[i].stopafter_s * 1000) {
			k_mutex_lock(&monitors_lock, K_FOREVER);
			stop_locked((enum photodiode_channel)i);
			k_mutex_unlock(&monitors_lock);
			continue;
		}

		if (housekeeping_power_get((enum housekeeping_power_output)i,
					   &pd_power) == 0 && !pd_power) {
			k_mutex_lock(&monitors_lock, K_FOREVER);
			stop_locked((enum photodiode_channel)i);
			k_mutex_unlock(&monitors_lock);
			continue;
		}

		if (local[i].autolevel &&
		    attenuator_estimate_transmission(&attenuators[local[i].attenuator_index],
						     0.0, 0.0, &atten)) {
			(void)autolevel_adjust(&local[i], &pd_status.channel[i],
					       &atten);
			k_mutex_lock(&monitors_lock, K_FOREVER);
			if (monitors[i].active && monitors[i].laser == local[i].laser) {
				monitors[i].level_percent = local[i].level_percent;
				monitors[i].high_count = local[i].high_count;
				monitors[i].low_count = local[i].low_count;
			}
			k_mutex_unlock(&monitors_lock);
		}

		publish_sample(&local[i], &pd_status.channel[i], time_ms);
	}
}

int throughput_monitor_start(const struct throughput_monitor_request *request,
			     struct throughput_monitor_status *status)
{
	enum photodiode_channel channel;
	uint8_t attenuator_index;
	struct throughput_state next = {0};
	int rc;

	if (request == NULL) {
		return -EINVAL;
	}

	if (photodiode_channel_for_laser(request->laser, &channel) != 0 ||
	    (request->fiber != 'M' && request->fiber != 'S')) {
		return -EINVAL;
	}
	rc = attenuator_index_from_laser_id(request->laser, &attenuator_index);
	if (rc != 0) {
		return rc;
	}

	rc = housekeeping_power_set((enum housekeeping_power_output)channel, true);
	if (rc != 0) {
		return rc;
	}

	next.active = true;
	next.autolevel = request->autolevel;
	next.binary = request->binary;
	next.laser = request->laser;
	next.channel = channel;
	next.attenuator_index = attenuator_index;
	next.fiber = request->fiber;
	next.stopafter_s = request->stopafter_s;
	next.started_ms = k_uptime_get();

	if (request->autolevel) {
		next.level_percent = 100.0f;
		(void)attenuator_set_db(&attenuators[attenuator_index], 120.0);
		rc = hispec_laser_set_output_percent_autooff(request->laser,
							     next.level_percent, 0U);
		if (rc != 0) {
			return rc;
		}
	}

	k_mutex_lock(&monitors_lock, K_FOREVER);
	monitors[channel] = next;
	if (status != NULL) {
		status->active = true;
		status->channel = channel;
		status->laser_name = hispec_laser_name(request->laser);
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
		if (monitors[i].active && monitors[i].laser == laser) {
			stop_locked((enum photodiode_channel)i);
		}
	}
	k_mutex_unlock(&monitors_lock);
}
