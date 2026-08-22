/*
 * tlsr_dis.c - TC32 disassembler.
 *
 * Copyright (C) 2026  telink_project contributors
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * The table is the vendor objdump's, transcribed. Each entry is
 * {width, value, mask, format}; the first entry whose masked bits match wins,
 * so order matters and 32-bit forms are tried before 16-bit ones.
 *
 * The format language is objdump's own:
 *   %A-Br   register from bits A..B          %A-Bd   decimal
 *   %A-BW   value * 4                        %A-BH   value * 2
 *   %A-Ba   pc-relative literal address      %A-BB   branch target
 *   %A-Bc   condition name
 *   %B      32-bit call target               %S      register in bits 3..6
 *   %D      high register (bits 0..2 + bit7) %N/%O/%M  register lists
 *   %s      shift amount, 0 meaning 32       %C      the 's' flag suffix
 *   %A?xy   'x' if bit A set else 'y'        %A'x    'x' if bit A set
 *   %c      nothing (an unconditional slot)
 */
#include <stdio.h>
#include <string.h>

#include "tlsr_dis.h"

struct insn { int w; uint32_t val, mask; const char *fmt; };

static const struct insn TAB[] = {
    { 32, 0x9000c000u, 0xf800d000u, "tjlex%c\t%B" },
    { 32, 0x90009800u, 0xf800f800u, "tjl%c\t%B" },
    { 16, 0x46c0u, 0xffffu, "tnop%c" },
    { 16, 0x0000u, 0xffc0u, "tand%C\t%0-2r, %3-5r" },
    { 16, 0x0040u, 0xffc0u, "txor%C\t%0-2r, %3-5r" },
    { 16, 0x0080u, 0xffc0u, "tshftl%C\t%0-2r, %3-5r" },
    { 16, 0x00c0u, 0xffc0u, "tshftr%C\t%0-2r, %3-5r" },
    { 16, 0x0100u, 0xffc0u, "tasr%C\t%0-2r, %3-5r" },
    { 16, 0x0140u, 0xffc0u, "taddc%C\t%0-2r, %3-5r" },
    { 16, 0x0180u, 0xffc0u, "tsubc%C\t%0-2r, %3-5r" },
    { 16, 0x01c0u, 0xffc0u, "trotr%C\t%0-2r, %3-5r" },
    { 16, 0x0200u, 0xffc0u, "tnand%c\t%0-2r, %3-5r" },
    { 16, 0x0240u, 0xffc0u, "tneg%C\t%0-2r, %3-5r" },
    { 16, 0x0280u, 0xffc0u, "tcmp%c\t%0-2r, %3-5r" },
    { 16, 0x02c0u, 0xffc0u, "tcmpn%c\t%0-2r, %3-5r" },
    { 16, 0x0300u, 0xffc0u, "tor%C\t%0-2r, %3-5r" },
    { 16, 0x0340u, 0xffc0u, "tmul%C\t%0-2r, %3-5r" },
    { 16, 0x0380u, 0xffc0u, "tbclr%C\t%0-2r, %3-5r" },
    { 16, 0x03c0u, 0xffc0u, "tmovn%C\t%0-2r, %3-5r" },
    { 16, 0x6bc0u, 0xfff8u, "tmcsr%c\t%0-2r" },
    { 16, 0x6bc8u, 0xfff8u, "tmrcs%c\t%0-2r" },
    { 16, 0x6bd0u, 0xfff8u, "tmssr%c\t%0-2r" },
    { 16, 0x6bd8u, 0xfff8u, "tmrss%c\t%0-2r" },
    { 16, 0x6800u, 0xfe00u, "treti\t%O" },
    { 16, 0x6000u, 0xff80u, "tadd%c\tsp, #%0-6W" },
    { 16, 0x6080u, 0xff80u, "tsub%c\tsp, #%0-6W" },
    { 16, 0x0700u, 0xff80u, "tjex%c\t%S" },
    { 16, 0x0400u, 0xff00u, "tadd%c\t%D, %S" },
    { 16, 0x0500u, 0xff00u, "tcmp%c\t%D, %S" },
    { 16, 0x0600u, 0xff00u, "tmov%c\t%D, %S" },
    { 16, 0x6400u, 0xfe00u, "tpush%c\t%N" },
    { 16, 0x6c00u, 0xfe00u, "tpop%c\t%O" },
    { 16, 0xe800u, 0xfe00u, "tadd%C\t%0-2r, %3-5r, %6-8r" },
    { 16, 0xea00u, 0xfe00u, "tsub%C\t%0-2r, %3-5r, %6-8r" },
    { 16, 0xec00u, 0xfe00u, "tadd%C\t%0-2r, %3-5r, #%6-8d" },
    { 16, 0xee00u, 0xfe00u, "tsub%C\t%0-2r, %3-5r, #%6-8d" },
    { 16, 0x1200u, 0xfe00u, "tstorerh%c\t%0-2r, [%3-5r, %6-8r]" },
    { 16, 0x1a00u, 0xfe00u, "tloadrh%c\t%0-2r, [%3-5r, %6-8r]" },
    { 16, 0x1600u, 0xf600u, "tloadrs%11?hb%c\t%0-2r, [%3-5r, %6-8r]" },
    { 16, 0x1000u, 0xfa00u, "tstorer%10'b%c\t%0-2r, [%3-5r, %6-8r]" },
    { 16, 0x1800u, 0xfa00u, "tloadr%10'b%c\t%0-2r, [%3-5r, %6-8r]" },
    { 16, 0xf000u, 0xf800u, "tshftl%C\t%0-2r, %3-5r, #%6-10d" },
    { 16, 0xf800u, 0xf800u, "tshftr%C\t%0-2r, %3-5r, %s" },
    { 16, 0xe000u, 0xf800u, "tasr%C\t%0-2r, %3-5r, %s" },
    { 16, 0xa000u, 0xf800u, "tmov%C\t%8-10r, #%0-7d" },
    { 16, 0xa800u, 0xf800u, "tcmp%c\t%8-10r, #%0-7d" },
    { 16, 0xb000u, 0xf800u, "tadd%C\t%8-10r, #%0-7d" },
    { 16, 0xb800u, 0xf800u, "tsub%C\t%8-10r, #%0-7d" },
    { 16, 0x0800u, 0xf800u, "tloadr%c\t%8-10r, [pc, #%0-7W]\t; (%0-7a)" },
    { 16, 0x5000u, 0xf800u, "tstorer%c\t%0-2r, [%3-5r, #%6-10W]" },
    { 16, 0x5800u, 0xf800u, "tloadr%c\t%0-2r, [%3-5r, #%6-10W]" },
    { 16, 0x4000u, 0xf800u, "tstorerb%c\t%0-2r, [%3-5r, #%6-10d]" },
    { 16, 0x4800u, 0xf800u, "tloadrb%c\t%0-2r, [%3-5r, #%6-10d]" },
    { 16, 0x2000u, 0xf800u, "tstorerh%c\t%0-2r, [%3-5r, #%6-10H]" },
    { 16, 0x2800u, 0xf800u, "tloadrh%c\t%0-2r, [%3-5r, #%6-10H]" },
    { 16, 0x3000u, 0xf800u, "tstorer%c\t%8-10r, [sp, #%0-7W]" },
    { 16, 0x3800u, 0xf800u, "tloadr%c\t%8-10r, [sp, #%0-7W]" },
    { 16, 0x7000u, 0xf800u, "tadd%c\t%8-10r, pc, #%0-7W" },
    { 16, 0x7800u, 0xf800u, "tadd%c\t%8-10r, sp, #%0-7W" },
    { 16, 0xd000u, 0xf800u, "tstorem%c\t%8-10r!, %M" },
    { 16, 0xd800u, 0xf800u, "tloadm%c\t%8-10r!, %M" },
    { 16, 0xcf00u, 0xff00u, "tserv%c\t%0-7d" },
    { 16, 0xc000u, 0xf000u, "tj%8-11c.n\t%0-7B" },
    { 16, 0x8000u, 0xf800u, "tj%c.n\t%0-10B" },
    { 0, 0, 0, NULL }
};

