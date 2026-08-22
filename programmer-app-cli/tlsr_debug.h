/*
 * tlsr_debug.h - CPU debug block on TLSR825x, over SWire.
 *
 * Copyright (C) 2026  telink_project contributors
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * Part of the shared core, so the CLI and the GUI drive the debug block
 * through exactly the same code.
 *
 * ===========================================================================
 * WHAT THE HARDWARE DOES.  Measured on a TLSR8253 outlet; the probes are in
 * recon/tools/swireprobe/dbgcaps*.c and each one restores the bench.
 *
 *   0x0602   CPU control.  0x05 flash-access stop, 0x06 stall, 0x08 run,
 *            0x84 run-with-comparators, 0x88 reset+run.  Bit1 is the hold used
 *            by the step sequence.  ALWAYS READS BACK 0x08 -- it is a command
 *            register, not a status register.
 *   0x0610   Four independent address comparators, 4 bytes each:
 *            0x0610 0x0614 0x0618 0x061C, value = addr(24 bits) | 0x01000000.
 *   0x0613   Byte 3 of the first comparator, and also the step/status byte:
 *            bit7 reads 1 when the core is HELD and 0 when it runs; writing
 *            bit7 is the go pulse; bit0 is comparator 0's enable.
 *   0x0680   32-word register file.  r0..r12 are writable; r15 (PC) is NOT.
 *   0x06BC   The live PC.
 *   0x0066   CLKSET.  Reads its reset value until firmware sets a clock.
 *
 * The measurements that shape this file, several of which correct earlier
 * beliefs in this project:
 *
 *   * FOUR comparators, not one.  Each was armed on its own from a fresh boot,
 *     with the core stepped off the address first, and each brought the core
 *     back and held it (dbgcaps7).  Two armed together both fire (dbgcaps8:
 *     five hits each, alternating).  An earlier "only word1 works" reading came
 *     from writing the whole 16-byte block, which also rewrites 0x0613.
 *
 *   * A comparator SURVIVES its own hit.  It reads back armed afterwards and
 *     fires again next time round (dbgcaps3 A).  The older note that the
 *     comparator is "consumed when it fires" was an artefact of arming at the
 *     address the core was already halted on.  So a breakpoint the user sets
 *     stays set, in hardware, until they clear it -- which is what a breakpoint
 *     has to mean.
 *
 *   * Writing the block while the core is held does NOT release it (dbgcaps3
 *     B: pure read, arming another slot, disarming it, and rewriting the slot
 *     that is holding us -- PC unchanged in all four).  The whole deferred-
 *     breakpoint mechanism this file used to carry is therefore gone.  The old
 *     "releases the core 6/6" measurement was watching a plain 0x06 stall drift.
 *
 *   * The core DOES leave a comparator it is halted on (dbgcaps8 J).  With P in
 *     one slot and the next instruction P2 in another, ten resumes alternated
 *     P2, P, P2, P... perfectly.  No step-off dance is needed.
 *
 *   * 0x0613 bit7 is a real halted flag: 0 free-running, 1 after a stall, 0
 *     after a resume, 1 at a comparator stop (dbgcaps3 C).  One byte read now
 *     answers "is the core executing", which used to cost ten PC samples over a
 *     quarter of a second.
 *
 *   * The register file is fresh after EVERY step, not only at a comparator hit
 *     (dbgcaps2 4): sp/lr/psr all tracked the core through an interrupt entry.
 *
 *   * When a step lands on the IRQ vector, the instruction HAS executed and LR
 *     holds the address the step should have reached -- measured LR ==
 *     before + length (dbgcaps6 M).  So the recovery is one comparator stop at
 *     LR, not a retry loop.  The older note here said an interrupt taken at X
 *     returns to X with X not yet executed; that was wrong.
 *
 *   * The soft reset 0x0602 <- 0x88 really does reset a live target: a sentinel
 *     written over .data came back as the flash image 1.5 s later, so the
 *     startup code ran again (dbgcaps4 D).  No power cycle is needed to restart
 *     the firmware once a session is open.
 *
 *   * The plain 0x06 stall leaks: the halted flag dropped after about 500 ms
 *     (dbgcaps4 H).  Every stop that has to persist is a comparator stop.
 *
 *   * Code can be read off the target while the core is held, in pieces of at
 *     most 512 bytes, provided the read does not issue its own 0x05 stop.  So
 *     instructions are decoded from the chip and no image file is required.
 *
 * ===========================================================================
 * GETTING INTO A DEBUGGABLE STATE
 *
 * Two conditions, and missing either one looks exactly like a dead link:
 *
 *   a. The firmware only runs if the target was powered up WITHOUT activation.
 *      Activation catches the chip in the post-reset window and that state
 *      survives a soft reset, so anything that calls tlsr_ensure_target() first
 *      can never observe running code.
 *   b. The bridge must then clock SWire twice as fast: spi_div=1 instead of the
 *      default 2.  The firmware sets a 48 MHz system clock and never rewrites
 *      SWIRE_DIV, so the slave's bit clock doubles and at the default rate
 *      every byte comes back shifted one bit -- 0x55 reads as 0xAA.
 *
 * tlsr_dbg_power_on() does (a); tlsr_dbg_attach() does (b) and verifies it.
 */
