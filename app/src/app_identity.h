/**
 * @file app_identity.h
 * @brief MQTT device identity and fixed command topic prefixes.
 */

#ifndef HISPEC_APP_IDENTITY_H
#define HISPEC_APP_IDENTITY_H

#define APP_MQTT_DEVICE_ID "hsfib-tib"
#define APP_MQTT_CMD_PREFIX "cmd/" APP_MQTT_DEVICE_ID "/req/"
#define APP_MQTT_RESP_PREFIX "cmd/" APP_MQTT_DEVICE_ID "/resp/"

#endif /* HISPEC_APP_IDENTITY_H */