static const char *const COND[16] = {
    "eq","ne","cs","cc","mi","pl","vs","vc",
    "hi","ls","ge","lt","gt","le","","nv"
};
static const char *const REG[16] = {
    "r0","r1","r2","r3","r4","r5","r6","r7",
    "r8","r9","r10","r11","r12","sp","lr","pc"
};

static uint32_t bits(uint32_t v, int lo, int hi)
{
    return (v >> lo) & ((1u << (hi - lo + 1)) - 1u);
}

static int32_t sx(uint32_t v, int n)
{
    return (v & (1u << (n - 1))) ? (int32_t)v - (int32_t)(1u << n) : (int32_t)v;
}

/* Append, never overflowing. */
static void app(char *out, int outsz, int *pos, const char *s)
{
    while (*s && *pos < outsz - 1)
        out[(*pos)++] = *s++;
    out[*pos] = 0;
}

static void appn(char *out, int outsz, int *pos, long v, int hex)
{
    char t[24];
    if (hex) sprintf(t, "0x%lx", v);
    else     sprintf(t, "%ld", v);
    app(out, outsz, pos, t);
}

static void reglist(char *out, int outsz, int *pos, uint32_t v, const char *extra)
{
    int i, first = 1;
    app(out, outsz, pos, "{");
    for (i = 0; i < 8; i++)
        if (v & (1u << i)) {
            if (!first) app(out, outsz, pos, ", ");
            app(out, outsz, pos, REG[i]);
            first = 0;
        }
    if (extra && (v & 0x100u)) {
        if (!first) app(out, outsz, pos, ", ");
        app(out, outsz, pos, extra);
    }
    app(out, outsz, pos, "}");
}

