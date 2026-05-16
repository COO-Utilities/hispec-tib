/**
 * @file app_identity.h
 * @brief MQTT device identity selected from the board profile.
 */

#ifndef HISPEC_APP_IDENTITY_H
#define HISPEC_APP_IDENTITY_H

/** @brief Return the MQTT device ID selected from the detected board strap. */
const char *app_mqtt_device_id(void);

#endif /* HISPEC_APP_IDENTITY_H */
