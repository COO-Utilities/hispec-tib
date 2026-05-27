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
