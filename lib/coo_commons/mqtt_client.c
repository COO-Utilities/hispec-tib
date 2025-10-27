/*
 * Copyright (c) 2025 Caltech Optical Observatories
 * SPDX-License-Identifier: Apache-2.0
 */

#include <coo_commons/mqtt_client.h>
#include <zephyr/logging/log.h>
#include <zephyr/net/socket.h>
#include <string.h>

LOG_MODULE_REGISTER(coo_mqtt, LOG_LEVEL_DBG);

/* Buffers for MQTT client */
static uint8_t rx_buffer[CONFIG_COO_MQTT_PAYLOAD_SIZE];
static uint8_t tx_buffer[CONFIG_COO_MQTT_PAYLOAD_SIZE];

/* MQTT broker details */
static struct sockaddr_storage broker;

/* Socket descriptor */
static struct zsock_pollfd fds[1];
static int nfds;

/* MQTT connectivity status flag */
static bool mqtt_connected;

/* MQTT client ID buffer */
static uint8_t client_id[50];

/* User callback for messages */
static mqtt_message_cb_t user_mqtt_cb = NULL;

/* Subscriptions */
#define MAX_SUBSCRIPTIONS 4
static struct mqtt_topic subscriptions[MAX_SUBSCRIPTIONS];
static int num_subscriptions = 0;

/* Retry configuration */
#define MSECS_WAIT_RECONNECT   5000
#define MSECS_NET_POLL_TIMEOUT 30000

void coo_mqtt_set_message_callback(mqtt_message_cb_t cb)
{
	user_mqtt_cb = cb;
}

int coo_mqtt_add_subscription(const char *topic_str, uint8_t qos)
{
	if (num_subscriptions >= MAX_SUBSCRIPTIONS) {
		return -ENOMEM;
	}
	subscriptions[num_subscriptions].topic.utf8 = topic_str;
	subscriptions[num_subscriptions].topic.size = strlen(topic_str);
	subscriptions[num_subscriptions].qos = qos;
	num_subscriptions++;
	return 0;
}

bool coo_mqtt_is_connected(void)
{
	return mqtt_connected;
}

static void prepare_fds(struct mqtt_client *client)
{
	if (client->transport.type == MQTT_TRANSPORT_NON_SECURE) {
		fds[0].fd = client->transport.tcp.sock;
	}

	fds[0].events = ZSOCK_POLLIN;
	nfds = 1;
}

static void clear_fds(void)
{
	nfds = 0;
}

static inline void on_mqtt_connect(void)
{
	mqtt_connected = true;
	LOG_INF("Connected to MQTT broker!");
	LOG_INF("Hostname: %s", CONFIG_COO_MQTT_BROKER_HOSTNAME);
	LOG_INF("Client ID: %s", client_id);
	LOG_INF("Port: %s", CONFIG_COO_MQTT_BROKER_PORT);
}

static inline void on_mqtt_disconnect(void)
{
	mqtt_connected = false;
	clear_fds();
	LOG_INF("Disconnected from MQTT broker");
}

/** Called when an MQTT payload is received */
static void on_mqtt_publish(struct mqtt_client *const client, const struct mqtt_evt *evt)
{
	int rc;
	uint8_t payload[CONFIG_COO_MQTT_PAYLOAD_SIZE + 1] = {0};

	rc = mqtt_read_publish_payload(client, payload, CONFIG_COO_MQTT_PAYLOAD_SIZE);
	if (rc < 0) {
		LOG_ERR("Failed to read received MQTT payload [%d]", rc);
		return;
	}

	/* Place null terminator at end of payload buffer */
	payload[rc] = '\0';

	LOG_INF("MQTT payload received!");
	LOG_INF("topic: '%s', payload: %s", evt->param.publish.message.topic.topic.utf8, payload);

	struct mqtt_publish_param *const publish_param = &evt->param.publish;
	publish_param->message.payload.data = payload;
	publish_param->message.payload.len = rc;

	if (user_mqtt_cb) {
		user_mqtt_cb(publish_param);
	}
}

