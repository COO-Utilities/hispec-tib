# SPDX-License-Identifier: Apache-2.0

zephyr_library()

# Network utilities (requires networking support)
# Includes both low-level sockets and high-level connection manager
zephyr_library_sources_ifdef(CONFIG_COO_NETWORK network.c)

# JSON utilities (requires JSON support)
zephyr_library_sources_ifdef(CONFIG_COO_JSON json_utils.c)

# MQTT client wrapper (requires MQTT library)
zephyr_library_sources_ifdef(CONFIG_COO_MQTT mqtt_client.c)

# Static command dispatch and MQTT/serial response helpers.
zephyr_library_sources_ifdef(CONFIG_COO_MQTT command_dispatch.c)
