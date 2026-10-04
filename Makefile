# Copyright 2026 Roman Kuzmitskii (@damex)
# SPDX-License-Identifier: MIT

PYTHON ?= python3
ZEPHYR_BASE ?= $(error ZEPHYR_BASE is not set)
ZEPHYR_TOOLCHAIN_VARIANT ?= host
ZMK_APP ?= $(error ZMK_APP is not set)

.PHONY: unit-test native-sim-test mock-test
unit-test:
	ZEPHYR_TOOLCHAIN_VARIANT=$(ZEPHYR_TOOLCHAIN_VARIANT) $(PYTHON) $(ZEPHYR_BASE)/scripts/twister --testsuite-root tests/unit --platform unit_testing --inline-logs --clobber-output

native-sim-test:
	ZEPHYR_TOOLCHAIN_VARIANT=$(ZEPHYR_TOOLCHAIN_VARIANT) $(PYTHON) $(ZEPHYR_BASE)/scripts/twister --testsuite-root tests/native_sim --platform native_sim/native/64 --inline-logs --clobber-output --outdir twister-out-native-sim

mock-test:
	cd $(ZMK_APP) && ZEPHYR_TOOLCHAIN_VARIANT=$(ZEPHYR_TOOLCHAIN_VARIANT) west build -p -d $(CURDIR)/mock-test-out/hid_relay_tap -b native_sim//zmk_test_mock -- -DZMK_CONFIG=$(CURDIR)/tests/mock/hid_relay_tap -DZMK_EXTRA_MODULES=$(CURDIR)/tests/mock/hid_relay_tap/module
	mock-test-out/hid_relay_tap/zephyr/zmk.exe
	cd $(ZMK_APP) && ZEPHYR_TOOLCHAIN_VARIANT=$(ZEPHYR_TOOLCHAIN_VARIANT) west build -p -d $(CURDIR)/mock-test-out/hid_state_sync -b native_sim//zmk_test_mock -- -DZMK_CONFIG=$(CURDIR)/tests/mock/hid_state_sync -DZMK_EXTRA_MODULES=$(CURDIR)/tests/mock/hid_state_sync/module
	mock-test-out/hid_state_sync/zephyr/zmk.exe
	cd $(ZMK_APP) && ZEPHYR_TOOLCHAIN_VARIANT=$(ZEPHYR_TOOLCHAIN_VARIANT) west build -p -d $(CURDIR)/mock-test-out/wire_central_hid_state -b native_sim//zmk_test_mock -- -DZMK_CONFIG=$(CURDIR)/tests/mock/wire_central_hid_state -DZMK_EXTRA_MODULES=$(CURDIR)/tests/mock/wire_central_hid_state/module
	mock-test-out/wire_central_hid_state/zephyr/zmk.exe
	cd $(ZMK_APP) && ZEPHYR_TOOLCHAIN_VARIANT=$(ZEPHYR_TOOLCHAIN_VARIANT) west build -p -d $(CURDIR)/mock-test-out/wire_peer_idle_keepalive -b native_sim//zmk_test_mock -- -DZMK_CONFIG=$(CURDIR)/tests/mock/wire_peer_idle_keepalive -DZMK_EXTRA_MODULES=$(CURDIR)/tests/mock/wire_peer_idle_keepalive/module
	mock-test-out/wire_peer_idle_keepalive/zephyr/zmk.exe
	cd $(ZMK_APP) && ZEPHYR_TOOLCHAIN_VARIANT=$(ZEPHYR_TOOLCHAIN_VARIANT) west build -p -d $(CURDIR)/mock-test-out/wire_peer_hid_state -b native_sim//zmk_test_mock -- -DZMK_CONFIG=$(CURDIR)/tests/mock/wire_peer_hid_state -DZMK_EXTRA_MODULES=$(CURDIR)/tests/mock/wire_peer_hid_state/module
	mock-test-out/wire_peer_hid_state/zephyr/zmk.exe
	cd $(ZMK_APP) && ZEPHYR_TOOLCHAIN_VARIANT=$(ZEPHYR_TOOLCHAIN_VARIANT) west build -p -d $(CURDIR)/mock-test-out/wire_relay_downlink -b native_sim//zmk_test_mock -- -DZMK_CONFIG=$(CURDIR)/tests/mock/wire_relay_downlink -DZMK_EXTRA_MODULES=$(CURDIR)/tests/mock/wire_relay_downlink/module
	mock-test-out/wire_relay_downlink/zephyr/zmk.exe
	cd $(ZMK_APP) && ZEPHYR_TOOLCHAIN_VARIANT=$(ZEPHYR_TOOLCHAIN_VARIANT) west build -p -d $(CURDIR)/mock-test-out/wire_relay_beacon_rssi -b native_sim//zmk_test_mock -- -DZMK_CONFIG=$(CURDIR)/tests/mock/wire_relay_beacon_rssi -DZMK_EXTRA_MODULES=$(CURDIR)/tests/mock/wire_relay_beacon_rssi/module
	mock-test-out/wire_relay_beacon_rssi/zephyr/zmk.exe
	cd $(ZMK_APP) && ZEPHYR_TOOLCHAIN_VARIANT=$(ZEPHYR_TOOLCHAIN_VARIANT) west build -p -d $(CURDIR)/mock-test-out/wire_relay_hid_state -b native_sim//zmk_test_mock -- -DZMK_CONFIG=$(CURDIR)/tests/mock/wire_relay_hid_state -DZMK_EXTRA_MODULES=$(CURDIR)/tests/mock/wire_relay_hid_state/module
	mock-test-out/wire_relay_hid_state/zephyr/zmk.exe
	cd $(ZMK_APP) && ZEPHYR_TOOLCHAIN_VARIANT=$(ZEPHYR_TOOLCHAIN_VARIANT) west build -p -d $(CURDIR)/mock-test-out/wire_relay_uplink -b native_sim//zmk_test_mock -- -DZMK_CONFIG=$(CURDIR)/tests/mock/wire_relay_uplink -DZMK_EXTRA_MODULES=$(CURDIR)/tests/mock/wire_relay_uplink/module
	mock-test-out/wire_relay_uplink/zephyr/zmk.exe
