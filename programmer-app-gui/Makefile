# TLSR825x programmer - Win32 GUI application
#
# Copyright (C) 2026  telink_project contributors
# SPDX-License-Identifier: GPL-2.0-only
#
#   mingw32-make            build ../bin/programmer_gui.exe
#   mingw32-make clean      remove the intermediates, keep the released binary
#
# The protocol core lives in ../programmer-app-cli and is compiled straight
# from there, so a fix to the SWire, flash or debug logic reaches both programs.
#
# The executable is written straight into ../bin, which is the one place in the
# tree where binaries live; objects and the compiled resource go into ./build.
# Building into bin directly also removes the copy step that once shipped a
# stale exe: bin/ could not be older than the source again.

CC      = gcc
WINDRES = windres
CORE_DIR = ../programmer-app-cli

CFLAGS  = -O2 -Wall -Wextra -std=c99 -I$(CORE_DIR)
LDFLAGS = -mwindows
LDLIBS  = -lcomctl32 -lcomdlg32 -lgdi32 -ladvapi32

BINDIR  = ../bin
BUILD   = build
TARGET  = $(BINDIR)/programmer_gui.exe

CORE_HDR = $(CORE_DIR)/tlsr_core.h $(CORE_DIR)/tlsr_internal.h \
           $(CORE_DIR)/tlsr_debug.h $(CORE_DIR)/version.h
OBJ      = $(BUILD)/main_gui.o $(BUILD)/tlsr_serial.o $(BUILD)/tlsr_bridge.o \
           $(BUILD)/tlsr_flash.o $(BUILD)/tlsr_debug.o $(BUILD)/app.res

all: $(TARGET)

$(TARGET): $(OBJ)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(OBJ) $(LDLIBS)

$(BUILD)/main_gui.o: main_gui.c $(CORE_HDR) res.h | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

# The core objects are built here rather than linked from the CLI directory so
# that a plain `make` in either folder works without building the other first.
$(BUILD)/%.o: $(CORE_DIR)/%.c $(CORE_HDR) | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

# Manifest resource: asks for comctl32 v6 so the controls are themed.
# Also carries the debugger toolbar icons, so a redrawn .ico rebuilds it, and
# the version block -- hence -I$(CORE_DIR), where version.h lives.
ICONS = $(wildcard icons/*.ico)

$(BUILD)/app.res: app.rc res.h $(CORE_DIR)/version.h $(ICONS) | $(BUILD)
	$(WINDRES) -I$(CORE_DIR) $< -O coff -o $@

$(BUILD):
	-mkdir $(BUILD)

# GNU make defaults RM to `rm -f`, which does not exist in a plain Windows
# shell -- the old rule failed silently and left stale objects behind, so a
# rebuild after editing a header quietly did nothing.
#
# Only the intermediates.  ../bin holds the released binaries and is tracked,
# so `clean` deliberately leaves it alone.
clean:
	-del /Q $(BUILD)\*.o $(BUILD)\*.res 2>NUL
	-rmdir $(BUILD) 2>NUL

.PHONY: all clean
