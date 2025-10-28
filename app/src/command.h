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

#define MAX_TOPIC_LEN 64
#define MAX_KEY_LEN   48
#define MAX_REQID_LEN 32
#define MAX_SESSION_ID_LEN 48
#define MAX_PAYLOAD_LEN CONFIG_COO_MQTT_PAYLOAD_SIZE
#define MAX_CORRELATION_DATA 16
#define MAX_PENDING_COMMANDS 2


enum MsgType { GET, SET, ACK, RESP_OK, RESP_ERROR };

struct Command {
	enum MsgType msg_type;

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

struct OutMsg mems_get(const struct Command *cmd);
struct OutMsg mems_set(const struct Command *cmd);

struct OutMsg laser_setting_get(const struct Command *cmd);
struct OutMsg laser_setting_set(const struct Command *cmd);

struct OutMsg power_get(const struct Command *cmd);
struct OutMsg power_set(const struct Command *cmd);

struct OutMsg atten_setting_get(const struct Command *cmd);
struct OutMsg atten_setting_set(const struct Command *cmd);

struct OutMsg status_get(const struct Command *cmd);

struct OutMsg sleep_set(const struct Command *cmd);


bool parse_msg_type_from_payload(const char *payload, enum MsgType *msg_type_out);
struct OutMsg invalid_command_response(const struct Command *cmd);
struct OutMsg unknown_response(const struct Command *cmd);
struct OutMsg unsupported_response(const struct Command *cmd);
struct OutMsg busy_response(const struct Command *cmd);
struct OutMsg dispatch_command(const struct Command *cmd);


extern struct k_msgq inbound_queue;
extern struct k_msgq outbound_queue;

#endif //COMMAND_H
