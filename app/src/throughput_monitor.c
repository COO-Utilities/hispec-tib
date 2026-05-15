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

#include <coo_commons/json_utils.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/sys/util.h>

LOG_MODULE_REGISTER(throughput_monitor, LOG_LEVEL_INF);

#define TP_INTERVAL_MS 100U
#define TP_ADC_USABLE_MV 5000.0
#define TP_LOW_FRACTION 0.20
#define TP_HIGH_FRACTION 0.80
#define TP_INSTANT_BAD_SAMPLES 5U
#define TP_MIN_ATTEN_TX 1.0e-9
#define PLANCK_J_S 6.62607015e-34
#define LIGHT_M_PER_S 299792458.0

BUILD_ASSERT((int)HISPEC_LASER_AUX_YJ_PHOTODIODE == (int)PHOTODIODE_CHANNEL_YJ,
	     "YJ photodiode relay index must match photodiode channel");
BUILD_ASSERT((int)HISPEC_LASER_AUX_HK_PHOTODIODE == (int)PHOTODIODE_CHANNEL_HK,
	     "HK photodiode relay index must match photodiode channel");
BUILD_ASSERT(IS_ENABLED(CONFIG_LITTLE_ENDIAN),
	     "throughput binary telemetry uses little-endian float layout");

struct pd_response {
	enum hispec_laser_id laser;
	enum photodiode_channel channel;
	double wavelength_nm;
	double responsivity_a_per_w;
	double transimpedance_v_per_a;
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

static const struct pd_response pd_responses[] = {
	{HISPEC_LASER_1028_Y, PHOTODIODE_CHANNEL_YJ, 1028.0, 0.66, 5.0e10},
	{HISPEC_LASER_1270_J, PHOTODIODE_CHANNEL_YJ, 1270.0, 0.85, 5.0e10},
	{HISPEC_LASER_1430_YJ, PHOTODIODE_CHANNEL_YJ, 1430.0, 0.93, 5.0e10},
	{HISPEC_LASER_1430_HK, PHOTODIODE_CHANNEL_HK, 1430.0, 0.5264, 2.375e9},
	{HISPEC_LASER_1510_H, PHOTODIODE_CHANNEL_HK, 1510.0, 0.60971, 2.375e9},
	{HISPEC_LASER_2330_K, PHOTODIODE_CHANNEL_HK, 2330.0, 1.23378, 2.375e9},
};

static struct throughput_state monitors[PHOTODIODE_CHANNEL_COUNT];
static K_MUTEX_DEFINE(monitors_lock);

static const struct pd_response *response_for_laser(enum hispec_laser_id laser)
{
	for (uint8_t i = 0U; i < ARRAY_SIZE(pd_responses); ++i) {
		if (pd_responses[i].laser == laser) {
			return &pd_responses[i];
		}
	}

	return NULL;
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

	clock_gettime(CLOCK_REALTIME, &ts);
	return ((uint64_t)ts.tv_sec * 1000ULL) + ((uint64_t)ts.tv_nsec / 1000000ULL);
}

static double pd_flux_from_mv(double net_mv, const struct pd_response *response)
{
	double signal_v;
	double power_w;
	double photon_j;

	if (response == NULL || net_mv <= 0.0) {
		return 0.0;
	}

	signal_v = net_mv / 1000.0;
	power_w = signal_v / (response->transimpedance_v_per_a *
			      response->responsivity_a_per_w);
	photon_j = PLANCK_J_S * LIGHT_M_PER_S / (response->wavelength_nm * 1.0e-9);
	return power_w / photon_j;
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
	const struct pd_response *response = response_for_laser(state->laser);
	struct attenuator_transmission_estimate atten = {0};
	struct hispec_laser_flux_estimate laser_flux = {0};
	struct OutMsg msg = {0};
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
	bool pd_route_configured;
	bool laser_route_configured;

	if (response == NULL || laser_name == NULL ||
	    !attenuator_estimate_transmission(&attenuators[state->attenuator_index],
					      0.0, 0.0, &atten) ||
	    laser_estimate_flux(state->laser, 0.0f, 0.0f, &laser_flux) != 0) {
		return;
	}

	channel_fiber_name(channel_fiber, sizeof(channel_fiber), state->channel, state->fiber);
	pd_ontime_s = hispec_laser_aux_power_on_time_s((enum hispec_laser_aux_output)state->channel);
	laser_current_ontime_s = hispec_laser_current_on_time_s(state->laser);
	route_name_for_pd(pd_route, sizeof(pd_route), state->channel, state->fiber);
	route_name_for_laser(laser_route, sizeof(laser_route), laser_name, state->fiber);
	(void)app_settings_get_route_loss(pd_route, laser_name, &pd_route_tx, &pd_route_configured);
	(void)app_settings_get_route_loss(laser_route, laser_name, &laser_route_tx,
					  &laser_route_configured);
	ARG_UNUSED(pd_route_configured);
	ARG_UNUSED(laser_route_configured);

	pd_flux = pd_flux_from_mv(pd->net_mv, response) / pd_route_tx;
	pd_flux_err = pd_flux_from_mv(pd->rms_mv_0p5s, response) / pd_route_tx;
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

	msg.target = OUT_TARGET_MQTT_BEST_EFFORT;
	msg.qos = 0;
	(void)app_mqtt_format_data_topic(state->channel == PHOTODIODE_CHANNEL_YJ ?
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

void throughput_monitor_thread(void *p1, void *p2, void *p3)
{
	ARG_UNUSED(p1);
	ARG_UNUSED(p2);
	ARG_UNUSED(p3);

	while (!devices_board_type_checked()) {
		k_sleep(K_MSEC(20));
	}

	while (1) {
		struct photodiode_status pd_status;
		struct throughput_state local[PHOTODIODE_CHANNEL_COUNT];
		int64_t now = k_uptime_get();
		uint64_t time_ms = realtime_ms();

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

			if (hispec_laser_aux_power_get((enum hispec_laser_aux_output)i,
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

		k_sleep(K_MSEC(TP_INTERVAL_MS));
	}
}

int throughput_monitor_start(const struct throughput_monitor_request *request,
			     struct throughput_monitor_status *status)
{
	const struct pd_response *response;
	uint8_t attenuator_index;
	struct throughput_state next = {0};
	int rc;

	if (request == NULL) {
		return -EINVAL;
	}

	response = response_for_laser(request->laser);
	if (response == NULL || (request->fiber != 'M' && request->fiber != 'S')) {
		return -EINVAL;
	}
	rc = attenuator_index_from_laser_id(request->laser, &attenuator_index);
	if (rc != 0) {
		return rc;
	}

	rc = hispec_laser_aux_power_set((enum hispec_laser_aux_output)response->channel, true);
	if (rc != 0) {
		return rc;
	}

	next.active = true;
	next.autolevel = request->autolevel;
	next.binary = request->binary;
	next.laser = request->laser;
	next.channel = response->channel;
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
	monitors[response->channel] = next;
	if (status != NULL) {
		status->active = true;
		status->channel = response->channel;
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
