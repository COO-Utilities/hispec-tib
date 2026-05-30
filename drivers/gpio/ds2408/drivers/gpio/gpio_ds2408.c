/**
 * @file gpio_ds2408.c
 * @brief Zephyr GPIO API shim for Maxim DS2408 1-Wire open-drain GPIO.
 *
 * All register access uses the Zephyr 1-Wire bus and may sleep. GPIO writes
 * maintain a shadow latch because DS2408 outputs are released high and sink low.
 */
/*
 * Copyright (c) 2026 Caltech Optical Observatories
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#define DT_DRV_COMPAT maxim_ds2408

#include <string.h>

#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/gpio/gpio_utils.h>
#include <zephyr/drivers/w1.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/util.h>
#include <zephyr/sys/util_macro.h>

LOG_MODULE_REGISTER(gpio_ds2408, CONFIG_GPIO_LOG_LEVEL);

#define DS2408_FAMILY_CODE                 0x29
#define DS2408_OUTPUT_RETRIES              3U

#define DS2408_CMD_READ_PIO_REGS           0xF0
#define DS2408_CMD_CHANNEL_ACCESS_WRITE    0x5A
#define DS2408_CMD_RESET_ACTIVITY_LATCHES  0xC3
#define DS2408_CONFIRM_BYTE                0xAA

#define DS2408_REG_LOGIC_STATE             0x88
#define DS2408_TESTMODE_MAGIC_PREFIX       0x96
#define DS2408_TESTMODE_MAGIC_SUFFIX       0x3C

enum ds2408_init_mode {
	DS2408_INIT_MODE_NONE,
	DS2408_INIT_MODE_INPUT,
	DS2408_INIT_MODE_OUTPUT_LOW,
	DS2408_INIT_MODE_OUTPUT_HIGH,
};

struct ds2408_config {
	struct gpio_driver_config common;
	const struct device *bus;
	uint8_t init_mask;
	uint8_t init_invert_mask;
	enum ds2408_init_mode init_mode;
	uint8_t family;
	bool overdrive;
	uint64_t rom_id;
};

struct ds2408_data {
	struct gpio_driver_data common;
	struct k_mutex lock;
	struct w1_slave_config slave_cfg;
	uint8_t direction_mask;
	uint8_t output_raw;
};

static bool ds2408_rom_is_zero(const struct w1_rom *rom)
{
	return w1_rom_to_uint64(rom) == 0ULL;
}

static int ds2408_read_reg(const struct device *dev, uint8_t reg, uint8_t *val)
{
	const struct ds2408_config *cfg = dev->config;
	struct ds2408_data *data = dev->data;
	uint8_t tx_buf[3] = { DS2408_CMD_READ_PIO_REGS, reg, 0x00 };
	int ret;

	ret = w1_write_read(cfg->bus, &data->slave_cfg, tx_buf, sizeof(tx_buf), val, 1);
	if (ret != 0) {
		return ret;
	}

	return 0;
}

static int ds2408_write_latch(const struct device *dev, uint8_t latch_value)
{
	const struct ds2408_config *cfg = dev->config;
	struct ds2408_data *data = dev->data;
	uint8_t tx_buf[3];
	uint8_t confirm;
	int ret;

	tx_buf[0] = DS2408_CMD_CHANNEL_ACCESS_WRITE;
	tx_buf[1] = latch_value;
	tx_buf[2] = (uint8_t)~latch_value;

	for (uint8_t i = 0; i < DS2408_OUTPUT_RETRIES; i++) {
		ret = w1_write_read(cfg->bus, &data->slave_cfg, tx_buf, sizeof(tx_buf), &confirm, 1);
		if (ret != 0) {
			return ret;
		}

		if (confirm == DS2408_CONFIRM_BYTE) {
			return 0;
		}
	}

	return -EIO;
}

static int ds2408_apply_outputs_locked(const struct device *dev)
{
	struct ds2408_data *data = dev->data;
	uint8_t effective_raw;

	/*
	 * DS2408 outputs are open-drain:
	 * raw high (1) means released, raw low (0) means actively sinking.
	 */
	effective_raw = (data->output_raw & data->direction_mask) | (uint8_t)~data->direction_mask;

	return ds2408_write_latch(dev, effective_raw);
}

