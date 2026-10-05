# Copyright 2026 Roman Kuzmitskii (@damex)
# SPDX-License-Identifier: MIT

include(${CMAKE_CURRENT_LIST_DIR}/../../common/mock.cmake)

zephyr_library_sources(
  src/test_batch.c
  ${mock_common}/src/mock.c
  ${esb_root}/src/esb_batch.c
  ${esb_root}/src/esb_wire.c
)
