/*
 * Copyright (c) 2025 Caltech Optical Observatories
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef COO_COMMONS_MQTT_CLIENT_H
#define COO_COMMONS_MQTT_CLIENT_H

#include <zephyr/net/mqtt.h>
#include <zephyr/kernel.h>

/**
 * @file mqtt_client.h
 * @brief Small MQTT 5 wrapper used by HISPEC-TIB.
 *
 * The wrapper owns one global broker config, subscription table, RX/TX buffers,
 * and connection flag. The application still owns when to connect, subscribe,
 * process, publish responses, and disconnect.
 */

#define COO_MQTT_BROKER_HOST_MAX 128

struct coo_mqtt_broker_config {
	char host[COO_MQTT_BROKER_HOST_MAX];
	uint16_t port;
};

/**
 * @brief Parse one MQTT broker endpoint string.
 *
 * @param endpoint Endpoint in `<host-or-ip>:<port>` form.
 * @param cfg Destination broker config.
 * @return true on valid endpoint, false on malformed host or port.
 */
bool coo_mqtt_parse_broker_endpoint(const char *endpoint,
				    struct coo_mqtt_broker_config *cfg);

/**
 * @brief Format a broker config as `<host-or-ip>:<port>`.
 *
 * @return 0 on success, -ENOSPC if @p out is too small.
 */
int coo_mqtt_format_broker_endpoint(const struct coo_mqtt_broker_config *cfg,
				    char *out, size_t out_len);

/**
 * @brief Resolve a broker config without changing the active MQTT broker.
 *
 * Numeric IPv4 hosts succeed without DNS. Hostnames require DNS support and a
 * configured resolver that can return an IPv4 address. @p resolved_ip may be
 * NULL when the caller only needs validation.
 */
int coo_mqtt_resolve_broker_config(const struct coo_mqtt_broker_config *cfg,
				   char *resolved_ip, size_t resolved_ip_len);

/**
 * @brief MQTT message callback function type
 *
 * Called when an MQTT message is received on a subscribed topic.
 *
 * @param pub Pointer to MQTT publish parameters containing topic, payload, QoS, etc.
 * @param user_data Caller-supplied callback context.
 */
typedef void (*mqtt_message_cb_t)(const struct mqtt_publish_param *pub,
				  void *user_data);

/**
 * @brief Initialize the MQTT client
 *
 * Resolves the broker hostname, configures the MQTT client structure,
 * and prepares buffers for communication.
 *
 * @param client Pointer to MQTT client structure to initialize
 * @param client_id Client identifier string (will be copied)
 * @return 0 on success, negative error code on failure
 */
int coo_mqtt_init(struct mqtt_client *client, const char *client_id);

/**
 * @brief Set broker endpoint for subsequent MQTT connect attempts.
 *
 * @param cfg Broker hostname or numeric IPv4 and TCP port.
 * @return 0 on success, negative errno on invalid config.
 */
int coo_mqtt_set_broker_config(const struct coo_mqtt_broker_config *cfg);

/**
 * @brief Connect to the MQTT broker
 *
 * Attempts a single connect/handshake sequence.
 *
 * @param client Pointer to initialized MQTT client
 * @return 0 on success, negative error code on failure
 */
int coo_mqtt_connect(struct mqtt_client *client);

/**
 * @brief Add a subscription topic
 *
 * Must be called before calling coo_mqtt_subscribe(). Maximum of 4 subscriptions
 * are supported by default (can be configured).
 *
 * @param topic_str Topic string to subscribe to (supports wildcards)
 * @param qos Quality of Service level (0, 1, or 2)
 * @return 0 on success, -ENOMEM if subscription limit reached
 */
int coo_mqtt_add_subscription(const char *topic_str, uint8_t qos);

/**
 * @brief Subscribe to all registered topics
 *
 * Should be called after connection is established. Subscribes to all
 * topics previously registered with coo_mqtt_add_subscription().
 *
 * @param client Pointer to connected MQTT client
 * @return 0 on success, negative error code on failure
 */
int coo_mqtt_subscribe(struct mqtt_client *client);

/**
 * @brief Set the message received callback
 *
 * The callback will be invoked whenever a message is received on any
 * subscribed topic.
 *
 * @param cb Callback function pointer
 * @param user_data Caller-owned pointer passed to cb, or NULL if unused
 */
void coo_mqtt_set_message_callback(mqtt_message_cb_t cb, void *user_data);

/**
 * @brief Process MQTT events
 *
 * Must be called regularly in the main loop. Polls the MQTT socket,
 * handles incoming messages, and sends keep-alive packets.
 *
 * @param client Pointer to connected MQTT client
 * @return 0 on success, negative error code on failure (e.g., disconnection)
 */
int coo_mqtt_process(struct mqtt_client *client);

/**
 * @brief Main MQTT event loop
 *
 * Subscribes to topics and enters the main processing loop. Blocks until
 * disconnection occurs. After disconnection, gracefully closes the connection.
 *
 * Typical usage:
 * @code
 *   coo_mqtt_connect(&client);
 *   coo_mqtt_run(&client);  // blocks until disconnected
 * @endcode
 *
 * @param client Pointer to connected MQTT client
 */
void coo_mqtt_run(struct mqtt_client *client);

/**
 * @brief Check if MQTT client is currently connected
 *
 * @return true if connected, false otherwise
 */
bool coo_mqtt_is_connected(void);

#endif /* COO_COMMONS_MQTT_CLIENT_H */
