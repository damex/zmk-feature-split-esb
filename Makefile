# Copyright 2026 Roman Kuzmitskii (@damex)
# SPDX-License-Identifier: MIT

PYTHON ?= python3
ZEPHYR_BASE ?= $(error ZEPHYR_BASE is not set)
ZEPHYR_TOOLCHAIN_VARIANT ?= host
ZMK_APP ?= $(error ZMK_APP is not set)

MOCK_ROOT := $(CURDIR)/tests/mock
MOCK_CASES += \
	central_hid_relay_burst \
	central_hid_relay_commands \
	central_hid_relay_host_pause \
	central_hid_relay_indicators \
	central_hid_relay_pointer \
	central_hid_relay_pointer_retry \
	central_hid_relay_tap \
	central_pointer \
	central_reconcile \
	central_rendezvous \
	central_return_path \
	central_sleep \
	central_startup_order \
	esb_link_rx_ring \
	esb_link_tx_failed \
	esb_link_tx_stall \
	event_codec \
	hid_state_sync \
	input_batch \
	peripheral_adaptive_retransmits \
	peripheral_burst_keepalive \
	peripheral_burst_start_keepalive \
	peripheral_epoch_adoption \
	peripheral_held_input \
	peripheral_idle_clock \
	peripheral_idle_keepalive \
	peripheral_keepalive \
	peripheral_lost_tap \
	peripheral_pointer \
	peripheral_rendezvous \
	peripheral_restart_retune \
	peripheral_sleep \
	peripheral_startup_order \
	relay_host_leds \
	relay_pointer \
	relay_reports \
	wire_central_hid_state \
	wire_central_simplex \
	wire_peer_held_input \
	wire_peer_hid_state \
	wire_peer_idle_keepalive \
	wire_peer_simplex \
	wire_relay_beacon_rssi \
	wire_relay_downlink \
	wire_relay_hid_state \
	wire_relay_simplex \
	wire_relay_uplink
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
