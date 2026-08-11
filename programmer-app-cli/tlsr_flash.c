/*
 * tlsr_flash.c - SPI-NOR flash and chip identification.
 *
 * Copyright (C) 2026  telink_project contributors
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * The flash is not in the CPU's address space.  It hangs off the target's
 * master-SPI (MSPI) controller, which is itself reached over SWire:
 *
 *      0x000D  MSPI ctrl   bit0 CS (0 = asserted), bit3 RD (shift data in)
 *      0x000C  MSPI data   shift register
 *      0x00B3  swire id    bit7 = FIFO mode, i.e. do not auto-increment
 *
 * The bridge implements the whole transaction in firmware (CMD_FLASH_RD /
 * CMD_FLASH_WR), which is what makes a full 512 KB dump take ~24 s instead of
 * ~95 s: driving MSPI from the host costs about ten USB round trips per block.
 *
 * Three details are easy to get wrong and cost a lot of debugging:
 *   - the RD bit must be set before reading, or the data register returns
 *     0xFE/0x00 filler rather than flash contents;
 *   - the first byte shifted out is stale and must be discarded;
 *   - a read burst must be a single SWire frame.  Splitting it re-runs the
 *     address phase and the target silently drops one byte per boundary.
 * The firmware handles all three; they are documented here because anyone
 * reimplementing this from the host side will hit them.
 *
 * The core must be halted first (0x0602 <- 0x05) or the running firmware
 * fights us for the MSPI controller.
 */
#include <stdio.h>
#include <string.h>

#include "tlsr_core.h"
#include "tlsr_internal.h"

/* ------------------------------------------------------------------- read -- */
int tlsr_flash_read(tlsr_dev *d, uint32_t addr, uint8_t *out, uint32_t n,
                    tlsr_progress_fn prog, void *user)
{
    uint32_t done = 0;
    int rc;

    rc = tlsr_halt(d);
    if (rc != TLSR_OK)
        return rc;

    while (done < n) {
        uint8_t p[5];
        uint16_t got = 0;
        uint32_t want = n - done;
        uint32_t a = addr + done;

        if (want > TLSR_FLASH_BLOCK)
            want = TLSR_FLASH_BLOCK;

        p[0] = (uint8_t)(a >> 16);
        p[1] = (uint8_t)(a >> 8);
        p[2] = (uint8_t)a;
        p[3] = (uint8_t)(want & 0xFF);
        p[4] = (uint8_t)(want >> 8);

        rc = tlsr_cmd(d, TLSR_CMD_FLASH_RD, p, 5, out + done,
                      (uint16_t)want, &got, 20000);
        if (rc != TLSR_OK)
            return rc;
        if (got != want) {
            tlsr_set_error("short flash read at 0x%06X: wanted %u, got %u",
                           (unsigned)a, (unsigned)want, got);
            return TLSR_E_PROTO;
        }
        done += got;
        TLSR_PROG(prog, user, done, n);
    }
    return TLSR_OK;
}

/* ------------------------------------------------- raw MSPI helpers -------- */
/* The bridge implements only the two hot paths in firmware (block read, page
 * program).  Erase and the JEDEC id are rare, so they are driven from the host
 * with the generic SWire register commands -- a handful of round trips each,
 * which is irrelevant next to the 300 ms the flash itself needs to erase. */

static int mspi_cs(tlsr_dev *d, int assert_low)
{
    /* bit0 low asserts chip select */
    return tlsr_write8(d, TLSR_REG_MSPI_CTRL, assert_low ? 0x00 : 0x01);
}

static int mspi_fifo(tlsr_dev *d, int on)
{
    return tlsr_write8(d, TLSR_REG_SWIRE_ID, on ? 0x80 : 0x00);
}

/* Push bytes into the data register.  More than one byte has to go to the
 * *same* address, so FIFO mode must be on or the target would auto-increment
 * into the neighbouring registers. */
static int mspi_put(tlsr_dev *d, const uint8_t *bytes, uint32_t n)
{
    int rc;

    if (n == 1)
        return tlsr_write8(d, TLSR_REG_MSPI_DATA, bytes[0]);

    rc = mspi_fifo(d, 1);
    if (rc != TLSR_OK)
        return rc;
    rc = tlsr_write(d, TLSR_REG_MSPI_DATA, bytes, n);
    mspi_fifo(d, 0);
    return rc;
}

/* Shift `n` bytes out of the flash.  The RD bit must be set first, and the
 * first byte to appear is stale, so we always read one extra and drop it. */