static int ds2408_apply_init_defaults_locked(const struct device *dev)
{
	const struct ds2408_config *cfg = dev->config;
	struct ds2408_data *data = dev->data;
	uint8_t logical_raw;

	if (cfg->init_mode == DS2408_INIT_MODE_NONE || cfg->init_mask == 0U) {
		return 0;
	}

	if ((cfg->init_mask & (uint8_t)~cfg->common.port_pin_mask) != 0U) {
		return -EINVAL;
	}

	data->common.invert &= (gpio_port_pins_t)~cfg->init_mask;
	data->common.invert |= cfg->init_invert_mask;

	if (cfg->init_mode == DS2408_INIT_MODE_INPUT) {
		data->direction_mask &= (uint8_t)~cfg->init_mask;
		data->output_raw |= cfg->init_mask;
		return ds2408_apply_outputs_locked(dev);
	}

	data->direction_mask |= cfg->init_mask;
	if (cfg->init_mode == DS2408_INIT_MODE_OUTPUT_HIGH) {
		logical_raw = cfg->init_mask;
	} else {
		logical_raw = 0U;
	}

	/* Convert logical defaults through the same active-low bit convention
	 * used by Zephyr's GPIO core for later gpio_pin_set() calls.
	 */
	logical_raw ^= cfg->init_invert_mask;
	logical_raw &= cfg->init_mask;

	data->output_raw &= (uint8_t)~cfg->init_mask;
	data->output_raw |= logical_raw;

	return ds2408_apply_outputs_locked(dev);
}

static int ds2408_setup_slave(const struct device *dev)
{
	const struct ds2408_config *cfg = dev->config;
	struct ds2408_data *data = dev->data;
	size_t slave_count;
	int ret;

	data->slave_cfg.overdrive = cfg->overdrive;
	w1_uint64_to_rom(cfg->rom_id, &data->slave_cfg.rom);

	ret = w1_reset_bus(cfg->bus);
	if (ret == 0) {
		LOG_ERR("No 1-Wire slave present on %s", cfg->bus->name);
		return -ENODEV;
	}
	if (ret < 0) {
		return ret;
	}

	slave_count = w1_get_slave_count(cfg->bus);
	if (slave_count == 1U) {
		if (ds2408_rom_is_zero(&data->slave_cfg.rom)) {
			ret = w1_read_rom(cfg->bus, &data->slave_cfg.rom);
			if (ret != 0) {
				LOG_ERR("Failed to read DS2408 ROM on %s (%d), raw=%016llx",
					cfg->bus->name, ret,
					(unsigned long long)w1_rom_to_uint64(&data->slave_cfg.rom));
				return ret;
			}
		}
	} else if (ds2408_rom_is_zero(&data->slave_cfg.rom)) {
		LOG_ERR("ROM ID required for multidrop DS2408 bus");
		return -EINVAL;
	}

	LOG_INF("DS2408 ROM on %s: %016llx",
		cfg->bus->name,
		(unsigned long long)w1_rom_to_uint64(&data->slave_cfg.rom));

	if ((cfg->family != 0U) && (cfg->family != data->slave_cfg.rom.family)) {
		LOG_ERR("ROM family 0x%02x does not match DS2408 family 0x%02x",
			data->slave_cfg.rom.family, cfg->family);
		return -EINVAL;
	}

	return 0;
}

static int ds2408_disable_test_mode(const struct device *dev)
{
	const struct ds2408_config *cfg = dev->config;
	struct ds2408_data *data = dev->data;
	uint8_t magic[10] = { DS2408_TESTMODE_MAGIC_PREFIX };
	int ret;

	if (ds2408_rom_is_zero(&data->slave_cfg.rom)) {
		return 0;
	}

	memcpy(&magic[1], &data->slave_cfg.rom, sizeof(data->slave_cfg.rom));
	magic[9] = DS2408_TESTMODE_MAGIC_SUFFIX;

	ret = w1_lock_bus(cfg->bus);
	if (ret != 0) {
		return ret;
	}

	ret = w1_reset_bus(cfg->bus);
	if (ret <= 0) {
		ret = (ret == 0) ? -ENODEV : ret;
		goto out;
	}

	ret = w1_write_block(cfg->bus, magic, sizeof(magic));
	if (ret != 0) {
		goto out;
	}

	ret = w1_reset_bus(cfg->bus);
	if (ret <= 0) {
		ret = (ret == 0) ? -ENODEV : ret;
		goto out;
	}

	ret = 0;

out:
	(void)w1_unlock_bus(cfg->bus);
	return ret;
}

