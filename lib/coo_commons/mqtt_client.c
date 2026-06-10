/**
 * @file mqtt_client.c
 * @brief Blocking MQTT connect/process helpers around Zephyr MQTT.
 *
 * Incoming payload bytes are copied into a stack buffer before the user
 * callback runs. Outgoing publishes are intentionally left to the application.
 */
/*
 * Copyright (c) 2025 Caltech Optical Observatories
 * SPDX-License-Identifier: Apache-2.0
 */

#include <coo_commons/mqtt_client.h>
#include <zephyr/logging/log.h>
#include <zephyr/net/socket.h>
#include <zephyr/net/net_ip.h>
#include <zephyr/sys/util.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>

LOG_MODULE_REGISTER(coo_mqtt, CONFIG_COO_MQTT_LOG_LEVEL);

/* Buffers for MQTT client */
static uint8_t rx_buffer[CONFIG_COO_MQTT_PAYLOAD_SIZE];
static uint8_t tx_buffer[CONFIG_COO_MQTT_PAYLOAD_SIZE];
static uint8_t publish_payload[CONFIG_COO_MQTT_PAYLOAD_SIZE + 1U];

/* MQTT broker details */
static struct sockaddr_storage broker;
static struct coo_mqtt_broker_config active_broker_cfg;

/* Socket descriptor */
static struct zsock_pollfd fds[1];
static int nfds;

/* MQTT connectivity status flag */
static bool mqtt_connected;

/* MQTT client ID buffer */
static uint8_t client_id[50];

/* User callback for messages */
static mqtt_message_cb_t user_mqtt_cb = NULL;
static void *user_mqtt_cb_data;

/* Subscriptions */
#define MAX_SUBSCRIPTIONS 4
static struct mqtt_topic subscriptions[MAX_SUBSCRIPTIONS];
static int num_subscriptions = 0;

/* Keep failed broker attempts below the application watchdog interval. */
#define MSECS_NET_POLL_TIMEOUT 3000
/* Keep connected idle polling short enough for the application watchdog loop. */
#define MSECS_PROCESS_POLL_TIMEOUT 100

bool coo_mqtt_parse_broker_endpoint(const char *endpoint,
				    struct coo_mqtt_broker_config *cfg)
{
	const char *colon;
	char *end = NULL;
	unsigned long port;
	size_t host_size;

	if (endpoint == NULL || cfg == NULL) {
		return false;
	}

	colon = strrchr(endpoint, ':');
	if (colon == NULL || colon == endpoint || colon[1] == '\0') {
		return false;
	}

	host_size = (size_t)(colon - endpoint);
	if (host_size >= sizeof(cfg->host)) {
		return false;
	}

	errno = 0;
	port = strtoul(colon + 1, &end, 10);
	if (errno != 0 || end == colon + 1 || *end != '\0' ||
	    port == 0UL || port > UINT16_MAX) {
		return false;
	}

	memcpy(cfg->host, endpoint, host_size);
	cfg->host[host_size] = '\0';
	cfg->port = (uint16_t)port;
	return true;
}

int coo_mqtt_format_broker_endpoint(const struct coo_mqtt_broker_config *cfg,
				    char *out, size_t out_len)
{
	int written;

	if (cfg == NULL || out == NULL || out_len == 0U) {
		return -EINVAL;
	}

	written = snprintk(out, out_len, "%s:%u", cfg->host, cfg->port);
	if (written < 0 || written >= (int)out_len) {
		return -ENOSPC;
	}

	return 0;
}

static int resolve_broker_addr_for_config(const struct coo_mqtt_broker_config *cfg,
					  struct sockaddr_storage *addr_out,
					  char *resolved_ip,
					  size_t resolved_ip_len)
{
	int rc;
	char port_str[6];
	char broker_ip[NET_IPV4_ADDR_LEN];
	struct sockaddr_in broker4 = {0};
	struct zsock_addrinfo *result = NULL;
	struct in_addr numeric_addr = { 0 };
#if defined(CONFIG_DNS_RESOLVER)
	const bool dns_supported = true;
#else
	const bool dns_supported = false;
#endif
	const struct zsock_addrinfo hints = {
		.ai_family = AF_INET,
		.ai_socktype = SOCK_STREAM
	};

	if (cfg == NULL || cfg->host[0] == '\0' || cfg->port == 0U) {
		LOG_ERR("Broker config missing");
		return -EINVAL;
	}
	(void)snprintk(port_str, sizeof(port_str), "%u", cfg->port);

	if (net_addr_pton(AF_INET, cfg->host, &numeric_addr) == 0) {
		broker4.sin_family = AF_INET;
		broker4.sin_port = htons(cfg->port);
		broker4.sin_addr = numeric_addr;
		goto log_addr;
	}

	if (!dns_supported) {
		LOG_ERR("Broker '%s' is not numeric IPv4 and DNS_RESOLVER is disabled",
			cfg->host);
		return -ENOTSUP;
	}

	rc = zsock_getaddrinfo(cfg->host, port_str, &hints, &result);
	if (rc != 0) {
		LOG_ERR("Failed to resolve broker hostname [%s]", zsock_gai_strerror(rc));
		return -EIO;
	}
	if (result == NULL) {
		LOG_ERR("Broker address not found");
		return -ENOENT;
	}

	broker4.sin_addr.s_addr = ((struct sockaddr_in *)result->ai_addr)->sin_addr.s_addr;
	broker4.sin_family = AF_INET;
	broker4.sin_port = ((struct sockaddr_in *)result->ai_addr)->sin_port;
	zsock_freeaddrinfo(result);

log_addr:
	if (net_addr_ntop(AF_INET, &broker4.sin_addr, broker_ip, sizeof(broker_ip)) == NULL) {
		snprintk(broker_ip, sizeof(broker_ip), "?.?.?.?");
	}
	LOG_INF("MQTT broker resolved: %s:%u", broker_ip, cfg->port);
	if (resolved_ip != NULL && resolved_ip_len > 0U) {
		strncpy(resolved_ip, broker_ip, resolved_ip_len - 1U);
		resolved_ip[resolved_ip_len - 1U] = '\0';
	}
	if (addr_out != NULL) {
		memset(addr_out, 0, sizeof(*addr_out));
		memcpy(addr_out, &broker4, sizeof(broker4));
	}

	return 0;
}

