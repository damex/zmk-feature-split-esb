# Copyright 2026 Roman Kuzmitskii (@damex)
# SPDX-License-Identifier: MIT

if(CONFIG_ZMK_SPLIT_ESB)
  zephyr_library()
  zephyr_library_compile_options(
      -Werror
      -Wextra
      -Wshadow
      -Wformat=2
      -Wlogical-op
      -Wduplicated-cond
      -Wduplicated-branches
      -Wmissing-prototypes
      -Wjump-misses-init
      -Wwrite-strings
      -Winit-self
      -Wold-style-definition
      -fanalyzer
  )
  zephyr_library_sources(src/esb_link.c)
  zephyr_library_sources(src/esb_wire.c)
  zephyr_library_sources(src/hop.c)
  zephyr_library_sources(src/hop_policy.c)
  zephyr_library_sources_ifdef(CONFIG_ZMK_SPLIT_ROLE_CENTRAL  src/central.c)
  zephyr_library_sources_ifdef(CONFIG_ZMK_SPLIT_ROLE_CENTRAL  src/hop_central.c)
  zephyr_library_sources_ifndef(CONFIG_ZMK_SPLIT_ROLE_CENTRAL src/peripheral.c)
  zephyr_library_sources_ifndef(CONFIG_ZMK_SPLIT_ROLE_CENTRAL src/hop_peripheral.c)
  if(NOT CONFIG_ZMK_SPLIT_ROLE_CENTRAL AND CONFIG_SETTINGS)
    zephyr_library_sources(src/esb_config.c)
  endif()
endif()
