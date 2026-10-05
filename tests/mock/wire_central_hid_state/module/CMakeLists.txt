# Copyright 2026 Roman Kuzmitskii (@damex)
# SPDX-License-Identifier: MIT

set(esb_root ${CMAKE_CURRENT_LIST_DIR}/../../../..)
set(mock_common ${esb_root}/tests/mock/common)

zephyr_library_named(zmk_feature_split_esb_test)
zephyr_library_include_directories(
  ${mock_common}/include
  ${esb_root}/src
  ${esb_root}/include
  ${APPLICATION_SOURCE_DIR}/include
)
zephyr_library_sources(
  src/test_radio.c
  ${mock_common}/src/mock.c
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
