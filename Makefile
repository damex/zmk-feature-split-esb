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
	cd $(ZMK_APP) && ZEPHYR_TOOLCHAIN_VARIANT=$(ZEPHYR_TOOLCHAIN_VARIANT) west build -p -d $(CURDIR)/mock-test-out/event_codec -b native_sim//zmk_test_mock -- -DZMK_CONFIG=$(CURDIR)/tests/mock/event_codec -DZMK_EXTRA_MODULES=$(CURDIR)/tests/mock/event_codec/module
	mock-test-out/event_codec/zephyr/zmk.exe
	cd $(ZMK_APP) && ZEPHYR_TOOLCHAIN_VARIANT=$(ZEPHYR_TOOLCHAIN_VARIANT) west build -p -d $(CURDIR)/mock-test-out/input_batch -b native_sim//zmk_test_mock -- -DZMK_CONFIG=$(CURDIR)/tests/mock/input_batch -DZMK_EXTRA_MODULES=$(CURDIR)/tests/mock/input_batch/module
	mock-test-out/input_batch/zephyr/zmk.exe
	cd $(ZMK_APP) && ZEPHYR_TOOLCHAIN_VARIANT=$(ZEPHYR_TOOLCHAIN_VARIANT) west build -p -d $(CURDIR)/mock-test-out/central_reconcile -b native_sim//zmk_test_mock -- -DZMK_CONFIG=$(CURDIR)/tests/mock/central_reconcile -DZMK_EXTRA_MODULES=$(CURDIR)/tests/mock/central_reconcile/module
	mock-test-out/central_reconcile/zephyr/zmk.exe
	cd $(ZMK_APP) && ZEPHYR_TOOLCHAIN_VARIANT=$(ZEPHYR_TOOLCHAIN_VARIANT) west build -p -d $(CURDIR)/mock-test-out/peripheral_held_input -b native_sim//zmk_test_mock -- -DZMK_CONFIG=$(CURDIR)/tests/mock/peripheral_held_input -DZMK_EXTRA_MODULES=$(CURDIR)/tests/mock/peripheral_held_input/module
	mock-test-out/peripheral_held_input/zephyr/zmk.exe
	cd $(ZMK_APP) && ZEPHYR_TOOLCHAIN_VARIANT=$(ZEPHYR_TOOLCHAIN_VARIANT) west build -p -d $(CURDIR)/mock-test-out/peripheral_keepalive -b native_sim//zmk_test_mock -- -DZMK_CONFIG=$(CURDIR)/tests/mock/peripheral_keepalive -DZMK_EXTRA_MODULES=$(CURDIR)/tests/mock/peripheral_keepalive/module
	mock-test-out/peripheral_keepalive/zephyr/zmk.exe
	cd $(ZMK_APP) && ZEPHYR_TOOLCHAIN_VARIANT=$(ZEPHYR_TOOLCHAIN_VARIANT) west build -p -d $(CURDIR)/mock-test-out/peripheral_rendezvous -b native_sim//zmk_test_mock -- -DZMK_CONFIG=$(CURDIR)/tests/mock/peripheral_rendezvous -DZMK_EXTRA_MODULES=$(CURDIR)/tests/mock/peripheral_rendezvous/module
	mock-test-out/peripheral_rendezvous/zephyr/zmk.exe
	cd $(ZMK_APP) && ZEPHYR_TOOLCHAIN_VARIANT=$(ZEPHYR_TOOLCHAIN_VARIANT) west build -p -d $(CURDIR)/mock-test-out/hid_relay_tap -b native_sim//zmk_test_mock -- -DZMK_CONFIG=$(CURDIR)/tests/mock/hid_relay_tap -DZMK_EXTRA_MODULES=$(CURDIR)/tests/mock/hid_relay_tap/module
	mock-test-out/hid_relay_tap/zephyr/zmk.exe
	cd $(ZMK_APP) && ZEPHYR_TOOLCHAIN_VARIANT=$(ZEPHYR_TOOLCHAIN_VARIANT) west build -p -d $(CURDIR)/mock-test-out/hid_relay_burst -b native_sim//zmk_test_mock -- -DZMK_CONFIG=$(CURDIR)/tests/mock/hid_relay_burst -DZMK_EXTRA_MODULES=$(CURDIR)/tests/mock/hid_relay_burst/module
	mock-test-out/hid_relay_burst/zephyr/zmk.exe
	cd $(ZMK_APP) && ZEPHYR_TOOLCHAIN_VARIANT=$(ZEPHYR_TOOLCHAIN_VARIANT) west build -p -d $(CURDIR)/mock-test-out/hid_relay_commands -b native_sim//zmk_test_mock -- -DZMK_CONFIG=$(CURDIR)/tests/mock/hid_relay_commands -DZMK_EXTRA_MODULES=$(CURDIR)/tests/mock/hid_relay_commands/module
	mock-test-out/hid_relay_commands/zephyr/zmk.exe
	cd $(ZMK_APP) && ZEPHYR_TOOLCHAIN_VARIANT=$(ZEPHYR_TOOLCHAIN_VARIANT) west build -p -d $(CURDIR)/mock-test-out/hid_relay_sink -b native_sim//zmk_test_mock -- -DZMK_CONFIG=$(CURDIR)/tests/mock/hid_relay_sink -DZMK_EXTRA_MODULES=$(CURDIR)/tests/mock/hid_relay_sink/module
	mock-test-out/hid_relay_sink/zephyr/zmk.exe
	cd $(ZMK_APP) && ZEPHYR_TOOLCHAIN_VARIANT=$(ZEPHYR_TOOLCHAIN_VARIANT) west build -p -d $(CURDIR)/mock-test-out/hid_state_sync -b native_sim//zmk_test_mock -- -DZMK_CONFIG=$(CURDIR)/tests/mock/hid_state_sync -DZMK_EXTRA_MODULES=$(CURDIR)/tests/mock/hid_state_sync/module
	mock-test-out/hid_state_sync/zephyr/zmk.exe
	cd $(ZMK_APP) && ZEPHYR_TOOLCHAIN_VARIANT=$(ZEPHYR_TOOLCHAIN_VARIANT) west build -p -d $(CURDIR)/mock-test-out/wire_central_hid_state -b native_sim//zmk_test_mock -- -DZMK_CONFIG=$(CURDIR)/tests/mock/wire_central_hid_state -DZMK_EXTRA_MODULES=$(CURDIR)/tests/mock/wire_central_hid_state/module
	mock-test-out/wire_central_hid_state/zephyr/zmk.exe
	cd $(ZMK_APP) && ZEPHYR_TOOLCHAIN_VARIANT=$(ZEPHYR_TOOLCHAIN_VARIANT) west build -p -d $(CURDIR)/mock-test-out/wire_central_simplex -b native_sim//zmk_test_mock -- -DZMK_CONFIG=$(CURDIR)/tests/mock/wire_central_simplex -DZMK_EXTRA_MODULES=$(CURDIR)/tests/mock/wire_central_simplex/module
	mock-test-out/wire_central_simplex/zephyr/zmk.exe
	cd $(ZMK_APP) && ZEPHYR_TOOLCHAIN_VARIANT=$(ZEPHYR_TOOLCHAIN_VARIANT) west build -p -d $(CURDIR)/mock-test-out/wire_peer_idle_keepalive -b native_sim//zmk_test_mock -- -DZMK_CONFIG=$(CURDIR)/tests/mock/wire_peer_idle_keepalive -DZMK_EXTRA_MODULES=$(CURDIR)/tests/mock/wire_peer_idle_keepalive/module
	mock-test-out/wire_peer_idle_keepalive/zephyr/zmk.exe
	cd $(ZMK_APP) && ZEPHYR_TOOLCHAIN_VARIANT=$(ZEPHYR_TOOLCHAIN_VARIANT) west build -p -d $(CURDIR)/mock-test-out/wire_peer_hid_state -b native_sim//zmk_test_mock -- -DZMK_CONFIG=$(CURDIR)/tests/mock/wire_peer_hid_state -DZMK_EXTRA_MODULES=$(CURDIR)/tests/mock/wire_peer_hid_state/module
	mock-test-out/wire_peer_hid_state/zephyr/zmk.exe
	cd $(ZMK_APP) && ZEPHYR_TOOLCHAIN_VARIANT=$(ZEPHYR_TOOLCHAIN_VARIANT) west build -p -d $(CURDIR)/mock-test-out/wire_peer_simplex -b native_sim//zmk_test_mock -- -DZMK_CONFIG=$(CURDIR)/tests/mock/wire_peer_simplex -DZMK_EXTRA_MODULES=$(CURDIR)/tests/mock/wire_peer_simplex/module
	mock-test-out/wire_peer_simplex/zephyr/zmk.exe
	cd $(ZMK_APP) && ZEPHYR_TOOLCHAIN_VARIANT=$(ZEPHYR_TOOLCHAIN_VARIANT) west build -p -d $(CURDIR)/mock-test-out/wire_peer_held_input -b native_sim//zmk_test_mock -- -DZMK_CONFIG=$(CURDIR)/tests/mock/wire_peer_held_input -DZMK_EXTRA_MODULES=$(CURDIR)/tests/mock/wire_peer_held_input/module
	mock-test-out/wire_peer_held_input/zephyr/zmk.exe
	cd $(ZMK_APP) && ZEPHYR_TOOLCHAIN_VARIANT=$(ZEPHYR_TOOLCHAIN_VARIANT) west build -p -d $(CURDIR)/mock-test-out/wire_relay_downlink -b native_sim//zmk_test_mock -- -DZMK_CONFIG=$(CURDIR)/tests/mock/wire_relay_downlink -DZMK_EXTRA_MODULES=$(CURDIR)/tests/mock/wire_relay_downlink/module
	mock-test-out/wire_relay_downlink/zephyr/zmk.exe
	cd $(ZMK_APP) && ZEPHYR_TOOLCHAIN_VARIANT=$(ZEPHYR_TOOLCHAIN_VARIANT) west build -p -d $(CURDIR)/mock-test-out/wire_relay_beacon_rssi -b native_sim//zmk_test_mock -- -DZMK_CONFIG=$(CURDIR)/tests/mock/wire_relay_beacon_rssi -DZMK_EXTRA_MODULES=$(CURDIR)/tests/mock/wire_relay_beacon_rssi/module
	mock-test-out/wire_relay_beacon_rssi/zephyr/zmk.exe
	cd $(ZMK_APP) && ZEPHYR_TOOLCHAIN_VARIANT=$(ZEPHYR_TOOLCHAIN_VARIANT) west build -p -d $(CURDIR)/mock-test-out/wire_relay_hid_state -b native_sim//zmk_test_mock -- -DZMK_CONFIG=$(CURDIR)/tests/mock/wire_relay_hid_state -DZMK_EXTRA_MODULES=$(CURDIR)/tests/mock/wire_relay_hid_state/module
	mock-test-out/wire_relay_hid_state/zephyr/zmk.exe
	cd $(ZMK_APP) && ZEPHYR_TOOLCHAIN_VARIANT=$(ZEPHYR_TOOLCHAIN_VARIANT) west build -p -d $(CURDIR)/mock-test-out/wire_relay_simplex -b native_sim//zmk_test_mock -- -DZMK_CONFIG=$(CURDIR)/tests/mock/wire_relay_simplex -DZMK_EXTRA_MODULES=$(CURDIR)/tests/mock/wire_relay_simplex/module
	mock-test-out/wire_relay_simplex/zephyr/zmk.exe
	cd $(ZMK_APP) && ZEPHYR_TOOLCHAIN_VARIANT=$(ZEPHYR_TOOLCHAIN_VARIANT) west build -p -d $(CURDIR)/mock-test-out/wire_relay_uplink -b native_sim//zmk_test_mock -- -DZMK_CONFIG=$(CURDIR)/tests/mock/wire_relay_uplink -DZMK_EXTRA_MODULES=$(CURDIR)/tests/mock/wire_relay_uplink/module
	mock-test-out/wire_relay_uplink/zephyr/zmk.exe
