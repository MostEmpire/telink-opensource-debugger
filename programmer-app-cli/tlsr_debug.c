/*
 * tlsr_debug.c - CPU debug block on TLSR825x, over SWire.
 *
 * Copyright (C) 2026  telink_project contributors
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * See tlsr_debug.h for what this hardware does and does not do.
 */
#include <stdio.h>      /* TLSR_LOG() formats through snprintf */
#include <string.h>

#include "tlsr_internal.h"
#include "tlsr_debug.h"

static const char *const g_names[TLSR_DBG_NNAMED] = {
    "r0", "r1", "r2", "r3", "r4", "r5", "r6", "r7",
    "r8", "r9", "r10", "r11 (fp)", "r12 (ip)", "r13 (sp)", "r14 (lr)",
    "r15 (pc)", "psr"
};

const char *tlsr_dbg_reg_name(int index)
{
    if (index >= 0 && index < TLSR_DBG_NNAMED)
        return g_names[index];
    if (index == TLSR_DBG_IDX_M64)
        return "m64";
    return NULL;
}

int tlsr_dbg_read_regs(tlsr_dev *d, tlsr_regs *out)
{
    uint8_t raw[TLSR_DBG_NREGS * 4];
    int rc, i;

    if (!d || !out)
        return TLSR_E_ARG;
    memset(out, 0, sizeof *out);

    rc = tlsr_read(d, TLSR_DBG_REGS, raw, sizeof raw);
    if (rc != TLSR_OK)
        return rc;

    for (i = 0; i < TLSR_DBG_NREGS; i++)
        out->r[i] = (uint32_t)raw[i * 4]             |
                    ((uint32_t)raw[i * 4 + 1] <<  8) |
                    ((uint32_t)raw[i * 4 + 2] << 16) |
                    ((uint32_t)raw[i * 4 + 3] << 24);
    out->valid = 1;
    return TLSR_OK;
}

int tlsr_dbg_stall(tlsr_dev *d)
{
    return tlsr_write8(d, TLSR_REG_CPU_CTRL, TLSR_CPU_STALL);
}

int tlsr_dbg_go(tlsr_dev *d)
{
    return tlsr_write8(d, TLSR_REG_CPU_CTRL, TLSR_CPU_GO_BKP);
}

int tlsr_dbg_step(tlsr_dev *d)
{
    return tlsr_write8(d, TLSR_DBG_STEP, TLSR_DBG_STEP_ONE);
}

/* Word 0 of the block is left zero; word 1 is the armed address.  Words 2 and
 * 3 are unidentified, so they are written as zero rather than guessed at. */
int tlsr_dbg_set_bp(tlsr_dev *d, uint32_t addr, int arm)
{
    uint8_t blk[16];
    uint32_t w1 = arm ? ((addr & 0x00FFFFFFu) | TLSR_DBG_BKP_ENABLE) : 0u;

    memset(blk, 0, sizeof blk);
    blk[4] = (uint8_t)w1;
    blk[5] = (uint8_t)(w1 >> 8);
    blk[6] = (uint8_t)(w1 >> 16);
    blk[7] = (uint8_t)(w1 >> 24);
    return tlsr_write(d, TLSR_DBG_BLOCK, blk, sizeof blk);
}

int tlsr_dbg_go_and_wait(tlsr_dev *d, uint32_t addr, unsigned timeout_ms,
                         tlsr_regs *out, tlsr_log_fn log, void *user)
{
    tlsr_regs regs;
    unsigned waited = 0;
    int rc;

    if (!d)
        return TLSR_E_ARG;
    if (!out)
        out = &regs;

    rc = tlsr_dbg_go(d);
    if (rc != TLSR_OK)
        return rc;

    while (waited < timeout_ms) {
        Sleep(50);
        waited += 50;
        /* A failed read here is expected while the core runs, so it is not an
         * error -- keep polling until the window closes. */
        if (tlsr_dbg_read_regs(d, out) != TLSR_OK)
            continue;
        if ((out->r[TLSR_DBG_IDX_PC] & 0x00FFFFFFu) == (addr & 0x00FFFFFFu)) {
            TLSR_LOG(log, user, "breakpoint hit at 0x%06X",
                     (unsigned)(addr & 0xFFFFFF));
            return TLSR_OK;
        }
    }

    TLSR_LOG(log, user,
             "0x%06X not reached in %u ms - the core is still running",
             (unsigned)(addr & 0xFFFFFF), timeout_ms);
    tlsr_set_error("breakpoint 0x%06X was not reached within %u ms",
                   (unsigned)(addr & 0xFFFFFF), timeout_ms);
    return TLSR_E_TIMEOUT;
}

