# Copyright 2026 Roman Kuzmitskii (@damex)
# SPDX-License-Identifier: MIT

include(${CMAKE_CURRENT_LIST_DIR}/../../common/mock.cmake)

zephyr_library_sources(
  src/test_radio.c
  ${mock_common}/src/mock.c
  ${mock_common}/src/mock_esb.c
  ${mock_common}/src/mock_radio_central.c
  ${esb_root}/src/esb_hid_relay_central.c
  ${esb_root}/src/esb_link_central.c
  ${esb_root}/src/spsc_latch.c
)
