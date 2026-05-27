# Copyright 2026 Roman Kuzmitskii (@damex)
# SPDX-License-Identifier: MIT

if(CONFIG_ZMK_SPLIT_ESB)
  zephyr_library()
  zephyr_library_compile_options(-Wconversion -Wsign-conversion)
  zephyr_library_sources(src/esb_link.c)
  zephyr_library_sources_ifdef(CONFIG_ZMK_SPLIT_ROLE_CENTRAL  src/central.c)
  zephyr_library_sources_ifndef(CONFIG_ZMK_SPLIT_ROLE_CENTRAL src/peripheral.c)
endif()
