# Copyright 2026 Roman Kuzmitskii (@damex)
# SPDX-License-Identifier: MIT

# Library, include paths and log module registration shared by mock cases.

set(esb_root ${CMAKE_CURRENT_LIST_DIR}/../../..)
set(mock_common ${CMAKE_CURRENT_LIST_DIR})

zephyr_library_named(zmk_feature_split_esb_test)
zephyr_library_include_directories(
  ${mock_common}/include
  ${esb_root}/src
  ${esb_root}/include
  ${APPLICATION_SOURCE_DIR}/include
)
zephyr_library_sources(${esb_root}/src/esb_log.c)
