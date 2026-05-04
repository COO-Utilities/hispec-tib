/**
 * @file command.h
 * @brief MQTT and serial command ingress, dispatch, and response queues.
 *
 * Commands from MQTT and the line-oriented serial console are normalized into
 * `struct Command` and executed by the common command executor thread. Handler
 * functions may touch hardware, sleep on Zephyr or bus I/O, enqueue warnings,
 * and return one `struct OutMsg` response for later serial/MQTT publication.
 */

#ifndef COMMAND_H
#define COMMAND_H

#include <zephyr/kernel.h>
#include <zephyr/data/json.h>
#include <zephyr/logging/log.h>
#include <zephyr/net/mqtt.h>
#include <string.h>

#define MAX_TOPIC_LEN 96
#define MAX_KEY_LEN   48
#define MAX_REQID_LEN 32
#define MAX_SESSION_ID_LEN 48
#define MAX_PAYLOAD_LEN CONFIG_COO_MQTT_PAYLOAD_SIZE
#define MAX_CORRELATION_DATA 16
#define MAX_PENDING_COMMANDS 2


/** Command request/response type understood by the dispatcher and builders. */
enum MsgType { MSG_GET, MSG_SET, ACK, RESP_OK, RESP_ERROR };

/** Ingress path used for response routing and serial-guard policy. */
enum CommandSource { CMD_SRC_MQTT = 0, CMD_SRC_SERIAL = 1 };

/** Publication target for responses, warnings, and telemetry. */
enum OutMsgTarget {
	OUT_TARGET_MQTT = 0,
	OUT_TARGET_SERIAL = 1,
	/* Fire-and-forget MQTT publication, used for warnings/telemetry that
	 * must not block command responses when MQTT is unavailable.
	 */
	OUT_TARGET_MQTT_BEST_EFFORT = 2,
};

struct Command {
	enum MsgType msg_type;
	enum CommandSource source;

	char key[MAX_KEY_LEN];  //topic instead
	char session_id[MAX_SESSION_ID_LEN]; //maybe or part of Mqtt?
	char response_topic[MAX_TOPIC_LEN];
	size_t payload_len;
	char payload[MAX_PAYLOAD_LEN];
	uint8_t correlation_data[MAX_CORRELATION_DATA];
	uint32_t corr_len;
};

/** Fully formatted outbound response or publication. */
struct OutMsg {
	enum MsgType msg_type;  // RES, ACK, ERROR
	enum OutMsgTarget target;
	char topic[MAX_TOPIC_LEN];
	uint8_t qos;
	size_t payload_len;
	char payload[MAX_PAYLOAD_LEN];
	uint8_t correlation_data[MAX_CORRELATION_DATA];
	size_t corr_len;
};

/** Work wrapper retained for possible Zephyr workqueue dispatch use. */
struct CommandWork {
	struct k_work work;
	struct Command cmd;
};


typedef struct OutMsg (*DispatchFunc)(const struct Command *cmd) ;

struct DispatchEntry {
	const char   *key;           /* e.g. "memsroute", "laser1/flux", etc. */
	DispatchFunc get_handler;    // may be none
	DispatchFunc set_handler;    // may be none
};


/* Handler prototypes for all commands (get/set where defined) */
struct OutMsg memsroute_get(const struct Command *cmd);
struct OutMsg memsroute_set(const struct Command *cmd);
/** Query one AS splitter channel, usually with command key split/yj or split/hk. */
struct OutMsg splitting_get(const struct Command *cmd);
/** Apply one AS-PCB splitter channel using channel, ratio1, and ratio2. */
struct OutMsg splitting_set(const struct Command *cmd);
struct OutMsg help_get(const struct Command *cmd);
struct OutMsg ip_get(const struct Command *cmd);
struct OutMsg ip_set(const struct Command *cmd);
struct OutMsg mqtt_get(const struct Command *cmd);
struct OutMsg mqtt_set(const struct Command *cmd);
struct OutMsg time_get(const struct Command *cmd);
struct OutMsg time_set(const struct Command *cmd);
struct OutMsg reboot_set(const struct Command *cmd);
struct OutMsg serial_guard_get(const struct Command *cmd);
struct OutMsg serial_guard_set(const struct Command *cmd);

struct OutMsg mems_get(const struct Command *cmd);
struct OutMsg mems_set(const struct Command *cmd);

struct OutMsg laser_setting_get(const struct Command *cmd);
struct OutMsg laser_setting_set(const struct Command *cmd);
/** Power on the TIB laser bank using the board power GPIO. */
struct OutMsg laserbank_poweron(const struct Command *cmd);
/** Power off the TIB laser bank using the board power GPIO. */
struct OutMsg laserbank_poweroff(const struct Command *cmd);
/** Clear laser-bank faults with a bounded laser-bank power cycle. */
struct OutMsg laserbank_clearfaults(const struct Command *cmd);


struct OutMsg atten_setting_get(const struct Command *cmd);
struct OutMsg atten_setting_set(const struct Command *cmd);
struct OutMsg pd_get(const struct Command *cmd);
struct OutMsg pd_set(const struct Command *cmd);
struct OutMsg pd_settings_get(const struct Command *cmd);
struct OutMsg pd_settings_set(const struct Command *cmd);

struct OutMsg status_get(const struct Command *cmd);
struct OutMsg temp_get(const struct Command *cmd);


/** Parse optional MQTT-style `msg_type` from JSON; missing/unknown returns false. */
bool parse_msg_type_from_payload(const char *payload, enum MsgType *msg_type_out);
struct OutMsg invalid_command_response(const struct Command *cmd);
struct OutMsg unknown_response(const struct Command *cmd);
struct OutMsg unsupported_response(const struct Command *cmd);
struct OutMsg busy_response(const struct Command *cmd);
struct OutMsg serial_active_response(const struct Command *cmd);
/** Dispatch one normalized command to the longest matching command-table entry. */
struct OutMsg dispatch_command(const struct Command *cmd);

/** Executor task: blocks on inbound_queue, runs a handler, and enqueues one response. */
void command_executor_thread(void *p1, void *p2, void *p3);

/**
 * @brief Initialize command-layer delayed actions.
 *
 * Registers the callbacks used by serial-override expiration and delayed
 * reboot. Call once before starting command ingress threads.
 */
int command_runtime_init(void);

/** Serial task: blocks on Zephyr console lines and queues normalized commands. */
void command_serial_thread(void *p1, void *p2, void *p3);

/**
 * @brief MQTT receive callback.
 *
 * Copies the MQTT topic, payload, response-topic property, and correlation data
 * before returning. Empty payload means GET; non-empty payload defaults to SET
 * unless JSON `msg_type:"get"` is present. Enqueues or publishes an immediate
 * error when serial guard or queue capacity rejects the command.
 */
void command_handle_mqtt_publish(const struct mqtt_publish_param *pub);

/** Extend the serial-command holdoff window that rejects MQTT command execution. */
void command_serial_note_activity(void);

/** Return false while serial override is active. */
bool command_network_mqtt_allowed(void);

/** Parse "<key> [payload]" into a queued Command; serial has no get/set words. */
void command_parse_serial_line(char *line);

/**
 * @brief Drain queued serial/MQTT responses.
 *
 * MQTT publishing happens only here from the main loop. Non-best-effort MQTT
 * messages are retried by requeueing when MQTT is down or publish fails.
 */
void command_drain_outbound_queue(struct mqtt_client *client, bool mqtt_available);


extern struct k_msgq inbound_queue;
extern struct k_msgq outbound_queue;

#endif //COMMAND_H
