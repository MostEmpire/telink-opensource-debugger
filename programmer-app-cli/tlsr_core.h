/*
 * tlsr_core.h - Telink TLSR825x programmer core.
 *
 * Copyright (C) 2026  telink_project contributors
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 2 as published by
 * the Free Software Foundation.  See the LICENSE file at the repository root.
 *
 * ---------------------------------------------------------------------------
 *
 * This is the whole protocol stack shared by the CLI and the GUI:
 *
 *      application  (programmer_cli.exe / programmer_gui.exe)
 *          |
 *      tlsr_flash.c    SPI-NOR flash through the target's MSPI controller
 *      tlsr_bridge.c   bridge command framing, SWire register/SRAM access
 *      tlsr_serial.c   Win32 handle on \\.\COMx
 *          |
 *      Blue Pill bridge  --SWire-->  TLSR825x
 *
 * Nothing here depends on a GUI, so the same object files link into both
 * programs; the GUI's Makefile compiles these sources straight out of this
 * directory.
 *
 * Every function returns 0 on success and a negative TLSR_E* code on failure,
 * and leaves a human-readable explanation in tlsr_last_error().
 */
#ifndef TLSR_CORE_H
#define TLSR_CORE_H

#include <stdint.h>
#include <stddef.h>

/* ------------------------------------------------------------ error codes -- */
#define TLSR_OK             0
#define TLSR_E_PORT       (-1)   /* could not open / talk to the COM port     */
#define TLSR_E_TIMEOUT    (-2)   /* bridge did not answer in time             */
#define TLSR_E_PROTO      (-3)   /* malformed reply from the bridge           */
#define TLSR_E_BRIDGE     (-4)   /* bridge reported a failure status          */
#define TLSR_E_TARGET     (-5)   /* target chip did not answer / not identified*/
#define TLSR_E_ARG        (-6)   /* bad argument from the caller              */
#define TLSR_E_IO         (-7)   /* file I/O problem                          */
#define TLSR_E_VERIFY     (-8)   /* read-back did not match what was written   */

/* Text for the most recent failure on this thread.  Never NULL. */
const char *tlsr_last_error(void);

/* ------------------------------------------------- bridge command protocol -- */
/* Wire framing, identical in both directions:
 *     0x55 | CMD | LEN_LO | LEN_HI | payload[LEN]
 * The bridge answers with the same shape, CMD replaced by a status byte. */
#define TLSR_BR_SYNC        0x55

#define TLSR_CMD_PING       0x01
#define TLSR_CMD_SYNC       0x02
#define TLSR_CMD_SWS_WRITE  0x03
#define TLSR_CMD_SWS_READ   0x04
#define TLSR_CMD_RESET      0x05
#define TLSR_CMD_SET_SPEED  0x06
#define TLSR_CMD_PWR        0x07
#define TLSR_CMD_SET_CFG    0x08
#define TLSR_CMD_GET_CFG    0x09
#define TLSR_CMD_ACTIVATE   0x0A
#define TLSR_CMD_GET_RAW    0x0B
#define TLSR_CMD_SELFTEST   0x0C
#define TLSR_CMD_PINTEST    0x0D
#define TLSR_CMD_ACT_READ   0x0E
#define TLSR_CMD_FLASH_RD   0x0F
#define TLSR_CMD_FLASH_WR   0x10

#define TLSR_ST_OK          0x00

/* Target power rails.  PB0 drives the 3V3 pad straight from a GPIO; PB12 is
 * the gate of an external P-MOSFET high-side switch and is ACTIVE LOW. */
#define TLSR_RAIL_PB0       0x01
#define TLSR_RAIL_MOSFET    0x02
#define TLSR_RAIL_BOTH      0x03

/* ------------------------------------------------ target register addresses -- */
/* SWire addresses are bare offsets placed on the wire, not CPU-side aliases. */
#define TLSR_REG_MSPI_DATA  0x000Cu   /* flash data shift register            */
#define TLSR_REG_MSPI_CTRL  0x000Du   /* bit0 CS (0=asserted), bit3 RD        */
#define TLSR_REG_CHIP_ID    0x007Eu   /* 2 bytes, 0x5562 on TLSR825x          */
#define TLSR_REG_SWS_SPEED  0x00B2u   /* SWire clock divider                  */
#define TLSR_REG_SWIRE_ID   0x00B3u   /* bit7 = FIFO (same-address burst)     */
#define TLSR_REG_CPU_CTRL   0x0602u   /* run-state control                    */