/** Handler for asynchronous MQTT events */
static void mqtt_event_handler(struct mqtt_client *const client, const struct mqtt_evt *evt)
{
	switch (evt->type) {
	case MQTT_EVT_CONNACK:
		if (evt->result != 0) {
			LOG_ERR("MQTT Event Connect failed [%d]", evt->result);
			break;
		}
		on_mqtt_connect();
		break;

	case MQTT_EVT_DISCONNECT:
		on_mqtt_disconnect();
		break;

	case MQTT_EVT_PINGRESP:
		LOG_DBG("PINGRESP packet");
		break;

	case MQTT_EVT_PUBACK:
		if (evt->result != 0) {
			LOG_ERR("MQTT PUBACK error [%d]", evt->result);
			break;
		}
		LOG_DBG("PUBACK packet ID: %u", evt->param.puback.message_id);
		break;

	case MQTT_EVT_PUBREC:
		if (evt->result != 0) {
			LOG_ERR("MQTT PUBREC error [%d]", evt->result);
			break;
		}

		LOG_DBG("PUBREC packet ID: %u", evt->param.pubrec.message_id);

		const struct mqtt_pubrel_param rel_param = {
			.message_id = evt->param.pubrec.message_id
		};

		mqtt_publish_qos2_release(client, &rel_param);
		break;

	case MQTT_EVT_PUBREL:
		if (evt->result != 0) {
			LOG_ERR("MQTT PUBREL error [%d]", evt->result);
			break;
		}

		LOG_DBG("PUBREL packet ID: %u", evt->param.pubrel.message_id);

		const struct mqtt_pubcomp_param rec_param = {
			.message_id = evt->param.pubrel.message_id
		};

		mqtt_publish_qos2_complete(client, &rec_param);
		break;

	case MQTT_EVT_PUBCOMP:
		if (evt->result != 0) {
			LOG_ERR("MQTT PUBCOMP error %d", evt->result);
			break;
		}

		LOG_DBG("PUBCOMP packet ID: %u", evt->param.pubcomp.message_id);
		break;

	case MQTT_EVT_SUBACK:
		if (evt->result == MQTT_SUBACK_FAILURE) {
			LOG_ERR("MQTT SUBACK error [%d]", evt->result);
			break;
		}

		LOG_INF("SUBACK packet ID: %d", evt->param.suback.message_id);
		break;

	case MQTT_EVT_PUBLISH:
		const struct mqtt_publish_param *p = &evt->param.publish;

		if (p->message.topic.qos == MQTT_QOS_1_AT_LEAST_ONCE) {
			const struct mqtt_puback_param ack_param = {
				.message_id = p->message_id
			};
			mqtt_publish_qos1_ack(client, &ack_param);
		} else if (p->message.topic.qos == MQTT_QOS_2_EXACTLY_ONCE) {
			const struct mqtt_pubrec_param rec_param = {
				.message_id = p->message_id
			};
			mqtt_publish_qos2_receive(client, &rec_param);
		}

		on_mqtt_publish(client, evt);
		break;

	default:
		break;
	}
}

/** Poll the MQTT socket for received data */
static int poll_mqtt_socket(struct mqtt_client *client, int timeout)
{
	int rc;

	prepare_fds(client);

	if (nfds <= 0) {
		return -EINVAL;
	}

	rc = zsock_poll(fds, nfds, timeout);
	if (rc < 0) {
		LOG_ERR("Socket poll error [%d]", rc);
	}

	return rc;
}

int coo_mqtt_subscribe(struct mqtt_client *client)
{
	int rc;

	const struct mqtt_subscription_list sub_list = {
		.list = subscriptions,
		.list_count = num_subscriptions,
		.message_id = 5841u
	};

	LOG_INF("Subscribing to %d topic(s)", sub_list.list_count);

	rc = mqtt_subscribe(client, &sub_list);
	if (rc != 0) {
		LOG_ERR("MQTT Subscribe failed [%d]", rc);
	}

	return rc;
}

