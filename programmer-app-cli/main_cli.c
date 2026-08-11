/*
 * main_cli.c - command line front end for the TLSR825x programmer.
 *
 * Copyright (C) 2026  telink_project contributors
 * SPDX-License-Identifier: GPL-2.0-only
 *
 *   programmer_cli.exe -p COM10 --info
 *   programmer_cli.exe -p COM10 --flash -f firmware.bin
 *   programmer_cli.exe -p COM10 --dump  -f dump.bin
 *
 * Anything that touches the chip powers it up and activates it first, so there
 * is no ordering ritual to remember.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>

#include "tlsr_core.h"
#include "tlsr_debug.h"

#define PROG "programmer_cli"

/* ------------------------------------------------------------- reporting -- */
static int g_quiet;
static int g_last_pct = -1;

static void cli_log(void *user, const char *line)
{
    (void)user;
    if (!g_quiet)
        printf("  %s\n", line);
}

/* A single self-overwriting progress line; only redrawn when the whole
 * percent changes so piping the output stays readable. */
static void cli_progress(void *user, uint32_t done, uint32_t total)
{
    int pct;
    (void)user;
    if (g_quiet || !total)
        return;
    pct = (int)((100.0 * done) / total);
    if (pct == g_last_pct)
        return;
    g_last_pct = pct;
    printf("\r  [%-40s] %3d%%  %u/%u bytes",
           "########################################" + (40 - pct * 40 / 100),
           pct, (unsigned)done, (unsigned)total);
    fflush(stdout);
    if (done >= total) {
        printf("\n");
        g_last_pct = -1;
    }
}

static int fail(const char *what)
{
    fprintf(stderr, "%s: %s\n", what, tlsr_last_error());
    return 1;
}

static void usage(void)
{
    printf(
"%s - Telink TLSR825x programmer over the Blue Pill SWire bridge\n"
"\n"
"USAGE\n"
"  %s -p PORT ACTION [OPTIONS]\n"
"\n"
"CONNECTION\n"
"  -p, --port PORT        bridge COM port, e.g. COM10\n"
"      --list             list COM ports and exit\n"
"\n"
"ACTIONS\n"
"      --info             identify bridge and target, print capabilities\n"
"      --dump             read flash to a file     (needs -f)\n"
"      --flash            erase+program+verify a file into flash (needs -f)\n"
"      --verify           compare flash against a file (needs -f)\n"
"      --erase            erase flash sectors      (-a, -n)\n"
"      --read             hex dump memory or flash (-a, -n)\n"
"      --write HEX        write bytes to memory    (-a), e.g. --write \"A5 5A\"\n"
"      --halt             stop the target core\n"
"      --run              resume the target core\n"
"      --reset            reset the target and let it run\n"
"      --power on|off     switch target power\n"
"      --activate         re-open the SWire link without touching anything\n"
"      --selftest         bridge loopback check (no target needed)\n"
"      --pintest          PA6/PA7 continuity check through the 750R\n"
"      --cfg              print the bridge's SWire timing\n"
"      --set-cfg K=V,..   change it, e.g. --set-cfg spi_div=2,cell=6\n"
"\n"
"DEBUGGER (see the note under --regs; this chip does not answer)\n"
"      --regs             dump the CPU register file\n"
"      --stall            debug-halt the core (0x0602 <- 0x06)\n"
"      --continue         resume with the breakpoint comparator live\n"
"      --step [N]         execute N instructions, default 1\n"
"      --step-over        step, but run through a call rather than into it\n"
"      --step-out         run to the address in LR (finish this function)\n"
"      --bp ADDR|off      arm or clear the hardware breakpoint\n"
"      --runto ADDR       resume and wait for the PC to reach ADDR\n"
"      --dbg-probe        report whether the CPU debug block answers at all\n"
"\n"
"OPTIONS\n"
"  -f, --file PATH        binary file for --dump/--flash/--verify\n"
"  -a, --addr ADDR        start address, decimal or 0x hex      (default 0)\n"
"  -n, --length N         byte count                            (default 256)\n"
"      --offset N         start N bytes into the file (--flash/--verify)\n"
"      --region NAME      flash | sram | reg          (--read/--write, default flash)\n"
"      --rail both|pb0|mosfet    which supply to switch          (default both)\n"
"      --timeout MS       how long --runto waits               (default 5000)\n"
"      --frames N         activation frames for --activate       (default 600)\n"
"      --no-erase         --flash: skip the erase stage.  Expert: NOR flash\n"
"                         only clears bits, so the sectors must be erased\n"
"      --no-verify        --flash/--write: skip the read-back check\n"
"  -q, --quiet            only print results and errors\n"
"  -h, --help             this text\n"
"\n"
"EXAMPLES\n"
"  %s -p COM10 --info\n"
"  %s -p COM10 --dump -f backup.bin\n"
"  %s -p COM10 --flash -f repaired.bin\n"
"  %s -p COM10 --flash -f image.bin --offset 0x1000 -n 0x1000 -a 0x1000\n"
"  %s -p COM10 --read -a 0x040000 -n 64 --region sram\n"
"  %s -p COM10 --write \"DE AD BE EF\" -a 0x040000 --region sram\n"
"  %s -p COM10 --dbg-probe\n"
"\n"
"SAFETY\n"
"  If the target is a mains device, it must be unplugged from mains.  Such\n"
"  supplies are usually not isolated, so bridging one to a USB-grounded\n"
"  programmer while live is dangerous.\n",
    /* One PROG per %s above: the program name, the USAGE line, and one per
     * example.  Keep these in step when editing the help text. */
    PROG, PROG, PROG, PROG, PROG, PROG, PROG, PROG, PROG);
}

