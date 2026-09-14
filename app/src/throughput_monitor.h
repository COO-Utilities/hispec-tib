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
	/* Already applied route; strings are consumed synchronously by start. */
	const char *input;
	const char *output;
	enum hispec_laser_id laser;
	enum photodiode_channel channel;
	bool has_laser;
	bool autolevel;
	bool binary;
	char fiber;
	uint32_t off_in_s;
	double max_flux_ph_s;
};

struct throughput_monitor_status {
	bool active;
	enum photodiode_channel channel;
	const char *laser_name;
	bool autolevel;
};

/** Background thread; sleeps between best-effort stream publications. */
void throughput_monitor_thread(void *p1, void *p2, void *p3);

/** Start or replace the monitor associated with the request's photodiode.
 * Captures route-loss settings for this run; restart to pick up their changes.
 * May block on hardware I/O; replacing an autolevel source stops its laser.
 * Both channels can stream. Dual autolevel is available for engineering use;
 * normal instrument light paths overlap and should use only one loop.
 */
int throughput_monitor_start(const struct throughput_monitor_request *request,
			     struct throughput_monitor_status *status);

/** Stop streaming and the laser used by this autolevel operation, including
 * after manual attenuation disables adjustments. Purely passive streams leave
 * laser output unchanged. Bank power, TECs, and other lasers remain unchanged.
 * May block on Modbus. On failure, streaming/autolevel remain disabled and a
 * later stop retries laser shutdown. All-channel stop attempts both channels.
 * Pass PHOTODIODE_CHANNEL_COUNT for all. Returns the first shutdown error.
 */
int throughput_monitor_stop(uint8_t channel, struct throughput_monitor_status *status);

/** Return true if either photodiode monitor is currently active. */
bool throughput_monitor_any_active(void);

/** Return true while autolevel owns the selected photodiode stream. */
bool throughput_monitor_autolevel_active(enum photodiode_channel channel);

/** Disable adjustments when another command changes a monitored attenuator.
 * Streaming continues; stopping the operation still stops its autolevel laser.
 */
void throughput_monitor_note_attenuator_changed(uint8_t attenuator_index);

/** Relinquish monitoring when a manual command changes this laser.
 * Does not change laser output; the caller owns the new manual setting.
 */
void throughput_monitor_note_laser_changed(enum hispec_laser_id laser);

#endif /* HISPEC_THROUGHPUT_MONITOR_H */
