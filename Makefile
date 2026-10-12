# Copyright 2026 Roman Kuzmitskii (@damex)
# SPDX-License-Identifier: MIT

PYTHON ?= python3
ZEPHYR_BASE ?= $(error ZEPHYR_BASE is not set)
ZEPHYR_TOOLCHAIN_VARIANT ?= host
ZMK_APP ?= $(error ZMK_APP is not set)

MOCK_ROOT := $(CURDIR)/tests/mock
MOCK_CASES += \
	central/held_input_two_buttons \
	central/hid_state_sync \
	central/idle_ack \
	central/key_repeated_press_heals_lost_release \
	central/lossy_totals \
	central/pointer_lossless \
	central/reconcile \
	central/rendezvous \
	central/return_path \
	central/self_pipe_not_a_source \
	central/sleep \
	central/startup_order \
	central_hid_relay/burst \
	central_hid_relay/burst_room \
	central_hid_relay/button_backlog \
	central_hid_relay/button_heal \
	central_hid_relay/commands \
	central_hid_relay/host_pause \
	central_hid_relay/idle_ack \
	central_hid_relay/indicators \
	central_hid_relay/mouse_keys \
	central_hid_relay/peripheral_motion_lossless \
	central_hid_relay/peripheral_motion_lossy \
	central_hid_relay/pointer \
	central_hid_relay/pointer_retry \
	central_hid_relay/poll \
	central_hid_relay/tap \
	central_hid_relay/totals \
	esb_link/rx_ring \
	event_codec \
	hid_relay_dongle/busy_channel \
	hid_relay_dongle/host_leds \
	hid_relay_dongle/lost_sweep \
	hid_relay_dongle/pointer \
	hid_relay_dongle/poll \
	hid_relay_dongle/reports \
	hid_relay_dongle/usb_drain \
	input_batch \
	peripheral/adaptive_retransmits \
	peripheral/bad_channel_lossless \
	peripheral/bad_channel_lossy \
	peripheral/burst_keepalive \
	peripheral/burst_start_keepalive \
	peripheral/busy_keys \
	peripheral/busy_pointer_lossless \
	peripheral/busy_pointer_lossy \
	peripheral/epoch_adoption \
	peripheral/held_input \
	peripheral/idle_clock \
	peripheral/idle_keepalive \
	peripheral/keepalive \
	peripheral/lossy_drop \
	peripheral/lost_tap \
	peripheral/pointer_lossless \
	peripheral/pointer_lossy \
	peripheral/rendezvous \
	peripheral/restart_retune \
	peripheral/sleep \
	peripheral/startup_order \
	peripheral/tx_failed \
	peripheral/tx_stall \
	wire/central_hid_state \
	wire/central_simplex \
	wire/peer_held_input \
	wire/peer_hid_state \
	wire/peer_idle_keepalive \
	wire/peer_simplex \
	wire/relay_beacon_rssi \
	wire/relay_downlink \
	wire/relay_hid_state \
	wire/relay_simplex \
	wire/relay_uplink
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