static int mspi_get(tlsr_dev *d, uint8_t *out, uint32_t n)
{
    uint8_t buf[16];
    int rc;

    if (n + 1 > sizeof buf)
        return TLSR_E_ARG;
    rc = tlsr_write8(d, TLSR_REG_MSPI_CTRL, 0x08);   /* CS still low, RD set */
    if (rc != TLSR_OK)
        return rc;
    rc = mspi_fifo(d, 1);
    if (rc != TLSR_OK)
        return rc;
    rc = tlsr_read(d, TLSR_REG_MSPI_DATA, buf, n + 1);
    mspi_fifo(d, 0);
    if (rc != TLSR_OK)
        return rc;
    memcpy(out, buf + 1, n);
    return TLSR_OK;
}

/* Read the status register; bit0 (WIP) is set while an erase or program runs. */
static int mspi_status(tlsr_dev *d, uint8_t *status)
{
    uint8_t op = 0x05;
    int rc;

    rc = mspi_cs(d, 1);
    if (rc == TLSR_OK) rc = mspi_put(d, &op, 1);
    if (rc == TLSR_OK) rc = mspi_get(d, status, 1);
    mspi_cs(d, 0);
    return rc;
}

static int mspi_wait_ready(tlsr_dev *d, unsigned timeout_ms)
{
    DWORD deadline = GetTickCount() + timeout_ms;
    for (;;) {
        uint8_t st = 0xFF;
        int rc = mspi_status(d, &st);
        if (rc != TLSR_OK)
            return rc;
        if (!(st & 0x01))
            return TLSR_OK;
        if (GetTickCount() > deadline) {
            tlsr_set_error("flash stayed busy for %u ms (status 0x%02X)",
                           timeout_ms, st);
            return TLSR_E_TIMEOUT;
        }
        Sleep(2);
    }
}

static int mspi_write_enable(tlsr_dev *d)
{
    uint8_t op = 0x06;
    int rc = mspi_cs(d, 1);
    if (rc == TLSR_OK) rc = mspi_put(d, &op, 1);
    mspi_cs(d, 0);
    return rc;
}

int tlsr_flash_jedec(tlsr_dev *d, uint8_t out[3])
{
    uint8_t op = 0x9F;
    int rc;

    rc = tlsr_halt(d);
    if (rc != TLSR_OK)
        return rc;
    rc = mspi_cs(d, 1);
    if (rc == TLSR_OK) rc = mspi_put(d, &op, 1);
    if (rc == TLSR_OK) rc = mspi_get(d, out, 3);
    mspi_cs(d, 0);
    return rc;
}

/* ------------------------------------------------------------------ erase -- */
int tlsr_flash_erase(tlsr_dev *d, uint32_t addr, uint32_t len,
                     tlsr_progress_fn prog, tlsr_log_fn log, void *user)
{
    uint32_t done = 0, sectors;
    int rc;

    if (addr % TLSR_FLASH_SECTOR) {
        tlsr_set_error("0x%06X is not on a %u-byte sector boundary",
                       (unsigned)addr, (unsigned)TLSR_FLASH_SECTOR);
        return TLSR_E_ARG;
    }
    if (len == 0)
        len = TLSR_FLASH_SECTOR;

    sectors = (len + TLSR_FLASH_SECTOR - 1) / TLSR_FLASH_SECTOR;
    TLSR_LOG(log, user, "erasing %u sector(s) from 0x%06X",
             (unsigned)sectors, (unsigned)addr);

    rc = tlsr_halt(d);
    if (rc != TLSR_OK)
        return rc;

    while (done < len) {
        uint32_t a = addr + done;
        uint8_t cmd[4];

        cmd[0] = 0x20;                       /* 4 KB sector erase */
        cmd[1] = (uint8_t)(a >> 16);
        cmd[2] = (uint8_t)(a >> 8);
        cmd[3] = (uint8_t)a;

        rc = mspi_write_enable(d);
        if (rc == TLSR_OK) rc = mspi_cs(d, 1);
        if (rc == TLSR_OK) rc = mspi_put(d, cmd, 4);
        if (rc == TLSR_OK) rc = mspi_cs(d, 0);   /* releasing CS starts it */
        if (rc == TLSR_OK) rc = mspi_wait_ready(d, 5000);
        if (rc != TLSR_OK)
            return rc;

        done += TLSR_FLASH_SECTOR;
        TLSR_PROG(prog, user, done < len ? done : len, len);
    }
    return TLSR_OK;
}