static int ds2408_pin_configure(const struct device *dev, gpio_pin_t pin, gpio_flags_t flags)
{
	const struct ds2408_config *cfg = dev->config;
	struct ds2408_data *data = dev->data;
	const gpio_flags_t supported_flags =
		GPIO_INPUT |
		GPIO_OUTPUT |
		GPIO_OUTPUT_INIT_LOW |
		GPIO_OUTPUT_INIT_HIGH |
		GPIO_ACTIVE_HIGH |
		GPIO_ACTIVE_LOW |
		GPIO_SINGLE_ENDED |
		GPIO_LINE_OPEN_DRAIN;
	int ret;

	if (k_is_in_isr()) {
		return -EWOULDBLOCK;
	}

	if ((flags & ~supported_flags) != 0U) {
		return -ENOTSUP;
	}

	if (((gpio_port_pins_t)BIT(pin) & cfg->common.port_pin_mask) == 0U) {
		return -EINVAL;
	}

	if (((flags & GPIO_SINGLE_ENDED) != 0U) &&
	    ((flags & GPIO_LINE_OPEN_DRAIN) == 0U)) {
		return -ENOTSUP;
	}

	if (((flags & GPIO_INPUT) != 0U) && ((flags & GPIO_OUTPUT) != 0U)) {
		return -ENOTSUP;
	}

	if ((flags & (GPIO_INPUT | GPIO_OUTPUT)) == 0U) {
		return -ENOTSUP;
	}

	k_mutex_lock(&data->lock, K_FOREVER);

	if ((flags & GPIO_OUTPUT) != 0U) {
		data->direction_mask |= BIT(pin);

		if ((flags & GPIO_OUTPUT_INIT_HIGH) != 0U) {
			data->output_raw |= BIT(pin);
		} else if ((flags & GPIO_OUTPUT_INIT_LOW) != 0U) {
			data->output_raw &= (uint8_t)~BIT(pin);
		}
	} else {
		data->direction_mask &= (uint8_t)~BIT(pin);
		/* Inputs are always released on open-drain outputs. */
		data->output_raw |= BIT(pin);
	}

	ret = ds2408_apply_outputs_locked(dev);
	k_mutex_unlock(&data->lock);

	return ret;
}

static int ds2408_port_get_raw(const struct device *dev, gpio_port_value_t *value)
{
	struct ds2408_data *data = dev->data;
	uint8_t logic_state;
	int ret;

	if (k_is_in_isr()) {
		return -EWOULDBLOCK;
	}

	k_mutex_lock(&data->lock, K_FOREVER);
	ret = ds2408_read_reg(dev, DS2408_REG_LOGIC_STATE, &logic_state);
	k_mutex_unlock(&data->lock);
	if (ret != 0) {
		return ret;
	}

	*value = logic_state;
	return 0;
}

static int ds2408_port_set_masked_raw(const struct device *dev, gpio_port_pins_t mask,
				      gpio_port_value_t value)
{
	const struct ds2408_config *cfg = dev->config;
	struct ds2408_data *data = dev->data;
	int ret;
	uint8_t active_mask = (uint8_t)(mask & cfg->common.port_pin_mask);

	if (k_is_in_isr()) {
		return -EWOULDBLOCK;
	}

	k_mutex_lock(&data->lock, K_FOREVER);

	if ((active_mask & (uint8_t)~data->direction_mask) != 0U) {
		k_mutex_unlock(&data->lock);
		return -EOPNOTSUPP;
	}

	data->output_raw &= (uint8_t)~active_mask;
	data->output_raw |= ((uint8_t)value & active_mask);

	ret = ds2408_apply_outputs_locked(dev);
	k_mutex_unlock(&data->lock);

	return ret;
}

