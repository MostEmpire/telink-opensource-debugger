/*
 * version.h - version of the host applications, in one place.
 *
 * Copyright (C) 2026  telink_project contributors
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * Both programmer_cli and programmer_gui are versioned together: they are one
 * product built from one protocol core, so a version that differed between
 * them would say nothing useful.  The GUI reaches this file through its
 * -I../programmer-app-cli, the same way it reaches tlsr_core.h.
 *
 * Included by C sources and by both app.rc files, so it must contain nothing
 * but object-like macros -- windres runs the preprocessor over it too.
 *
 * This is NOT the bridge firmware version.  That one lives in
 * blue-pill-firmware/protocol.h as FW_VERSION, is reported over the wire by
 * PING, and moves independently: an older bridge is expected to work with a
 * newer host.
 */
#ifndef VERSION_H
#define VERSION_H

#define APP_VER_MAJOR 1
#define APP_VER_MINOR 1
#define APP_VER_PATCH 0

#define APP_VERSION   "1.1.0"

/* Win32 file/product version fields are four numbers; the fourth is a build
 * counter this project does not keep, so it stays zero. */
#define APP_VER_FILE  APP_VER_MAJOR, APP_VER_MINOR, APP_VER_PATCH, 0

#define APP_PRODUCT   "TLSR825x Programmer"
#define APP_COMPANY   "telink_project contributors"
#define APP_COPYRIGHT "Copyright (C) 2026 telink_project contributors. GPL-2.0-only."

#endif /* VERSION_H */
