// Copyright 2026 Roman Kuzmitskii (@damex)
// SPDX-License-Identifier: MIT

/* Fake log backend shared by mock cases. */

#include "mock_log.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include <zephyr/logging/log_backend.h>
#include <zephyr/logging/log_output.h>
#include <zephyr/sys/printk.h>
#include <zephyr/sys/util.h>

#define LINE_LENGTH_MAX 160
#define LINES_MAX 64

static char lines[LINES_MAX][LINE_LENGTH_MAX];
static size_t line_count;
static bool lines_overflowed;
static char pending_line[LINE_LENGTH_MAX];
static size_t pending_length;
static uint8_t output_buffer[LINE_LENGTH_MAX];

static void finish_line(void) {
    if (line_count == LINES_MAX) {
        lines_overflowed = true;
        pending_length = 0;
        return;
    }
    memcpy(lines[line_count], pending_line, pending_length);
    lines[line_count][pending_length] = '\0';
    line_count++;
    pending_length = 0;
}

static void take_character(uint8_t character) {
    if (character == '\n') {
        finish_line();
        return;
    }
    if (character == '\r' || pending_length == LINE_LENGTH_MAX - 1) {
        return;
    }
    pending_line[pending_length] = (char)character;
    pending_length++;
}

static int capture_output(uint8_t *data, size_t length, void *context) {
    ARG_UNUSED(context);
    for (size_t index = 0; index < length; index++) {
        take_character(data[index]);
    }
    return (int)length;
}

LOG_OUTPUT_DEFINE(capture_log_output, capture_output, output_buffer, sizeof(output_buffer));

static void capture_process(const struct log_backend *const backend,
                            union log_msg_generic *message) {
    ARG_UNUSED(backend);
    log_output_msg_process(&capture_log_output, &message->log, LOG_OUTPUT_FLAG_LEVEL);
}

static const struct log_backend_api capture_api = {
    .process = capture_process,
};

LOG_BACKEND_DEFINE(mock_log_capture, capture_api, true);

size_t mock_log_count(const char *needle) {
    if (lines_overflowed) {
        printk("FAIL: log capture overflowed past %u lines\n", (unsigned int)LINES_MAX);
        exit(1);
    }
    size_t matches = 0;
    for (size_t line = 0; line < line_count; line++) {
        if (strstr(lines[line], needle) != NULL) {
            matches++;
        }
    }
    return matches;
}
