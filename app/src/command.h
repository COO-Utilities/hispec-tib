//
// Created by Jeb Bailey on 5/27/25.
//

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


enum MsgType { MSG_GET, MSG_SET, ACK, RESP_OK, RESP_ERROR };
enum CommandSource { CMD_SRC_MQTT = 0, CMD_SRC_SERIAL = 1 };
enum OutMsgTarget { OUT_TARGET_MQTT = 0, OUT_TARGET_SERIAL = 1 };

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

struct OutMsg power_get(const struct Command *cmd);
struct OutMsg power_set(const struct Command *cmd);

struct OutMsg atten_setting_get(const struct Command *cmd);
struct OutMsg atten_setting_set(const struct Command *cmd);

struct OutMsg status_get(const struct Command *cmd);
struct OutMsg temp_get(const struct Command *cmd);

struct OutMsg sleep_set(const struct Command *cmd);


bool parse_msg_type_from_payload(const char *payload, enum MsgType *msg_type_out);
struct OutMsg invalid_command_response(const struct Command *cmd);
struct OutMsg unknown_response(const struct Command *cmd);
struct OutMsg unsupported_response(const struct Command *cmd);
struct OutMsg busy_response(const struct Command *cmd);
struct OutMsg serial_active_response(const struct Command *cmd);
struct OutMsg dispatch_command(const struct Command *cmd);

/* Executor task: consumes inbound_queue and publishes one response to outbound_queue. */
void command_executor_thread(void *p1, void *p2, void *p3);

/* Serial task: polls the console UART passed in p1 and queues complete command lines. */
void command_serial_thread(void *p1, void *p2, void *p3);

/* MQTT receive callback: copies topic/payload/properties into a queued Command. */
void command_handle_mqtt_publish(const struct mqtt_publish_param *pub);

/* Extend the serial-command holdoff window that temporarily disconnects MQTT control. */
void command_serial_note_activity(void);

bool command_network_mqtt_allowed(void);

/* Parse a mutable serial command line into a queued Command. */
void command_parse_serial_line(char *line);

/* Drain queued serial/MQTT responses; MQTT messages are retried when publish fails. */
void command_drain_outbound_queue(struct mqtt_client *client, bool mqtt_available);


extern struct k_msgq inbound_queue;
extern struct k_msgq outbound_queue;

#endif //COMMAND_H