#ifndef TLSR_DEBUG_H
#define TLSR_DEBUG_H

#include "tlsr_core.h"

/* ------------------------------------------------------------- registers -- */
#define TLSR_DBG_BLOCK      0x0610u   /* 4 comparators, 4 bytes each          */
#define TLSR_DBG_NSLOTS     4         /* comparators the hardware has         */
/*
 * One of the four is reserved for the debugger itself, so three are offered as
 * user breakpoints.  That is not tidiness, it is what "stopped" means on this
 * part: nothing else holds the core.  The 0x06 stall leaks -- the halted flag
 * drops after about half a second when an interrupt breaks it -- so a core that
 * is sitting still is a core sitting on an armed comparator.  The reserved slot
 * is that anchor, and it doubles as the target for run-to, step-over's return
 * address, step-out, and the breakpoint that runs an interrupt handler out.
 */
#define TLSR_DBG_ANCHOR     3         /* the debugger's own slot              */
#define TLSR_DBG_NBP        3         /* user breakpoints                     */
#define TLSR_DBG_REGS       0x0680u   /* 128 bytes: 32 LE u32                 */
#define TLSR_DBG_PC         0x06BCu   /* the live PC                          */
#define TLSR_DBG_PC_LATCH   TLSR_DBG_PC          /* old name, same register   */
#define TLSR_DBG_STEP       0x0613u   /* bit7 go on write / held on read      */
#define TLSR_DBG_HOLD_BIT   0x02u     /* bit1 of 0x0602: hold the core        */
#define TLSR_DBG_GO_BIT     0x80u     /* bit7 of 0x0613                       */
#define TLSR_DBG_HELD_BIT   0x80u     /* same bit, reading as "core is held"  */

#define TLSR_DBG_BKP_ENABLE 0x01000000u  /* bit24 arms a comparator           */

/* Extra values for TLSR_REG_CPU_CTRL, alongside TLSR_CPU_* in tlsr_core.h. */
#define TLSR_CPU_STALL      0x06u     /* hold the core; leaks after ~500 ms   */
#define TLSR_CPU_GO_BKP     0x84u     /* resume with the comparators live     */

/* The system clock byte.  Reading it is how we tell "firmware has booted"
 * from "caught before it ran": until something runs, it holds its reset
 * value.  0x20 (48 MHz) is one common choice, not a requirement. */
#define TLSR_REG_CLKSET     0x0066u
#define TLSR_CLKSET_RESET   0x06u     /* what it reads before firmware runs   */
#define TLSR_CLKSET_48M     0x20u     /* one common choice; NOT a requirement */

/* Bridge sample-clock divider to use while the firmware runs, and the one the
 * programmer uses on a target caught at boot.  Which of the two the bridge is
 * set to is how "is a session open" is decided, so the programming value has to
 * be a known number rather than whatever happened to be there. */
#define TLSR_DBG_LIVE_SPI_DIV  1
#define TLSR_DBG_PROG_SPI_DIV  2

/* How long a comparator stop is watched before it is believed.
 *
 * A pause stalls the core, arms its address, and resumes.  The trap is that the
 * halted flag is still set from the stall when the first poll happens, and the
 * PC has not moved either, so a stall that never actually resumed looks exactly
 * like an instant comparator hit -- and then leaks half a second later, leaving
 * the front end showing registers from an instruction the core left long ago.
 * A comparator stop is stable indefinitely; the stall is not.  So watch it for
 * longer than the stall survives. */
#define TLSR_DBG_STOP_CONFIRM_MS 700