static int ds2408_port_set_bits_raw(const struct device *dev, gpio_port_pins_t pins)
{
	return ds2408_port_set_masked_raw(dev, pins, pins);
}

static int ds2408_port_clear_bits_raw(const struct device *dev, gpio_port_pins_t pins)
{
	return ds2408_port_set_masked_raw(dev, pins, 0U);
}

static int ds2408_port_toggle_bits(const struct device *dev, gpio_port_pins_t pins)
{
	const struct ds2408_config *cfg = dev->config;
	struct ds2408_data *data = dev->data;
	uint8_t active_mask = (uint8_t)(pins & cfg->common.port_pin_mask);
	int ret;

	if (k_is_in_isr()) {
		return -EWOULDBLOCK;
	}

	k_mutex_lock(&data->lock, K_FOREVER);

	if ((active_mask & (uint8_t)~data->direction_mask) != 0U) {
		k_mutex_unlock(&data->lock);
		return -EOPNOTSUPP;
	}

	data->output_raw ^= active_mask;
	ret = ds2408_apply_outputs_locked(dev);

	k_mutex_unlock(&data->lock);
	return ret;
}

#ifdef CONFIG_GPIO_GET_DIRECTION
static int ds2408_port_get_direction(const struct device *dev, gpio_port_pins_t map,
				     gpio_port_pins_t *inputs, gpio_port_pins_t *outputs)
{
	const struct ds2408_config *cfg = dev->config;
	struct ds2408_data *data = dev->data;
	gpio_port_pins_t masked_map = map & cfg->common.port_pin_mask;

	k_mutex_lock(&data->lock, K_FOREVER);

	if (inputs != NULL) {
		*inputs = masked_map & (gpio_port_pins_t)~data->direction_mask;
	}

	if (outputs != NULL) {
		*outputs = masked_map & data->direction_mask;
	}

	k_mutex_unlock(&data->lock);
	return 0;
}
#endif

static int ds2408_pin_interrupt_configure(const struct device *port, gpio_pin_t pin,
					  enum gpio_int_mode mode, enum gpio_int_trig trig)
{
	ARG_UNUSED(port);
	ARG_UNUSED(pin);
	ARG_UNUSED(mode);
	ARG_UNUSED(trig);

	return -ENOTSUP;
}

static int ds2408_manage_callback(const struct device *port, struct gpio_callback *cb, bool set)
{
	ARG_UNUSED(port);
	ARG_UNUSED(cb);
	ARG_UNUSED(set);

	return -ENOTSUP;
}

static uint32_t ds2408_get_pending_int(const struct device *dev)
{
	ARG_UNUSED(dev);
	return 0;
}

static int ds2408_init(const struct device *dev)
{
	const struct ds2408_config *cfg = dev->config;
	struct ds2408_data *data = dev->data;
	int ret;

	if (!device_is_ready(cfg->bus)) {
		LOG_ERR("W1 bus %s not ready", cfg->bus->name);
		return -ENODEV;
	}

	k_mutex_init(&data->lock);
	/* Leave the DS2408 released unless devicetree explicitly requests init
	 * defaults. A Channel Access Write always writes the full latch.
	 */
	data->direction_mask = 0U;
	data->output_raw = (uint8_t)cfg->common.port_pin_mask;

	ret = ds2408_setup_slave(dev);
	if (ret != 0) {
		return ret;
	}

	ret = ds2408_disable_test_mode(dev);
	if (ret != 0) {
		LOG_WRN("Failed to run DS2408 test-mode clear sequence: %d", ret);
	}

	ret = ds2408_apply_init_defaults_locked(dev);
	if (ret != 0) {
		return ret;
	}

	/* Clear stale activity latches; ignore failures. */
	{
		uint8_t confirm = 0U;
		uint8_t cmd = DS2408_CMD_RESET_ACTIVITY_LATCHES;

		(void)w1_write_read(cfg->bus, &data->slave_cfg, &cmd, 1, &confirm, 1);
	}

	return 0;
}

