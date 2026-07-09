# Copyright 2026 Roman Kuzmitskii (@damex)
# SPDX-License-Identifier: MIT

cmake_minimum_required(VERSION 3.20.0)

project(esb_split_unit_test)
find_package(Zephyr COMPONENTS unittest HINTS $ENV{ZEPHYR_BASE})

if(NOT EXISTS "/usr/include/gnu/stubs-32.h")
  string(REPLACE "-m32" "-m64" CMAKE_C_FLAGS "${CMAKE_C_FLAGS}")
  string(REPLACE "-m32" "-m64" CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS}")
endif()

set(esb_root ${CMAKE_CURRENT_SOURCE_DIR}/../..)

target_include_directories(testbinary PRIVATE
  ${esb_root}/src
)

target_sources(testbinary PRIVATE
  src/test_esb_keepalive.c
  src/test_esb_hid_state.c
  src/test_hop_policy.c
  ${esb_root}/src/esb_keepalive.c
  ${esb_root}/src/hop_policy.c
)
