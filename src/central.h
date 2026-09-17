// Copyright 2026 Roman Kuzmitskii (@damex)
// SPDX-License-Identifier: MIT

#pragma once

#include <stddef.h>
#include <stdint.h>

void central_ingest_packet(uint8_t pipe, const uint8_t *data, size_t length);
