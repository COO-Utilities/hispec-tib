# SPDX-License-Identifier: Apache-2.0

# MCUboot enables no common components; avoid creating an empty library there.
if(NOT CONFIG_COO_NETWORK AND NOT CONFIG_COO_JSON AND NOT CONFIG_COO_MQTT AND NOT CONFIG_COO_OTA)
  return()
endif()

zephyr_library()

# Network utilities (requires networking and connection-manager support)
zephyr_library_sources_ifdef(CONFIG_COO_NETWORK network.c)

# JSON utilities (requires JSON support)
zephyr_library_sources_ifdef(CONFIG_COO_JSON json_utils.c)

# MQTT client wrapper (requires MQTT library)
zephyr_library_sources_ifdef(CONFIG_COO_MQTT mqtt_client.c)

# Static command dispatch and MQTT/serial response helpers.
zephyr_library_sources_ifdef(CONFIG_COO_MQTT command_dispatch.c)

# MCUboot lifecycle; independent of MQTT/command dispatch and app hardware.
zephyr_library_sources_ifdef(CONFIG_COO_OTA ota.c)
# img_mgmt.h exposes MCUboot types; keep its interface dependency with OTA.
zephyr_library_link_libraries_ifdef(CONFIG_COO_OTA MCUBOOT_BOOTUTIL)