int coo_mqtt_resolve_broker_config(const struct coo_mqtt_broker_config *cfg,
				   char *resolved_ip, size_t resolved_ip_len)
{
	return resolve_broker_addr_for_config(cfg, NULL, resolved_ip, resolved_ip_len);
}

static int resolve_broker_addr(void)
{
	return resolve_broker_addr_for_config(&active_broker_cfg, &broker, NULL, 0U);
}

int coo_mqtt_set_broker_config(const struct coo_mqtt_broker_config *cfg)
{
	if (cfg == NULL || cfg->host[0] == '\0' || cfg->port == 0U) {
		return -EINVAL;
	}

	strncpy(active_broker_cfg.host, cfg->host, sizeof(active_broker_cfg.host) - 1U);
	active_broker_cfg.host[sizeof(active_broker_cfg.host) - 1U] = '\0';
	active_broker_cfg.port = cfg->port;

	return 0;
}

void coo_mqtt_set_message_callback(mqtt_message_cb_t cb, void *user_data)
{
	user_mqtt_cb = cb;
	user_mqtt_cb_data = user_data;
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
	LOG_INF("Hostname: %s", active_broker_cfg.host);
	LOG_INF("Client ID: %s", client_id);
	LOG_INF("Port: %u", active_broker_cfg.port);
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
	struct mqtt_publish_param publish_param = evt->param.publish;

	/* mqtt_read_publish_payload() drains Zephyr's MQTT RX buffer into a local
	 * buffer so the command layer can copy it before this event handler returns.
	 */
	rc = mqtt_read_publish_payload(client, publish_payload, CONFIG_COO_MQTT_PAYLOAD_SIZE);
	if (rc < 0) {
		LOG_ERR("Failed to read received MQTT payload [%d]", rc);
		return;
	}

	/* Place null terminator at end of payload buffer */
	publish_payload[rc] = '\0';

	LOG_INF("MQTT payload received!");
	LOG_INF("topic: '%.*s', payload: %s",
		(int)evt->param.publish.message.topic.topic.size,
		evt->param.publish.message.topic.topic.utf8,
		publish_payload);

	publish_param.message.payload.data = publish_payload;
	publish_param.message.payload.len = rc;

	if (user_mqtt_cb) {
		user_mqtt_cb(&publish_param, user_mqtt_cb_data);
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

	/* zsock_poll() blocks the caller until MQTT input or keepalive work is due. */
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
	int keepalive_ms = mqtt_keepalive_time_left(client);
	int timeout_ms = keepalive_ms;
	bool waited_for_keepalive;

	if (timeout_ms < 0 || timeout_ms > MSECS_PROCESS_POLL_TIMEOUT) {
		timeout_ms = MSECS_PROCESS_POLL_TIMEOUT;
	}
	waited_for_keepalive = (keepalive_ms >= 0 && timeout_ms == keepalive_ms);

	rc = poll_mqtt_socket(client, timeout_ms);
	if (rc < 0) {
		return rc;
	}

	if (rc > 0) {
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
	} else if (waited_for_keepalive) {
		/* Socket poll reached the MQTT keepalive deadline. */
		rc = mqtt_live(client);
		if (rc == -EAGAIN) {
			return 0;
		}
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

int coo_mqtt_connect(struct mqtt_client *client)
{
	int rc;

	mqtt_connected = false;

	rc = resolve_broker_addr();
	if (rc != 0) {
		return rc;
	}

	rc = mqtt_connect(client);
	if (rc != 0) {
		LOG_ERR("MQTT Connect failed [%d]", rc);
		return rc;
	}

	/* Poll MQTT socket for CONNACK */
	rc = poll_mqtt_socket(client, MSECS_NET_POLL_TIMEOUT);
	if (rc < 0) {
		mqtt_abort(client);
		return rc;
	}

	if (rc > 0) {
		rc = mqtt_input(client);
		if (rc != 0) {
			mqtt_abort(client);
			return rc;
		}
	}

	if (!mqtt_connected) {
		mqtt_abort(client);
		return -ETIMEDOUT;
	}

	return 0;
}

int coo_mqtt_init(struct mqtt_client *client, const char *id_str)
{
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
