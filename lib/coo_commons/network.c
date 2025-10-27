/*
 * Copyright (c) 2024 Caltech Optical Observatories
 * SPDX-License-Identifier: Apache-2.0
 */

#include <coo_commons/network.h>
#include <zephyr/kernel.h>
#include <zephyr/net/socket.h>
#include <zephyr/logging/log.h>
#include <zephyr/net/net_mgmt.h>
#include <zephyr/net/net_event.h>
#include <zephyr/net/conn_mgr_monitor.h>
#include <zephyr/net/conn_mgr_connectivity.h>
#include <errno.h>

LOG_MODULE_REGISTER(coo_network, LOG_LEVEL_DBG);

/* ============================================================================
 * CONNECTION MANAGER - High-level network initialization
 * ============================================================================ */

/* Network connection management */
static volatile bool network_online = false;
static coo_network_event_cb_t user_event_cb = NULL;

#define NET_L4_EVENT_MASK (NET_EVENT_L4_CONNECTED | NET_EVENT_L4_DISCONNECTED)
static struct net_mgmt_event_callback net_l4_mgmt_cb;

static void net_l4_evt_handler(struct net_mgmt_event_callback *cb,
                                uint32_t mgmt_event,
                                struct net_if *iface)
{
	switch (mgmt_event) {
	case NET_EVENT_L4_CONNECTED:
		network_online = true;
		LOG_INF("Network up!");
		if (user_event_cb) {
			user_event_cb(true);
		}
		break;
	case NET_EVENT_L4_DISCONNECTED:
		network_online = false;
		LOG_INF("Network down!");
		if (user_event_cb) {
			user_event_cb(false);
		}
		break;
	default:
		break;
	}
}

void coo_network_log_mac_addr(void)
{
	struct net_if *iface = net_if_get_default();
	struct net_linkaddr *mac;

	if (!iface) {
		LOG_WRN("No default network interface");
		return;
	}

	mac = net_if_get_link_addr(iface);

	LOG_INF("MAC Address: %02X:%02X:%02X:%02X:%02X:%02X",
		mac->addr[0], mac->addr[1], mac->addr[2],
		mac->addr[3], mac->addr[4], mac->addr[5]);
}

bool coo_network_is_ready(void)
{
	return network_online;
}

int coo_network_init(coo_network_event_cb_t event_cb)
{
	int rc;
	struct net_if *iface;

	user_event_cb = event_cb;

	iface = net_if_get_default();
	if (iface == NULL) {
		LOG_ERR("No network interface configured");
		return -ENETDOWN;
	}

	coo_network_log_mac_addr();

	/* Register callbacks for IPv4 address events */
	net_mgmt_init_event_callback(&net_l4_mgmt_cb, net_l4_evt_handler,
	                             NET_EVENT_IPV4_ADDR_ADD |
	                             NET_EVENT_IPV4_ADDR_DEL);
	net_mgmt_add_event_callback(&net_l4_mgmt_cb);

	LOG_INF("Bringing up network...");

	/* Bring up all network interfaces managed by conn_mgr */
	rc = conn_mgr_all_if_up(true);
	if (rc) {
		LOG_ERR("conn_mgr_all_if_up() failed (%d)", rc);
		return rc;
	}

	/* Register callbacks for L4 events */
	net_mgmt_init_event_callback(&net_l4_mgmt_cb, &net_l4_evt_handler, NET_L4_EVENT_MASK);
	net_mgmt_add_event_callback(&net_l4_mgmt_cb);

	/* If using static IP, L4 Connect callback will happen before conn_mgr is initialized,
	 * so resend events here to check for connectivity */
	conn_mgr_mon_resend_status();

	return 0;
}

int coo_network_wait_ready(uint32_t timeout_ms)
{
	uint32_t elapsed = 0;
	const uint32_t check_interval = 100; /* ms */

	LOG_INF("Waiting for network connection...");

	if (timeout_ms == 0) {
		/* Wait forever */
		while (!network_online) {
			k_msleep(check_interval);
			elapsed += check_interval;
			if (elapsed >= 10000) { /* Log every 10 seconds */
				LOG_WRN("Network not ready yet (waiting...)");
				elapsed = 0;
			}
		}
	} else {
		/* Wait with timeout */
		while (!network_online && elapsed < timeout_ms) {
			k_msleep(check_interval);
			elapsed += check_interval;
		}

		if (!network_online) {
			LOG_ERR("Network connection timeout after %u ms", timeout_ms);
			return -ETIMEDOUT;
		}
	}

	LOG_INF("Network stack ready (DHCP or static IP set).");
	return 0;
}

/* ============================================================================
 * SOCKET UTILITIES - Low-level socket operations
 * ============================================================================ */

int coo_net_tcp_socket_create(uint16_t port, bool is_server)
{
	int sock;
	struct sockaddr_in addr;
	int optval = 1;

	sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
	if (sock < 0) {
		return -errno;
	}

	/* Set socket options */
	setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &optval, sizeof(optval));

	if (is_server) {
		addr.sin_family = AF_INET;
		addr.sin_port = htons(port);
		addr.sin_addr.s_addr = INADDR_ANY;

		if (bind(sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
			close(sock);
			return -errno;
		}

		if (listen(sock, 5) < 0) {
			close(sock);
			return -errno;
		}
	}

	return sock;
}

int coo_net_udp_socket_create(uint16_t port)
{
	int sock;
	struct sockaddr_in addr;
	int optval = 1;

	sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
	if (sock < 0) {
		return -errno;
	}

	setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &optval, sizeof(optval));

	addr.sin_family = AF_INET;
	addr.sin_port = htons(port);
	addr.sin_addr.s_addr = INADDR_ANY;

	if (bind(sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
		close(sock);
		return -errno;
	}

	return sock;
}

int coo_net_tcp_connect(int sockfd, const struct sockaddr *addr, int timeout_ms)
{
	struct timeval timeout;

	timeout.tv_sec = timeout_ms / 1000;
	timeout.tv_usec = (timeout_ms % 1000) * 1000;

	setsockopt(sockfd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
	setsockopt(sockfd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));

	if (connect(sockfd, addr, sizeof(struct sockaddr_in)) < 0) {
		return -errno;
	}

	return 0;
}

int coo_net_send_retry(int sockfd, const void *buf, size_t len, int max_retries)
{
	int ret;
	int retries = 0;

	while (retries < max_retries) {
		ret = send(sockfd, buf, len, 0);
		if (ret >= 0) {
			return ret;
		}

		if (errno != EAGAIN && errno != EWOULDBLOCK) {
			return -errno;
		}

		retries++;
		k_sleep(K_MSEC(100));
	}

	return -ETIMEDOUT;
}

int coo_net_recv_timeout(int sockfd, void *buf, size_t len, int timeout_ms)
{
	struct timeval timeout;

	timeout.tv_sec = timeout_ms / 1000;
	timeout.tv_usec = (timeout_ms % 1000) * 1000;

	setsockopt(sockfd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));

	return recv(sockfd, buf, len, 0);
}
