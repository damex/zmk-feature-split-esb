// Copyright 2026 Roman Kuzmitskii (@damex)
// SPDX-License-Identifier: MIT

/*
 * Fake log backend shared by mock cases, keeps every formatted line from boot.
 * Lines land as they are logged only with CONFIG_LOG_MODE_IMMEDIATE.
 */
#pragma once

#include <stddef.h>

/* Lines holding needle, exits 1 once the capture overflowed. */
size_t mock_log_count(const char *needle);
