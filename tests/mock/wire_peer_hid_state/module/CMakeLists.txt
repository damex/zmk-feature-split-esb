# Copyright 2026 Roman Kuzmitskii (@damex)
# SPDX-License-Identifier: MIT

include(${CMAKE_CURRENT_LIST_DIR}/../../common/mock.cmake)

zephyr_library_sources(
  src/test_wire.c
  ${mock_common}/src/mock.c
  ${mock_common}/src/mock_wire.c
  ${esb_root}/src/cobs.c
  ${esb_root}/src/esb_keepalive.c
  ${esb_root}/src/esb_sensor_sync.c
  ${esb_root}/src/esb_wire.c
  ${esb_root}/src/peripheral.c
  ${esb_root}/src/wire_frame.c
  ${esb_root}/src/wire_link.c
  ${esb_root}/src/wire_peripheral.c
)