int tlsr_dbg_step_over(tlsr_dev *d, unsigned timeout_ms, tlsr_regs *out,
                       tlsr_log_fn log, void *user)
{
    tlsr_regs regs;
    uint32_t pc0, lr;
    int rc;

    if (!d)
        return TLSR_E_ARG;
    if (!out)
        out = &regs;

    rc = tlsr_dbg_stall(d);
    if (rc != TLSR_OK)
        return rc;
    rc = tlsr_dbg_read_regs(d, out);
    if (rc != TLSR_OK)
        return rc;
    pc0 = out->r[TLSR_DBG_IDX_PC];

    rc = tlsr_dbg_step(d);
    if (rc != TLSR_OK)
        return rc;
    rc = tlsr_dbg_read_regs(d, out);
    if (rc != TLSR_OK)
        return rc;
    lr = out->r[TLSR_DBG_IDX_LR];

    /* TC32 `tjl` is 4 bytes and the 16-bit call forms are 2, so a link
     * register pointing just past where we were means we entered a call. */
    if ((lr & ~1u) == ((pc0 + 4) & ~1u) || (lr & ~1u) == ((pc0 + 2) & ~1u)) {
        TLSR_LOG(log, user, "stepped into a call - running to 0x%06X",
                 (unsigned)(lr & 0xFFFFFF));
        rc = tlsr_dbg_set_bp(d, lr, 1);
        if (rc != TLSR_OK)
            return rc;
        return tlsr_dbg_go_and_wait(d, lr, timeout_ms, out, log, user);
    }

    TLSR_LOG(log, user, "stepped over, PC=0x%06X",
             (unsigned)(out->r[TLSR_DBG_IDX_PC] & 0xFFFFFF));
    return TLSR_OK;
}

int tlsr_dbg_step_out(tlsr_dev *d, unsigned timeout_ms, tlsr_regs *out,
                      tlsr_log_fn log, void *user)
{
    tlsr_regs regs;
    uint32_t lr;
    int rc;

    if (!d)
        return TLSR_E_ARG;
    if (!out)
        out = &regs;

    rc = tlsr_dbg_stall(d);
    if (rc != TLSR_OK)
        return rc;
    rc = tlsr_dbg_read_regs(d, out);
    if (rc != TLSR_OK)
        return rc;

    lr = out->r[TLSR_DBG_IDX_LR];
    if (!lr) {
        tlsr_set_error("LR is zero - there is no caller to return to");
        return TLSR_E_TARGET;
    }

    TLSR_LOG(log, user, "running to caller at 0x%06X",
             (unsigned)(lr & 0xFFFFFF));
    rc = tlsr_dbg_set_bp(d, lr, 1);
    if (rc != TLSR_OK)
        return rc;
    return tlsr_dbg_go_and_wait(d, lr, timeout_ms, out, log, user);
}

int tlsr_dbg_present(tlsr_dev *d, int *present, tlsr_log_fn log, void *user)
{
    tlsr_regs regs;
    uint8_t latch[4];
    int rc, i, any = 0;

    if (!d || !present)
        return TLSR_E_ARG;
    *present = 0;

    /* Reads are only trustworthy with the core held, and 0x05 is the halt that
     * this chip actually honours.  Without this the whole probe measures the
     * bit-slip, not the chip. */
    rc = tlsr_halt(d);
    if (rc != TLSR_OK)
        return rc;

    rc = tlsr_dbg_read_regs(d, &regs);
    if (rc != TLSR_OK)
        return rc;
    /* Only r0..psr count.  The tail of the 128-byte block (the banked set and
     * the multiplier word) drifts even with the core held -- measured on
     * 2026-08-09 -- so scanning all 32 words reports noise as a working debug
     * block, which would switch on the very buttons that cannot work. */
    for (i = 0; i <= TLSR_DBG_IDX_PSR && !any; i++)
        if (regs.r[i])
            any = 1;

    if (!any && tlsr_read(d, TLSR_DBG_PC_LATCH, latch, sizeof latch) == TLSR_OK)
        for (i = 0; i < 4 && !any; i++)
            if (latch[i])
                any = 1;

    *present = any;
    if (any)
        TLSR_LOG(log, user, "CPU debug block answers - PC 0x%06X",
                 (unsigned)(regs.r[TLSR_DBG_IDX_PC] & 0xFFFFFF));
    else
        TLSR_LOG(log, user, "no CPU debug block: 0x0680 and 0x06BC read zero "
                            "with the core held");
    return TLSR_OK;
}
