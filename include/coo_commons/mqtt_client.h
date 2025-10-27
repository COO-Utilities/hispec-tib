/*
 * Copyright (c) 2025 Caltech Optical Observatories
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef COO_COMMONS_MQTT_CLIENT_H
#define COO_COMMONS_MQTT_CLIENT_H

#include <zephyr/net/mqtt.h>
#include <zephyr/kernel.h>

/**
 * @brief MQTT message callback function type
 *
 * Called when an MQTT message is received on a subscribed topic.
 *
 * @param pub Pointer to MQTT publish parameters containing topic, payload, QoS, etc.
 */
typedef void (*mqtt_message_cb_t)(const struct mqtt_publish_param *pub);

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
 * @brief Connect to the MQTT broker
 *
 * Blocks until connection is established or fails. Automatically retries
 * on failure with exponential backoff.
 *
 * @param client Pointer to initialized MQTT client
 */
void coo_mqtt_connect(struct mqtt_client *client);

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
 */
void coo_mqtt_set_message_callback(mqtt_message_cb_t cb);

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