/* The IRQ vector, from the image header: offset 0x10 is `tj __irq`.  A step
 * that lands here took an interrupt; the instruction still executed and LR
 * holds where the step should have arrived. */
#define TLSR_DBG_IRQ_VECTOR  0x000010u

/* How many interrupts one logical step will run through before giving up.
 * Measured at about one interrupt every couple of instructions on this
 * firmware, and each recovery is a single comparator stop. */
#define TLSR_DBG_STEP_TRIES  8

/* Largest code read that stays intact at the doubled divider with the core
 * held.  1 KB came back with 238 wrong bytes and knocked the core loose. */
#define TLSR_DBG_CODE_CHUNK  512u

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
    int      valid;                   /* the read succeeded                   */
    int      full;                    /* sp/lr/psr are meaningful             */
} tlsr_regs;

/* "r0" ... "r15 (pc)", "psr" for 0..16, "m64" for 31, NULL otherwise. */
const char *tlsr_dbg_reg_name(int index);

/* ----------------------------------------------------------------- state -- */
/*
 * Attachment and execution are two different questions and this enum keeps them
 * apart.  POWER_OFF and ON_BOOT describe a target the debugger is not attached
 * to; the last three describe an open session.
 */
typedef enum {
    TLSR_DBG_NO_TARGET = 0,   /* powered, but nothing answers                 */
    TLSR_DBG_POWER_OFF,       /* the bridge is not powering the target        */
    TLSR_DBG_ON_BOOT,         /* powered with no activation: the firmware is
                                 running and the debugger is detached.  The
                                 link is at the programming divider, so the
                                 chip cannot be read in this state -- Attach is
                                 the only thing to do with it.                */
    TLSR_DBG_BOOT_HELD,       /* activated, caught before the firmware ran    */
    TLSR_DBG_RUNNING,         /* attached, firmware executing                 */
    TLSR_DBG_HELD,            /* attached, held by the leaky 0x06 stall       */
    TLSR_DBG_AT_BREAKPOINT    /* attached, stopped on a comparator            */
} tlsr_dbg_state;

const char *tlsr_dbg_state_name(tlsr_dbg_state s);

/* What the caller may do right now.  Front ends switch controls on these bits
 * instead of on a single "debug works" flag. */
#define TLSR_CAP_MEM_READ    0x00001u  /* registers and SRAM                  */
#define TLSR_CAP_MEM_WRITE   0x00002u
#define TLSR_CAP_FLASH       0x00004u  /* needs the 0x05 stop; drops debug    */
#define TLSR_CAP_PC          0x00008u  /* the PC is readable                  */
#define TLSR_CAP_REGS_FULL   0x00010u  /* r0..psr are meaningful              */
#define TLSR_CAP_HALT        0x00020u
#define TLSR_CAP_RESUME      0x00040u
#define TLSR_CAP_BREAKPOINT  0x00080u
#define TLSR_CAP_STEP_OVER   0x00100u
#define TLSR_CAP_STEP_INTO   0x00200u
#define TLSR_CAP_STEP_OUT    0x00400u  /* breakpoint on LR                    */
#define TLSR_CAP_STACK       0x00800u  /* read the stack at SP                */
#define TLSR_CAP_PROFILE     0x01000u  /* sample the PC while running         */
#define TLSR_CAP_STEP_INSN   0x02000u  /* one instruction                     */
#define TLSR_CAP_ATTACH      0x04000u  /* a session can be opened             */
#define TLSR_CAP_DETACH      0x08000u  /* a session is open and can be closed */
#define TLSR_CAP_RESET       0x10000u  /* the core can be rebooted            */
#define TLSR_CAP_RUN_TO      0x20000u  /* run to a chosen address             */
#define TLSR_CAP_CODE        0x40000u  /* instructions can be read off the chip */

typedef struct {
    tlsr_dbg_state state;
    unsigned  caps;
    uint32_t  pc;             /* last sampled PC, if TLSR_CAP_PC             */
    int       halted;         /* 0x0613 bit7                                 */
    uint8_t   clkset;         /* 0x20 once the firmware has booted           */
    int       live_timing;    /* bridge is at the running-firmware divider   */
    int       attached;       /* a session is open                           */
    int       powered;        /* the bridge is driving the target rails      */
    int       have_code;      /* a host-side code image is loaded            */
    int       nbp;            /* how many breakpoints are set                */
    uint32_t  bp[TLSR_DBG_NBP];
    int       bp_hit;         /* index into bp[] the core is stopped on, -1  */
} tlsr_dbg_status;

