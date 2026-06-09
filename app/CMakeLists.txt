#-------------------------------------------------------------------------------
# HiSPEC-TIB Zephyr Application
#
# Copyright (c) 2021 Nordic Semiconductor ASA
# SPDX-License-Identifier: Apache-2.0

cmake_minimum_required(VERSION 3.13.1)

# Default to pyOCD for this application so west flash/debug use the open
# source runner unless a developer explicitly overrides it on the command line.
#set(BOARD_FLASH_RUNNER pyocd CACHE STRING "Default west flash runner")
#set(BOARD_DEBUG_RUNNER pyocd CACHE STRING "Default west debug runner")


find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})

project(app LANGUAGES C)

set(HISPEC_BUILD_VERSION_HEADER
        ${CMAKE_CURRENT_BINARY_DIR}/generated/hispec_build_version.h)

add_custom_target(hispec_build_version
        COMMAND ${CMAKE_COMMAND}
                -DSOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}
                -DOUTPUT_FILE=${HISPEC_BUILD_VERSION_HEADER}
                -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/git_build_version.cmake
        BYPRODUCTS ${HISPEC_BUILD_VERSION_HEADER}
        COMMENT "Generating HISPEC git build version")

target_include_directories(app PRIVATE
        ${CMAKE_CURRENT_BINARY_DIR}/generated
)

add_dependencies(app hispec_build_version)

target_sources(app PRIVATE
        src/main.c
        src/app_identity.c
        src/app_settings.c
        src/attenuator.c
        src/attenuator_calibration.c
        src/attenuator_command.c
        src/command.c
        src/devices.c
        src/housekeeping.c
        src/laser_command.c
        src/lasers.c
        src/laserbank_tempcontrol.c
        src/maiman.c
        src/mems_command.c
        src/photodiode.c
        src/photodiode_command.c
        src/throughput_command.c
        src/throughput_monitor.c
        src/mems_switching.c
)

target_sources_ifdef(CONFIG_SNTP app PRIVATE
        src/sntp_sync.c
)
