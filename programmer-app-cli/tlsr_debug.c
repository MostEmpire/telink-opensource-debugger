/*
 * tlsr_debug.c - CPU debug block on TLSR825x, over SWire.
 *
 * Copyright (C) 2026  telink_project contributors
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * See tlsr_debug.h for what this hardware does, what it does not, and which
 * probe measured each claim.
 */
#include <stdio.h>      /* TLSR_LOG() formats through snprintf */
#include <string.h>

#include "tlsr_internal.h"
#include "tlsr_debug.h"
#include "tlsr_dis.h"

/* How long to wait for the halted flag after asking the core to stop, and how
 * often to look.  A comparator hit on hot code arrives in tens of milliseconds;
 * the poll interval is what decides how responsive a button feels. */
#define HALT_POLL_MS      10
#define PAUSE_WAIT_MS     1500

/* How long the core is given to reach the address a pause armed.  Two tries,
 * because a stall that drifted may have reported an address the core had
 * already left. */
#define PAUSE_TRIES       5

static int slot_write(tlsr_dev *d, int slot, uint32_t addr, int arm);
static int slot_read(tlsr_dev *d, int slot, uint32_t *addr);
static int anchor_clear(tlsr_dev *d);
static int pc_is_moving(tlsr_dev *d);
static int stuck_error(tlsr_dev *d, const char *what);

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

const char *tlsr_dbg_state_name(tlsr_dbg_state s)
{
    switch (s) {
    case TLSR_DBG_NO_TARGET:     return "no target";
    case TLSR_DBG_POWER_OFF:     return "power off";
    case TLSR_DBG_ON_BOOT:       return "running (detached)";
    case TLSR_DBG_BOOT_HELD:     return "caught at boot";
    case TLSR_DBG_RUNNING:       return "running";
    case TLSR_DBG_HELD:          return "held";
    case TLSR_DBG_AT_BREAKPOINT: return "at breakpoint";
    }
    return "?";
}

/* ------------------------------------------------------------- registers -- */
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
    /* The file tracks the core through steps and comparator stops alike, so
     * this is normally true; it stays as a flag because a core that has never
     * stopped since power-up reads zeros. */
    out->full = (out->r[TLSR_DBG_IDX_SP] != 0 || out->r[TLSR_DBG_IDX_PSR] != 0);
    return TLSR_OK;
}

uint32_t tlsr_dbg_read_pc(tlsr_dev *d)
{
    uint8_t b[4];
    if (!d || tlsr_read(d, TLSR_DBG_PC, b, sizeof b) != TLSR_OK)
        return 0xFFFFFFFFu;
    return (uint32_t)b[0] | ((uint32_t)b[1] << 8) |
           ((uint32_t)b[2] << 16) | ((uint32_t)b[3] << 24);
}

int tlsr_dbg_is_halted(tlsr_dev *d)
{
    uint8_t v = 0;
    if (!d || tlsr_read8(d, TLSR_DBG_STEP, &v) != TLSR_OK)
        return -1;
    return (v & TLSR_DBG_HELD_BIT) ? 1 : 0;
}

/* The PC, but only once it has stopped moving.  A single read of a running core
 * is a random sample; requiring consecutive equal reads is something running
 * code will not produce and link noise cannot fake.  Failed reads come back as
 * 0xFFFFFFFF and are rejected outright -- two failures in a row "agree", which
 * is how a dead link once got accepted as a valid single-step result. */
static int pc_settled(tlsr_dev *d, uint32_t *val)
{
    uint32_t a, b;
    int i;

    for (i = 0; i < 3; i++) {
        a = tlsr_dbg_read_pc(d);
        if (a == 0xFFFFFFFFu) { Sleep(10); continue; }
        b = tlsr_dbg_read_pc(d);
        if (b == 0xFFFFFFFFu) { Sleep(10); continue; }
        if (a == b) { *val = a & 0x00FFFFFFu; return 1; }
        *val = b & 0x00FFFFFFu;
        return 0;                       /* genuinely moving */
    }
    *val = 0xFFFFFFFFu;
    return 0;                           /* could not read it at all */
}

/* ---------------------------------------------------------- core control -- */
int tlsr_dbg_halt(tlsr_dev *d)
{
    return tlsr_write8(d, TLSR_REG_CPU_CTRL, TLSR_CPU_STALL);
}

/*
 * Step and resume both follow Telink's own sequence, read out of the BDT 2.2.1
 * binary.  The key point is that 0x0613's go bit does nothing on its own:
 * 0x0602 has to be written first, and both writes are read-modify-write so the
 * other bits in those registers survive -- which matters more than it looks,
 * because bit0 of 0x0613 is the first comparator's enable.
 */
int tlsr_dbg_step_insn(tlsr_dev *d)
{
    uint8_t a = 0, b = 0;
    uint32_t pc;
    unsigned lifted = 0;
    int i, rc;

    if (!d)
        return TLSR_E_ARG;
    if ((rc = tlsr_read8(d, TLSR_REG_CPU_CTRL, &a)) != TLSR_OK)
        return rc;

    /* Assert the hold before anything else. It is what keeps the core in place
     * while the comparators below are lifted -- clearing the one that is
     * holding a stopped core would otherwise let it go. */
    rc = tlsr_write8(d, TLSR_REG_CPU_CTRL, (uint8_t)(a | TLSR_DBG_HOLD_BIT));
    if (rc != TLSR_OK)
        return rc;

    /*
     * Lift any comparator sitting on the address we are stepping FROM: the
     * ordinary disable/step/re-enable every hardware debugger does.
     *
     * It is not what makes stepping work here -- the core steps perfectly well
     * with a comparator armed at the address it is on, measured directly. It
     * matters for the paths that let the core RUN during a step: the interrupt
     * recovery resumes to a comparator on LR, and a comparator still armed
     * behind us would catch the core on the way and be mistaken for the
     * handler failing to return.
     */
    pc = tlsr_dbg_read_pc(d) & 0x00FFFFFFu;
    if (pc != 0x00FFFFFFu) {
        /*
         * From the software mirror, not by reading the block back. Four extra
         * SWire reads per step is not a rounding error here: every millisecond
         * the core is held is a millisecond of interrupt latency this firmware's
         * radio does not have, and holding it long enough is what eventually
         * locks it up. tlsr_dbg_bp_sync() and bp_reconcile() keep the mirror
         * honest, including across the CLI's process-per-command boundary.
         */
        if (d->dbg_anchor_on && d->dbg_anchor == pc) {
            slot_write(d, TLSR_DBG_ANCHOR, 0, 0);
            lifted |= 1u << TLSR_DBG_ANCHOR;
        }
        for (i = 0; i < TLSR_DBG_NBP; i++)
            if (d->dbg_bp_used[i] && d->dbg_bp[i] == pc) {
                slot_write(d, i, 0, 0);
                lifted |= 1u << i;
            }
    }

    /* Re-read 0x0613 after the lifting: clearing slot 0 rewrites this byte. */
    if ((rc = tlsr_read8(d, TLSR_DBG_STEP, &b)) != TLSR_OK)
        b = TLSR_DBG_HELD_BIT;
    rc = tlsr_write8(d, TLSR_DBG_STEP, (uint8_t)(b | TLSR_DBG_GO_BIT));

    for (i = 0; i < TLSR_DBG_NSLOTS; i++) {
        if (!(lifted & (1u << i)))
            continue;
        slot_write(d, i, pc, 1);
    }
    return rc;
}