/* ---------------------------------------------------------------- helpers -- */
static int parse_num(const char *s, uint32_t *out)
{
    char *end = NULL;
    unsigned long v;

    if (!s || !*s)
        return -1;
    v = strtoul(s, &end, 0);          /* 0 => honours 0x and plain decimal */
    if (end == s || (end && *end))
        return -1;
    *out = (uint32_t)v;
    return 0;
}

static uint8_t *read_file(const char *path, uint32_t *len)
{
    FILE *f = fopen(path, "rb");
    long size;
    uint8_t *buf;

    if (!f)
        return NULL;
    fseek(f, 0, SEEK_END);
    size = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (size <= 0) {
        fclose(f);
        return NULL;
    }
    buf = (uint8_t *)malloc((size_t)size);
    if (!buf) {
        fclose(f);
        return NULL;
    }
    if (fread(buf, 1, (size_t)size, f) != (size_t)size) {
        free(buf);
        fclose(f);
        return NULL;
    }
    fclose(f);
    *len = (uint32_t)size;
    return buf;
}

static void hexdump(const uint8_t *data, uint32_t n, uint32_t base)
{
    uint32_t off;
    for (off = 0; off < n; off += 16) {
        uint32_t i, line = (n - off > 16) ? 16 : (n - off);
        printf("%08X  ", (unsigned)(base + off));
        for (i = 0; i < 16; i++) {
            if (i < line) printf("%02X ", data[off + i]);
            else          printf("   ");
        }
        printf(" |");
        for (i = 0; i < line; i++) {
            uint8_t c = data[off + i];
            putchar((c >= 32 && c < 127) ? c : '.');
        }
        printf("|\n");
    }
}

static void print_info(const tlsr_info *in)
{
    char hex[64];

    printf("bridge      : %s  firmware %u.%u\n", in->bridge_id,
           in->bridge_version >> 8, in->bridge_version & 0xFF);
    printf("target power: %s\n",
           in->rails == 0 ? "off" :
           in->rails == TLSR_RAIL_PB0 ? "PB0 only" :
           in->rails == TLSR_RAIL_MOSFET ? "MOSFET only" : "PB0 + MOSFET");
    if (!in->identified) {
        printf("target      : no answer\n");
        return;
    }
    printf("chip id     : 0x%04X  (%s)\n", in->chip_id, in->chip_name);
    printf("flash       : %s %s  %u KB\n",
           tlsr_hex(hex, sizeof hex, in->flash_jedec, 3),
           in->flash_vendor, (unsigned)(in->flash_size / 1024));
    printf("sram        : %u KB at 0x%06X\n",
           (unsigned)(in->sram_size / 1024), TLSR_SRAM_BASE);
    printf("swire div   : 0x%02X\n", in->swire_div);
    printf("flash[0..15]: %s\n", tlsr_hex(hex, sizeof hex, in->flash_head, 16));
    printf("\nsupported   : halt/resume/reset, read+write registers and SRAM,\n"
           "              read/erase/program flash, target power control\n");
    printf("unsupported : PC readback, r0-r14, single-step, breakpoints\n"
           "              (not exposed by this chip's SWire interface)\n");
}