/* Find the table entry for the word at addr; returns entry index or -1. */
static int match(const uint8_t *code, uint32_t len, uint32_t base, uint32_t addr,
                 uint32_t *given, int *width)
{
    uint32_t off, lo16, hi32;
    int i;

    if (addr < base) return -1;
    off = addr - base;
    if (off + 2 > len) return -1;
    lo16 = (uint32_t)code[off] | ((uint32_t)code[off + 1] << 8);

    if (off + 4 <= len) {
        hi32 = (lo16 << 16) |
               ((uint32_t)code[off + 2] | ((uint32_t)code[off + 3] << 8));
        for (i = 0; TAB[i].fmt; i++)
            if (TAB[i].w == 32 && (hi32 & TAB[i].mask) == TAB[i].val) {
                *given = hi32; *width = 4; return i;
            }
    }
    for (i = 0; TAB[i].fmt; i++)
        if (TAB[i].w == 16 && (lo16 & TAB[i].mask) == TAB[i].val) {
            *given = lo16; *width = 2; return i;
        }
    *given = lo16; *width = 2;
    return -1;
}

int tlsr_dis_len(const uint8_t *code, uint32_t len, uint32_t base, uint32_t addr)
{
    uint32_t g; int w;
    if (addr < base || addr - base + 2 > len) return 0;
    (void)match(code, len, base, addr, &g, &w);
    return w;
}

int tlsr_dis_target(const uint8_t *code, uint32_t len, uint32_t base,
                    uint32_t addr, uint32_t *target)
{
    uint32_t g; int w, i;
    i = match(code, len, base, addr, &g, &w);
    if (i < 0) return 0;
    if (w == 4) {                                  /* tjl / tjlex */
        uint32_t hh = g >> 16, ll = g & 0xffffu;
        int32_t off = (int32_t)(((hh & 0x7ffu) << 12) | ((ll & 0x7ffu) << 1));
        if (target) *target = addr + 4 + (uint32_t)sx((uint32_t)off, 23);
        return 1;
    }
    if ((g & 0xf000u) == 0xc000u) {                /* conditional */
        if (target) *target = addr + 4 + (uint32_t)(sx(bits(g, 0, 7), 8) << 1);
        return 1;
    }
    if ((g & 0xf800u) == 0x8000u) {                /* unconditional */
        if (target) *target = addr + 4 + (uint32_t)(sx(bits(g, 0, 10), 11) << 1);
        return 1;
    }
    return 0;
}

