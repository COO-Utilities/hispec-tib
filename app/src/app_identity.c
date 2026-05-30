/**
 * @file app_identity.c
 * @brief Board-profile MQTT device identity.
 *
 * The selected board strap owns the MQTT device namespace. Command-dispatch
 * helpers format the command, response, warning, and telemetry topic templates.
 */

#include "app_identity.h"

#include "devices.h"

const char *app_mqtt_device_id(void)
{
	switch (devices_board_type()) {
	case HISPEC_BOARD_TIB:
		return "hsfib-tib";
	case HISPEC_BOARD_CAL_HK:
		return "hsfib-rcal";
	case HISPEC_BOARD_CAL_YJ:
		return "hsfib-bcal";
	case HISPEC_BOARD_AS:
		return "hsfib-as";
	case HISPEC_BOARD_UNKNOWN:
	default:
		return "hsfib-unknown";
	}
}
