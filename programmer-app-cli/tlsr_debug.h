/*
 * tlsr_debug.h - CPU debug block on TLSR825x, over SWire.
 *
 * Copyright (C) 2026  telink_project contributors
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * Part of the shared core, so the CLI and the GUI drive the debug block
 * through exactly the same code.
 *
 * READ THIS BEFORE TRUSTING ANY OF IT.  The register map below comes from
 * pvvx's TlsrPgm, which is a working tool for this chip family.  On the
 * TLSR8253 in the outlets it does not answer:
 *
 *   - 0x0680 (the register file) reads back 128 zero bytes,
 *   - 0x06BC (the PC latch) reads zero,
 *   - writing 0x06 to 0x0602 does not stall the core; it reads back 0x08.
 *
 * See debugger/PROTOCOL.md for the bench log.  The block is either absent on
 * this part or gated behind an entry sequence we do not have, so every
 * function here is written to fail visibly rather than to pretend.  Call
 * tlsr_dbg_present() first and tell the user what it found; that is what both
 * front ends do.
 *
 * One consequence worth repeating: SWire reads are only trustworthy with the
 * core stopped.  With it running, bytes come back bit-shifted -- the 0x007E
 * chip id itself does.  Anything read in that state is noise.
 */
#ifndef TLSR_DEBUG_H
#define TLSR_DEBUG_H

#include "tlsr_core.h"

/* ------------------------------------------------------------- registers -- */
#define TLSR_DBG_BLOCK      0x0610u   /* 16 bytes, 4 LE u32; word1 = bkp addr */
#define TLSR_DBG_STEP       0x0613u   /* write 0x80 = execute one instruction */
#define TLSR_DBG_REGS       0x0680u   /* 128 bytes: 32 LE u32                 */
#define TLSR_DBG_PC_LATCH   0x06BCu   /* PC captured on a breakpoint hit      */

#define TLSR_DBG_STEP_ONE   0x80u
#define TLSR_DBG_BKP_ENABLE 0x01000000u  /* bit24 arms the address comparator */

/* Extra values for TLSR_REG_CPU_CTRL, alongside TLSR_CPU_* in tlsr_core.h. */
#define TLSR_CPU_STALL      0x06u     /* debug halt (not honoured on TLSR8253)*/
#define TLSR_CPU_GO_BKP     0x84u     /* resume with the comparator live      */

/* ------------------------------------------------------------ the values -- */
#define TLSR_DBG_NREGS      32
#define TLSR_DBG_IDX_SP     13
#define TLSR_DBG_IDX_LR     14
#define TLSR_DBG_IDX_PC     15
#define TLSR_DBG_IDX_PSR    16
#define TLSR_DBG_IDX_M64    31        /* (mul32 * 32) >> 32                   */
#define TLSR_DBG_NNAMED     17        /* r0..r15 and psr have names           */

typedef struct {
    uint32_t r[TLSR_DBG_NREGS];
    int      valid;                   /* the read succeeded (not: is useful)  */
} tlsr_regs;

/* "r0" ... "r15 (pc)", "psr" for 0..16, "m64" for 31, NULL otherwise. */
const char *tlsr_dbg_reg_name(int index);

/* ------------------------------------------------------------ operations -- */
int tlsr_dbg_read_regs(tlsr_dev *d, tlsr_regs *out);
int tlsr_dbg_stall(tlsr_dev *d);        /* 0x0602 <- 0x06                     */
int tlsr_dbg_go(tlsr_dev *d);           /* 0x0602 <- 0x84, comparator live    */
int tlsr_dbg_step(tlsr_dev *d);         /* one instruction; core must be held */
int tlsr_dbg_set_bp(tlsr_dev *d, uint32_t addr, int arm);

/* Resume, then poll the PC until it equals addr or the timeout expires.
 * Returns TLSR_OK on a hit, TLSR_E_TIMEOUT otherwise -- and on a timeout the
 * core is left running, which is worth saying out loud to the user. */
int tlsr_dbg_go_and_wait(tlsr_dev *d, uint32_t addr, unsigned timeout_ms,
                         tlsr_regs *out, tlsr_log_fn log, void *user);

/* Step one instruction; if that stepped INTO a call, run to the return
 * address instead.  The test is on LR rather than on the instruction, because
 * fetching the instruction would mean driving MSPI, which needs the core in
 * 0x05 -- and that would drop the stall we are debugging in. */
int tlsr_dbg_step_over(tlsr_dev *d, unsigned timeout_ms, tlsr_regs *out,
                       tlsr_log_fn log, void *user);

/* Run to the address in LR, i.e. finish the current function. */
int tlsr_dbg_step_out(tlsr_dev *d, unsigned timeout_ms, tlsr_regs *out,
                      tlsr_log_fn log, void *user);

/* Does this chip actually expose the block?  Stops the core first, because a
 * running core makes every read untrustworthy, then looks for any non-zero
 * byte in the register file or the PC latch.  *present is 0 or 1; the return
 * value only reports whether the probe itself completed. */
int tlsr_dbg_present(tlsr_dev *d, int *present, tlsr_log_fn log, void *user);

#endif /* TLSR_DEBUG_H */