int tlsr_dbg_resume(tlsr_dev *d)
{
    uint8_t a = 0, b = 0;
    int rc;

    if (!d)
        return TLSR_E_ARG;

    /*
     * Drop the anchor first. It is the comparator that is holding a stopped
     * core, so leaving it armed means the core runs one lap and comes straight
     * back to where it was -- which reads as a core that has not moved at all,
     * and looks exactly like the firmware having locked up. That mis-reading
     * cost a whole round of "the target is wedged" investigation.
     */
    anchor_clear(d);

    if (tlsr_read8(d, TLSR_REG_CPU_CTRL, &a) != TLSR_OK ||
        tlsr_read8(d, TLSR_DBG_STEP, &b) != TLSR_OK)
        return tlsr_write8(d, TLSR_REG_CPU_CTRL, TLSR_CPU_RUN);
    rc = tlsr_write8(d, TLSR_REG_CPU_CTRL,
                     (uint8_t)(a & (uint8_t)~TLSR_DBG_HOLD_BIT));
    if (rc != TLSR_OK)
        return rc;
    return tlsr_write8(d, TLSR_DBG_STEP, (uint8_t)(b | TLSR_DBG_GO_BIT));
}

int tlsr_dbg_go(tlsr_dev *d)
{
    return tlsr_write8(d, TLSR_REG_CPU_CTRL, TLSR_CPU_GO_BKP);
}

/* Wait for the halted flag.  Returns 1 with *at set to the settled PC, 0 on
 * timeout.  This is the one place that decides "has it stopped", and it uses
 * the flag rather than PC sampling: one byte instead of ten reads spread over a
 * quarter of a second. */
static int wait_halted(tlsr_dev *d, unsigned timeout_ms, uint32_t *at)
{
    unsigned waited = 0;

    while (waited <= timeout_ms) {
        if (tlsr_dbg_is_halted(d) == 1) {
            uint32_t pc = 0;
            if (pc_settled(d, &pc)) {
                if (at) *at = pc;
                return 1;
            }
        }
        Sleep(HALT_POLL_MS);
        waited += HALT_POLL_MS;
    }
    if (at) pc_settled(d, at);
    return 0;
}

/* ------------------------------------------------------------ code reads -- */
uint32_t tlsr_dbg_read_code(tlsr_dev *d, uint32_t addr, uint8_t *out, uint32_t n)
{
    uint32_t got = 0;

    if (!d || !out || !n)
        return 0;

    while (got < n) {
        uint32_t want = n - got;
        int rc;
        if (want > TLSR_DBG_CODE_CHUNK)
            want = TLSR_DBG_CODE_CHUNK;
        if (addr + got >= 0x800000u) {
            /* SRAM, which SWire also aliases low. */
            rc = tlsr_read(d, (addr + got) - 0x800000u, out + got, want);
        } else {
            /* Flash, without the 0x05 stop: that would end the session, and
             * the debug hold already keeps the CPU off the MSPI bus. */
            rc = tlsr_flash_read_nohalt(d, addr + got, out + got, want, NULL, NULL);
        }
        if (rc != TLSR_OK)
            break;
        got += want;
    }
    return got;
}

/* Fill buf with the bytes at addr, preferring a host-side image when one covers
 * the address.  Returns how many bytes are usable. */
static uint32_t fetch_insn_bytes(tlsr_dev *d, uint32_t addr, uint8_t *buf,
                                 uint32_t n)
{
    if (d->code && d->code_len && addr >= d->code_base &&
        addr - d->code_base + n <= d->code_len) {
        memcpy(buf, d->code + (addr - d->code_base), n);
        return n;
    }
    return tlsr_dbg_read_code(d, addr, buf, n);
}

/* ------------------------------------------------------------ comparators -- */
/* One slot, written on its own.  Writing the whole 16-byte block would also
 * rewrite 0x0613, which carries the go pulse and slot 0's enable -- that is
 * what made an earlier survey conclude there was only one comparator. */
static int slot_write(tlsr_dev *d, int slot, uint32_t addr, int arm)
{
    uint32_t v = arm ? ((addr & 0x00FFFFFFu) | TLSR_DBG_BKP_ENABLE) : 0u;
    uint8_t b[4];

    b[0] = (uint8_t)v;
    b[1] = (uint8_t)(v >> 8);
    b[2] = (uint8_t)(v >> 16);
    b[3] = (uint8_t)(v >> 24);
    return tlsr_write(d, TLSR_DBG_BLOCK + (uint32_t)slot * 4u, b, 4);
}

static int slot_read(tlsr_dev *d, int slot, uint32_t *addr)
{
    uint8_t b[4];
    uint32_t v;

    if (tlsr_read(d, TLSR_DBG_BLOCK + (uint32_t)slot * 4u, b, 4) != TLSR_OK)
        return -1;
    v = (uint32_t)b[0] | ((uint32_t)b[1] << 8) |
        ((uint32_t)b[2] << 16) | ((uint32_t)b[3] << 24);
    if (addr)
        *addr = v & 0x00FFFFFFu;
    return (v & TLSR_DBG_BKP_ENABLE) ? 1 : 0;
}

/*
 * The anchor: slot TLSR_DBG_ANCHOR, which the debugger keeps for itself.
 *
 * A stopped core on this part is a core sitting on an armed comparator; the
 * 0x06 stall leaks within about half a second. So every stop ends by arming the
 * anchor at wherever the core came to rest, and every operation that moves the
 * core moves or clears it. Clearing the anchor is also how the core is let go,
 * which is why anchor_clear() is never called except on a deliberate resume.
 */
static int anchor_set(tlsr_dev *d, uint32_t addr)
{
    d->dbg_anchor = addr & 0x00FFFFFFu;
    d->dbg_anchor_on = 1;
    return slot_write(d, TLSR_DBG_ANCHOR, addr, 1);
}

static int anchor_clear(tlsr_dev *d)
{
    d->dbg_anchor_on = 0;
    d->dbg_anchor = 0;
    return slot_write(d, TLSR_DBG_ANCHOR, 0, 0);
}

/* Hold the core where it has just stopped. Arming a comparator while the core
 * is already held does not disturb it -- measured for a fresh slot, for
 * disarming another slot, and for rewriting the holding slot itself. */
static void anchor_here(tlsr_dev *d, uint32_t pc)
{
    if (pc != 0x00FFFFFFu)
        anchor_set(d, pc);
}

/*
 * Make the software list match the comparators.
 *
 * The chip is the authority, not this process: in the CLI every command is a
 * new process that starts with an empty list, so allocating a slot from that
 * list would silently overwrite a breakpoint an earlier command had set. Three
 * word reads, and it cannot drift, because every add and remove writes the chip.
 */
static void bp_reconcile(tlsr_dev *d)
{
    uint32_t at = 0;
    int i, on;

    for (i = 0; i < TLSR_DBG_NBP; i++) {
        at = 0;
        on = slot_read(d, i, &at);
        if (on < 0)
            continue;                       /* a bad read is not evidence */
        d->dbg_bp_used[i] = (uint8_t)(on ? 1 : 0);
        d->dbg_bp[i] = on ? at : 0u;
    }
    /* The anchor too, for the same reason: a fresh process has no idea a
     * previous one left the core held on it, and the step path uses the mirror
     * rather than re-reading the block. */
    at = 0;
    on = slot_read(d, TLSR_DBG_ANCHOR, &at);
    if (on >= 0) {
        d->dbg_anchor_on = (uint8_t)(on ? 1 : 0);
        d->dbg_anchor = on ? at : 0u;
    }
}

int tlsr_dbg_bp_sync(tlsr_dev *d)
{
    int i, rc = TLSR_OK;
    if (!d)
        return TLSR_E_ARG;
    /* Armed slots first, cleared ones after: clearing can release a held core,
     * and when it does the breakpoints should already be in place. */
    for (i = 0; i < TLSR_DBG_NBP; i++)
        if (d->dbg_bp_used[i]) {
            int r = slot_write(d, i, d->dbg_bp[i], 1);
            if (r != TLSR_OK) rc = r;
        }
    for (i = 0; i < TLSR_DBG_NBP; i++)
        if (!d->dbg_bp_used[i]) {
            int r = slot_write(d, i, 0, 0);
            if (r != TLSR_OK) rc = r;
        }
    return rc;
}

