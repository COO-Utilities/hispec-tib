#!/usr/bin/env bash
#
# CLion's West plugin stores one project-level West build directory even when
# multiple CMake profiles exist. Keep the normal profile on app/build, but send
# the timing/debug profile to app/build-timing when it passes debug.conf.

set -euo pipefail

script_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
hispec_tib_dir="$(cd -- "${script_dir}/.." && pwd)"
west_topdir="$(cd -- "${hispec_tib_dir}/.." && pwd)"
real_west="${west_topdir}/.venv/bin/west"
timing_build_dir="${hispec_tib_dir}/app/build-timing"

args=("$@")
use_timing_build=0

for arg in "${args[@]}"; do
	case "${arg}" in
		-DEXTRA_CONF_FILE=debug.conf|*-DEXTRA_CONF_FILE=*debug.conf*)
			use_timing_build=1
			break
			;;
	esac
done

if [[ "${args[0]:-}" == "build" && "${use_timing_build}" == "1" ]]; then
	for ((i = 0; i < ${#args[@]}; i++)); do
		case "${args[i]}" in
			--build-dir)
				if (( i + 1 < ${#args[@]} )); then
					args[i + 1]="${timing_build_dir}"
				fi
				;;
			--build-dir=*)
				args[i]="--build-dir=${timing_build_dir}"
				;;
		esac
	done
fi

exec "${real_west}" "${args[@]}"
