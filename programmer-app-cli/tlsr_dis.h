/*
 * tlsr_dis.h - TC32 disassembler.
 *
 * Copyright (C) 2026  telink_project contributors
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * TC32 is Telink's Thumb-1 clone: 16-bit instructions throughout, except the
 * two-halfword call. The opcode table here is the one the vendor's own objdump
 * carries, so the mnemonics match what tc32-elf-objdump prints -- which is
 * deliberate, since the point of a disassembly view is to be comparable with
 * the listings the toolchain produces.
 *
 * Note the assembler accepts slightly different spellings than objdump prints
 * (objdump says `tadds`, the assembler wants `tadd`). This prints the objdump
 * form, because it is for reading, not for feeding back in.
 */
#ifndef TLSR_DIS_H
#define TLSR_DIS_H

#include <stdint.h>

/* Decode one instruction.
 *
 *   code/len  a buffer of code bytes
 *   base      the address code[0] corresponds to
 *   addr      the address to decode
 *   out/outsz where the text goes
 *
 * Returns the instruction length in bytes (2 or 4), or 0 if addr is outside the
 * buffer. Anything undecodable comes out as ".short 0x...." and still returns 2,
 * so a caller can always walk forward. */
int tlsr_dis_one(const uint8_t *code, uint32_t len, uint32_t base,
                 uint32_t addr, char *out, int outsz);

/* Length only, without formatting -- for walking. Same return convention. */
int tlsr_dis_len(const uint8_t *code, uint32_t len, uint32_t base, uint32_t addr);

/* If the instruction at addr is a branch or call with a computable target,
 * store it in *target and return 1. Otherwise return 0. */
int tlsr_dis_target(const uint8_t *code, uint32_t len, uint32_t base,
                    uint32_t addr, uint32_t *target);

#endif /* TLSR_DIS_H */