/* CPU control values.  0x05 is the halt: it genuinely holds the core and is
 * what a working session writes.  0x06 does NOT hold it and breaks MSPI. */
#define TLSR_CPU_HALT       0x05
#define TLSR_CPU_RUN        0x08
#define TLSR_CPU_REBOOT     0x88

/* SWire memory map, all confirmed on hardware (see docs in the repo root). */
#define TLSR_SRAM_BASE      0x040000u   /* also aliased at 0x840000          */
#define TLSR_SRAM_SIZE      (48u * 1024u)
#define TLSR_REG_SPACE      0x000800u   /* register file size                */
#define TLSR_FLASH_SECTOR   4096u
#define TLSR_FLASH_PAGE     256u

/* Activation is a time window, not a frame count: the target's SWire block
 * only answers ~8 ms after power-on, so what matters is that the bridge is
 * still hammering when it opens.  600 frames is ~56 ms, seven times the
 * measured threshold. */
#define TLSR_ACTIVATE_FRAMES 600

/* --------------------------------------------------------------- callbacks -- */
/* Progress during long transfers, and a line of narration for a log window.
 * Both may be NULL. */
typedef void (*tlsr_progress_fn)(void *user, uint32_t done, uint32_t total);
typedef void (*tlsr_log_fn)(void *user, const char *line);

/* ------------------------------------------------------------------ device -- */
typedef struct tlsr_dev tlsr_dev;   /* opaque connection handle */

/* SWire timing parameters, mirroring the firmware's sw_cfg_t.  Defaults are
 * measured from a working capture; they are exposed because the cell timing is
 * the one thing that may need adjusting on a different board. */
typedef struct {
    uint8_t spi_div;      /* 0..7  -> sample clock = 72 MHz / 2^(div+1)  */
    uint8_t cell;         /* sample periods per SWire cell               */
    uint8_t low0, low1;   /* low run encoding a '0' and a '1'            */
    uint8_t thr;          /* decode threshold                            */
    uint8_t addr_bytes;   /* SWire address width (3 on TLSR825x)         */
    uint8_t slave_bits;   /* cells per byte the target sends back        */
    uint8_t slave_off;    /* index of the data MSB within those cells    */
    uint8_t slack;        /* spare cells granted to the reply            */
} tlsr_cfg;

/* What the bridge reports about itself and the attached chip. */
typedef struct {
    char     bridge_id[9];      /* e.g. "TLSRSWS2"                        */
    uint16_t bridge_version;
    uint8_t  rails;             /* TLSR_RAIL_* currently asserted          */

    int      identified;        /* target answered with a plausible id     */
    uint16_t chip_id;           /* 0x5562 on TLSR825x                      */
    const char *chip_name;
    uint8_t  flash_jedec[3];
    const char *flash_vendor;
    uint32_t flash_size;
    uint32_t sram_size;
    uint8_t  swire_div;
    uint8_t  flash_head[16];    /* first 16 bytes of flash                 */
    uint8_t  boot_marker;       /* flash[0]; 0x00 means a bricked image    */
} tlsr_info;

/* ----------------------------------------------------------- serial layer --- */
/* `port` may be "COM10" or "\\\\.\\COM10"; the prefix is added when missing,
 * which matters because plain "COM10" fails for ports above COM9. */
tlsr_dev *tlsr_open(const char *port);
void      tlsr_close(tlsr_dev *d);

/* List COM ports present in the registry.  Returns how many were written. */
int       tlsr_list_ports(char names[][16], int max_names);

/* --------------------------------------------------------- bridge commands -- */
int tlsr_cmd(tlsr_dev *d, uint8_t cmd, const uint8_t *payload, uint16_t len,
             uint8_t *reply, uint16_t reply_max, uint16_t *reply_len,
             unsigned timeout_ms);