/* Measure, do not assume.  Safe to call in any state, including with no target
 * and with the rails off. */
int tlsr_dbg_status_read(tlsr_dev *d, tlsr_dbg_status *out);

/* ------------------------------------------------------ power and session -- */
/* Power the target WITHOUT activating it, so the firmware boots and runs.
 * This is the state the debugger needs and the one the programmer tabs must
 * not create by accident, which is why it is a call of its own rather than a
 * flag on tlsr_power(). */
int tlsr_dbg_power_on(tlsr_dev *d, tlsr_log_fn log, void *user);
int tlsr_dbg_power_off(tlsr_dev *d, tlsr_log_fn log, void *user);

/* Open a session on a target that is already running: switch the bridge to the
 * live divider and confirm the firmware is up.  If the chip turns out to be
 * caught at boot (someone activated it) this cold-cycles it without activating
 * and tries again, so Attach always means the same thing to the user. */
int tlsr_dbg_attach(tlsr_dev *d, tlsr_log_fn log, void *user);

/* Close the session: clear the comparators, let the core run, put the bridge
 * back on its programming divider.  The target stays powered and running -- the
 * programmer tabs activate for themselves when they need to. */
int tlsr_dbg_detach(tlsr_dev *d, tlsr_log_fn log, void *user);
int tlsr_dbg_attached(tlsr_dev *d);

/* Reboot the core without dropping the session: 0x0602 <- 0x88, verified by
 * watching CLKSET come back to 0x20.  Breakpoints are re-armed afterwards. */
int tlsr_dbg_reset(tlsr_dev *d, tlsr_log_fn log, void *user);

/* ------------------------------------------------------------ operations -- */
int tlsr_dbg_read_regs(tlsr_dev *d, tlsr_regs *out);

/* The live PC, straight from 0x06BC.  Returns 0xFFFFFFFF if the read failed. */
uint32_t tlsr_dbg_read_pc(tlsr_dev *d);

/* Is the core held?  One byte: 0x0613 bit7.  Returns 1 held, 0 running,
 * negative if the register could not be read. */
int tlsr_dbg_is_halted(tlsr_dev *d);

/* Read code from the target so instructions can be decoded without an image
 * file.  Handles both flash (addr < 0x800000) and SRAM, and splits the request
 * into pieces small enough to survive the doubled divider.  Returns how many
 * bytes landed. */
uint32_t tlsr_dbg_read_code(tlsr_dev *d, uint32_t addr, uint8_t *out, uint32_t n);

/* --------------------------------------------------------- breakpoints ---- */
/*
 * A software list backed by three of the four hardware comparators.  Every
 * change is written to the chip straight away: the block can be written with
 * the core held without releasing it, so there is nothing to defer.  A
 * comparator survives its own hit, so a breakpoint stays live across any number
 * of stops until it is removed.
 *
 * One thing does need care.  Clearing the comparator that is holding a stopped
 * core lets it go -- which is exactly what "remove the breakpoint I am stopped
 * on" would otherwise do.  tlsr_dbg_bp_remove() therefore anchors the core at
 * its current PC before releasing the slot, so removing a breakpoint never
 * starts the program.
 */
int tlsr_dbg_bp_add(tlsr_dev *d, uint32_t addr);      /* slot, or -1 if full  */
int tlsr_dbg_bp_remove(tlsr_dev *d, uint32_t addr);   /* 1 removed, 0 absent  */
int tlsr_dbg_bp_toggle(tlsr_dev *d, uint32_t addr, int *added);
int tlsr_dbg_bp_at(tlsr_dev *d, uint32_t addr);       /* is one set here?     */
int tlsr_dbg_bp_count(tlsr_dev *d);
int tlsr_dbg_bp_list(tlsr_dev *d, uint32_t *out, int max);
int tlsr_dbg_bp_clear_all(tlsr_dev *d);

/* Push the whole software list into the hardware.  Called after anything that
 * might have disturbed the block -- a reset, or reconnecting to a session. */
int tlsr_dbg_bp_sync(tlsr_dev *d);

/* --------------------------------------------------------- run / stop ----- */
/* Hold the core.  Leaky: an interrupt breaks it within about half a second, so
 * this is only a way of finding out where the core is.  Use tlsr_dbg_pause()
 * for a stop that stays. */
