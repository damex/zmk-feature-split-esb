# Copyright 2026 Roman Kuzmitskii (@damex)
# SPDX-License-Identifier: MIT

PYTHON ?= python3
ZEPHYR_BASE ?= $(error ZEPHYR_BASE is not set)
ZEPHYR_TOOLCHAIN_VARIANT ?= host
ZMK_APP ?= $(error ZMK_APP is not set)

MOCK_ROOT := $(CURDIR)/tests/mock
MOCK_CASES ?= event_codec input_batch esb_link_rx_ring central_reconcile peripheral_held_input peripheral_keepalive peripheral_rendezvous peripheral_epoch_adoption hid_relay_tap hid_relay_burst hid_relay_commands hid_relay_sink hid_state_sync wire_central_hid_state wire_central_simplex wire_peer_idle_keepalive wire_peer_hid_state wire_peer_simplex wire_peer_held_input wire_relay_downlink wire_relay_beacon_rssi wire_relay_hid_state wire_relay_simplex wire_relay_uplink
MOCK_TARGETS := $(addprefix mock-test-,$(MOCK_CASES))

.PHONY: unit-test native-sim-test mock-test $(MOCK_TARGETS)
unit-test:
	ZEPHYR_TOOLCHAIN_VARIANT=$(ZEPHYR_TOOLCHAIN_VARIANT) $(PYTHON) $(ZEPHYR_BASE)/scripts/twister --testsuite-root tests/unit --platform unit_testing --inline-logs --clobber-output

native-sim-test:
	ZEPHYR_TOOLCHAIN_VARIANT=$(ZEPHYR_TOOLCHAIN_VARIANT) $(PYTHON) $(ZEPHYR_BASE)/scripts/twister --testsuite-root tests/native_sim --platform native_sim/native/64 --inline-logs --clobber-output --outdir twister-out-native-sim

mock-test: $(MOCK_TARGETS)

define mock_case_rule
mock-test-$(1):
	cd $$(ZMK_APP) && ZEPHYR_TOOLCHAIN_VARIANT=$$(ZEPHYR_TOOLCHAIN_VARIANT) west build -p -d $$(CURDIR)/mock-test-out/$(1) -b native_sim//zmk_test_mock -- -DZMK_CONFIG=$$(MOCK_ROOT)/$(1) -DZMK_EXTRA_MODULES="$$(MOCK_ROOT)/$(1)/module;$$(MOCK_ROOT)/common"
	mock-test-out/$(1)/zephyr/zmk.exe
endef

$(foreach case,$(MOCK_CASES),$(eval $(call mock_case_rule,$(case))))
