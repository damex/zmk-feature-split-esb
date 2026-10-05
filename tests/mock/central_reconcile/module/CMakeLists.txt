# Copyright 2026 Roman Kuzmitskii (@damex)
# SPDX-License-Identifier: MIT

include(${CMAKE_CURRENT_LIST_DIR}/../../common/mock.cmake)

zephyr_library_sources(
  src/test_radio.c
  ${mock_common}/src/mock.c
  ${esb_root}/src/central.c
  ${esb_root}/src/central_input.c
  ${esb_root}/src/esb_keepalive.c
  ${esb_root}/src/esb_link_central.c
  ${esb_root}/src/esb_peripheral_changed.c
  ${esb_root}/src/esb_sensor_sync.c
  ${esb_root}/src/esb_wire.c
  ${esb_root}/src/spsc_latch.c
)