int tlsr_dbg_halt(tlsr_dev *d);

/* Let the core run, with whatever comparators are armed still live. */
int tlsr_dbg_resume(tlsr_dev *d);
int tlsr_dbg_go(tlsr_dev *d);           /* 0x0602 <- 0x84                     */

/* Stop the core somewhere it will stay.  Stall to find out roughly where it
 * is, arm that address, and let it come back to it, so the stop comes from a
 * comparator.  Falls back to the plain stall for code that does not run again,
 * and says so. */
int tlsr_dbg_pause(tlsr_dev *d, tlsr_regs *out, tlsr_log_fn log, void *user);

/* Resume and wait for any armed comparator to fire.  Returns TLSR_OK with the
 * core held, or TLSR_E_TIMEOUT with it still running -- which callers should
 * say out loud, since the breakpoints stay armed and may fire later. */
int tlsr_dbg_continue(tlsr_dev *d, unsigned timeout_ms, tlsr_regs *out,
                      tlsr_log_fn log, void *user);

/* Run until addr is fetched, then hold.  Uses a scratch comparator, so it does
 * not disturb the user's breakpoints -- and it stops at one of those instead if
 * that comes first. */
int tlsr_dbg_run_to(tlsr_dev *d, uint32_t addr, unsigned timeout_ms,
                    tlsr_regs *out, tlsr_log_fn log, void *user);

/* -------------------------------------------------------------- stepping -- */
/* The raw hardware step: Telink's 0x0602/0x0613 sequence, one instruction.
 * Lands on the IRQ vector whenever an interrupt is taken, which on this
 * firmware is often -- prefer tlsr_dbg_step(). */
int tlsr_dbg_step_insn(tlsr_dev *d);

/* One instruction of the code you are actually reading.  Wraps the raw step: a
 * landing on the IRQ vector is recovered by running the handler out to LR,
 * which is exactly where the step should have arrived.  A taken branch is a
 * legitimate result and is never "recovered".
 *
 * *skipped, if given, receives how many interrupts were run through. */
int tlsr_dbg_step(tlsr_dev *d, tlsr_regs *out, int *skipped,
                  tlsr_log_fn log, void *user);

/* Decode the instruction at addr well enough to step it.  Reads the bytes off
 * the target unless a host-side image covers the address.  Returns 0 if the
 * instruction could not be fetched. */
int tlsr_dbg_insn(tlsr_dev *d, uint32_t addr, int *len, int *is_call,
                  uint32_t *call_target);

/* One instruction, entering calls.  Identical to tlsr_dbg_step(): a hardware
 * step follows a call into the callee by itself.  Kept as its own name because
 * that is what the button means. */
int tlsr_dbg_step_into(tlsr_dev *d, unsigned timeout_ms, tlsr_regs *out,
                       tlsr_log_fn log, void *user);

/* One instruction, running any call to completion: stop at the instruction
 * after the call rather than descending into it.  Recognises a call from the
 * encoding, and also catches a register-indirect call after the fact by
 * noticing that LR now points just past the instruction we stepped. */
int tlsr_dbg_step_over(tlsr_dev *d, unsigned timeout_ms, tlsr_regs *out,
                       tlsr_log_fn log, void *user);

/* Run to the address in LR: finish the current function and stop just after
 * the call that entered it. */
int tlsr_dbg_step_out(tlsr_dev *d, unsigned timeout_ms, tlsr_regs *out,
                      tlsr_log_fn log, void *user);

/* --------------------------------------------------------------- extras --- */
/* Read n bytes of stack starting at SP.  Returns how many were read. */
int tlsr_dbg_read_stack(tlsr_dev *d, uint32_t sp, uint8_t *out, uint32_t n);

/* Sample the PC while the core runs: a statistical profiler. */
int tlsr_dbg_profile(tlsr_dev *d, uint32_t *hits, int n, unsigned gap_ms);

/* An optional host-side copy of the flashed image.  Only a shortcut now that
 * code can be read off the chip: it saves a SWire round trip per decode.  The
 * buffer is borrowed, not copied, and must outlive the calls. */
void tlsr_dbg_set_code(tlsr_dev *d, const uint8_t *img, uint32_t len, uint32_t base);
int  tlsr_dbg_have_code(tlsr_dev *d);

#endif /* TLSR_DEBUG_H */
