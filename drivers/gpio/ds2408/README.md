# DS2408 Zephyr GPIO Driver

Out-of-tree Zephyr module for the Maxim DS2408 1-Wire 8-bit GPIO expander.

## Usage

Add to your `west.yml` manifest:

```yaml
manifest:
  projects:
    - name: ds2408
      url: <your ds2408 repo url>
      revision: main
      path: modules/gpio/ds2408
```

Then run:

```sh
west update
```

## Configuration

Enable in your `prj.conf`:

```conf
CONFIG_GPIO=y
CONFIG_W1=y
CONFIG_GPIO_DS2408=y
```

## Devicetree overlay

Example with a `zephyr,w1-gpio` bus and one DS2408:

```dts
/ {
	w1_pd_power: w1-pd-power {
		compatible = "zephyr,w1-gpio";
		status = "okay";
		gpios = <&gpioe 9 (GPIO_ACTIVE_HIGH | GPIO_OPEN_DRAIN | GPIO_PULL_UP)>;
		#address-cells = <1>;
		#size-cells = <0>;

		ds2408: gpio@0 {
			compatible = "maxim,ds2408";
			status = "okay";
			gpio-controller;
			#gpio-cells = <2>;
		};
	};

	zephyr,user {
		relay0-gpios = <&ds2408 0 GPIO_ACTIVE_HIGH>;
	};
};
```

For multidrop 1-Wire buses, provide a 64-bit ROM ID using `reg`.

## Notes

- DS2408 outputs are open-drain.
- Pins configured as input are released (high-Z at the DS2408 transistor).
- On init, the driver releases all pins before regular GPIO configuration.

