# SPDX-License-Identifier: Apache-2.0

cmake_minimum_required(VERSION 3.20.0)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(bento_demo_app)

target_sources(app PRIVATE
  src/main.c
  src/app_buzzer.c
  src/app_sensor.c
  src/app_ledstrip.c
)
