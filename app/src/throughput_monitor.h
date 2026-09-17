/**
 * @file throughput_monitor.h
 * @brief Throughput monitor command worker and photodiode stream ownership.
 *
 * The monitor owns streaming publication and optional autolevel decisions. It
 * reads photodiode snapshots, attenuator state, command-supplied route losses, and laser
 * estimates, but it does not read the ADC directly or publish MQTT directly.
 */

#ifndef HISPEC_THROUGHPUT_MONITOR_H
#define HISPEC_THROUGHPUT_MONITOR_H

#include <stdbool.h>
#include <stdint.h>

#include "lasers.h"
#include "photodiode.h"

#define THROUGHPUT_DEFAULT_INITIAL_LEVEL 0.5

struct throughput_monitor_request {
	/* Resolved run configuration. Return correction always applies; launch
	 * transmission is NaN when the external source power is unknown.
	 */
	double pd_route_tx;
	double laser_route_tx;
	enum hispec_laser_id laser;
	enum photodiode_channel channel;
	bool has_laser;
	bool autolevel;
	bool binary;
	char fiber;
	uint32_t off_in_s;
	double max_flux_ph_s;
	double initial_level;
};

struct throughput_monitor_status {
	bool active;
	enum photodiode_channel channel;
	const char *laser_name;
	bool autolevel;
};

/** Background thread; wakes on fresh ADC state and enqueues best-effort telemetry. */
void throughput_monitor_thread(void *p1, void *p2, void *p3);

/** Check exclusions and quiesce the target before the command changes routes.
 * Both channels can stream; only one autolevel operation may own the shared
 * optical path. Dark/calibration acquisition excludes monitoring. Replacing an
 * owned source stops its laser (may block on Modbus); a same-source continuation
 * retains the shutdown obligation. On error, no routing should be attempted.
 */
int throughput_monitor_prepare_start(const struct throughput_monitor_request *request);

/** Start after successful prepare and command-owned route setup. Command dispatch
 * serializes that sequence. Copies resolved losses for the run; restart to change
 * them. May block on PD power, DAC and laser I/O. Any failure after prepare must
 * use throughput_monitor_stop, including route setup failures in the command.
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

/** Disable adjustments when another command changes a monitored attenuator.
 * Streaming continues; stopping the operation still stops its autolevel laser.
 */
void throughput_monitor_note_attenuator_changed(uint8_t attenuator_index);

/** Disable autolevel before a manual laser level write, retaining streaming and
 * any owned laser shutdown. The sampling loop refreshes confirmed source state.
 * Pass stop_monitoring=true for tuning/settings changes to relinquish the run.
 * Takes the monitor lock; does not perform hardware I/O or change laser output.
 */
void throughput_monitor_note_laser_changed(enum hispec_laser_id laser, bool stop_monitoring);

/** Quiesce this laser's stream and apply validated settings while serializing
 * against autolevel. May block on laser I/O/NVS. Failed updates retain the
 * owned laser identity for stop retry; success relinquishes the stopped stream.
 */
int throughput_monitor_update_laser_settings(enum hispec_laser_id laser,
	const struct app_laser_channel_settings *settings, bool persist);

#endif /* HISPEC_THROUGHPUT_MONITOR_H */