/* ------------------------------------------------------- bridge timing --- */
/* Same order as the fields of tlsr_cfg, which is what makes the byte-wise
 * walk below legal. */
#define NUM_CFG 9
static const char *const g_cfg_names[NUM_CFG] = {
    "spi_div", "cell", "low0", "low1", "thr",
    "addr_bytes", "slave_bits", "slave_off", "slack"
};

static void print_cfg(const tlsr_cfg *c)
{
    const uint8_t *p = (const uint8_t *)c;
    int i;
    for (i = 0; i < NUM_CFG; i++)
        printf("%-11s: %u\n", g_cfg_names[i], p[i]);
    printf("\nsample clock: %u MHz  (72 / 2^(spi_div+1))\n",
           72u / (1u << (c->spi_div + 1)));
}

/* "spi_div=2,cell=6".  Returns 0, or -1 after explaining what was wrong. */
static int parse_cfg_spec(const char *spec, tlsr_cfg *c)
{
    uint8_t *p = (uint8_t *)c;
    const char *s = spec;

    while (*s) {
        const char *eq, *end;
        size_t klen;
        uint32_t v;
        char num[16];
        int i, found = -1;

        while (*s == ',' || *s == ' ')
            s++;
        if (!*s)
            break;
        eq = strchr(s, '=');
        if (!eq) {
            fprintf(stderr, "%s: '%s' is not KEY=VALUE\n", PROG, s);
            return -1;
        }
        klen = (size_t)(eq - s);
        for (i = 0; i < NUM_CFG; i++)
            if (strlen(g_cfg_names[i]) == klen &&
                !strncmp(g_cfg_names[i], s, klen)) {
                found = i;
                break;
            }
        if (found < 0) {
            fprintf(stderr, "%s: unknown timing field '%.*s'\n",
                    PROG, (int)klen, s);
            return -1;
        }
        end = strchr(eq + 1, ',');
        if (!end)
            end = eq + 1 + strlen(eq + 1);
        if ((size_t)(end - eq - 1) >= sizeof num) {
            fprintf(stderr, "%s: value for %s is too long\n",
                    PROG, g_cfg_names[found]);
            return -1;
        }
        memcpy(num, eq + 1, (size_t)(end - eq - 1));
        num[end - eq - 1] = 0;
        if (parse_num(num, &v) || v > 255) {
            fprintf(stderr, "%s: bad value '%s' for %s\n",
                    PROG, num, g_cfg_names[found]);
            return -1;
        }
        p[found] = (uint8_t)v;
        s = end;
    }
    return 0;
}

/* ------------------------------------------------------------ debugger --- */
/* Printed before any debug action, because on the TLSR8253 in these outlets
 * the block does not answer and a wall of zeros otherwise reads like a bug in
 * this tool rather than a property of the chip. */
static void dbg_note(void)
{
    if (g_quiet)
        return;
    printf("note: the CPU debug block (0x0610/0x0613/0x0680) is pvvx's map for\n"
           "      TLSR825x.  Measured on TLSR8253 it does not answer - see\n"
           "      --dbg-probe.  Zeros below mean the chip, not a failed read.\n");
}

static void print_regs(const tlsr_regs *r)
{
    int i;
    for (i = 0; i < TLSR_DBG_NNAMED; i++)
        printf("  %-9s 0x%08X  %d\n", tlsr_dbg_reg_name(i),
               (unsigned)r->r[i], (int)r->r[i]);
    printf("  %-9s 0x%08X  (mul32*32)>>32\n",
           tlsr_dbg_reg_name(TLSR_DBG_IDX_M64),
           (unsigned)r->r[TLSR_DBG_IDX_M64]);
}

