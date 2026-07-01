# SPDX-License-Identifier: Apache-2.0

cmake_minimum_required(VERSION 3.20.0)

# SOLID_COLOR selects the steady color: RED, GREEN, or BLUE (default RED).
set(SOLID_COLOR "RED" CACHE STRING "Steady LED color: RED, GREEN, or BLUE")

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(bento_blinky_solid)

target_sources(app PRIVATE src/main.c)

# Kept dim on purpose: the LTST-E683CEGBW is bright even at low values.
if(SOLID_COLOR STREQUAL "RED")
  target_compile_definitions(app PRIVATE SOLID_COLOR_R=0x10 SOLID_COLOR_G=0x00 SOLID_COLOR_B=0x00)
elseif(SOLID_COLOR STREQUAL "GREEN")
  target_compile_definitions(app PRIVATE SOLID_COLOR_R=0x00 SOLID_COLOR_G=0x10 SOLID_COLOR_B=0x00)
elseif(SOLID_COLOR STREQUAL "BLUE")
  target_compile_definitions(app PRIVATE SOLID_COLOR_R=0x00 SOLID_COLOR_G=0x00 SOLID_COLOR_B=0x10)
else()
  message(FATAL_ERROR "Unknown SOLID_COLOR '${SOLID_COLOR}'; expected RED, GREEN, or BLUE")
endif()