static DEVICE_API(gpio, ds2408_gpio_api) = {
	.pin_configure = ds2408_pin_configure,
	.port_get_raw = ds2408_port_get_raw,
	.port_set_masked_raw = ds2408_port_set_masked_raw,
	.port_set_bits_raw = ds2408_port_set_bits_raw,
	.port_clear_bits_raw = ds2408_port_clear_bits_raw,
	.port_toggle_bits = ds2408_port_toggle_bits,
	.pin_interrupt_configure = ds2408_pin_interrupt_configure,
	.manage_callback = ds2408_manage_callback,
	.get_pending_int = ds2408_get_pending_int,
#ifdef CONFIG_GPIO_GET_DIRECTION
	.port_get_direction = ds2408_port_get_direction,
#endif
};

#define DS2408_ROM_FROM_REG(inst) \
	COND_CODE_1(DT_INST_NODE_HAS_PROP(inst, reg), (DT_INST_REG_ADDR_U64(inst)), (0ULL))

#define DS2408_INIT_PIN_MASK_BY_IDX(idx, inst) \
	BIT(DT_INST_GPIO_PIN_BY_IDX(inst, init_gpios, idx))

#define DS2408_INIT_INVERT_MASK_BY_IDX(idx, inst) \
	(((DT_INST_GPIO_FLAGS_BY_IDX(inst, init_gpios, idx) & GPIO_ACTIVE_LOW) != 0U) ? \
	 BIT(DT_INST_GPIO_PIN_BY_IDX(inst, init_gpios, idx)) : 0U)

#define DS2408_INIT_PIN_MASK(inst) \
	COND_CODE_1(DT_INST_NODE_HAS_PROP(inst, init_gpios), \
		    (LISTIFY(DT_INST_PROP_LEN(inst, init_gpios), \
			     DS2408_INIT_PIN_MASK_BY_IDX, (|), inst)), \
		    (0U))

#define DS2408_INIT_INVERT_MASK(inst) \
	COND_CODE_1(DT_INST_NODE_HAS_PROP(inst, init_gpios), \
		    (LISTIFY(DT_INST_PROP_LEN(inst, init_gpios), \
			     DS2408_INIT_INVERT_MASK_BY_IDX, (|), inst)), \
		    (0U))

#define DS2408_INIT_MODE(inst) \
	COND_CODE_1(DT_INST_PROP(inst, input), (DS2408_INIT_MODE_INPUT), \
		    (COND_CODE_1(DT_INST_PROP(inst, output_low), \
				 (DS2408_INIT_MODE_OUTPUT_LOW), \
				 (COND_CODE_1(DT_INST_PROP(inst, output_high), \
					      (DS2408_INIT_MODE_OUTPUT_HIGH), \
					      (DS2408_INIT_MODE_NONE))))))

#define DS2408_DEFINE(inst) \
	static struct ds2408_data ds2408_data_##inst; \
	static const struct ds2408_config ds2408_config_##inst = { \
		.common = { \
			.port_pin_mask = GPIO_PORT_PIN_MASK_FROM_NGPIOS(DT_INST_PROP(inst, ngpios)), \
		}, \
		.bus = DEVICE_DT_GET(DT_INST_BUS(inst)), \
		.init_mask = DS2408_INIT_PIN_MASK(inst), \
		.init_invert_mask = DS2408_INIT_INVERT_MASK(inst), \
		.init_mode = DS2408_INIT_MODE(inst), \
		.family = DT_INST_PROP_OR(inst, family_code, DS2408_FAMILY_CODE), \
		.overdrive = DT_INST_PROP_OR(inst, overdrive_speed, false), \
		.rom_id = DS2408_ROM_FROM_REG(inst), \
	}; \
	DEVICE_DT_INST_DEFINE(inst, ds2408_init, NULL, \
			      &ds2408_data_##inst, &ds2408_config_##inst, \
			      POST_KERNEL, CONFIG_GPIO_DS2408_INIT_PRIORITY, \
			      &ds2408_gpio_api);

DT_INST_FOREACH_STATUS_OKAY(DS2408_DEFINE)