int tlsr_dbg_bp_at(tlsr_dev *d, uint32_t addr)
{
    int i;
    if (!d)
        return 0;
    addr &= 0x00FFFFFFu;
    for (i = 0; i < TLSR_DBG_NBP; i++)
        if (d->dbg_bp_used[i] && d->dbg_bp[i] == addr)
            return 1;
    return 0;
}

int tlsr_dbg_bp_count(tlsr_dev *d)
{
    int i, n = 0;
    if (!d)
        return 0;
    for (i = 0; i < TLSR_DBG_NBP; i++)
        if (d->dbg_bp_used[i]) n++;
    return n;
}

int tlsr_dbg_bp_list(tlsr_dev *d, uint32_t *out, int max)
{
    int i, n = 0;
    if (!d || !out)
        return 0;
    for (i = 0; i < TLSR_DBG_NBP && n < max; i++)
        if (d->dbg_bp_used[i]) out[n++] = d->dbg_bp[i];
    return n;
}

int tlsr_dbg_bp_add(tlsr_dev *d, uint32_t addr)
{
    int i;

    if (!d)
        return -1;
    bp_reconcile(d);
    addr &= 0x00FFFFFFu;
    for (i = 0; i < TLSR_DBG_NBP; i++)
        if (d->dbg_bp_used[i] && d->dbg_bp[i] == addr)
            return i;                          /* already there */
    for (i = 0; i < TLSR_DBG_NBP; i++) {
        if (d->dbg_bp_used[i])
            continue;
        d->dbg_bp[i] = addr;
        d->dbg_bp_used[i] = 1;
        if (slot_write(d, i, addr, 1) != TLSR_OK) {
            d->dbg_bp_used[i] = 0;
            tlsr_set_error("could not write the breakpoint comparator");
            return -1;
        }
        return i;
    }
    tlsr_set_error("all %d hardware breakpoints are in use. This part has no "
                   "software breakpoints -- the code runs from flash, so there "
                   "is nowhere to plant one. Remove one first.", TLSR_DBG_NBP);
    return -1;
}

int tlsr_dbg_bp_remove(tlsr_dev *d, uint32_t addr)
{
    uint32_t pc = 0;
    int i;

    if (!d)
        return 0;
    bp_reconcile(d);
    addr &= 0x00FFFFFFu;
    for (i = 0; i < TLSR_DBG_NBP; i++) {
        if (!d->dbg_bp_used[i] || d->dbg_bp[i] != addr)
            continue;
        /* Clearing the comparator that is holding the core would let it go, and
         * "remove the breakpoint I am stopped on" is the ordinary gesture. Put
         * the anchor on this address first, so something still holds it. */
        if (tlsr_dbg_is_halted(d) == 1 && pc_settled(d, &pc) && pc == addr)
            anchor_set(d, addr);
        d->dbg_bp_used[i] = 0;
        d->dbg_bp[i] = 0;
        slot_write(d, i, 0, 0);
        return 1;
    }
    return 0;
}

int tlsr_dbg_bp_toggle(tlsr_dev *d, uint32_t addr, int *added)
{
    if (!d)
        return TLSR_E_ARG;
    bp_reconcile(d);
    if (tlsr_dbg_bp_at(d, addr)) {
        if (added) *added = 0;
        return tlsr_dbg_bp_remove(d, addr) ? TLSR_OK : TLSR_E_TARGET;
    }
    if (added) *added = 1;
    return tlsr_dbg_bp_add(d, addr) >= 0 ? TLSR_OK : TLSR_E_TARGET;
}

int tlsr_dbg_bp_clear_all(tlsr_dev *d)
{
    uint32_t pc = 0;
    int i;

    if (!d)
        return TLSR_E_ARG;
    if (tlsr_dbg_is_halted(d) == 1 && pc_settled(d, &pc))
        anchor_here(d, pc);            /* same reason as bp_remove() */
    for (i = 0; i < TLSR_DBG_NBP; i++) {
        d->dbg_bp_used[i] = 0;
        d->dbg_bp[i] = 0;
    }
    return tlsr_dbg_bp_sync(d);
}

/* -------------------------------------------------------------- stepping -- */
int tlsr_dbg_insn(tlsr_dev *d, uint32_t addr, int *len, int *is_call,
                  uint32_t *call_target)
{
    uint8_t buf[8];
    uint32_t got, target = 0;
    int w;

    if (len)         *len = 2;
    if (is_call)     *is_call = 0;
    if (call_target) *call_target = 0;

    if (!d)
        return 0;
    memset(buf, 0, sizeof buf);
    got = fetch_insn_bytes(d, addr, buf, sizeof buf);
    if (got < 2)
        return 0;

    w = tlsr_dis_len(buf, got, addr, addr);
    if (w <= 0)
        return 0;
    if (len) *len = w;

    /* A call is the two-halfword tjl/tjlex.  Register-indirect calls are not
     * distinguishable here -- tlsr_dbg_step_over() catches those afterwards by
     * looking at LR, which needs no encoding knowledge at all. */
    if (w == 4) {
        if (is_call) *is_call = 1;
        if (tlsr_dis_target(buf, got, addr, addr, &target) && call_target)
            *call_target = target;
    }
    return 1;
}

/*
 * Run the interrupt handler out and land where the step should have arrived.
 *
 * Measured: when a step lands on the IRQ vector the instruction HAS executed,
 * and LR holds before+length -- the very address the step was heading for.  So
 * one comparator stop at LR finishes the job.  The older code here breakpointed
 * the address we started from and retried the step, on the belief that the
 * instruction had not run; that would have executed it twice.
 */
static int step_irq_recover(tlsr_dev *d, uint32_t *pc, tlsr_log_fn log,
                            void *user)
{
    tlsr_regs r;
    uint32_t lr, land = 0;
    int rc;

    if (tlsr_dbg_read_regs(d, &r) != TLSR_OK)
        return TLSR_E_TARGET;
    lr = r.r[TLSR_DBG_IDX_LR] & 0x00FFFFFFu;
    if (!lr || lr == TLSR_DBG_IRQ_VECTOR) {
        tlsr_set_error("stepped into an interrupt and LR does not say where to "
                       "come back to (0x%06X)", (unsigned)lr);
        return TLSR_E_TARGET;
    }

    /* The anchor is the debugger's own slot, so this costs no user breakpoint
     * -- and it lands us exactly where the step was heading, which leaves the
     * anchor already in the right place. */
    rc = anchor_set(d, lr);
    if (rc != TLSR_OK)
        return rc;
    rc = tlsr_dbg_go(d);
    if (rc == TLSR_OK)
        rc = wait_halted(d, 2000, &land) ? TLSR_OK : TLSR_E_TIMEOUT;

    if (rc != TLSR_OK || land != lr) {
        if (!stuck_error(d, "stepping"))
            tlsr_set_error("stepped into an interrupt and the handler did not "
                           "return to 0x%06X (PC 0x%06X)",
                           (unsigned)lr, (unsigned)land);
        return TLSR_E_TIMEOUT;
    }
    TLSR_LOG(log, user, "ran an interrupt handler out and returned to 0x%06X",
             (unsigned)lr);
    *pc = land;
    return TLSR_OK;
}

