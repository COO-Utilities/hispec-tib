#-------------------------------------------------------------------------------
# HiSPEC-TIB Zephyr Application
#
# Copyright (c) 2021 Nordic Semiconductor ASA
# SPDX-License-Identifier: Apache-2.0

cmake_minimum_required(VERSION 3.13.1)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})

project(app LANGUAGES C)

target_sources(app PRIVATE
        src/main.c
        src/attenuator.c
        src/command.c
        src/devices.c
        src/maiman.c
        src/photodiode.c
        src/mems_switching.c
)
