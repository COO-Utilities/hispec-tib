/**
 * @file app_identity.h
 * @brief MQTT device identity and board-profile topic helpers.
 */

#ifndef HISPEC_APP_IDENTITY_H
#define HISPEC_APP_IDENTITY_H

#include <stddef.h>

/** @brief Return the MQTT device ID selected from the detected board strap. */
const char *app_mqtt_device_id(void);

/** @brief Format `cmd/<device>/req/` for subscription and ingress parsing. */
int app_mqtt_format_request_prefix(char *buf, size_t buf_len);

/** @brief Format `cmd/<device>/resp/<key>` for default command replies. */
int app_mqtt_format_response_topic(const char *key, char *buf, size_t buf_len);

/** @brief Format `dt/<device>/<suffix>` for telemetry and warnings. */
int app_mqtt_format_data_topic(const char *suffix, char *buf, size_t buf_len);

#endif /* HISPEC_APP_IDENTITY_H */
