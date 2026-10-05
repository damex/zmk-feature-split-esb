# Copyright 2026 Roman Kuzmitskii (@damex)
# SPDX-License-Identifier: MIT

include(${CMAKE_CURRENT_LIST_DIR}/../../common/mock.cmake)

zephyr_library_sources(
  src/test_radio.c
  ${mock_common}/src/mock.c
  ${mock_common}/src/mock_esb.c
  ${mock_common}/src/mock_radio_central.c
  ${mock_common}/src/mock_wire.c
  ${esb_root}/src/cobs.c
  ${esb_root}/src/esb_keepalive.c
  ${esb_root}/src/esb_link_central.c
  ${esb_root}/src/hid_state_central.c
  ${esb_root}/src/hop.c
  ${esb_root}/src/hop_central.c
  ${esb_root}/src/hop_policy.c
  ${esb_root}/src/spsc_latch.c
  ${esb_root}/src/wire_central.c
  ${esb_root}/src/wire_frame.c
  ${esb_root}/src/wire_link.c
)
