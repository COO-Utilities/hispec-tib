/**
 * @file app_identity.c
 * @brief Board-profile MQTT identity and topic formatting.
 *
 * The selected board strap owns the MQTT device namespace so one firmware
 * image can expose the correct command and telemetry topics on every PCB.
 */

#include "app_identity.h"

#include <errno.h>
#include <string.h>
#include <zephyr/sys/printk.h>

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

static int format_topic(char *buf, size_t buf_len, const char *prefix,
			const char *suffix)
{
	int written;

	if (buf == NULL || buf_len == 0U || prefix == NULL) {
		return -EINVAL;
	}

	written = snprintk(buf, buf_len, "%s%s%s",
			   prefix,
			   app_mqtt_device_id(),
			   suffix != NULL ? suffix : "");
	return (written < 0 || written >= (int)buf_len) ? -ENOSPC : 0;
}

int app_mqtt_format_request_prefix(char *buf, size_t buf_len)
{
	return format_topic(buf, buf_len, "cmd/", "/req/");
}

int app_mqtt_format_response_topic(const char *key, char *buf, size_t buf_len)
{
	char suffix[96];
	int written;

	if (key == NULL) {
		key = "";
	}

	written = snprintk(suffix, sizeof(suffix), "/resp/%s", key);
	if (written < 0 || written >= (int)sizeof(suffix)) {
		return -ENOSPC;
	}

	return format_topic(buf, buf_len, "cmd/", suffix);
}

int app_mqtt_format_data_topic(const char *suffix, char *buf, size_t buf_len)
{
	char topic_suffix[64];
	int written;

	if (suffix == NULL || suffix[0] == '\0') {
		return -EINVAL;
	}

	written = snprintk(topic_suffix, sizeof(topic_suffix), "/%s", suffix);
	if (written < 0 || written >= (int)sizeof(topic_suffix)) {
		return -ENOSPC;
	}

	return format_topic(buf, buf_len, "dt/", topic_suffix);
}
