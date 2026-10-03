# Copyright 2026 Roman Kuzmitskii (@damex)
# SPDX-License-Identifier: MIT

cmake_minimum_required(VERSION 3.20.0)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(esb_wire_link_test)

set(esb_root ${CMAKE_CURRENT_SOURCE_DIR}/../../..)

target_include_directories(app PRIVATE
  ${esb_root}/src
)

target_sources(app PRIVATE
  src/test_wire_link.c
  ${esb_root}/src/cobs.c
  ${esb_root}/src/wire_frame.c
  ${esb_root}/src/wire_link.c
)
