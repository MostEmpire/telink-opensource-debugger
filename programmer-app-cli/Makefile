# TLSR825x programmer - command line application
#
# Copyright (C) 2026  telink_project contributors
# SPDX-License-Identifier: GPL-2.0-only
#
#   mingw32-make            build ../bin/programmer_cli.exe
#   mingw32-make clean      remove the intermediates, keep the released binary
#
# Built with MinGW-w64 gcc.  Only the Win32 API is used, so there are no
# third-party dependencies to install.
#
# The executable is written straight into ../bin, which is the one place in the
# tree where binaries live; object files go into ./build.  Nothing buildable is
# ever left next to the sources.

CC      = gcc
CFLAGS  ?= -O2 -Wall -Wextra -std=c99
LDFLAGS ?=
LDLIBS  ?= -ladvapi32

BINDIR  = ../bin
BUILD   = build
TARGET  = $(BINDIR)/programmer_cli.exe

# tlsr_*.c are the shared protocol core; the GUI compiles these same files.
CORE    = tlsr_serial.c tlsr_bridge.c tlsr_flash.c tlsr_debug.c
SRC     = $(CORE) main_cli.c
OBJ     = $(addprefix $(BUILD)/,$(SRC:.c=.o))
HDR     = tlsr_core.h tlsr_internal.h tlsr_debug.h

all: $(TARGET)

$(TARGET): $(OBJ)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(OBJ) $(LDLIBS)

$(BUILD)/%.o: %.c $(HDR) | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD):
	-mkdir $(BUILD)

# Only the intermediates.  ../bin holds the released binaries and is tracked,
# so `clean` deliberately leaves it alone.
clean:
	-del /Q $(BUILD)\*.o 2>NUL
	-rmdir $(BUILD) 2>NUL

.PHONY: all clean