int tlsr_dis_one(const uint8_t *code, uint32_t len, uint32_t base,
                 uint32_t addr, char *out, int outsz)
{
    uint32_t given = 0, pc;
    int width = 2, idx, pos = 0;
    const char *f;

    if (!out || outsz < 8) return 0;
    out[0] = 0;
    if (addr < base || addr - base + 2 > len) return 0;

    idx = match(code, len, base, addr, &given, &width);
    if (idx < 0) {
        app(out, outsz, &pos, ".short ");
        appn(out, outsz, &pos, (long)(given & 0xffffu), 1);
        return 2;
    }
    pc = addr + 4;
    f = TAB[idx].fmt;

    while (*f && pos < outsz - 1) {
        if (*f != '%') {
            char t[2];
            t[0] = (*f == '\t') ? ' ' : *f;
            t[1] = 0;
            app(out, outsz, &pos, t);
            f++;
            continue;
        }
        f++;
        if (*f >= '0' && *f <= '9') {              /* %A-Bx or %A'x or %A?xy */
            int a = 0, b;
            while (*f >= '0' && *f <= '9') a = a * 10 + (*f++ - '0');
            if (*f == '-') {
                f++;
                b = 0;
                while (*f >= '0' && *f <= '9') b = b * 10 + (*f++ - '0');
                {
                    uint32_t v = bits(given, a, b);
                    char c = *f++;
                    switch (c) {
                    case 'r': app(out, outsz, &pos, REG[v & 15]); break;
                    case 'd': appn(out, outsz, &pos, (long)v, 0); break;
                    case 'W': appn(out, outsz, &pos, (long)v * 4, 0); break;
                    case 'H': appn(out, outsz, &pos, (long)v * 2, 0); break;
                    case 'c': app(out, outsz, &pos, COND[v & 15]); break;
                    case 'a': appn(out, outsz, &pos,
                                   (long)(((pc & ~3u) + v * 4)), 1); break;
                    case 'B': {
                        int n = b - a + 1;
                        uint32_t dst = pc + (uint32_t)(sx(v, n) << 1);
                        appn(out, outsz, &pos, (long)dst, 1);
                        break;
                    }
                    case '?': {
                        char t[2];
                        t[0] = ((given >> a) & 1) ? f[0] : f[1];
                        t[1] = 0;
                        app(out, outsz, &pos, t);
                        f += 2;
                        break;
                    }
                    default: break;
                    }
                }
            } else if (*f == '\'') {
                char t[2];
                f++;
                if ((given >> a) & 1) { t[0] = *f; t[1] = 0; app(out, outsz, &pos, t); }
                f++;
            } else if (*f == '?') {
                char t[2];
                f++;
                t[0] = ((given >> a) & 1) ? f[0] : f[1];
                t[1] = 0;
                app(out, outsz, &pos, t);
                f += 2;
            }
            continue;
        }
        switch (*f++) {
        case 'c': break;                            /* unconditional slot */
        case 'C': app(out, outsz, &pos, "s"); break;
        case 'S': app(out, outsz, &pos, REG[bits(given, 3, 6) & 15]); break;
        case 'D': app(out, outsz, &pos,
                      REG[((given & 7u) | ((given & 0x80u) >> 4)) & 15]); break;
        case 'N': reglist(out, outsz, &pos, given, "lr"); break;
        case 'O': reglist(out, outsz, &pos, given, "pc"); break;
        case 'M': reglist(out, outsz, &pos, given, NULL); break;
        case 's': {
            uint32_t v = bits(given, 6, 10);
            app(out, outsz, &pos, "#");
            appn(out, outsz, &pos, (long)(v ? v : 32), 0);
            break;
        }
        case 'B': {
            uint32_t hh = given >> 16, ll = given & 0xffffu;
            uint32_t off = ((hh & 0x7ffu) << 12) | ((ll & 0x7ffu) << 1);
            uint32_t dst = pc + (uint32_t)sx(off, 23);
            if ((given & 0xf800f800u) != 0x90009800u) dst &= ~3u;
            appn(out, outsz, &pos, (long)dst, 1);
            break;
        }
        default: break;
        }
    }
    return width;
}
