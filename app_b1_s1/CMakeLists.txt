cmake_minimum_required(VERSION 3.13.1)

# Point to the shared app root (sources + prj.conf + app.overlay)
set(APP_ROOT ${CMAKE_CURRENT_LIST_DIR}/../app)

# Make the build use that directory for prj.conf and app.overlay:
set(APPLICATION_CONFIG_DIR ${APP_ROOT} CACHE PATH "" FORCE)  # <-- documented mechanism

# Preselect the board + shield for this variant:
#set(BOARD w5500_evb_pico2/rp2350a/m33 CACHE STRING "" FORCE)
set(BOARD nucleo_h563zi/stm32h563xx CACHE STRING "" FORCE)
#set(SHIELD pcb_a CACHE STRING "" FORCE)


find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(app_b1_s1 LANGUAGES C)

# Pull in common sources/includes
include(${APP_ROOT}/core.cmake)