/* ------------------------------------------------------------------- main -- */
int main(int argc, char **argv)
{
    const char *port = NULL, *file = NULL, *write_hex = NULL;
    const char *action = NULL, *region = "flash", *rail_s = "both", *power_s = NULL;
    const char *bp_s = NULL, *cfg_spec = NULL;
    uint32_t addr = 0, length = 0, offset = 0, timeout_ms = 5000, steps = 1;
    uint32_t frames = TLSR_ACTIVATE_FRAMES;
    unsigned stages = TLSR_PROG_ALL;   /* --no-erase / --no-verify clear bits */
    int i, rc = 0;
    uint8_t rail = TLSR_RAIL_BOTH;
    tlsr_dev *d = NULL;

    if (argc < 2) {
        usage();
        return 1;
    }

    for (i = 1; i < argc; i++) {
        const char *a = argv[i];
        #define NEXT(var) do {                                            \
                if (i + 1 >= argc) {                                      \
                    fprintf(stderr, "%s: %s needs a value\n", PROG, a);    \
                    return 1;                                             \
                }                                                         \
                (var) = argv[++i];                                        \
            } while (0)

        if      (!strcmp(a, "-h") || !strcmp(a, "--help")) { usage(); return 0; }
        else if (!strcmp(a, "-q") || !strcmp(a, "--quiet")) g_quiet = 1;
        /* Stage switches, not actions -- they must be tested before the
         * generic "--xxx is an action" branch below would swallow them. */
        else if (!strcmp(a, "--no-erase"))  stages &= ~TLSR_PROG_ERASE;
        else if (!strcmp(a, "--no-verify")) stages &= ~TLSR_PROG_VERIFY;
        else if (!strcmp(a, "-p") || !strcmp(a, "--port")) NEXT(port);
        else if (!strcmp(a, "-f") || !strcmp(a, "--file")) NEXT(file);
        else if (!strcmp(a, "--region")) NEXT(region);
        else if (!strcmp(a, "--rail"))   NEXT(rail_s);
        else if (!strcmp(a, "--write"))   { NEXT(write_hex); action = "write"; }
        else if (!strcmp(a, "--power"))   { NEXT(power_s);   action = "power"; }
        else if (!strcmp(a, "--bp"))      { NEXT(bp_s);      action = "bp"; }
        else if (!strcmp(a, "--set-cfg")) { NEXT(cfg_spec);  action = "set-cfg"; }
        else if (!strcmp(a, "--runto")) {
            const char *v; NEXT(v);
            if (parse_num(v, &addr)) { fprintf(stderr, "%s: bad address '%s'\n", PROG, v); return 1; }
            action = "runto";
        }
        /* --step takes an optional count, so only swallow the next argument
         * when it is a number rather than the next switch. */
        else if (!strcmp(a, "--step")) {
            action = "step";
            if (i + 1 < argc && argv[i + 1][0] != '-') {
                if (parse_num(argv[++i], &steps) || !steps) {
                    fprintf(stderr, "%s: bad step count '%s'\n", PROG, argv[i]);
                    return 1;
                }
            }
        }
        else if (!strcmp(a, "--timeout")) {
            const char *v; NEXT(v);
            if (parse_num(v, &timeout_ms)) { fprintf(stderr, "%s: bad timeout '%s'\n", PROG, v); return 1; }
        }
        else if (!strcmp(a, "--frames")) {
            const char *v; NEXT(v);
            if (parse_num(v, &frames) || !frames || frames > 65535) {
                fprintf(stderr, "%s: activation frames must be 1..65535, not '%s'\n",
                        PROG, v);
                return 1;
            }
        }
        else if (!strcmp(a, "-a") || !strcmp(a, "--addr")) {
            const char *v; NEXT(v);
            if (parse_num(v, &addr)) { fprintf(stderr, "%s: bad address '%s'\n", PROG, v); return 1; }
        }
        else if (!strcmp(a, "-n") || !strcmp(a, "--length")) {
            const char *v; NEXT(v);
            if (parse_num(v, &length)) { fprintf(stderr, "%s: bad length '%s'\n", PROG, v); return 1; }
        }
        else if (!strcmp(a, "--offset")) {
            const char *v; NEXT(v);
            if (parse_num(v, &offset)) { fprintf(stderr, "%s: bad offset '%s'\n", PROG, v); return 1; }
        }
        else if (!strncmp(a, "--", 2)) {
            if (action && strcmp(action, "write") && strcmp(action, "power")) {
                fprintf(stderr, "%s: %s conflicts with --%s\n", PROG, a, action);
                return 1;
            }
            action = a + 2;
        }
        else {
            fprintf(stderr, "%s: unexpected argument '%s'\n", PROG, a);
            return 1;
        }
        #undef NEXT
    }

    if (action && !strcmp(action, "list")) {
        char names[32][16];
        int n = tlsr_list_ports(names, 32), k;
        if (!n) {
            printf("no COM ports found\n");
        } else {
            printf("COM ports (from the registry; an entry can be stale if a\n"
                   "device was unplugged uncleanly):\n");
            for (k = 0; k < n; k++)
                printf("  %s\n", names[k]);
        }
        return 0;
    }

    if (!action) { fprintf(stderr, "%s: no action given (try --help)\n", PROG); return 1; }
    if (!port)   { fprintf(stderr, "%s: no port given, use -p COMx (or --list)\n", PROG); return 1; }

    if      (!strcmp(rail_s, "pb0"))    rail = TLSR_RAIL_PB0;
    else if (!strcmp(rail_s, "mosfet")) rail = TLSR_RAIL_MOSFET;
    else                                rail = TLSR_RAIL_BOTH;

    d = tlsr_open(port);
    if (!d)
        return fail("open");

    if (!g_quiet)
        printf("connected to %s\n", port);

    /* ---- actions that do not need the target ---------------------------- */
    if (!strcmp(action, "selftest")) {
        uint8_t echo[8]; int ok = 0; char h[32];
        tlsr_power(d, 1, rail); Sleep(200);
        rc = tlsr_selftest(d, echo, &ok);
        if (rc != TLSR_OK) { rc = fail("selftest"); goto done; }
        printf("loopback   : %s\n", ok ? "PASS" : "FAIL");
        printf("echoed hdr : %s\n", tlsr_hex(h, sizeof h, echo, 5));
        if (!ok)
            printf("the bridge could not decode its own frame - check the 750R\n"
                   "and the PA6/PA7 joints before suspecting the target\n");
        rc = ok ? 0 : 1;
        goto done;
    }
    if (!strcmp(action, "pintest")) {
        uint8_t lv[3]; int ok = 0;
        tlsr_power(d, 1, rail); Sleep(200);
        rc = tlsr_pintest(d, lv, &ok);
        if (rc != TLSR_OK) { rc = fail("pintest"); goto done; }
        printf("PA7 low  -> PA6 = %u  (expect 0)\n", lv[0]);
        printf("PA7 high -> PA6 = %u  (expect 1)\n", lv[1]);
        printf("released -> PA6 = %u\n", lv[2]);
        printf("%s\n", ok ? "PASS" : "FAIL - PA6 is not following PA7");
        rc = ok ? 0 : 1;
        goto done;
    }
    if (!strcmp(action, "power")) {
        int on = power_s && (!strcmp(power_s, "on") || !strcmp(power_s, "1"));
        rc = tlsr_power(d, on, rail);
        if (rc != TLSR_OK) { rc = fail("power"); goto done; }
        printf("target power %s\n", on ? "on" : "off");
        goto done;
    }
    if (!strcmp(action, "cfg")) {
        tlsr_cfg c;
        uint8_t rails = 0;
        rc = tlsr_get_cfg(d, &c, &rails);
        if (rc != TLSR_OK) { rc = fail("cfg"); goto done; }
        print_cfg(&c);
        rc = 0;
        goto done;
    }
    if (!strcmp(action, "set-cfg")) {
        tlsr_cfg c;
        uint8_t rails = 0;
        /* Read first, so a spec that names two fields leaves the other seven
         * exactly as the bridge had them. */
        rc = tlsr_get_cfg(d, &c, &rails);
        if (rc != TLSR_OK) { rc = fail("cfg"); goto done; }
        if (parse_cfg_spec(cfg_spec, &c)) { rc = 1; goto done; }
        rc = tlsr_set_cfg(d, &c);
        if (rc != TLSR_OK) { rc = fail("set-cfg"); goto done; }
        print_cfg(&c);
        printf("\napplied - this is not persistent, a replug restores defaults\n");
        rc = 0;
        goto done;
    }

    /* ---- everything below needs a live target --------------------------- */
    rc = tlsr_ensure_target(d, cli_log, NULL);
    if (rc != TLSR_OK) { rc = fail("target"); goto done; }

    if (!strcmp(action, "info")) {
        tlsr_info in;
        rc = tlsr_identify(d, &in, cli_log, NULL);
        print_info(&in);
        rc = (rc == TLSR_OK) ? 0 : 1;
    }
    else if (!strcmp(action, "halt")) {
        rc = tlsr_halt(d) == TLSR_OK ? 0 : fail("halt");
        if (!rc) printf("core halted\n");
    }
    else if (!strcmp(action, "run")) {
        rc = tlsr_run(d) == TLSR_OK ? 0 : fail("run");
        if (!rc) printf("core resumed\n");
    }
    else if (!strcmp(action, "reset")) {
        rc = tlsr_reboot(d) == TLSR_OK ? 0 : fail("reset");
        if (!rc) printf("target reset and running\n");
    }
    else if (!strcmp(action, "activate")) {
        /* tlsr_ensure_target above already opened the link; repeat the burst
         * with the requested frame count so --frames means something, and so
         * this stays the one action whose whole job is re-opening the link. */
        rc = tlsr_activate(d, (uint16_t)frames);
        if (rc != TLSR_OK) { rc = fail("activate"); goto done; }
        printf("SWire link is open (%u frames)\n", (unsigned)frames);
        rc = 0;
    }
    else if (!strcmp(action, "dbg-probe")) {
        int present = 0;
        rc = tlsr_dbg_present(d, &present, cli_log, NULL);
        if (rc != TLSR_OK) { rc = fail("dbg-probe"); goto done; }
        printf("CPU debug block: %s\n", present ? "responds" : "no response");
        if (!present)
            printf("  0x0680 (register file) and 0x06BC (PC latch) both read\n"
                   "  zero with the core held, so --regs, --step, --bp and\n"
                   "  --runto have nothing to work with on this part.\n");
        rc = present ? 0 : 1;
    }
    else if (!strcmp(action, "regs")) {
        tlsr_regs r;
        dbg_note();
        /* Reads are only trustworthy with the core held; a running core makes
         * every byte come back bit-shifted. */
        if (tlsr_halt(d) != TLSR_OK) { rc = fail("halt"); goto done; }
        rc = tlsr_dbg_read_regs(d, &r);
        if (rc != TLSR_OK) { rc = fail("regs"); goto done; }
        print_regs(&r);
        rc = 0;
    }
    else if (!strcmp(action, "stall")) {
        dbg_note();
        rc = tlsr_dbg_stall(d) == TLSR_OK ? 0 : fail("stall");
        if (!rc) {
            uint8_t st = 0;
            printf("wrote 0x06 to 0x0602\n");
            if (tlsr_read8(d, TLSR_REG_CPU_CTRL, &st) == TLSR_OK) {
                printf("reads back 0x%02X - %s\n", st,
                       st == TLSR_CPU_STALL ? "stalled"
                                            : "NOT stalled, the write did not take");
                if (st != TLSR_CPU_STALL)
                    rc = 1;
            }
        }
    }
    else if (!strcmp(action, "continue")) {
        dbg_note();
        rc = tlsr_dbg_go(d) == TLSR_OK ? 0 : fail("continue");
        if (!rc)
            printf("running, breakpoint comparator live\n"
                   "  with the core running SWire reads are unreliable, and the\n"
                   "  firmware may take the SWS pad - recovering then needs a\n"
                   "  power cycle and --activate\n");
    }
    else if (!strcmp(action, "step-over")) {
        tlsr_regs r;
        dbg_note();
        rc = tlsr_dbg_step_over(d, timeout_ms, &r, cli_log, NULL);
        if (rc != TLSR_OK) { rc = fail("step-over"); goto done; }
        printf("PC=0x%06X\n", (unsigned)(r.r[TLSR_DBG_IDX_PC] & 0xFFFFFF));
        rc = 0;
    }
    else if (!strcmp(action, "step-out")) {
        tlsr_regs r;
        dbg_note();
        rc = tlsr_dbg_step_out(d, timeout_ms, &r, cli_log, NULL);
        if (rc != TLSR_OK) { rc = fail("step-out"); goto done; }
        printf("PC=0x%06X\n", (unsigned)(r.r[TLSR_DBG_IDX_PC] & 0xFFFFFF));
        rc = 0;
    }
    else if (!strcmp(action, "step")) {
        tlsr_regs r;
        uint32_t k;
        dbg_note();
        if (tlsr_dbg_stall(d) != TLSR_OK) { rc = fail("stall"); goto done; }
        for (k = 0; k < steps; k++)
            if (tlsr_dbg_step(d) != TLSR_OK) { rc = fail("step"); goto done; }
        if (tlsr_dbg_read_regs(d, &r) != TLSR_OK) { rc = fail("regs"); goto done; }
        printf("stepped %u instruction(s), PC=0x%06X\n", (unsigned)steps,
               (unsigned)(r.r[TLSR_DBG_IDX_PC] & 0xFFFFFF));
        rc = 0;
    }
    else if (!strcmp(action, "bp")) {
        int arm = strcmp(bp_s, "off") != 0;
        uint32_t bp = 0;
        dbg_note();
        if (arm && parse_num(bp_s, &bp)) {
            fprintf(stderr, "%s: --bp wants an address or 'off', not '%s'\n",
                    PROG, bp_s);
            rc = 1; goto done;
        }
        rc = tlsr_dbg_set_bp(d, bp, arm) == TLSR_OK ? 0 : fail("bp");
        if (!rc)
            printf(arm ? "breakpoint armed at 0x%06X\n"
                       : "breakpoint cleared\n", (unsigned)(bp & 0xFFFFFF));
    }
    else if (!strcmp(action, "runto")) {
        tlsr_regs r;
        dbg_note();
        if (tlsr_dbg_set_bp(d, addr, 1) != TLSR_OK) { rc = fail("bp"); goto done; }
        printf("running to 0x%06X (up to %u ms) ...\n",
               (unsigned)(addr & 0xFFFFFF), (unsigned)timeout_ms);
        rc = tlsr_dbg_go_and_wait(d, addr, timeout_ms, &r, cli_log, NULL);
        if (rc == TLSR_OK)
            print_regs(&r);
        rc = (rc == TLSR_OK) ? 0 : 1;
    }
    else if (!strcmp(action, "read")) {
        uint8_t *buf;
        if (!length) length = 256;
        buf = (uint8_t *)malloc(length);
        if (!buf) { fprintf(stderr, "out of memory\n"); rc = 1; goto done; }
        if (!strcmp(region, "flash"))
            rc = tlsr_flash_read(d, addr, buf, length, cli_progress, NULL);
        else
            rc = tlsr_read(d, addr, buf, length);
        if (rc != TLSR_OK) { free(buf); rc = fail("read"); goto done; }
        if (file) {
            FILE *o = fopen(file, "wb");
            if (!o) { fprintf(stderr, "cannot write %s\n", file); free(buf); rc = 1; goto done; }
            fwrite(buf, 1, length, o);
            fclose(o);
            printf("%u bytes -> %s\n", (unsigned)length, file);
        } else {
            hexdump(buf, length, addr);
        }
        free(buf);
        rc = 0;
    }
    else if (!strcmp(action, "write")) {
        uint8_t buf[256];
        int n = tlsr_parse_hex(write_hex, buf, sizeof buf);
        if (n <= 0) { fprintf(stderr, "%s: could not parse hex '%s'\n", PROG, write_hex); rc = 1; goto done; }
        if (!strcmp(region, "flash")) {
            fprintf(stderr, "%s: flash cannot be written byte-wise; it must be\n"
                            "erased in %u-byte sectors first. Use --flash with a\n"
                            "file, which erases, programs and verifies.\n",
                    PROG, (unsigned)TLSR_FLASH_SECTOR);
            rc = 1; goto done;
        }
        rc = tlsr_write(d, addr, buf, (uint32_t)n);
        if (rc != TLSR_OK) { rc = fail("write"); goto done; }
        printf("wrote %d bytes at 0x%06X\n", n, (unsigned)addr);
        rc = 0;
        /* Read back by default, the same as the GUI's Memory tab.  Registers
         * in particular do not all hold what you wrote. */
        if (stages & TLSR_PROG_VERIFY) {
            uint8_t back[256];
            if (tlsr_read(d, addr, back, (uint32_t)n) != TLSR_OK) {
                fprintf(stderr, "read-back failed: %s\n", tlsr_last_error());
                rc = 1;
            } else if (memcmp(back, buf, (size_t)n) != 0) {
                char h[3 * 256 + 1];
                fprintf(stderr, "read-back DIFFERS: %s\n",
                        tlsr_hex(h, sizeof h, back, (size_t)n));
                rc = 1;
            } else {
                printf("read-back matches\n");
            }
        }
    }
    else if (!strcmp(action, "dump")) {
        tlsr_info in;
        uint8_t *buf;
        uint32_t size;
        FILE *o;
        if (!file) { fprintf(stderr, "%s: --dump needs -f FILE\n", PROG); rc = 1; goto done; }
        if (tlsr_identify(d, &in, cli_log, NULL) != TLSR_OK) { rc = fail("identify"); goto done; }
        size = length ? length : in.flash_size;
        buf = (uint8_t *)malloc(size);
        if (!buf) { fprintf(stderr, "out of memory\n"); rc = 1; goto done; }
        printf("reading %u KB of flash from 0x%06X\n",
               (unsigned)(size / 1024), (unsigned)addr);
        rc = tlsr_flash_read(d, addr, buf, size, cli_progress, NULL);
        if (rc != TLSR_OK) { free(buf); rc = fail("dump"); goto done; }
        o = fopen(file, "wb");
        if (!o) { fprintf(stderr, "cannot write %s\n", file); free(buf); rc = 1; goto done; }
        fwrite(buf, 1, size, o);
        fclose(o);
        free(buf);
        printf("%u bytes -> %s\n", (unsigned)size, file);
        rc = 0;
    }
    else if (!strcmp(action, "flash") || !strcmp(action, "verify")) {
        uint32_t flen = 0, n;
        uint8_t *blob;
        if (!file) { fprintf(stderr, "%s: --%s needs -f FILE\n", PROG, action); rc = 1; goto done; }
        blob = read_file(file, &flen);
        if (!blob) { fprintf(stderr, "%s: cannot read %s\n", PROG, file); rc = 1; goto done; }
        if (offset >= flen) {
            fprintf(stderr, "%s: --offset 0x%X is past the end of %s (%u bytes)\n",
                    PROG, (unsigned)offset, file, (unsigned)flen);
            free(blob); rc = 1; goto done;
        }
        n = length ? length : (flen - offset);
        if (offset + n > flen)
            n = flen - offset;

        if (!strcmp(action, "verify")) {
            uint32_t bad = 0;
            printf("verifying %u bytes at 0x%06X against %s+0x%X\n",
                   (unsigned)n, (unsigned)addr, file, (unsigned)offset);
            rc = tlsr_flash_verify(d, addr, blob + offset, n, &bad,
                                   cli_progress, NULL);
            if (rc == TLSR_OK) printf("verify OK\n");
            else               fprintf(stderr, "%s\n", tlsr_last_error());
        } else {
            printf("programming %u bytes at 0x%06X from %s+0x%X\n",
                   (unsigned)n, (unsigned)addr, file, (unsigned)offset);
            rc = tlsr_flash_program_ex(d, addr, blob + offset, n, stages,
                                       cli_progress, cli_log, NULL);
            if (rc == TLSR_OK)
                printf("programmed%s%s\n",
                       (stages & TLSR_PROG_ERASE) ? "" : " (no erase)",
                       (stages & TLSR_PROG_VERIFY) ? " and verified"
                                                   : " (not verified)");
            else
                fprintf(stderr, "%s\n", tlsr_last_error());
        }
        free(blob);
        rc = (rc == TLSR_OK) ? 0 : 1;
    }
    else if (!strcmp(action, "erase")) {
        if (!length) length = TLSR_FLASH_SECTOR;
        printf("erasing %u bytes at 0x%06X\n", (unsigned)length, (unsigned)addr);
        rc = tlsr_flash_erase(d, addr, length, cli_progress, cli_log, NULL);
        if (rc != TLSR_OK) { rc = fail("erase"); goto done; }
        printf("erased\n");
        rc = 0;
    }
    else {
        fprintf(stderr, "%s: unknown action --%s (try --help)\n", PROG, action);
        rc = 1;
    }

done:
    tlsr_close(d);
    return rc;
}