int tlsr_dbg_step(tlsr_dev *d, tlsr_regs *out, int *skipped,
                  tlsr_log_fn log, void *user)
{
    tlsr_regs regs;
    uint32_t start = 0, now = 0;
    int attempt, n = 0, rc, slen = 0, half = 0, ran_past = 0;

    if (!d)
        return TLSR_E_ARG;
    if (!out)
        out = &regs;
    if (skipped)
        *skipped = 0;

    /* Stepping from a PC that is still moving makes every reported delta
     * meaningless, so make sure the core really is held first. Bring it to rest
     * the same way Pause does -- never with the stall, which can deadlock a core
     * that is fetching from flash. */
    if (tlsr_dbg_is_halted(d) != 1 || !pc_settled(d, &start)) {
        if (tlsr_dbg_pause(d, NULL, log, user) != TLSR_OK ||
            !pc_settled(d, &start)) {
            tlsr_set_error("the core is not held, so there is nothing to step");
            return TLSR_E_TARGET;
        }
    }

    /* How long the instruction we are about to execute is. Needed because the
     * two-halfword call takes two pulses of the go bit, and one pulse leaves the
     * PC in the middle of it -- see the retry below. */
    tlsr_dbg_insn(d, start, &slen, NULL, NULL);

    for (attempt = 0; attempt < TLSR_DBG_STEP_TRIES; attempt++) {
        rc = tlsr_dbg_step_insn(d);
        if (rc != TLSR_OK)
            return rc;
        Sleep(8);

        if (!pc_settled(d, &now)) {
            /* The step let the core go -- rare, but the hold is not perfect.
             * Catch it again rather than reporting a moving PC. */
            if (tlsr_dbg_pause(d, NULL, NULL, NULL) != TLSR_OK ||
                !pc_settled(d, &now)) {
                tlsr_set_error("the core would not settle after a step");
                return TLSR_E_TARGET;
            }
        }

        if (now == TLSR_DBG_IRQ_VECTOR) {
            n++;
            rc = step_irq_recover(d, &now, log, user);
            if (rc != TLSR_OK)
                return rc;
            /* The instruction already executed, so this IS the step result. */
        }

        /*
         * A four-byte call needs two pulses. One pulse leaves the PC on the
         * second halfword -- a real machine state, but not an instruction the
         * user is looking at, and "step into a call" that lands two bytes later
         * instead of in the callee is simply the wrong answer. Measured:
         * 0x0140D2 `tjl 0x141ec` stepped to 0x0140D4; a second pulse reaches
         * 0x0141EC. Only retried once, and only for the exact +2 landing, so a
         * genuine branch to start+2 is never mistaken for this.
         */
        if (slen == 4 && now == start + 2 && !half) {
            half = 1;
            continue;
        }

        /*
         * The PC did not move. Not all instructions retire under a single step
         * on this part: measured at 0x000494, `tloadr r3, [r3, #0]` inside the
         * interrupt handler, where four steps in a row left the PC exactly where
         * it was -- and with a breakpoint on the handler's entry armed at the
         * same time, that never fired, so the core had genuinely not gone
         * anywhere rather than quietly run a whole lap. The same instruction is
         * executed perfectly well by simply running: a comparator on the next
         * address catches it immediately.
         *
         * So fall back to running one instruction's worth. A self-branch is
         * excluded first, because for that the PC not moving is the correct
         * answer and running to the next address would just time out.
         */
        if (now == start && slen > 0 && !ran_past) {
            uint32_t tgt = 0;
            uint8_t code[8];
            memset(code, 0, sizeof code);
            if (fetch_insn_bytes(d, start, code, sizeof code) >= 2 &&
                tlsr_dis_target(code, sizeof code, start, start, &tgt) &&
                tgt == start) {
                TLSR_LOG(log, user, "0x%06X branches to itself -- the core is "
                         "spinning here", (unsigned)start);
                anchor_here(d, now);
                tlsr_dbg_read_regs(d, out);
                return TLSR_OK;
            }
            ran_past = 1;
            TLSR_LOG(log, user, "the instruction at 0x%06X does not retire under "
                     "a single step; running to 0x%06X instead",
                     (unsigned)start, (unsigned)(start + (uint32_t)slen));
            rc = tlsr_dbg_run_to(d, start + (uint32_t)slen, 2000, out, log, user);
            if (rc == TLSR_OK) {
                if (skipped) *skipped = n;
                return TLSR_OK;
            }
            if (!stuck_error(d, "stepping"))
                tlsr_set_error("0x%06X will neither step nor run through to "
                               "0x%06X", (unsigned)start,
                               (unsigned)(start + (uint32_t)slen));
            return rc;
        }

        /* Hold it here. Without this the core is left on the step's own hold,
         * which an interrupt breaks within about half a second -- long enough
         * for the front end to read registers, and then the core is gone. */
        anchor_here(d, now);
        tlsr_dbg_read_regs(d, out);
        if (skipped)
            *skipped = n;
        if (n)
            TLSR_LOG(log, user, "stepped 0x%06X -> 0x%06X, through %d "
                     "interrupt(s)", (unsigned)start, (unsigned)now, n);
        else
            TLSR_LOG(log, user, "stepped 0x%06X -> 0x%06X",
                     (unsigned)start, (unsigned)now);
        return TLSR_OK;
    }

    tlsr_set_error("could not complete a single step from 0x%06X",
                   (unsigned)start);
    return TLSR_E_TIMEOUT;
}

int tlsr_dbg_step_into(tlsr_dev *d, unsigned timeout_ms, tlsr_regs *out,
                       tlsr_log_fn log, void *user)
{
    (void)timeout_ms;
    return tlsr_dbg_step(d, out, NULL, log, user);
}

int tlsr_dbg_step_over(tlsr_dev *d, unsigned timeout_ms, tlsr_regs *out,
                       tlsr_log_fn log, void *user)
{
    tlsr_regs regs;
    uint32_t pc, after, newpc, lr;
    int len = 2, is_call = 0, rc;

    if (!d)
        return TLSR_E_ARG;
    if (!out)
        out = &regs;

    if (!pc_settled(d, &pc)) {
        tlsr_set_error("the core is not held, so there is nothing to step over");
        return TLSR_E_TARGET;
    }

    if (tlsr_dbg_insn(d, pc, &len, &is_call, NULL) && is_call) {
        after = pc + (uint32_t)len;
        TLSR_LOG(log, user, "running the call at 0x%06X through to 0x%06X",
                 (unsigned)pc, (unsigned)after);
        return tlsr_dbg_run_to(d, after, timeout_ms, out, log, user);
    }

    /* Not a call as far as the encoding goes.  Step it, then check whether we
     * landed inside one anyway: a register-indirect call leaves LR pointing
     * just past the instruction we were on, and the PC somewhere else. */
    rc = tlsr_dbg_step(d, out, NULL, log, user);
    if (rc != TLSR_OK)
        return rc;

    after = pc + (uint32_t)len;
    newpc = out->r[TLSR_DBG_IDX_PC] & 0x00FFFFFFu;
    lr    = out->r[TLSR_DBG_IDX_LR] & 0x00FFFFFFu;
    if (newpc != after && lr == after) {
        TLSR_LOG(log, user, "that was a call through a register; running it "
                 "through to 0x%06X", (unsigned)after);
        return tlsr_dbg_run_to(d, after, timeout_ms, out, log, user);
    }
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

    rc = tlsr_dbg_read_regs(d, out);
    if (rc != TLSR_OK)
        return rc;
    if (!out->full) {
        tlsr_set_error("the register file has not been latched yet. Stop the "
                       "core first.");
        return TLSR_E_TARGET;
    }
    lr = out->r[TLSR_DBG_IDX_LR] & 0x00FFFFFFu;
    if (!lr) {
        tlsr_set_error("LR is zero - there is no caller to return to");
        return TLSR_E_TARGET;
    }
    TLSR_LOG(log, user, "running to the caller at 0x%06X", (unsigned)lr);
    return tlsr_dbg_run_to(d, lr, timeout_ms, out, log, user);
}

/* ------------------------------------------------------------- run / stop -- */
/*
 * Is this stop real, or is it the stall that never let go?
 *
 * Right after a pause resumes the core, the halted flag is still set from the
 * stall and the PC has not moved, so a resume that did not take looks exactly
 * like an instant comparator hit. Half a second later the stall leaks and the
 * core is gone -- and the front end is showing registers from an instruction it
 * left long ago. That mis-read is what made an otherwise working pause produce
 * a "held" core that then would not step and corrupted a code read.
 *
 * A comparator stop is stable indefinitely, so watching for longer than the
 * stall survives separates the two with no ambiguity.
 */
static int stop_confirmed(tlsr_dev *d, uint32_t want, unsigned ms)
{
    unsigned waited = 0;
    uint32_t pc = 0;

    while (waited < ms) {
        Sleep(50);
        waited += 50;
        if (tlsr_dbg_is_halted(d) != 1)
            return 0;
        if (!pc_settled(d, &pc) || pc != want)
            return 0;
    }
    return 1;
}