/* ---------------------------------------------------------------- program -- */
/* One page at a time; the caller's range may start and end anywhere. */
static int program_pages(tlsr_dev *d, uint32_t addr, const uint8_t *data,
                         uint32_t n, tlsr_progress_fn prog, void *user)
{
    uint32_t done = 0;

    while (done < n) {
        uint8_t p[3 + TLSR_FLASH_PAGE];
        uint32_t a = addr + done;
        uint32_t room = TLSR_FLASH_PAGE - (a & (TLSR_FLASH_PAGE - 1));
        uint32_t chunk = n - done;
        int rc;

        if (chunk > room)
            chunk = room;                /* never cross a page boundary */

        p[0] = (uint8_t)(a >> 16);
        p[1] = (uint8_t)(a >> 8);
        p[2] = (uint8_t)a;
        memcpy(p + 3, data + done, chunk);

        rc = tlsr_cmd(d, TLSR_CMD_FLASH_WR, p, (uint16_t)(chunk + 3),
                      NULL, 0, NULL, 20000);
        if (rc != TLSR_OK)
            return rc;
        done += chunk;
        TLSR_PROG(prog, user, done, n);
    }
    return TLSR_OK;
}

int tlsr_flash_program(tlsr_dev *d, uint32_t addr, const uint8_t *data,
                       uint32_t n, tlsr_progress_fn prog, tlsr_log_fn log,
                       void *user)
{
    return tlsr_flash_program_ex(d, addr, data, n, TLSR_PROG_ALL,
                                 prog, log, user);
}

int tlsr_flash_program_ex(tlsr_dev *d, uint32_t addr, const uint8_t *data,
                          uint32_t n, unsigned stages, tlsr_progress_fn prog,
                          tlsr_log_fn log, void *user)
{
    uint32_t base, span, head_len, tail_start, tail_len;
    uint8_t *buf = NULL;
    int step = 0, steps;
    int rc;

    if (!n)
        return TLSR_OK;

    /* Numbering the steps out loud only helps if the count matches what will
     * actually run, so derive it from the requested stages. */
    steps = 1 + ((stages & TLSR_PROG_ERASE) ? 2 : 0)
              + ((stages & TLSR_PROG_VERIFY) ? 1 : 0);

    rc = tlsr_halt(d);
    if (rc != TLSR_OK)
        return rc;

    if (!(stages & TLSR_PROG_ERASE)) {
        /* No erase means no sector is destroyed, so there is nothing to
         * preserve: program exactly what the caller asked for and nothing
         * else.  Bits can only go 1 -> 0 from here. */
        TLSR_LOG(log, user, "erase skipped - programming into flash as it "
                 "stands, which can only clear bits");
        TLSR_LOG(log, user, "step %d/%d: programming %u bytes at 0x%06X",
                 ++step, steps, (unsigned)n, (unsigned)addr);
        rc = program_pages(d, addr, data, n, prog, user);
        if (rc != TLSR_OK)
            return rc;
    } else {
        /* Flash erases in whole sectors, so anything already living in the
         * sectors we touch has to be read out and written back or it is
         * destroyed.  On these outlets that neighbouring data is the pairing
         * and calibration records, which must survive a repair. */
        base       = addr - (addr % TLSR_FLASH_SECTOR);
        span       = ((n + (addr - base) + TLSR_FLASH_SECTOR - 1)
                      / TLSR_FLASH_SECTOR) * TLSR_FLASH_SECTOR;
        head_len   = addr - base;
        tail_start = addr + n;
        tail_len   = (base + span) - tail_start;

        buf = (uint8_t *)malloc(span);
        if (!buf) {
            tlsr_set_error("out of memory for a %u byte sector buffer",
                           (unsigned)span);
            return TLSR_E_ARG;
        }

        if (head_len || tail_len) {
            TLSR_LOG(log, user, "step %d/%d: preserving %u bytes of "
                     "surrounding sector data", ++step, steps,
                     (unsigned)(head_len + tail_len));
            if (head_len) {
                rc = tlsr_flash_read(d, base, buf, head_len, NULL, NULL);
                if (rc != TLSR_OK)
                    goto out;
            }
            if (tail_len) {
                rc = tlsr_flash_read(d, tail_start, buf + head_len + n,
                                     tail_len, NULL, NULL);
                if (rc != TLSR_OK)
                    goto out;
            }
        } else {
            step++;                      /* whole sectors: nothing to preserve */
        }
        memcpy(buf + head_len, data, n);

        TLSR_LOG(log, user, "step %d/%d: erasing %u sector(s) at 0x%06X",
                 ++step, steps, (unsigned)(span / TLSR_FLASH_SECTOR),
                 (unsigned)base);
        rc = tlsr_flash_erase(d, base, span, NULL, NULL, NULL);
        if (rc != TLSR_OK)
            goto out;

        TLSR_LOG(log, user, "step %d/%d: programming %u bytes at 0x%06X",
                 ++step, steps, (unsigned)span, (unsigned)base);
        rc = program_pages(d, base, buf, span, prog, user);
        if (rc != TLSR_OK)
            goto out;
    }

    if (stages & TLSR_PROG_VERIFY) {
        TLSR_LOG(log, user, "step %d/%d: verifying %u bytes at 0x%06X",
                 ++step, steps, (unsigned)n, (unsigned)addr);
        rc = tlsr_flash_verify(d, addr, data, n, NULL, NULL, NULL);
    } else {
        TLSR_LOG(log, user, "verify skipped - the write was not read back");
        rc = TLSR_OK;
    }

out:
    free(buf);
    return rc;
}