int coo_mqtt_process(struct mqtt_client *client)
{
	int rc;

	rc = poll_mqtt_socket(client, mqtt_keepalive_time_left(client));
	if (rc != 0) {
		if (fds[0].revents & ZSOCK_POLLIN) {
			/* MQTT data received */
			rc = mqtt_input(client);
			if (rc != 0) {
				LOG_ERR("MQTT Input failed [%d]", rc);
				return rc;
			}
			/* Socket error */
			if (fds[0].revents & (ZSOCK_POLLHUP | ZSOCK_POLLERR)) {
				LOG_ERR("MQTT socket closed / error");
				return -ENOTCONN;
			}
		}
	} else {
		/* Socket poll timed out, time to call mqtt_live() */
		rc = mqtt_live(client);
		if (rc != 0) {
			LOG_ERR("MQTT Live failed [%d]", rc);
			return rc;
		}
	}

	return 0;
}

void coo_mqtt_run(struct mqtt_client *client)
{
	int rc;

	/* Subscribe to MQTT topics */
	coo_mqtt_subscribe(client);

	/* Thread will primarily remain in this loop */
	while (mqtt_connected) {
		rc = coo_mqtt_process(client);
		if (rc != 0) {
			break;
		}
	}
	/* Gracefully close connection */
	mqtt_disconnect(client, NULL);
}

void coo_mqtt_connect(struct mqtt_client *client)
{
	int rc = 0;

	mqtt_connected = false;

	/* Block until MQTT CONNACK event callback occurs */
	while (!mqtt_connected) {
		rc = mqtt_connect(client);
		if (rc != 0) {
			LOG_ERR("MQTT Connect failed [%d]", rc);
			k_msleep(MSECS_WAIT_RECONNECT);
			continue;
		}

		/* Poll MQTT socket for response */
		rc = poll_mqtt_socket(client, MSECS_NET_POLL_TIMEOUT);
		if (rc > 0) {
			mqtt_input(client);
		}

		if (!mqtt_connected) {
			mqtt_abort(client);
		}
	}
}

int coo_mqtt_init(struct mqtt_client *client, const char *id_str)
{
	int rc;
	uint8_t broker_ip[NET_IPV4_ADDR_LEN];
	struct sockaddr_in *broker4;
	struct addrinfo *result;
	const struct addrinfo hints = {
		.ai_family = AF_INET,
		.ai_socktype = SOCK_STREAM
	};

	/* Resolve IP address of MQTT broker */
	rc = getaddrinfo(CONFIG_COO_MQTT_BROKER_HOSTNAME,
				CONFIG_COO_MQTT_BROKER_PORT, &hints, &result);
	if (rc != 0) {
		LOG_ERR("Failed to resolve broker hostname [%s]", gai_strerror(rc));
		return -EIO;
	}
	if (result == NULL) {
		LOG_ERR("Broker address not found");
		return -ENOENT;
	}

	broker4 = (struct sockaddr_in *)&broker;
	broker4->sin_addr.s_addr = ((struct sockaddr_in *)result->ai_addr)->sin_addr.s_addr;
	broker4->sin_family = AF_INET;
	broker4->sin_port = ((struct sockaddr_in *)result->ai_addr)->sin_port;
	freeaddrinfo(result);

	/* Log resolved IP address */
	inet_ntop(AF_INET, &broker4->sin_addr.s_addr, broker_ip, sizeof(broker_ip));
	LOG_INF("Connecting to MQTT broker @ %s", broker_ip);

	/* MQTT client configuration */
	strncpy(client_id, id_str, sizeof(client_id) - 1);
	client_id[sizeof(client_id) - 1] = '\0';

	mqtt_client_init(client);
	client->broker = &broker;
	client->evt_cb = mqtt_event_handler;
	client->client_id.utf8 = client_id;
	client->client_id.size = strlen(client->client_id.utf8);
	client->password = NULL;
	client->user_name = NULL;
	client->protocol_version = MQTT_VERSION_5_0;

	/* MQTT buffers configuration */
	client->rx_buf = rx_buffer;
	client->rx_buf_size = sizeof(rx_buffer);
	client->tx_buf = tx_buffer;
	client->tx_buf_size = sizeof(tx_buffer);

	/* MQTT transport configuration */
	client->transport.type = MQTT_TRANSPORT_NON_SECURE;

	return 0;
}