/*
 * Pause: stop the core somewhere it will stay, WITHOUT ever stalling it.
 *
 * The obvious way is 0x0602 <- 0x06 to find out where the core is and then arm
 * that address. Do not: the stall takes the bus away from the MSPI controller,
 * and this firmware executes from flash, so a stall that lands mid-fetch
 * deadlocks the core. It is reproducible -- the target locked up at 0x000494
 * with `tloadr r3, [r3, #0]` under the PC, immovable by stepping and immune to
 * the soft reset, and only a cold power cycle brought it back. Every lock-up
 * seen on this bench followed a stall.
 *
 * None of that is necessary. 0x06BC gives the live PC of a RUNNING core, so a
 * sample of it is already a real fetch address: arm a comparator there and wait
 * for execution to come round again. If the sample was a one-shot address the
 * wait times out and another sample is taken.
 */
int tlsr_dbg_pause(tlsr_dev *d, tlsr_regs *out, tlsr_log_fn log, void *user)
{
    tlsr_regs regs;
    uint32_t p = 0, land = 0;
    int t;

    if (!d)
        return TLSR_E_ARG;
    if (!out)
        out = &regs;

    /* Already stopped? Then there is nothing to do, and re-arming anything
     * would only risk moving the core. */
    if (tlsr_dbg_is_halted(d) == 1 && pc_settled(d, &p)) {
        tlsr_dbg_read_regs(d, out);
        TLSR_LOG(log, user, "already held at 0x%06X", (unsigned)p);
        return TLSR_OK;
    }

    /*
     * Cast a net rather than a line: sample several PCs and arm every spare
     * comparator at once, so whichever address the code reaches first stops it.
     * Sampling one address at a time and waiting missed often enough to be
     * annoying -- 5 pauses out of 25 timed out on a perfectly healthy target,
     * because plenty of what a target executes is not reached again within a
     * second and a half.
     */
    for (t = 0; t < PAUSE_TRIES; t++) {
        uint32_t cand[TLSR_DBG_NSLOTS];
        int slots[TLSR_DBG_NSLOTS];
        int ncand = 0, i, k, rc;

        /* The anchor is always ours; spare user slots are free to borrow. */
        slots[ncand] = TLSR_DBG_ANCHOR;
        cand[ncand] = 0;
        ncand++;
        for (i = 0; i < TLSR_DBG_NBP && ncand < TLSR_DBG_NSLOTS; i++)
            if (!d->dbg_bp_used[i]) { slots[ncand] = i; cand[ncand] = 0; ncand++; }

        /*
         * Pick addresses that RECUR, not merely distinct ones.
         *
         * A comparator only fires when its address is fetched again, so the
         * useful candidate is a hot address -- and the cheapest evidence that an
         * address is hot is having sampled it twice. Taking the first few
         * distinct samples instead fails badly right after a boot, where the
         * firmware is running one-shot startup code: measured, a pause straight
         * after Attach could miss three times in a row and give up while the
         * core was perfectly healthy.
         */
        {
            uint32_t seen[48];
            int count[48], nseen = 0, j;
            for (i = 0; i < 96 && nseen < (int)(sizeof seen / sizeof seen[0]); i++) {
                uint32_t sp = tlsr_dbg_read_pc(d) & 0x00FFFFFFu;
                if (sp == 0x00FFFFFFu || sp == 0) continue;
                for (j = 0; j < nseen; j++) if (seen[j] == sp) break;
                if (j < nseen) count[j]++;
                else { seen[nseen] = sp; count[nseen] = 1; nseen++; }
            }
            /* Most-seen first, by simple selection -- ncand is at most four. */
            k = 0;
            while (k < ncand && k < nseen) {
                int best = -1;
                for (j = 0; j < nseen; j++)
                    if (count[j] > 0 && (best < 0 || count[j] > count[best]))
                        best = j;
                if (best < 0) break;
                cand[k++] = seen[best];
                count[best] = 0;
            }
        }
        if (!k)
            continue;
        for (i = 0; i < k; i++)
            slot_write(d, slots[i], cand[i], 1);
        d->dbg_anchor = cand[0];
        d->dbg_anchor_on = 1;

        rc = tlsr_dbg_go(d);
        if (rc == TLSR_OK && wait_halted(d, PAUSE_WAIT_MS, &land)) {
            /* Release the borrowed slots before confirming, so what holds the
             * core from here on is the anchor alone. */
            for (i = 1; i < k; i++)
                slot_write(d, slots[i], 0, 0);
            anchor_here(d, land);
            if (stop_confirmed(d, land, TLSR_DBG_STOP_CONFIRM_MS)) {
                tlsr_dbg_read_regs(d, out);
                TLSR_LOG(log, user, "paused at 0x%06X", (unsigned)land);
                return TLSR_OK;
            }
        }
        for (i = 1; i < k; i++)
            slot_write(d, slots[i], 0, 0);
        TLSR_LOG(log, user, "none of %d sampled addresses came round again; "
                 "sampling further on", k);
    }

    /*
     * Nothing sampled recurred. Rather than stall -- which risks the lock-up
     * described above -- say so and leave the core alone. Distinguish the two
     * reasons, because they call for completely different things from the user.
     */
    anchor_clear(d);
    tlsr_dbg_read_regs(d, out);
    if (stuck_error(d, "pausing")) {
        /* message already set */
    } else {
        tlsr_set_error("could not bring the core to rest: none of the addresses "
                       "it was sampled at came round again. Set a breakpoint on "
                       "code you know runs, and continue to it.");
    }
    return TLSR_E_TIMEOUT;
}

int tlsr_dbg_continue(tlsr_dev *d, unsigned timeout_ms, tlsr_regs *out,
                      tlsr_log_fn log, void *user)
{
    tlsr_regs regs;
    uint32_t land = 0;
    int rc, n;

    if (!d)
        return TLSR_E_ARG;
    if (!out)
        out = &regs;

    tlsr_dbg_bp_sync(d);
    n = tlsr_dbg_bp_count(d);

    /*
     * Dropping the anchor is what lets the core go, and no step-off dance is
     * needed: the core does leave a comparator it is halted on -- measured with
     * the next instruction armed in a second slot, ten resumes alternating
     * perfectly between the two. An older version ran blind for 30 ms with
     * everything disarmed to "get clear", which silently missed any hit in that
     * window.
     */
    anchor_clear(d);
    rc = tlsr_dbg_go(d);
    if (rc != TLSR_OK)
        return rc;

    if (!n) {
        TLSR_LOG(log, user, "running (no breakpoints set)");
        return TLSR_OK;
    }

    TLSR_LOG(log, user, "running, waiting for %d breakpoint%s", n,
             n == 1 ? "" : "s");
    if (wait_halted(d, timeout_ms, &land)) {
        anchor_here(d, land);
        tlsr_dbg_read_regs(d, out);
        TLSR_LOG(log, user, "stopped at 0x%06X", (unsigned)land);
        return TLSR_OK;
    }
    if (!stuck_error(d, "continuing"))
        tlsr_set_error("no breakpoint fired within %u ms. They are still armed, "
                       "so one may fire later -- a comparator only fires when "
                       "that address is fetched, and code that runs rarely may "
                       "take a while to get there.", timeout_ms);
    return TLSR_E_TIMEOUT;
}

