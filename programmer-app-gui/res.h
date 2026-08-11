/*
 * res.h - resource ids shared between app.rc and main_gui.c
 *
 * Copyright (C) 2026  telink_project contributors
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * The debugger toolbar icons are compiled into the exe as ICON resources.
 * Each .ico carries 16/24/32/48 at 32-bit RGBA, so LoadImage() picks the size
 * the toolbar asks for instead of scaling one bitmap badly.
 */
#ifndef RES_H
#define RES_H

#define IDI_D_CONTINUE   200
#define IDI_D_PAUSE      201
#define IDI_D_STOP       202
#define IDI_D_RESTART    203
#define IDI_D_STEPINTO   204
#define IDI_D_STEPOVER   205
#define IDI_D_STEPOUT    206
#define IDI_D_STEPINST   207
#define IDI_D_RUNCUR     208
#define IDI_D_BPTOGGLE   209
#define IDI_D_BPCLEAR    210
#define IDI_D_REFRESH    211

#endif
