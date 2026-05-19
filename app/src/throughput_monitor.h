/**
 * @file throughput_monitor.h
 * @brief Throughput monitor command worker and photodiode stream ownership.
 *
 * The monitor owns streaming publication and optional autolevel decisions. It
 * reads photodiode snapshots, attenuator state, route-loss settings, and laser
 * estimates, but it does not read the ADC directly or publish MQTT directly.
 */

#ifndef HISPEC_THROUGHPUT_MONITOR_H
#define HISPEC_THROUGHPUT_MONITOR_H

#include <stdbool.h>
#include <stdint.h>

#include "lasers.h"
#include "photodiode.h"

struct throughput_monitor_request {
	enum hispec_laser_id laser;
	bool autolevel;
	bool binary;
	char fiber;
	uint32_t stopafter_s;
};

struct throughput_monitor_status {
	bool active;
	enum photodiode_channel channel;
	const char *laser_name;
	bool autolevel;
};

#define THROUGHPUT_MONITOR_INTERVAL_MS 100U

/**
 * Run one throughput/autolevel service pass.
 *
 * This may enqueue best-effort telemetry and may block indirectly when
 * autolevel adjusts attenuators or laser output. It does not publish MQTT
 * directly.
 */
void throughput_monitor_run_once(void);

/** Start or replace the monitor associated with the request's photodiode. */
int throughput_monitor_start(const struct throughput_monitor_request *request,
			     struct throughput_monitor_status *status);

/** Stop one channel or both channels. Pass PHOTODIODE_CHANNEL_COUNT for all. */
int throughput_monitor_stop(uint8_t channel, struct throughput_monitor_status *status);

/** Return true while autolevel owns the selected photodiode stream. */
bool throughput_monitor_autolevel_active(enum photodiode_channel channel);

/** Disable autolevel when another command changes a monitored attenuator. */
void throughput_monitor_note_attenuator_changed(uint8_t attenuator_index);

/** Stop any monitor using a laser whose output/settings changed externally. */
void throughput_monitor_note_laser_changed(enum hispec_laser_id laser);

#endif /* HISPEC_THROUGHPUT_MONITOR_H */