/* ----------------------------------------------------------------- verify -- */
int tlsr_flash_verify(tlsr_dev *d, uint32_t addr, const uint8_t *data,
                      uint32_t n, uint32_t *first_bad, tlsr_progress_fn prog,
                      void *user)
{
    uint8_t *back = (uint8_t *)malloc(n ? n : 1);
    uint32_t i;
    int rc;

    if (!back) {
        tlsr_set_error("out of memory for a %u byte verify buffer", (unsigned)n);
        return TLSR_E_ARG;
    }
    rc = tlsr_flash_read(d, addr, back, n, prog, user);
    if (rc != TLSR_OK) {
        free(back);
        return rc;
    }
    for (i = 0; i < n; i++) {
        if (back[i] != data[i]) {
            if (first_bad)
                *first_bad = addr + i;
            tlsr_set_error("verify failed at 0x%06X: flash has 0x%02X, "
                           "file has 0x%02X", (unsigned)(addr + i),
                           back[i], data[i]);
            free(back);
            return TLSR_E_VERIFY;
        }
    }
    free(back);
    return TLSR_OK;
}

/* --------------------------------------------------------- identification -- */
static const char *chip_name(uint16_t id)
{
    switch (id) {
    case 0x5562: return "TLSR825x (TLSR8251 / 8253 / 8258)";
    case 0x5325: return "TLSR8253";
    case 0x5326: return "TLSR8258";
    default:     return "unrecognised";
    }
}

static const char *flash_vendor(uint8_t mfr)
{
    switch (mfr) {
    case 0xC8: return "GigaDevice";
    case 0xEF: return "Winbond";
    case 0x5E: return "Zbit";
    case 0x85: return "Puya";
    case 0x1C: return "Eon";
    case 0xA1: return "Fudan";
    case 0x0B: return "XTX";
    case 0x68: return "Boya";
    default:   return "unknown";
    }
}

int tlsr_identify(tlsr_dev *d, tlsr_info *info, tlsr_log_fn log, void *user)
{
    uint8_t id[2], jedec[3] = { 0, 0, 0 };
    uint16_t ver = 0;
    uint8_t rails = 0;
    int rc;

    memset(info, 0, sizeof *info);
    info->chip_name    = "no answer";
    info->flash_vendor = "";
    info->sram_size    = TLSR_SRAM_SIZE;

    rc = tlsr_ping(d, info->bridge_id, &ver);
    if (rc != TLSR_OK)
        return rc;
    info->bridge_version = ver;

    rc = tlsr_ensure_target(d, log, user);
    if (rc != TLSR_OK) {
        tlsr_get_cfg(d, NULL, &rails);
        info->rails = rails;
        return rc;
    }
    tlsr_get_cfg(d, NULL, &rails);
    info->rails = rails;

    rc = tlsr_read(d, TLSR_REG_CHIP_ID, id, 2);
    if (rc != TLSR_OK)
        return rc;
    info->chip_id    = (uint16_t)(id[0] | (id[1] << 8));
    info->identified = (info->chip_id != 0x0000 && info->chip_id != 0xFFFF);
    info->chip_name  = info->identified ? chip_name(info->chip_id) : "no answer";
    if (!info->identified) {
        tlsr_set_error("target did not identify (chip id 0x%04X)", info->chip_id);
        return TLSR_E_TARGET;
    }

    tlsr_read8(d, TLSR_REG_SWS_SPEED, &info->swire_div);

    /* The JEDEC id comes back through the same MSPI path as a normal read; the
     * firmware handles the opcode and the stale first byte. */
    rc = tlsr_halt(d);
    if (rc != TLSR_OK)
        return rc;

    rc = tlsr_flash_read(d, 0, info->flash_head, sizeof info->flash_head,
                         NULL, NULL);
    if (rc != TLSR_OK)
        return rc;
    info->boot_marker = info->flash_head[0];

    /* Size is taken from the JEDEC capacity byte when it looks sane, otherwise
     * we fall back to the 512 KB these outlets ship with. */
    if (tlsr_flash_jedec(d, jedec) == TLSR_OK) {
        memcpy(info->flash_jedec, jedec, 3);
        info->flash_vendor = flash_vendor(jedec[0]);
        info->flash_size   = (jedec[2] >= 0x10 && jedec[2] <= 0x20)
                             ? (1u << jedec[2]) : (512u * 1024u);
    } else {
        info->flash_vendor = "unknown";
        info->flash_size   = 512u * 1024u;
    }
    return TLSR_OK;
}