int tlsr_dbg_run_to(tlsr_dev *d, uint32_t addr, unsigned timeout_ms,
                    tlsr_regs *out, tlsr_log_fn log, void *user)
{
    tlsr_regs regs;
    uint32_t land = 0;
    int rc;

    if (!d)
        return TLSR_E_ARG;
    if (!out)
        out = &regs;

    addr &= 0x00FFFFFFu;
    tlsr_dbg_bp_sync(d);

    /* The anchor becomes the destination. It is the debugger's own slot, so
     * running to an address never costs the user a breakpoint -- and if one of
     * theirs is reached first, that stop wins, which is what a debugger should
     * do. */
    rc = anchor_set(d, addr);
    if (rc != TLSR_OK)
        return rc;
    rc = tlsr_dbg_go(d);
    if (rc == TLSR_OK)
        rc = wait_halted(d, timeout_ms, &land) ? TLSR_OK : TLSR_E_TIMEOUT;

    if (rc == TLSR_OK) {
        anchor_here(d, land);       /* re-aim it at where we actually stopped */
        tlsr_dbg_read_regs(d, out);
        if (land == addr)
            TLSR_LOG(log, user, "stopped at 0x%06X", (unsigned)land);
        else
            TLSR_LOG(log, user, "stopped at 0x%06X on the way -- a breakpoint "
                     "came first", (unsigned)land);
        return TLSR_OK;
    }
    anchor_clear(d);                /* do not leave a phantom stop behind */
    if (!stuck_error(d, "running to the chosen address"))
        tlsr_set_error("0x%06X was not reached within %u ms. The core is still "
                       "running; that address may simply not be executed.",
                       (unsigned)addr, timeout_ms);
    return TLSR_E_TIMEOUT;
}

/* ----------------------------------------------------------------- misc --- */
int tlsr_dbg_read_stack(tlsr_dev *d, uint32_t sp, uint8_t *out, uint32_t n)
{
    /* The CPU sees SRAM at 0x840000; SWire addresses it at 0x040000.  Accept
     * either so a caller can pass SP straight through from the register file. */
    uint32_t a = (sp >= 0x800000u) ? (sp - 0x800000u) : sp;
    if (!d || !out || !n)
        return TLSR_E_ARG;
    if (tlsr_read(d, a, out, n) != TLSR_OK)
        return 0;
    return (int)n;
}

int tlsr_dbg_profile(tlsr_dev *d, uint32_t *hits, int n, unsigned gap_ms)
{
    int i, got = 0;
    if (!d || !hits || n <= 0)
        return TLSR_E_ARG;
    for (i = 0; i < n; i++) {
        /* 0x06BC rather than the 128-byte register file: one word instead of
         * 128 bytes, which is both faster and far less traffic to garble on a
         * marginal link. */
        uint32_t pc = tlsr_dbg_read_pc(d);
        if (pc != 0xFFFFFFFFu)
            hits[got++] = pc;
        if (gap_ms)
            Sleep(gap_ms);
    }
    return got;
}

void tlsr_dbg_set_code(tlsr_dev *d, const uint8_t *img, uint32_t len, uint32_t base)
{
    if (!d)
        return;
    d->code = img;
    d->code_len = img ? len : 0u;
    d->code_base = base;
}

int tlsr_dbg_have_code(tlsr_dev *d)
{
    return d && d->code && d->code_len ? 1 : 0;
}

int tlsr_dbg_attached(tlsr_dev *d)
{
    return d ? d->dbg_attached : 0;
}

/* ----------------------------------------------------------------- state -- */
int tlsr_dbg_status_read(tlsr_dev *d, tlsr_dbg_status *out)
{
    uint8_t id[2] = { 0, 0 }, clk = 0, rails = 0;
    tlsr_cfg c;
    uint32_t pc = 0;
    int halted, i;

    if (!d || !out)
        return TLSR_E_ARG;
    memset(out, 0, sizeof *out);
    out->bp_hit = -1;
    out->have_code = tlsr_dbg_have_code(d);
    out->attached = d->dbg_attached;
    out->nbp = tlsr_dbg_bp_list(d, out->bp, TLSR_DBG_NSLOTS);

    /* Ask the bridge rather than trusting a per-process flag: a session
     * deliberately outlives one invocation, so the CLI is normally looking at a
     * bridge some earlier process configured. */
    if (tlsr_get_cfg(d, &c, &rails) == TLSR_OK) {
        out->live_timing = (c.spi_div == TLSR_DBG_LIVE_SPI_DIV);
        out->powered = rails ? 1 : 0;
    } else {
        out->live_timing = d->dbg_attached;
        out->powered = 1;                     /* cannot tell; assume the worst */
    }

    if (!out->powered) {
        out->state = TLSR_DBG_POWER_OFF;
        out->caps = 0;
        return TLSR_OK;
    }

    /*
     * A session lives in the bridge and the chip, not in this process: the CLI
     * is one invocation per command, so the attached flag is false on every run
     * after the one that opened the session. The live divider is what is
     * actually true of the world, so adopt it.
     */
    if (!d->dbg_attached && out->live_timing) {
        d->dbg_attached = 1;
        out->attached = 1;
    }

    /*
     * Detached and powered: the firmware is running but the bridge is on the
     * programming divider, where every byte comes back shifted one bit.  There
     * is nothing to measure and nothing to do but attach, so do not spend a
     * second pretending otherwise.  The exception is a target that has been
     * activated by the programmer tabs -- that one answers, and is worth
     * reporting as caught at boot.
     */
    if (!d->dbg_attached) {
        if (d->activated &&
            tlsr_read(d, TLSR_REG_CHIP_ID, id, 2) == TLSR_OK &&
            id[0] == 0x62 && id[1] == 0x55) {
            out->state = TLSR_DBG_BOOT_HELD;
            out->caps = TLSR_CAP_MEM_READ | TLSR_CAP_MEM_WRITE |
                        TLSR_CAP_FLASH | TLSR_CAP_ATTACH;
            return TLSR_OK;
        }
        out->state = TLSR_DBG_ON_BOOT;
        out->caps = TLSR_CAP_ATTACH;
        return TLSR_OK;
    }

    /* Attached: the link should answer at the live divider. */
    if (tlsr_read(d, TLSR_REG_CHIP_ID, id, 2) != TLSR_OK ||
        id[0] != 0x62 || id[1] != 0x55) {
        out->state = TLSR_DBG_NO_TARGET;
        out->caps = TLSR_CAP_ATTACH | TLSR_CAP_DETACH;
        return TLSR_OK;
    }

    out->caps = TLSR_CAP_MEM_READ | TLSR_CAP_MEM_WRITE | TLSR_CAP_DETACH |
                TLSR_CAP_RESET;

    bp_reconcile(d);
    out->nbp = tlsr_dbg_bp_list(d, out->bp, TLSR_DBG_NBP);

    if (tlsr_read(d, TLSR_REG_CLKSET, &clk, 1) != TLSR_OK)
        clk = 0;
    out->clkset = clk;

    out->caps |= TLSR_CAP_PC;
    halted = tlsr_dbg_is_halted(d);
    out->halted = (halted == 1);

    if (halted != 1) {
        /*
         * Not held by us. Either the core is executing, or it is sitting in the
         * post-reset window because something activated it -- and the way to
         * tell is to look at the PC, not at CLKSET. An earlier version gated
         * this on CLKSET reading 0x20, which is one image's choice of a 48 MHz
         * clock rather than anything the part guarantees; firmware that stays
         * on the boot clock was reported as "caught at boot" while it ran.
         *
         * Six reads with no sleeps between them: SWire round trips are already
         * milliseconds apart, so a core that is executing will not return the
         * same address six times, and a core held by bus arbitration cannot
         * return anything else.
         */
        uint32_t seen = 0xFFFFFFFFu;
        int good = 0, moved = 0;

        /* Failed reads are discarded, not counted as "the PC did not change" --
         * one dropped read would otherwise be indistinguishable from a core
         * that is not executing, and would grey the whole tab out. Two good
         * samples are the minimum for any verdict at all. */
        for (i = 0; i < 8 && !moved; i++) {
            uint32_t p = tlsr_dbg_read_pc(d);
            if (p == 0xFFFFFFFFu)
                continue;
            good++;
            if (seen == 0xFFFFFFFFu) seen = p;
            else if (p != seen)      moved = 1;
        }
        if (good >= 2 && !moved) {
            /* Nothing is executing: caught before the firmware ran, or stopped
             * dead. Flash still works, which is what the programmer wants. */
            out->state = TLSR_DBG_BOOT_HELD;
            out->caps |= TLSR_CAP_FLASH | TLSR_CAP_ATTACH;
            return TLSR_OK;
        }
        out->state = TLSR_DBG_RUNNING;
        out->pc = seen & 0x00FFFFFFu;   /* one real sample of a moving PC */
        out->caps |= TLSR_CAP_HALT | TLSR_CAP_PROFILE;
        return TLSR_OK;
    }

    if (!pc_settled(d, &pc)) {
        /* The flag says held but the PC will not settle -- a degraded link.
         * Report running rather than presenting an address that is not real. */
        out->state = TLSR_DBG_RUNNING;
        out->caps |= TLSR_CAP_HALT | TLSR_CAP_PROFILE;
        return TLSR_OK;
    }
    out->pc = pc;

    for (i = 0; i < out->nbp; i++)
        if (out->bp[i] == pc) { out->bp_hit = i; break; }
    out->state = (out->bp_hit >= 0) ? TLSR_DBG_AT_BREAKPOINT : TLSR_DBG_HELD;

    out->caps |= TLSR_CAP_RESUME | TLSR_CAP_BREAKPOINT | TLSR_CAP_RUN_TO |
                 TLSR_CAP_STEP_INSN | TLSR_CAP_STEP_INTO | TLSR_CAP_STEP_OVER |
                 TLSR_CAP_CODE;
    {
        tlsr_regs r;
        if (tlsr_dbg_read_regs(d, &r) == TLSR_OK && r.full)
            out->caps |= TLSR_CAP_REGS_FULL | TLSR_CAP_STACK | TLSR_CAP_STEP_OUT;
    }
    return TLSR_OK;
}

