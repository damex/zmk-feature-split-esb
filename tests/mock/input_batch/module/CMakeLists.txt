# Copyright 2026 Roman Kuzmitskii (@damex)
# SPDX-License-Identifier: MIT

set(esb_root ${CMAKE_CURRENT_LIST_DIR}/../../../..)

zephyr_library_named(zmk_feature_split_esb_test)
zephyr_library_include_directories(
  ${esb_root}/src
  ${APPLICATION_SOURCE_DIR}/include
)
zephyr_library_sources(
  src/test_batch.c
  ${esb_root}/src/esb_batch.c
  ${esb_root}/src/esb_wire.c
)
