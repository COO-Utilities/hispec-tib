#!/usr/bin/env bash
# CLion and command-line builds share MCUboot artifacts. Routine USB writes only
# the confirmed app HEX; debugging uses matching symbols without a raw ELF load.
set -euo pipefail

script_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
hispec_tib_dir="$(cd -- "${script_dir}/.." && pwd)"
real_west="${hispec_tib_dir}/../.venv/bin/west"

reject() { echo "HISPEC west: $*" >&2; exit 2; }

west_cmd=("$real_west")
while [[ "${1:-}" == -v || "${1:-}" == -vv || "${1:-}" == --verbose || "${1:-}" == -q ]]; do
	west_cmd+=("$1"); shift
done
action="${1:-}"
case "$action" in
	build|flash|debug|debugserver|attach) shift ;;
	--version|--help) exec "$real_west" "$@" ;;
	-*) reject "unsupported global option: $action" ;;
	*) exec "${west_cmd[@]}" "$@" ;;
esac
build_dir="${hispec_tib_dir}/app/build-ota"
timing_conf=0
args=()
while (( $# )); do
	case "$1" in
		-d|--build-dir) requested_dir="${2:?missing build directory}"; shift ;;
		--build-dir=*) requested_dir="${1#*=}" ;;
		*) requested_dir="" ;;
	esac
	if [[ -n "$requested_dir" ]]; then
		case "${requested_dir%/}" in
			*/build-ota-timing|build-ota-timing) build_dir="${hispec_tib_dir}/app/build-ota-timing" ;;
			*/build-ota|build-ota) ;;
			*) reject "select build-ota or build-ota-timing; obsolete build directories are unsupported" ;;
		esac
	elif [[ "$action" == build ]]; then
		case "$1" in
			--no-sysbuild) reject "all builds require --sysbuild" ;;
			--sysbuild) ;;
			-DEXTRA_CONF_FILE=*) reject "use -Dapp_EXTRA_CONF_FILE=debug.conf with sysbuild" ;;
			-Dapp_EXTRA_CONF_FILE=debug.conf)
				build_dir="${hispec_tib_dir}/app/build-ota-timing"; timing_conf=1; args+=("$1") ;;
			flash|debug|debugserver|attach|--target=flash|--target=debug*|--target=attach|-tflash|-tdebug*|-tattach|-t=flash|-t=debug*|-t=attach)
				reject "use west flash/debug/attach through this wrapper, not a build target" ;;
			*) args+=("$1") ;;
		esac
	else
		# Deliberately limit runner options: no erase, raw image, address or tool overrides.
		case "$1" in
			--domain) [[ "${2:-}" == app ]] || reject "only the app domain is supported"; shift ;;
			--domain=app|--no-load|--) ;;
			--no-rebuild) args+=("$1") ;;
			-r|--runner)
				[[ "$action:$2" == flash:stm32cubeprogrammer || "$action:$2" == debug:stlink_gdbserver || "$action:$2" == debugserver:stlink_gdbserver || "$action:$2" == attach:stlink_gdbserver ]] || reject "unsupported runner"
				shift ;;
			--context|-H|--help) args+=("$1") ;;
			--port-number|--dev-id) args+=("$1" "${2:?missing runner option value}"); shift ;;
			*) reject "unsupported $action option: $1" ;;
		esac
	fi
	shift
done
if [[ "$action" == build ]]; then
	if [[ "$build_dir" == */build-ota-timing && "$timing_conf" == 0 ]]; then
		[[ " ${args[*]:-} " == *" -- "* ]] || args+=(--)
		args+=(-Dapp_EXTRA_CONF_FILE=debug.conf)
	fi
	exec "${west_cmd[@]}" build --sysbuild --build-dir "$build_dir" ${args[@]+"${args[@]}"}
fi

# Refuse stale standalone artifacts even with --no-rebuild. These checks are local.
config="${build_dir}/app/zephyr/.config"
grep -qx 'CONFIG_BOOTLOADER_MCUBOOT=y' "$config" &&
grep -qx 'CONFIG_COO_OTA=y' "$config" &&
grep -qx 'CONFIG_MCUBOOT_GENERATE_CONFIRMED_IMAGE=y' "$config" || reject "build the MCUboot app first"
if [[ "$action" == flash ]]; then
	exec "${west_cmd[@]}" flash --build-dir "$build_dir" --domain app \
		--runner stm32cubeprogrammer --hex-file "$build_dir/app/zephyr/zephyr.signed.confirmed.hex" ${args[@]+"${args[@]}"}
fi
# No rebuild on attach/debug: the ELF must match the image previously installed.
exec "${west_cmd[@]}" "$action" --no-rebuild --build-dir "$build_dir" --domain app \
	--runner stlink_gdbserver --elf-file "$build_dir/app/zephyr/zephyr.elf" --no-load ${args[@]+"${args[@]}"}