/* ------------------------------------------------------ power and session -- */
/*
 * Remember the bridge timing to go back to when the session closes -- but never
 * remember the LIVE divider as that.
 *
 * Whether a session is open is decided by which divider the bridge is on, since
 * that is the one fact that survives this process. So saving spi_div=1 as "the
 * timing to restore" makes detach a no-op and leaves the debugger permanently
 * attached: measured, Attach right after a previous session captured the live
 * value and Detach then reported "running" instead of going back to ON_BOOT.
 */
static void save_prog_cfg(tlsr_dev *d)
{
    tlsr_cfg c;
    uint8_t rails = 0;

    if (d->dbg_saved_ok)
        return;
    if (tlsr_get_cfg(d, &c, &rails) != TLSR_OK)
        return;
    if (c.spi_div == TLSR_DBG_LIVE_SPI_DIV)
        c.spi_div = TLSR_DBG_PROG_SPI_DIV;
    d->dbg_saved = c;
    d->dbg_saved_ok = 1;
}

/* Put the bridge back on the programming divider, whatever it is on now. */
static void restore_prog_timing(tlsr_dev *d)
{
    tlsr_cfg c;
    uint8_t rails = 0;

    save_prog_cfg(d);
    if (d->dbg_saved_ok) {
        tlsr_set_cfg(d, &d->dbg_saved);
        return;
    }
    if (tlsr_get_cfg(d, &c, &rails) == TLSR_OK) {
        c.spi_div = TLSR_DBG_PROG_SPI_DIV;
        tlsr_set_cfg(d, &c);
    }
}

int tlsr_dbg_power_on(tlsr_dev *d, tlsr_log_fn log, void *user)
{
    int rc;

    if (!d)
        return TLSR_E_ARG;

    save_prog_cfg(d);
    /* Powering up is not attaching, so the link must read as detached -- which
     * means the programming divider. */
    restore_prog_timing(d);
    d->dbg_attached = 0;

    /* Off first, so this always produces a cold boot rather than a no-op on an
     * already powered target. */
    tlsr_power(d, 0, TLSR_RAIL_BOTH);
    Sleep(900);
    rc = tlsr_power(d, 1, TLSR_RAIL_BOTH);
    if (rc != TLSR_OK)
        return rc;
    /* Deliberately no tlsr_activate(): activation catches the chip before the
     * firmware runs, and that state survives a soft reset. */
    d->activated = 0;
    d->dbg_powered = 1;
    TLSR_LOG(log, user, "target powered with no activation, so the firmware "
             "boots and runs");
    Sleep(1200);
    return TLSR_OK;
}

int tlsr_dbg_power_off(tlsr_dev *d, tlsr_log_fn log, void *user)
{
    int rc;

    if (!d)
        return TLSR_E_ARG;
    save_prog_cfg(d);
    if (d->dbg_attached)
        tlsr_dbg_detach(d, NULL, NULL);
    restore_prog_timing(d);
    rc = tlsr_power(d, 0, TLSR_RAIL_BOTH);
    d->activated = 0;
    d->dbg_powered = 0;
    d->dbg_attached = 0;
    if (rc == TLSR_OK)
        TLSR_LOG(log, user, "target power off");
    return rc;
}

/* Does the target answer at the divider the bridge is currently set to?
 *
 * Two matching reads, because an un-activated SWire slave does not go quiet --
 * it returns noise, and a single read can land on something that looks like an
 * answer. Requiring the same value twice rejects that. */
static int target_answers(tlsr_dev *d)
{
    uint8_t a[2], b[2];

    if (tlsr_read(d, TLSR_REG_CHIP_ID, a, 2) != TLSR_OK) return 0;
    if (tlsr_read(d, TLSR_REG_CHIP_ID, b, 2) != TLSR_OK) return 0;
    if (a[0] != b[0] || a[1] != b[1]) return 0;
    return (a[0] == 0x62 && a[1] == 0x55);
}

static int set_div(tlsr_dev *d, uint8_t div)
{
    tlsr_cfg c = d->dbg_saved;
    c.spi_div = div;
    return tlsr_set_cfg(d, &c);
}

/*
 * Open a session by MEASURING the target, not by assuming what its firmware
 * does with the system clock.
 *
 * An earlier version required CLKSET to read 0x20 before it would attach. That
 * is one particular image's choice of a 48 MHz clock, not a property of the
 * part: firmware that stays on the boot clock would have been told "no firmware
 * is running" while it plainly was, and the tool would have been useless on it.
 *
 * Two things actually need establishing, and both are directly observable:
 *
 *   which divider the target answers at -- the SWire slave's bit clock follows
 *   the system clock, so firmware that raises the clock and does not rewrite
 *   SWIRE_DIV answers at the doubled rate and nowhere else. Trying both and
 *   keeping whichever replies costs two chip-id reads and works whatever the
 *   firmware picked;
 *
 *   whether the core is executing -- ask the PC, not the clock.
 *
 * A target that answers at the live divider is running by construction: nothing
 * but firmware could have raised the clock. A target that answers at the
 * programming divider might be running at the boot clock, caught in the
 * post-reset window by an earlier activation, or stopped by us; the PC and the
 * halted flag separate those.
 */
