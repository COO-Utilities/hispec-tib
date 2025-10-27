/*
 * Copyright (c) 2024 Caltech Optical Observatories
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef APP_LIB_NETWORK_H_
#define APP_LIB_NETWORK_H_

#include <zephyr/net/socket.h>

/**
 * @file network.h
 * @brief Network utilities for COO applications
 *
 * This module provides helper functions for robust socket setup,
 * connection management, and network diagnostics. Includes both
 * low-level socket utilities and high-level connection manager integration.
 */

/* ============================================================================
 * CONNECTION MANAGER - High-level network initialization
 * ============================================================================ */

/**
 * @brief Network event callback function type
 *
 * Called when network connectivity state changes.
 *
 * @param connected true if network is now connected, false if disconnected
 */
typedef void (*coo_network_event_cb_t)(bool connected);

/**
 * @brief Initialize network subsystem
 *
 * Sets up network event callbacks and brings up all network interfaces
 * managed by the connection manager. Supports both DHCP and static IP
 * configurations.
 *
 * @param event_cb Optional callback for network state changes (can be NULL)
 * @return 0 on success, negative error code on failure
 */
int coo_network_init(coo_network_event_cb_t event_cb);

/**
 * @brief Wait for network connectivity
 *
 * Blocks until network is ready (L4 connected). Logs status periodically
 * while waiting.
 *
 * @param timeout_ms Maximum time to wait in milliseconds (0 = wait forever)
 * @return 0 if network is ready, -ETIMEDOUT if timeout occurred
 */
int coo_network_wait_ready(uint32_t timeout_ms);

/**
 * @brief Check if network is currently connected
 *
 * @return true if network is ready, false otherwise
 */
bool coo_network_is_ready(void);

/**
 * @brief Log MAC address of default interface
 *
 * Helper function to print MAC address to console/logs.
 */
void coo_network_log_mac_addr(void);

/* ============================================================================
 * SOCKET UTILITIES - Low-level socket operations
 * ============================================================================ */

/**
 * @brief Create and configure a TCP socket
 *
 * @param port Port number to bind to (0 for client sockets)
 * @param is_server True if this is a server socket
 * @return Socket file descriptor, or negative error code
 */
int coo_net_tcp_socket_create(uint16_t port, bool is_server);

/**
 * @brief Create and configure a UDP socket
 *
 * @param port Port number to bind to
 * @return Socket file descriptor, or negative error code
 */
int coo_net_udp_socket_create(uint16_t port);

/**
 * @brief Connect to a TCP server with timeout
 *
 * @param sockfd Socket file descriptor
 * @param addr Server address
 * @param timeout_ms Timeout in milliseconds
 * @return 0 on success, negative error code on failure
 */
int coo_net_tcp_connect(int sockfd, const struct sockaddr *addr, int timeout_ms);

/**
 * @brief Send data with retry logic
 *
 * @param sockfd Socket file descriptor
 * @param buf Buffer containing data to send
 * @param len Length of data
 * @param max_retries Maximum number of retry attempts
 * @return Number of bytes sent, or negative error code
 */
int coo_net_send_retry(int sockfd, const void *buf, size_t len, int max_retries);

/**
 * @brief Receive data with timeout
 *
 * @param sockfd Socket file descriptor
 * @param buf Buffer to store received data
 * @param len Maximum bytes to receive
 * @param timeout_ms Timeout in milliseconds
 * @return Number of bytes received, or negative error code
 */
int coo_net_recv_timeout(int sockfd, void *buf, size_t len, int timeout_ms);

#endif /* APP_LIB_NETWORK_H_ */
