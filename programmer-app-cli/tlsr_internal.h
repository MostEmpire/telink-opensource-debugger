/*
 * tlsr_internal.h - shared between the core .c files, not part of the API.
 *
 * Copyright (C) 2026  telink_project contributors
 * SPDX-License-Identifier: GPL-2.0-only
 */
#ifndef TLSR_INTERNAL_H
#define TLSR_INTERNAL_H

#include <windows.h>
#include <stdarg.h>
#include <stdlib.h>
#include <stdint.h>

#include "tlsr_core.h"

struct tlsr_dev {
    HANDLE h;
    char   port[32];
    tlsr_cfg cfg;          /* last configuration read from the bridge   */
    int      cfg_valid;
    int      activated;    /* target has answered since we connected     */
};

/* Largest SWire payload the bridge will encode in one frame, and the largest
 * flash block it will transfer per command.  Both are limited by the buffers
 * in the firmware; exceeding them earns a bad-length status. */
#define TLSR_CHUNK        128u
#define TLSR_FLASH_BLOCK 1024u

void tlsr_set_error(const char *fmt, ...);

int  tlsr_write_all(tlsr_dev *d, const uint8_t *data, uint32_t n);
int  tlsr_read_exact(tlsr_dev *d, uint8_t *out, uint32_t n, unsigned timeout_ms);
void tlsr_purge(tlsr_dev *d);

/* Convenience for the callbacks, which are all optional. */
#define TLSR_LOG(fn, user, ...) do {                       \
        if (fn) {                                          \
            char _b[256];                                  \
            snprintf(_b, sizeof _b, __VA_ARGS__);          \
            (fn)((user), _b);                              \
        }                                                  \
    } while (0)

#define TLSR_PROG(fn, user, done, total) do {              \
        if (fn) (fn)((user), (uint32_t)(done), (uint32_t)(total)); \
    } while (0)

#endif /* TLSR_INTERNAL_H */