int tlsr_dbg_attach(tlsr_dev *d, tlsr_log_fn log, void *user)
{
    tlsr_cfg c;
    uint8_t rails = 0;
    int rc, i, cycled = 0;

    if (!d)
        return TLSR_E_ARG;

    rc = tlsr_get_cfg(d, &c, &rails);
    if (rc != TLSR_OK)
        return rc;
    save_prog_cfg(d);
    if (!rails) {
        tlsr_set_error("the target is not powered. Switch target power on "
                       "first -- the firmware has to be running before there "
                       "is anything to attach to.");
        return TLSR_E_TARGET;
    }

    for (;;) {
        int live_ok = 0, prog_ok = 0;

        /*
         * Exhaust the live rate BEFORE trying the programming one, and give the
         * target time to get there.
         *
         * A board that has only just been powered answers at the boot clock for
         * a moment and then raises it. Interleaving the two rates and taking
         * whichever replied first therefore latched onto the boot clock, and
         * attach reported success on the programming divider seconds before the
         * firmware clocked itself up -- after which every read came back
         * shifted and the PC read as zero. Wait it out instead.
         */
        for (i = 0; i < 12 && !live_ok; i++) {
            if (set_div(d, TLSR_DBG_LIVE_SPI_DIV) != TLSR_OK)
                return TLSR_E_BRIDGE;
            live_ok = target_answers(d);
            if (!live_ok)
                Sleep(150);
        }

        if (!live_ok) {
            if (set_div(d, TLSR_DBG_PROG_SPI_DIV) != TLSR_OK)
                return TLSR_E_BRIDGE;
            prog_ok = target_answers(d);
        }

        if (live_ok) {
            TLSR_LOG(log, user, "target answers at spi_div=%d: its firmware has "
                     "raised the system clock, so it is running",
                     TLSR_DBG_LIVE_SPI_DIV);
            break;
        }

        if (prog_ok) {
            /* Answers at the programming rate. Running at the boot clock, or
             * not executing at all -- the PC says which. */
            if (tlsr_dbg_is_halted(d) == 1) {
                TLSR_LOG(log, user, "target answers at spi_div=%d and the core "
                         "is held by the debug block", TLSR_DBG_PROG_SPI_DIV);
                break;
            }
            if (pc_is_moving(d)) {
                TLSR_LOG(log, user, "target answers at spi_div=%d and the core "
                         "is executing: firmware that leaves the system clock "
                         "alone", TLSR_DBG_PROG_SPI_DIV);
                break;
            }
        }

        if (cycled) {
            restore_prog_timing(d);
            tlsr_set_error("nothing is running to attach to. The target %s, and "
                           "the core is not executing. Either the flash holds "
                           "no runnable image, or the target is not being "
                           "powered by the bridge.",
                           prog_ok ? "answers but is held before its firmware"
                                   : "does not answer at either SWire rate");
            return TLSR_E_TARGET;
        }

        /* Caught in the post-reset window by an earlier activation, most
         * likely -- and that survives a soft reset, so only a cold cycle with
         * no activation gets the firmware going. */
        TLSR_LOG(log, user, "the core is not executing -- cold cycling the "
                 "target without activation so its firmware runs");
        restore_prog_timing(d);
        tlsr_power(d, 0, TLSR_RAIL_BOTH);
        Sleep(1200);
        tlsr_power(d, 1, TLSR_RAIL_BOTH);
        Sleep(3000);
        d->activated = 0;
        cycled = 1;
    }

    d->dbg_attached = 1;
    d->dbg_powered = 1;

    /*
     * A session outlives one process -- the CLI is one invocation per command --
     * so breakpoints an earlier run armed are still in the chip. Adopt them
     * rather than wiping them, unless this process has a list of its own.
     */
    if (!tlsr_dbg_bp_count(d)) {
        for (i = 0; i < TLSR_DBG_NBP; i++) {
            uint32_t a = 0;
            if (slot_read(d, i, &a) == 1 && a) {
                d->dbg_bp[i] = a;
                d->dbg_bp_used[i] = 1;
                TLSR_LOG(log, user, "adopted the breakpoint already set at "
                         "0x%06X", (unsigned)a);
            }
        }
    }
    tlsr_dbg_bp_sync(d);
    TLSR_LOG(log, user, "attached: %d hardware breakpoints available",
             TLSR_DBG_NBP);
    return TLSR_OK;
}

int tlsr_dbg_detach(tlsr_dev *d, tlsr_log_fn log, void *user)
{
    int i;

    if (!d)
        return TLSR_E_ARG;

    /* Leave the core running and the comparators clear -- a detached debugger
     * must not be holding the chip anywhere. */
    for (i = 0; i < TLSR_DBG_NSLOTS; i++)
        slot_write(d, i, 0, 0);
    tlsr_dbg_resume(d);

    restore_prog_timing(d);
    d->dbg_attached = 0;
    d->dbg_anchor_on = 0;

    TLSR_LOG(log, user, "detached: the core is running and the bridge is back "
             "on its programming divider. The target is still powered.");
    return TLSR_OK;
}

/* Is the core executing?  Distinct PC values over a short burst.  Cheap, and
 * the only direct evidence that a reset took: CLKSET is a poor witness here
 * because the firmware puts it back to 0x20 within about 40 ms, so a poll can
 * easily never see it drop. */
/*
 * Say so when the target has locked up, rather than leaving the caller to
 * report a timeout and let the user guess.
 *
 * This firmware services a radio and timers to hard deadlines, and every hold
 * costs milliseconds because SWire is slow. Held often enough it stops
 * executing -- reproducibly, ending at 0x000494 -- and a soft reset will not
 * rescue it. Returns 1 if that is what happened, having set the error text.
 */
static int stuck_error(tlsr_dev *d, const char *what)
{
    uint32_t pc;

    if (tlsr_dbg_is_halted(d) == 1 || pc_is_moving(d))
        return 0;
    pc = tlsr_dbg_read_pc(d) & 0x00FFFFFFu;
    tlsr_set_error("%s: the target has stopped executing, stuck at 0x%06X. "
                   "Firmware with real-time work to do -- a radio, a motor, a "
                   "protocol timer -- can be broken by holding the core, and "
                   "then it does not resume. Press Restart; it falls back to a "
                   "cold power cycle, which a soft reset cannot substitute "
                   "for.", what, (unsigned)pc);
    return 1;
}

static int pc_is_moving(tlsr_dev *d)
{
    uint32_t first = tlsr_dbg_read_pc(d);
    int i;

    if (first == 0xFFFFFFFFu)
        return 0;
    for (i = 0; i < 12; i++) {
        uint32_t p = tlsr_dbg_read_pc(d);
        if (p != 0xFFFFFFFFu && p != first)
            return 1;
        Sleep(8);
    }
    return 0;
}

int tlsr_dbg_reset(tlsr_dev *d, tlsr_log_fn log, void *user)
{
    int i, rc;

    if (!d)
        return TLSR_E_ARG;
    if (!d->dbg_attached) {
        tlsr_set_error("not attached, so there is no session to restart");
        return TLSR_E_TARGET;
    }

    /* Clear the comparators first: a reset that lands straight back on a
     * breakpoint would stop the firmware in its startup code, which looks like
     * a failed reset. They go back at the end. */
    for (i = 0; i < TLSR_DBG_NSLOTS; i++)
        slot_write(d, i, 0, 0);
    d->dbg_anchor_on = 0;

    rc = tlsr_write8(d, TLSR_REG_CPU_CTRL, TLSR_CPU_REBOOT);
    if (rc != TLSR_OK)
        return rc;
    TLSR_LOG(log, user, "core reset (0x0602 <- 0x88)");
    Sleep(1200);

    /*
     * Check that the firmware is actually executing, not just that CLKSET reads
     * 0x20 -- it does that whether or not anything happened.  This matters
     * because a soft reset does NOT rescue a core that has locked up: measured
     * on a firmware that had wedged at 0x000494, 0x88 left it wedged at
     * 0x000494 while every register still read as if all were well.  A cold
     * cycle with no activation does rescue it, so fall through to one.
     */
    if (pc_is_moving(d)) {
        tlsr_dbg_bp_sync(d);
        TLSR_LOG(log, user, "firmware restarted and running");
        return TLSR_OK;
    }

    TLSR_LOG(log, user, "the soft reset did not restart it -- the core is not "
             "executing. Cold cycling the target instead.");
    tlsr_power(d, 0, TLSR_RAIL_BOTH);
    Sleep(1200);
    tlsr_power(d, 1, TLSR_RAIL_BOTH);
    Sleep(3000);
    d->activated = 0;

    for (i = 0; i < 10 && !pc_is_moving(d); i++)
        Sleep(150);
    tlsr_dbg_bp_sync(d);
    if (!pc_is_moving(d)) {
        tlsr_set_error("the target did not come back after a reset and a cold "
                       "power cycle: the core is still not executing");
        return TLSR_E_TARGET;
    }
    TLSR_LOG(log, user, "firmware restarted and running after a cold cycle");
    return TLSR_OK;
}
