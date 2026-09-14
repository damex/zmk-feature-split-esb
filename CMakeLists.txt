# Copyright 2026 Roman Kuzmitskii (@damex)
# SPDX-License-Identifier: MIT

if(CONFIG_ZMK_SPLIT_ESB)
  zephyr_include_directories(include)
  zephyr_library()
  zephyr_library_include_directories(${APPLICATION_SOURCE_DIR}/include)
  zephyr_library_compile_options(
      -Werror
      -Wextra
      -Wshadow
      -Wformat=2
      -Wlogical-op
      -Wduplicated-cond
      -Wmissing-prototypes
      -Wjump-misses-init
      -Wwrite-strings
      -Winit-self
      -Wold-style-definition
      -fanalyzer
  )
  zephyr_library_sources(src/esb_keepalive.c)
  zephyr_library_sources(src/esb_sensor_sync.c)
  zephyr_library_sources(src/esb_wire.c)
  if(CONFIG_ZMK_SPLIT_ROLE_CENTRAL OR CONFIG_ZMK_SPLIT_ESB_PERIPHERAL_TRANSPORT_ESB)
    zephyr_library_sources(src/esb_link.c)
    zephyr_library_sources(src/hop.c)
    zephyr_library_sources(src/hop_policy.c)
    zephyr_library_sources_ifdef(CONFIG_ZMK_SLEEP src/esb_sleep.c)
  endif()
  zephyr_library_sources_ifdef(CONFIG_ZMK_SPLIT_ROLE_CENTRAL src/central.c)
  zephyr_library_sources_ifdef(CONFIG_ZMK_SPLIT_ROLE_CENTRAL src/esb_survey.c)
  zephyr_library_sources_ifdef(CONFIG_ZMK_SPLIT_ROLE_CENTRAL src/esb_link_central.c)
  zephyr_library_sources_ifdef(CONFIG_ZMK_SPLIT_ROLE_CENTRAL src/hop_central.c)
  zephyr_library_sources_ifdef(CONFIG_ZMK_SPLIT_ROLE_CENTRAL src/esb_peripheral_changed.c)
  zephyr_library_sources_ifdef(CONFIG_ZMK_SPLIT_ROLE_CENTRAL src/hid_state_central.c)
  if(CONFIG_ZMK_SPLIT_ESB_HID_RELAY AND CONFIG_ZMK_SPLIT_ROLE_CENTRAL)
    zephyr_library_sources(src/esb_hid_relay_central.c)
  endif()
  zephyr_library_sources_ifdef(CONFIG_ZMK_SPLIT_ESB_PERIPHERAL_TRANSPORT_ESB src/esb_hid_relay_peripheral.c)
  if(CONFIG_ZMK_SPLIT_ESB_HID_RELAY AND NOT CONFIG_ZMK_SPLIT_ROLE_CENTRAL)
    zephyr_library_sources(src/esb_hid_relay_usb.c)
  endif()
  zephyr_library_sources_ifndef(CONFIG_ZMK_SPLIT_ROLE_CENTRAL src/peripheral.c)
  zephyr_library_sources_ifdef(CONFIG_ZMK_SPLIT_ESB_PERIPHERAL_TRANSPORT_ESB src/esb_batch.c)
  zephyr_library_sources_ifdef(CONFIG_ZMK_SPLIT_ESB_PERIPHERAL_TRANSPORT_ESB src/esb_link_peripheral.c)
  zephyr_library_sources_ifdef(CONFIG_ZMK_SPLIT_ESB_PERIPHERAL_TRANSPORT_ESB src/hop_peripheral.c)
  zephyr_library_sources_ifdef(CONFIG_ZMK_SPLIT_ESB_PERIPHERAL_TRANSPORT_ESB src/esb_peripheral.c)
  zephyr_library_sources_ifdef(CONFIG_ZMK_SPLIT_ESB_PERIPHERAL_TRANSPORT_WIRE src/wire_peripheral.c)
  zephyr_library_sources_ifdef(CONFIG_ZMK_SPLIT_ESB_WIRE_RELAY src/wire_relay.c)
  zephyr_library_sources_ifdef(CONFIG_ZMK_SPLIT_ESB_WIRE src/cobs.c)
  zephyr_library_sources_ifdef(CONFIG_ZMK_SPLIT_ESB_WIRE src/wire_frame.c)
  zephyr_library_sources_ifdef(CONFIG_ZMK_SPLIT_ESB_WIRE src/wire_link.c)
  if(CONFIG_ZMK_SPLIT_ESB_PERIPHERAL_TRANSPORT_ESB AND CONFIG_SETTINGS)
    zephyr_library_sources(src/esb_config.c)
  endif()
endif()
