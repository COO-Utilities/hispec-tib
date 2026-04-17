#-------------------------------------------------------------------------------
# HiSPEC-TIB Zephyr Application
#
# Copyright (c) 2021 Nordic Semiconductor ASA
# SPDX-License-Identifier: Apache-2.0

cmake_minimum_required(VERSION 3.13.1)

# Include local out-of-tree modules that are not yet in west manifest.
set(LOCAL_DS2408_MODULE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../modules/gpio/ds2408")
if(EXISTS "${LOCAL_DS2408_MODULE_DIR}/zephyr/module.yml")
  list(APPEND ZEPHYR_EXTRA_MODULES "${LOCAL_DS2408_MODULE_DIR}")
endif()

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})

project(app LANGUAGES C)

target_sources(app PRIVATE
        src/main.c
        src/app_settings.c
        src/attenuator.c
        src/command.c
        src/devices.c
        src/maiman.c
        src/photodiode.c
        src/mems_switching.c
        src/sntp_sync.c
        src/attenuator.h
        src/app_settings.h
        src/command.h
        src/devices.h
        src/maiman.h
        src/laser_properties.h
        src/photodiode.h
        src/mems_switching.h
        src/sntp_sync.h
)
