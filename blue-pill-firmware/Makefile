# Blue-Pill USB SWire bridge firmware
#
#   make                    build ../bin/blue-pill-firmware.bin and .hex
#   make test               native unit tests for the codec, no hardware
#   make clean              remove ./build, keep the released images
#
# Like the two application Makefiles, this one keeps every intermediate in
# ./build and writes the finished images straight into ../bin, which is the
# single place in the tree where binaries live.  The images carry the same
# names there as in the documentation, so there is no rename step to forget.

CROSS   ?= arm-none-eabi-
CC       = $(CROSS)gcc
OBJCOPY  = $(CROSS)objcopy
SIZE     = $(CROSS)size

BINDIR   = ../bin
BUILD    = build
NAME     = blue-pill-firmware

CFLAGS  = -mcpu=cortex-m3 -mthumb -O3 -g -std=c11 \
          -ffunction-sections -fdata-sections -Wall -Wextra
LDFLAGS = -T stm32f103.ld -nostartfiles -Wl,--gc-sections \
          -Wl,-Map=$(BUILD)/firmware.map

SRC = startup.c main.c swire.c swire_codec.c usb.c
OBJ = $(addprefix $(BUILD)/,$(SRC:.c=.o))

all: $(BINDIR)/$(NAME).bin $(BINDIR)/$(NAME).hex

# Native unit tests for the encode/decode logic (no hardware involved).
test: | $(BUILD)
	gcc -O1 -Wall -Wextra -I. -o $(BUILD)/test_codec test/test_codec.c swire_codec.c
	$(BUILD)/test_codec

$(BUILD)/%.o: %.c | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/firmware.elf: $(OBJ) stm32f103.ld
	$(CC) $(CFLAGS) $(LDFLAGS) $(OBJ) -o $@
	$(SIZE) $@

$(BINDIR)/$(NAME).bin: $(BUILD)/firmware.elf
	$(OBJCOPY) -O binary $< $@

$(BINDIR)/$(NAME).hex: $(BUILD)/firmware.elf
	$(OBJCOPY) -O ihex $< $@

$(BUILD):
	-mkdir $(BUILD)

# Only the intermediates.  ../bin holds the released images and is tracked, so
# `clean` deliberately leaves it alone.  (The old rule used `rm -f`, which does
# not exist in a plain Windows shell and failed silently.)
clean:
	-del /Q $(BUILD)\*.o $(BUILD)\*.elf $(BUILD)\*.map $(BUILD)\test_codec.exe 2>NUL
	-rmdir $(BUILD) 2>NUL

# --- flashing -------------------------------------------------------------
# ST-Link:
flash-openocd: $(BUILD)/firmware.elf
	openocd -f interface/stlink.cfg -f target/stm32f1x.cfg \
	  -c "program $(BUILD)/firmware.elf verify reset exit"

# USB-UART on PA9/PA10 with BOOT0=1 (SERIAL=COMx on the command line):
SERIAL ?= COM3
flash-serial: $(BINDIR)/$(NAME).hex
	stm32flash -w $(BINDIR)/$(NAME).hex -v -g 0x0 $(SERIAL)

.PHONY: all clean test flash-openocd flash-serial