int tlsr_ping(tlsr_dev *d, char id[9], uint16_t *version);
int tlsr_get_cfg(tlsr_dev *d, tlsr_cfg *cfg, uint8_t *rails);
int tlsr_set_cfg(tlsr_dev *d, const tlsr_cfg *cfg);
int tlsr_power(tlsr_dev *d, int on, uint8_t rails);
int tlsr_activate(tlsr_dev *d, uint16_t frames);
int tlsr_selftest(tlsr_dev *d, uint8_t echo[8], int *ok);
int tlsr_pintest(tlsr_dev *d, uint8_t levels[3], int *ok);
int tlsr_get_raw(tlsr_dev *d, uint8_t *out, uint32_t max, uint32_t *len);

/* ------------------------------------------------- SWire register / SRAM ---- */
int tlsr_read(tlsr_dev *d, uint32_t addr, uint8_t *out, uint32_t n);
int tlsr_write(tlsr_dev *d, uint32_t addr, const uint8_t *data, uint32_t n);
int tlsr_read8(tlsr_dev *d, uint32_t addr, uint8_t *out);
int tlsr_write8(tlsr_dev *d, uint32_t addr, uint8_t val);

/* ------------------------------------------------------------ core control -- */
int tlsr_halt(tlsr_dev *d);
int tlsr_run(tlsr_dev *d);
int tlsr_reboot(tlsr_dev *d);

/* Bring the target to a usable state: power it if it is off, activate it if it
 * is not answering.  Every operation that touches the chip calls this, so the
 * caller never has to sequence power and activation by hand. */
int tlsr_ensure_target(tlsr_dev *d, tlsr_log_fn log, void *user);

/* ------------------------------------------------------------------ flash --- */
int tlsr_flash_read(tlsr_dev *d, uint32_t addr, uint8_t *out, uint32_t n,
                    tlsr_progress_fn prog, void *user);
int tlsr_flash_erase(tlsr_dev *d, uint32_t addr, uint32_t len,
                     tlsr_progress_fn prog, tlsr_log_fn log, void *user);

/* Optional stages of a program run.  Both are on by default; dropping one is
 * an expert shortcut, so the skipped stage is always announced through `log`
 * rather than passing silently.
 *
 * Without ERASE nothing is erased and the surrounding sector is NOT read and
 * written back either -- only [addr, addr+n) is programmed.  That is faster
 * and leaves neighbours untouched, but NOR flash can only clear bits, so it
 * succeeds solely where the target bytes are already 0xFF (or the write only
 * turns 1s into 0s).  Otherwise the verify stage is what catches it.
 *
 * Without VERIFY nothing is read back, so a bad write is not detected here. */
#define TLSR_PROG_ERASE   0x01u
#define TLSR_PROG_VERIFY  0x02u
#define TLSR_PROG_ALL     (TLSR_PROG_ERASE | TLSR_PROG_VERIFY)

/* Erase the covered sectors, program, then read back and compare.  The parts
 * of each touched sector that fall outside [addr, addr+n) are preserved.
 * Equivalent to tlsr_flash_program_ex() with TLSR_PROG_ALL. */
int tlsr_flash_program(tlsr_dev *d, uint32_t addr, const uint8_t *data,
                       uint32_t n, tlsr_progress_fn prog, tlsr_log_fn log,
                       void *user);
int tlsr_flash_program_ex(tlsr_dev *d, uint32_t addr, const uint8_t *data,
                          uint32_t n, unsigned stages, tlsr_progress_fn prog,
                          tlsr_log_fn log, void *user);
int tlsr_flash_verify(tlsr_dev *d, uint32_t addr, const uint8_t *data,
                      uint32_t n, uint32_t *first_bad, tlsr_progress_fn prog,
                      void *user);
/* Manufacturer / type / capacity bytes, e.g. C8 60 13 = GigaDevice 512 KB. */
int tlsr_flash_jedec(tlsr_dev *d, uint8_t out[3]);

/* ----------------------------------------------------------- identification -- */
int tlsr_identify(tlsr_dev *d, tlsr_info *info, tlsr_log_fn log, void *user);

/* --------------------------------------------------------------- utilities -- */
/* Formats "58 80 07 36 ..." into buf; returns buf. */
char *tlsr_hex(char *buf, size_t buflen, const uint8_t *data, size_t n);
/* Parses "58 80 07" / "5880 07" / "0x58,0x80" into out; returns count or -1. */
int   tlsr_parse_hex(const char *text, uint8_t *out, size_t max);

#endif /* TLSR_CORE_H */
