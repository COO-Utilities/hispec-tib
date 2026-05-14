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
        src/app_scheduled_actions.c
        src/app_settings.c
        src/app_warning.c
        src/attenuator.c
        src/command.c
        src/devices.c
        src/lasers.c
        src/laserbank_control.c
        src/maiman.c
        src/photodiode.c
        src/throughput_monitor.c
        src/tempsense.c
        src/tempsense.h
        src/mems_switching.c
        src/sntp_sync.c
        src/attenuator.h
        src/app_scheduled_actions.h
        src/app_settings.h
        src/app_warning.h
        src/app_identity.h
        src/command.h
        src/devices.h
        src/lasers.h
        src/laserbank_control.h
        src/maiman.h
        src/laser_properties.h
        src/photodiode.h
        src/throughput_monitor.h
        src/mems_switching.h
        src/sntp_sync.h
)
