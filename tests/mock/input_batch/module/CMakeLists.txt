# Copyright 2026 Roman Kuzmitskii (@damex)
# SPDX-License-Identifier: MIT

set(esb_root ${CMAKE_CURRENT_LIST_DIR}/../../../..)
set(mock_common ${esb_root}/tests/mock/common)

zephyr_library_named(zmk_feature_split_esb_test)
zephyr_library_include_directories(
  ${mock_common}/include
  ${esb_root}/src
  ${APPLICATION_SOURCE_DIR}/include
)
zephyr_library_sources(
  src/test_batch.c
  ${mock_common}/src/mock.c
  ${esb_root}/src/esb_batch.c
  ${esb_root}/src/esb_wire.c
)
