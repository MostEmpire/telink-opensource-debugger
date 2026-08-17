/*
 * main_gui.c - Win32 front end for the TLSR825x programmer.
 *
 * Copyright (C) 2026  telink_project contributors
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * Plain Win32: a main window, a tab control and common controls created in
 * code.  No frameworks, no dialog resources, no external dependencies beyond
 * comctl32/comdlg32 -- the same toolkit the Sysinternals utilities use.
 *
 * Threading rule, which the whole file depends on:
 *
 *   - the bridge is NOT thread safe, so every operation runs on one worker
 *     thread, one at a time;
 *   - the worker never touches a window, and the UI thread never touches the
 *     bridge.  The worker appends to a log buffer and publishes state (progress,
 *     power rails) under a critical section; a 100 ms timer on the UI thread
 *     moves those into the controls.
 *
 * That is why long operations (a 512 KB dump takes ~25 s) keep the window
 * responsive, narrate progress, and cannot race the worker for the serial port.
 */
#include <windows.h>
#include <commctrl.h>
#include <commdlg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "tlsr_core.h"
#include "tlsr_debug.h"
#include "res.h"
#include "version.h"

/* APP_TITLE captions the message boxes, where a version number would just be
 * noise repeated on every prompt.  APP_CAPTION carries it where it belongs:
 * the title bar and the first line of the log, which is what someone reads
 * when asked "which build are you running?". */
#define APP_TITLE   "TLSR825x Programmer"
#define APP_CAPTION APP_TITLE " " APP_VERSION

/* ---------------------------------------------------------- control ids --- */
enum {
    IDC_PORT = 1000, IDC_RESCAN, IDC_CONNECT, IDC_STATUS, IDC_POWER,
    IDC_PROGRESS, IDC_VERDICT, IDC_TABS, IDC_LOG,

    IDC_T_PROBE = 1100, IDC_T_PWRON, IDC_T_PWROFF, IDC_T_HALT, IDC_T_RUN,
    IDC_T_RESET, IDC_T_IDENT, IDC_T_CAPS, IDC_T_REGIONS,

    IDC_M_REGION = 1200, IDC_M_MODE, IDC_M_ADDR, IDC_M_LEN, IDC_M_READ,
    IDC_M_SAVE, IDC_M_PATCH, IDC_M_WRITE, IDC_M_OUT, IDC_M_NOTE,
    IDC_M_PATCHLBL, IDC_M_L1, IDC_M_L2, IDC_M_L3,
    /* Read / Write group frames and the two stage checkboxes. */
    IDC_M_GRPR, IDC_M_GRPW, IDC_M_DOERASE, IDC_M_DOVERIFY,

    IDC_F_MODE = 1300, IDC_F_ADDR, IDC_F_LEN, IDC_F_READ, IDC_F_DUMP,
    IDC_F_ERASE, IDC_F_FILE, IDC_F_BROWSE, IDC_F_OFF, IDC_F_PLEN, IDC_F_PADDR,
    IDC_F_PROG, IDC_F_VERIFY, IDC_F_OUT, IDC_F_NOTE, IDC_F_L1, IDC_F_L2,
    IDC_F_L3, IDC_F_L4, IDC_F_L5, IDC_F_L6,
    IDC_F_GRPR, IDC_F_GRPW, IDC_F_DOERASE, IDC_F_DOVERIFY, IDC_F_DUMPRNG,

    IDC_B_SELF = 1400, IDC_B_PIN, IDC_B_RAIL, IDC_B_ACT, IDC_B_GO,
    IDC_B_REFRESH, IDC_B_APPLY, IDC_B_L1, IDC_B_L2, IDC_B_HINT,
    IDC_B_CFG0,  /* nine consecutive edits follow */

    /* Labels that used to be created with id 0.  GetDlgItem() cannot find a
     * control whose id is 0, so show_ids() never revealed them and they stayed
     * invisible for the life of the window. */
    IDC_PORTLBL = 1450,
    IDC_B_CFGL0 = 1460,          /* nine consecutive cfg name labels */

    /* --- Debugger tab -------------------------------------------------- */
    IDC_D_CONTINUE = 1500, IDC_D_PAUSE, IDC_D_STOP, IDC_D_RESTART,
    IDC_D_STEPINTO, IDC_D_STEPOVER, IDC_D_STEPOUT, IDC_D_STEPINST,
    IDC_D_RUNCUR, IDC_D_BPTOGGLE, IDC_D_BPCLEAR, IDC_D_REFRESH,
    IDC_D_ADDR, IDC_D_ADDRLBL, IDC_D_STATE, IDC_D_REGS, IDC_D_NOTE
};

/* How long a run-to-breakpoint waits before giving up and saying the core is
 * still running.  The CLI's --timeout is the same knob. */
#define DBG_WAIT_MS 5000

#define NUM_CFG 9
static const char *g_cfg_names[NUM_CFG] = {
    "spi_div", "cell", "low0", "low1", "thr",
    "addr_bytes", "slave_bits", "slave_off", "slack"
};

/* Memory regions offered in the Memory tab. */
enum { REGION_FLASH = 0, REGION_SRAM = 1, REGION_REGS = 2 };

/* Range entry: a byte count, or the address the range ends at. */
enum { MODE_SIZE = 0, MODE_END = 1 };

/* ------------------------------------------------------------- app state -- */
static HINSTANCE g_inst;
static HWND      g_main, g_tabs, g_log;
static HFONT     g_font, g_mono, g_bold;
static tlsr_dev *g_dev;
static tlsr_info g_info;
static int       g_have_info;

/* Shared between the UI thread and the worker, guarded by g_lock. */
static CRITICAL_SECTION g_lock;
static char   g_logbuf[64 * 1024];
static size_t g_loglen;
static int    g_prog_pct = -1;
static int    g_busy;
static int    g_rails = -1;        /* -1 unknown, else a TLSR_RAIL_* mask */

/* Which phase of an operation the worker is in, and how a finished write ended.
 * Both are published here rather than painted directly: the worker must not
 * touch a window, so the 100 ms UI timer is what turns these into colour.
 *
 * g_verdict_seq exists because two runs in a row can end the same way -- the
 * value alone would not tell the UI thread that a new result had arrived, and
 * the PASS/FAIL would keep the timeout of the previous one. */
static int      g_stage = TLSR_STAGE_IDLE;
static int      g_verdict;              /* 0 none, 1 PASS, 2 FAIL          */
static unsigned g_verdict_seq;

/* --- debugger state.  Written by the worker, read by the UI thread after a
 * WM_APP+2; the values are plain words so a torn read is not possible. */
static HWND      g_tip;                    /* shared tooltip control          */

/* The two faces of the target-power button, loaded once and swapped as the
 * rails change.  Owned for the life of the process, so they are never freed. */
static HICON     g_ico_pwr_on, g_ico_pwr_off;
static tlsr_regs g_dbg;                    /* .valid says the read worked     */
static int       g_dbg_halted;
static int       g_dbg_bp_armed;
static uint32_t  g_dbg_bp_addr;

/* Measured on outlet #1, 2026-08-09 (see debugger/PROTOCOL.md "BENCH RESULT").
 * On TLSR8253 the CPU debug block does not answer: 0x0680 reads as all zeros,
 * 0x06BC reads zero, and 0x0602 <- 0x06 does not stall the core.  Anything that
 * needs a readable PC therefore cannot work, so those buttons start disabled.
 * They switch on by themselves if a register read ever returns real content --
 * that way a future firmware or entry sequence needs no code change here. */
static int       g_dbg_hw_ok;

static const int g_dbg_needs_pc[] = {
    IDC_D_PAUSE, IDC_D_STEPINTO, IDC_D_STEPOVER, IDC_D_STEPOUT, IDC_D_STEPINST,
    IDC_D_RUNCUR, IDC_D_BPTOGGLE, IDC_D_BPCLEAR, 0
};

/* A job is a function plus a copy of whatever parameters it needs; copying
 * them means the worker never reads a control while the user edits it. */
typedef struct {
    void (*fn)(void);
    char     path[MAX_PATH];
    char     text[512];
    uint32_t addr, len, off;
    int      flag;
    unsigned stages;         /* TLSR_PROG_* from the Erase/Verify checkboxes */
} job_t;

static job_t  g_job;
static HANDLE g_job_evt;

/* ------------------------------------------------------------- log helper -- */
static void ui_log(const char *fmt, ...)
{
    char line[512];
    va_list ap;
    size_t n;

    va_start(ap, fmt);
    vsnprintf(line, sizeof line - 2, fmt, ap);
    va_end(ap);
    n = strlen(line);
    line[n++] = '\r';
    line[n++] = '\n';

    EnterCriticalSection(&g_lock);
    if (g_loglen + n < sizeof g_logbuf) {
        memcpy(g_logbuf + g_loglen, line, n);
        g_loglen += n;
    }
    LeaveCriticalSection(&g_lock);
}

static void core_log(void *user, const char *line)
{
    (void)user;
    ui_log("  %s", line);
}

static void core_progress(void *user, uint32_t done, uint32_t total)
{
    int pct;
    (void)user;
    if (!total)
        return;
    pct = (int)((100.0 * done) / total);
    EnterCriticalSection(&g_lock);
    g_prog_pct = pct;
    LeaveCriticalSection(&g_lock);
}

/* Throttled narration so a long transfer reads as progress, not a hang.
 * Touched only by the worker thread, so it needs no lock. */
static DWORD       g_narrate_next;
static const char *g_narrate_label;

/* The core calls this as it moves between erase, program and verify; the timer
 * turns it into the bar colour.  Called on the worker thread. */
static void core_stage(void *user, int stage)
{
    (void)user;
    EnterCriticalSection(&g_lock);
    g_stage = stage;
    /* Each stage restarts at nothing done, so drop the previous stage's fill
     * rather than letting a full bar sit there until the first callback. */
    g_prog_pct = 0;
    LeaveCriticalSection(&g_lock);

    /* Rename the narration with the stage, and give the new stage its own
     * throttle window.  Without this a program run reports "programming:
     * 8192/16384" while it is in fact erasing. */
    switch (stage) {
    case TLSR_STAGE_READ:    g_narrate_label = "reading";     break;
    case TLSR_STAGE_ERASE:   g_narrate_label = "erasing";     break;
    case TLSR_STAGE_PROGRAM: g_narrate_label = "programming"; break;
    case TLSR_STAGE_VERIFY:  g_narrate_label = "verifying";   break;
    default: return;                       /* idle: leave the label alone */
    }
    g_narrate_next = GetTickCount() + 2000;
}

static void set_stage(int stage) { core_stage(NULL, stage); }

/* The outcome of a write, for the PASS/FAIL under the bar.  Only operations
 * that change the target report one -- a read has nothing to pass or fail. */
static void set_verdict(int pass)
{
    EnterCriticalSection(&g_lock);
    g_verdict = pass ? 1 : 2;
    g_verdict_seq++;
    LeaveCriticalSection(&g_lock);
}

static void narrate_begin(const char *label)
{
    g_narrate_label = label;
    g_narrate_next  = GetTickCount() + 2000;
}

static void narrate_progress(void *user, uint32_t done, uint32_t total)
{
    core_progress(user, done, total);
    if (done && done < total && GetTickCount() >= g_narrate_next) {
        g_narrate_next = GetTickCount() + 2000;
        ui_log("    %s: %u/%u bytes (%u%%)", g_narrate_label,
               (unsigned)done, (unsigned)total,
               (unsigned)((100.0 * done) / total));
    }
}

/* The worker publishes the rail state; only it may talk to the bridge. */
static void publish_rails(void)
{
    uint8_t rails = 0;
    int val = -1;
    if (g_dev && tlsr_get_cfg(g_dev, NULL, &rails) == TLSR_OK)
        val = rails;
    EnterCriticalSection(&g_lock);
    g_rails = val;
    LeaveCriticalSection(&g_lock);
}

/* -------------------------------------------------------- control helpers -- */
static HWND ctl(const char *cls, const char *text, DWORD style,
                int x, int y, int w, int h, int id)
{
    /* WS_CLIPSIBLINGS matters here.  The tab pages are siblings of the tab
     * control rather than children of it, so they sit on top of its client
     * area.  Without clipping, resizing repaints the tab control across that
     * area and erases the lists sitting over it -- they then stay blank
     * because nothing invalidates them afterwards. */
    HWND c = CreateWindowExA(0, cls, text, WS_CHILD | WS_CLIPSIBLINGS | style,
                             x, y, w, h, g_main, (HMENU)(INT_PTR)id,
                             g_inst, NULL);
    SendMessageA(c, WM_SETFONT, (WPARAM)g_font, TRUE);
    return c;
}

static HWND mk_button(const char *t, int x, int y, int w, int id)
{
    return ctl("BUTTON", t, BS_PUSHBUTTON | WS_TABSTOP, x, y, w, 24, id);
}
static HWND mk_static(const char *t, int x, int y, int w, int id)
{
    return ctl("STATIC", t, SS_LEFT, x, y, w, 18, id);
}
static HWND mk_edit(const char *t, int x, int y, int w, int id)
{
    return ctl("EDIT", t, WS_BORDER | ES_AUTOHSCROLL | WS_TABSTOP,
               x, y, w, 22, id);
}
static HWND mk_out(int x, int y, int w, int h, int id)
{
    HWND e = ctl("EDIT", "", WS_BORDER | WS_VSCROLL | WS_HSCROLL | ES_MULTILINE |
                 ES_READONLY | ES_AUTOVSCROLL, x, y, w, h, id);
    SendMessageA(e, WM_SETFONT, (WPARAM)g_mono, TRUE);
    return e;
}
static HWND mk_list(int x, int y, int w, int h, int id)
{
    HWND l = ctl(WC_LISTVIEWA, "", LVS_REPORT | LVS_SINGLESEL | WS_BORDER,
                 x, y, w, h, id);
    ListView_SetExtendedListViewStyle(l, LVS_EX_FULLROWSELECT | LVS_EX_GRIDLINES);
    return l;
}
static HWND mk_combo(int x, int y, int w, int id, DWORD extra)
{
    return ctl("COMBOBOX", "", CBS_DROPDOWNLIST | CBS_HASSTRINGS | WS_VSCROLL |
               WS_TABSTOP | extra, x, y, w, 200, id);
}
static HWND mk_check(const char *t, int x, int y, int w, int id, int checked)
{
    HWND c = ctl("BUTTON", t, BS_AUTOCHECKBOX | WS_TABSTOP, x, y, w, 20, id);
    SendMessageA(c, BM_SETCHECK, checked ? BST_CHECKED : BST_UNCHECKED, 0);
    return c;
}
/* A group frame telling the reader where reading stops and writing starts.
 *
 * Create these AFTER the controls they enclose.  CreateWindowEx puts each new
 * child at the BOTTOM of the z-order, so a frame made last sits behind its
 * contents -- which is what we want, and the reverse of the intuition that
 * bit us when every tab came up blank. */
static HWND mk_group(const char *t, int x, int y, int w, int h, int id)
{
    return ctl("BUTTON", t, BS_GROUPBOX, x, y, w, h, id);
}
static int is_checked(int id)
{
    return SendMessageA(GetDlgItem(g_main, id), BM_GETCHECK, 0, 0)
           == BST_CHECKED;
}

/* Take the visual style off one control.  uxtheme is loaded by hand rather
 * than linked so the build needs no extra import library, and the module stays
 * loaded because the control keeps calling back into it. */
static void unthemify(HWND c)
{
    typedef HRESULT (WINAPI *set_theme_fn)(HWND, LPCWSTR, LPCWSTR);
    static HMODULE ux;
    set_theme_fn f;

    if (!c)
        return;
    if (!ux)
        ux = LoadLibraryA("uxtheme.dll");
    if (!ux)
        return;
    f = (set_theme_fn)(void *)GetProcAddress(ux, "SetWindowTheme");
    if (f)
        f(c, L"", L"");
}

/* Stage colours.  Erase and verify are the two stages that are easy to mistake
 * for a stalled program run -- erase because the bar barely moves, verify
 * because it looks exactly like a read -- so those are the two that get a
 * colour of their own.  Everything else stays the green it has always been. */
static void set_bar_stage(int stage)
{
    COLORREF c = RGB(0, 170, 0);            /* read / program / idle       */

    if (stage == TLSR_STAGE_ERASE)  c = RGB(240, 140, 0);   /* orange      */
    if (stage == TLSR_STAGE_VERIFY) c = RGB(0, 110, 220);   /* blue        */
    SendMessageA(GetDlgItem(g_main, IDC_PROGRESS), PBM_SETBARCOLOR, 0,
                 (LPARAM)c);
}

/* A BS_ICON button shows no text at all, so an icon-only toolbar is unusable
 * without tips.  One tooltip control is created for the window and every icon
 * button registers itself with it. */
static void add_tip(HWND c, const char *tip)
{
    TOOLINFOA ti;
    if (!g_tip || !c)
        return;
    memset(&ti, 0, sizeof ti);
    ti.cbSize   = sizeof ti;
    ti.uFlags   = TTF_IDISHWND | TTF_SUBCLASS;
    ti.hwnd     = g_main;
    ti.uId      = (UINT_PTR)c;
    ti.lpszText = (LPSTR)tip;
    SendMessageA(g_tip, TTM_ADDTOOLA, 0, (LPARAM)&ti);
}

/* Replace the text of a tip already registered by add_tip().  The power button
 * is icon-only and replaced a label that named the live rails, so the tip is
 * where that detail went; a static string could not carry it. */
static void set_tip(HWND c, const char *text)
{
    TOOLINFOA ti;
    if (!g_tip || !c)
        return;
    memset(&ti, 0, sizeof ti);
    ti.cbSize   = sizeof ti;
    ti.uFlags   = TTF_IDISHWND | TTF_SUBCLASS;
    ti.hwnd     = g_main;
    ti.uId      = (UINT_PTR)c;
    ti.lpszText = (LPSTR)text;
    SendMessageA(g_tip, TTM_UPDATETIPTEXTA, 0, (LPARAM)&ti);
}

static HWND mk_iconbtn(int resid, int x, int y, int id, const char *tip)
{
    HWND  b  = ctl("BUTTON", "", BS_PUSHBUTTON | BS_ICON | WS_TABSTOP,
                   x, y, 32, 32, id);
    HICON ic = (HICON)LoadImageA(g_inst, MAKEINTRESOURCEA(resid), IMAGE_ICON,
                                 24, 24, LR_DEFAULTCOLOR);
    if (ic)
        SendMessageA(b, BM_SETIMAGE, IMAGE_ICON, (LPARAM)ic);
    add_tip(b, tip);
    return b;
}

static void cb_add(int id, const char *s)
{
    SendMessageA(GetDlgItem(g_main, id), CB_ADDSTRING, 0, (LPARAM)s);
}
static void cb_sel(int id, int i)
{
    SendMessageA(GetDlgItem(g_main, id), CB_SETCURSEL, i, 0);
}
static int cb_cur(int id)
{
    return (int)SendMessageA(GetDlgItem(g_main, id), CB_GETCURSEL, 0, 0);
}

static void lv_column(HWND l, int i, const char *title, int width)
{
    LVCOLUMNA c;
    memset(&c, 0, sizeof c);
    c.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_SUBITEM;
    c.pszText = (LPSTR)title;
    c.cx = width;
    c.iSubItem = i;
    ListView_InsertColumn(l, i, &c);
}

static void lv_row(HWND l, int row, const char *a, const char *b, const char *c)
{
    LVITEMA it;
    memset(&it, 0, sizeof it);
    it.mask = LVIF_TEXT;
    it.iItem = row;
    it.pszText = (LPSTR)a;
    ListView_InsertItem(l, &it);
    if (b) ListView_SetItemText(l, row, 1, (LPSTR)b);
    if (c) ListView_SetItemText(l, row, 2, (LPSTR)c);
}

static void set_text(int id, const char *s) { SetWindowTextA(GetDlgItem(g_main, id), s); }
static void get_text(int id, char *out, int max) { GetWindowTextA(GetDlgItem(g_main, id), out, max); }

static int get_num(int id, uint32_t *out)
{
    char buf[64], *end = NULL;
    unsigned long v;
    get_text(id, buf, sizeof buf);
    if (!buf[0])
        return -1;
    v = strtoul(buf, &end, 0);
    if (end == buf)
        return -1;
    *out = (uint32_t)v;
    return 0;
}

/* ------------------------------------------------------- value list parser -- */
/* Accepts "0x1F", "31", "0b00011111" and any mix of them separated by spaces
 * or commas.  Each value becomes one byte, written to consecutive addresses. */
static int parse_values(const char *text, uint8_t *out, size_t max,
                        char *err, size_t errlen)
{
    size_t count = 0;
    const char *p = text;

    while (*p) {
        char tok[64];
        size_t k = 0;
        unsigned long v;
        char *end = NULL;
        int base = 10;

        while (*p == ' ' || *p == '\t' || *p == ',')
            p++;
        if (!*p)
            break;
        while (*p && *p != ' ' && *p != '\t' && *p != ',' && k < sizeof tok - 1)
            tok[k++] = *p++;
        tok[k] = 0;

        if (tok[0] == '0' && (tok[1] == 'x' || tok[1] == 'X')) {
            base = 16;
            v = strtoul(tok + 2, &end, 16);
        } else if (tok[0] == '0' && (tok[1] == 'b' || tok[1] == 'B')) {
            base = 2;
            v = strtoul(tok + 2, &end, 2);
        } else {
            v = strtoul(tok, &end, 10);
        }
        (void)base;

        if (end == tok || (end && *end)) {
            snprintf(err, errlen, "'%s' is not a number "
                     "(use 0x1F, 31 or 0b00011111)", tok);
            return -1;
        }
        if (v > 0xFF) {
            snprintf(err, errlen, "'%s' is %lu; each value must fit in one "
                     "byte (0-255)", tok, v);
            return -1;
        }
        if (count >= max) {
            snprintf(err, errlen, "too many values (limit %u)", (unsigned)max);
            return -1;
        }
        out[count++] = (uint8_t)v;
    }
    if (!count)
        snprintf(err, errlen, "nothing to write");
    return (int)count;
}

/* ---------------------------------------------------------- region model -- */
/* Registers are only usable once the target has been identified; before that
 * the entry is drawn greyed and cannot be selected. */
static int region_supported(int region)
{
    if (region == REGION_REGS)
        return g_have_info && g_info.identified;
    return 1;
}

static void region_extent(int region, uint32_t *base, uint32_t *size)
{
    switch (region) {
    case REGION_SRAM:
        *base = TLSR_SRAM_BASE;
        *size = TLSR_SRAM_SIZE;
        break;
    case REGION_REGS:
        *base = 0;
        *size = TLSR_REG_SPACE;
        break;
    default:
        *base = 0;
        *size = g_info.flash_size ? g_info.flash_size : 512u * 1024u;
        break;
    }
}

static const char *region_name(int region)
{
    return region == REGION_SRAM ? "SRAM" :
           region == REGION_REGS ? "Registers" : "Flash";
}

/* Turn the address plus the size/end field into a start and a length. */
static int resolve_range(int addr_id, int len_id, int mode,
                         uint32_t *addr, uint32_t *len, char *err, size_t errlen)
{
    uint32_t a = 0, v = 0;

    if (get_num(addr_id, &a)) {
        snprintf(err, errlen, "enter a start address");
        return -1;
    }
    if (get_num(len_id, &v)) {
        snprintf(err, errlen, mode == MODE_END ? "enter an end address"
                                               : "enter a size in bytes");
        return -1;
    }
    if (mode == MODE_END) {
        if (v < a) {
            snprintf(err, errlen, "end address 0x%06X is below the start "
                     "address 0x%06X", (unsigned)v, (unsigned)a);
            return -1;
        }
        *len = v - a + 1;          /* inclusive, so 0x00..0x0F is 16 bytes */
    } else {
        *len = v;
    }
    if (*len == 0) {
        snprintf(err, errlen, "the range is empty");
        return -1;
    }
    *addr = a;
    return 0;
}

/* The note under the range fields: in size mode it states how much of the
 * block is available, in end-address mode where the block ends. */
static void update_note(int note_id, int region, int mode)
{
    uint32_t base, size;
    char s[160];

    region_extent(region, &base, &size);
    if (mode == MODE_END)
        snprintf(s, sizeof s, "%s ends at 0x%06X   (starts at 0x%06X)",
                 region_name(region), (unsigned)(base + size - 1),
                 (unsigned)base);
    else
        snprintf(s, sizeof s, "%s size: %u bytes (0x%X)",
                 region_name(region), (unsigned)size, (unsigned)size);
    set_text(note_id, s);
}

/* Switching between "size" and "end address" keeps the range the user already
 * described rather than reinterpreting the number.  End addresses are
 * inclusive: 0x000000 with a size of 512 ends at 0x0001FF, because that is the
 * last byte the operation touches, and it matches the block notes elsewhere
 * ("SRAM ends at 0x04BFFF").  Sizes are shown in decimal, addresses in hex. */
static void convert_range_field(int addr_id, int len_id, int new_mode)
{
    uint32_t a = 0, v = 0;
    char buf[32];

    if (get_num(addr_id, &a) || get_num(len_id, &v))
        return;                       /* nothing sensible to convert */

    if (new_mode == MODE_END) {
        uint32_t end = v ? a + v - 1 : a;
        snprintf(buf, sizeof buf, "0x%06X", (unsigned)end);
    } else {
        uint32_t size = (v >= a) ? (v - a + 1) : 0;
        snprintf(buf, sizeof buf, "%u", (unsigned)size);
    }
    set_text(len_id, buf);
}

static void update_mode_label(int label_id, int mode)
{
    /* The "(incl.)" matters: an end address of 0x0FFF reads 4096 bytes, not
     * 4095.  Putting it in the label keeps it beside the field at any window
     * size, where a separate static would have to be squeezed or hidden. */
    set_text(label_id, mode == MODE_END ? "End addr (incl.)" : "Size");
}

/* The write uses the Address field from the Read group above, which stops
 * being obvious once the two halves are framed apart -- so the target address
 * is echoed here, inside the Write frame. */
static void update_patch_label(void)
{
    uint32_t a = 0;
    char s[80];
    if (get_num(IDC_M_ADDR, &a))
        snprintf(s, sizeof s, "Bytes -> ?");
    else
        snprintf(s, sizeof s, "Bytes -> 0x%06X", (unsigned)a);
    set_text(IDC_M_PATCHLBL, s);
}

/* --------------------------------------------------------------- worker --- */
static int need_target(void)
{
    if (!g_dev) {
        ui_log("not connected");
        return 0;
    }
    if (tlsr_ensure_target(g_dev, core_log, NULL) != TLSR_OK) {
        ui_log("! %s", tlsr_last_error());
        return 0;
    }
    return 1;
}

static DWORD WINAPI worker_main(LPVOID arg)
{
    (void)arg;
    for (;;) {
        WaitForSingleObject(g_job_evt, INFINITE);
        if (g_job.fn)
            g_job.fn();
        /* Any operation may have powered the target up on our behalf, so the
         * rail state is refreshed after every job rather than only after the
         * explicit power buttons. */
        publish_rails();
        EnterCriticalSection(&g_lock);
        g_busy = 0;
        g_prog_pct = 0;
        g_stage = TLSR_STAGE_IDLE;
        LeaveCriticalSection(&g_lock);
    }
    return 0;                       /* not reached; the loop never exits */
}

static void submit(void (*fn)(void))
{
    EnterCriticalSection(&g_lock);
    if (g_busy) {
        LeaveCriticalSection(&g_lock);
        ui_log("busy - wait for the current operation to finish");
        return;
    }
    g_busy = 1;
    g_job.fn = fn;
    LeaveCriticalSection(&g_lock);
    SetEvent(g_job_evt);
}

/* ------------------------------------------------------- identity display -- */
static void show_info(void)
{
    HWND ident = GetDlgItem(g_main, IDC_T_IDENT);
    HWND caps  = GetDlgItem(g_main, IDC_T_CAPS);
    HWND regs  = GetDlgItem(g_main, IDC_T_REGIONS);
    char v[128], h[80];
    int r = 0;

    ListView_DeleteAllItems(ident);
    ListView_DeleteAllItems(caps);
    ListView_DeleteAllItems(regs);
    if (!g_have_info)
        return;

    snprintf(v, sizeof v, "%s  fw %u.%u", g_info.bridge_id,
             g_info.bridge_version >> 8, g_info.bridge_version & 0xFF);
    lv_row(ident, r++, "Bridge", v, NULL);
    snprintf(v, sizeof v, "0x%04X", g_info.chip_id);
    lv_row(ident, r++, "Chip ID", v, NULL);
    lv_row(ident, r++, "Chip", g_info.chip_name, NULL);
    if (g_info.identified) {
        snprintf(v, sizeof v, "%s  %s",
                 tlsr_hex(h, sizeof h, g_info.flash_jedec, 3), g_info.flash_vendor);
        lv_row(ident, r++, "Flash", v, NULL);
        snprintf(v, sizeof v, "%u KB", (unsigned)(g_info.flash_size / 1024));
        lv_row(ident, r++, "Flash size", v, NULL);
        snprintf(v, sizeof v, "%u KB at 0x%06X",
                 (unsigned)(g_info.sram_size / 1024), (unsigned)TLSR_SRAM_BASE);
        lv_row(ident, r++, "SRAM", v, NULL);
        snprintf(v, sizeof v, "0x%02X", g_info.swire_div);
        lv_row(ident, r++, "SWire divider", v, NULL);
        snprintf(v, sizeof v, "0x%02X", g_info.boot_marker);
        lv_row(ident, r++, "Boot marker", v, NULL);
    }

    r = 0;
    lv_row(caps, r++, "Halt / resume / reset core",       "yes", "");
    lv_row(caps, r++, "Read + write registers and SRAM",  "yes", "");
    lv_row(caps, r++, "Read flash",                       "yes", "");
    lv_row(caps, r++, "Erase + program flash",            "yes", "");
    lv_row(caps, r++, "Target power control",             "yes", "");
    lv_row(caps, r++, "Read the program counter",         "no",
           "measured: 0x06BC reads 0x00000000 in every reachable state");
    lv_row(caps, r++, "Read r0-r15 + psr",                "no",
           "measured: all 128 bytes at 0x0680 read back zero");
    lv_row(caps, r++, "Single-step",                      "no",
           "0x0613 exists but cannot be confirmed without a readable PC");
    lv_row(caps, r++, "Hardware breakpoint",              "no",
           "0x0614 arms, but a hit cannot be detected without a PC");
    lv_row(caps, r++, "Write registers / set PC",         "no",
           "no register file is exposed to write to");
    lv_row(caps, r++, "Reliable SWire while the core runs", "no",
           "reads bit-slip: the 0x007E chip id itself comes back shifted");

    r = 0;
    lv_row(regs, r++, "SWire registers", "0x000000 - 0x0007FF", "read / write");
    snprintf(v, sizeof v, "0x%06X - 0x%06X", (unsigned)TLSR_SRAM_BASE,
             (unsigned)(TLSR_SRAM_BASE + TLSR_SRAM_SIZE - 1));
    lv_row(regs, r++, "SRAM", v, "read / write");
    snprintf(v, sizeof v, "0x000000 - 0x%06X",
             (unsigned)(g_info.flash_size ? g_info.flash_size - 1 : 0x7FFFF));
    lv_row(regs, r++, "Flash", v, "read / write / erase");
}

/* ------------------------------------------------------ register display -- */
static void dbg_update_enable(void)
{
    int i;
    for (i = 0; g_dbg_needs_pc[i]; i++)
        EnableWindow(GetDlgItem(g_main, g_dbg_needs_pc[i]), g_dbg_hw_ok);
    SetWindowTextA(GetDlgItem(g_main, IDC_D_NOTE), g_dbg_hw_ok
        ? "Debug block is answering - stepping and breakpoints enabled."
        : "No CPU debug block on this chip (0x0680 and 0x06BC read zero) - "
          "stepping and breakpoints disabled.  See the log.");
}

static void show_regs(void)
{
    HWND lv = GetDlgItem(g_main, IDC_D_REGS);
    HWND st = GetDlgItem(g_main, IDC_D_STATE);
    char hex[24], dec[24], line[96];
    int i;

    /* Any real content in r0..psr means the block answered after all; light the
     * dependent buttons up rather than keeping them off on yesterday's finding. */
    if (g_dbg.valid && !g_dbg_hw_ok) {
        for (i = 0; i <= TLSR_DBG_IDX_PSR; i++)
            if (g_dbg.r[i]) { g_dbg_hw_ok = 1; break; }
        if (g_dbg_hw_ok) {
            ui_log("debug block returned real register content - enabling "
                   "stepping and breakpoints");
            dbg_update_enable();
        }
    }

    ListView_DeleteAllItems(lv);
    if (!g_dbg.valid) {
        SetWindowTextA(st, g_dbg_halted ? "core halted - registers unavailable"
                                        : "core running");
        return;
    }
    for (i = 0; i < TLSR_DBG_NNAMED; i++) {
        snprintf(hex, sizeof hex, "0x%08X", (unsigned)g_dbg.r[i]);
        snprintf(dec, sizeof dec, "%d", (int)g_dbg.r[i]);
        lv_row(lv, i, tlsr_dbg_reg_name(i), hex, dec);
    }
    /* The 32-word block also carries a banked/shadow set and the wide
     * multiplier result; show the multiplier since it is the only one whose
     * meaning is documented. */
    snprintf(hex, sizeof hex, "0x%08X", (unsigned)g_dbg.r[TLSR_DBG_IDX_M64]);
    lv_row(lv, TLSR_DBG_NNAMED, tlsr_dbg_reg_name(TLSR_DBG_IDX_M64), hex,
           "(mul32*32)>>32");

    snprintf(line, sizeof line, "%s   PC=0x%06X   SP=0x%06X   LR=0x%06X%s",
             g_dbg_halted ? "halted" : "running",
             (unsigned)(g_dbg.r[TLSR_DBG_IDX_PC] & 0xFFFFFF),
             (unsigned)g_dbg.r[TLSR_DBG_IDX_SP],
             (unsigned)(g_dbg.r[TLSR_DBG_IDX_LR] & 0xFFFFFF),
             g_dbg_bp_armed ? "   [bp armed]" : "");
    SetWindowTextA(st, line);
}

/* ------------------------------------------------------------------ jobs --- */
static void dbg_publish(void);

static void job_probe(void)
{
    ui_log("> probing target: powering up, activating, reading identity ...");
    if (tlsr_identify(g_dev, &g_info, core_log, NULL) != TLSR_OK) {
        g_have_info = 0;
        ui_log("! %s", tlsr_last_error());
    } else {
        g_have_info = 1;
        ui_log("  %s, %u KB %s flash, boot marker 0x%02X",
               g_info.chip_name, (unsigned)(g_info.flash_size / 1024),
               g_info.flash_vendor, g_info.boot_marker);
        /* Settle the Debugger tab on measurement rather than on a default:
         * ask the chip whether the debug block answers at all. */
        if (tlsr_dbg_present(g_dev, &g_dbg_hw_ok, core_log, NULL) != TLSR_OK)
            g_dbg_hw_ok = 0;
    }
    PostMessageA(g_main, WM_APP + 1, 0, 0);
    dbg_publish();
}

static void job_power(void)
{
    int on = g_job.flag;
    uint8_t r = TLSR_RAIL_BOTH;
    int sel = g_job.len;             /* rail index copied at submit time */

    if      (sel == 1) r = TLSR_RAIL_PB0;
    else if (sel == 2) r = TLSR_RAIL_MOSFET;

    if (tlsr_power(g_dev, on, r) != TLSR_OK)
        ui_log("! %s", tlsr_last_error());
    else
        ui_log("target power %s", on ? "on" : "off");
}

/* Connecting opens the serial port and nothing else: the target is left in
 * whatever state it was in, because powering a board up is something the user
 * asks for, not a side effect of plugging in the bridge.  This job only reads
 * the rail state back so the header can report it honestly. */
static void job_link_state(void)
{
    uint8_t rails = 0;

    if (!g_dev) { ui_log("not connected"); return; }
    if (tlsr_get_cfg(g_dev, NULL, &rails) != TLSR_OK) {
        ui_log("! %s", tlsr_last_error());
        return;
    }
    if (rails)
        ui_log("target power is already on - left as it was found");
    else
        ui_log("target is NOT powered.  It powers up on the first operation "
               "that needs it, or from Target -> Power on / Re-probe.");
}

static void job_halt(void)
{
    if (!need_target()) return;
    ui_log(tlsr_halt(g_dev) == TLSR_OK ? "core halted" : "! halt failed");
}
static void job_run(void)
{
    if (!need_target()) return;
    ui_log(tlsr_run(g_dev) == TLSR_OK ? "core resumed" : "! resume failed");
}
static void job_reset(void)
{
    if (!need_target()) return;
    ui_log(tlsr_reboot(g_dev) == TLSR_OK ? "target reset and running"
                                         : "! reset failed");
}

/* ------------------------------------------------------------ debugger --- *
 * Thin wrappers over tlsr_debug.c, which is shared with the CLI so both front
 * ends drive the block identically.  The register map is pvvx's; the bench
 * result -- that this chip does not answer it -- is in debugger/PROTOCOL.md
 * and is why most of these buttons start disabled.
 */
static int g_dbg_warned;

static void dbg_warn_once(void)
{
    if (g_dbg_warned) return;
    g_dbg_warned = 1;
    ui_log("note: the debug registers (0x0610/0x0613/0x0680) come from pvvx "
           "TlsrPgm; on this chip they read back zero, so expect no answer.");
}

static void dbg_publish(void) { PostMessageA(g_main, WM_APP + 2, 0, 0); }


static int dbg_read_regs(void)
{
    if (tlsr_dbg_read_regs(g_dev, &g_dbg) != TLSR_OK) {
        ui_log("! register file read failed: %s", tlsr_last_error());
        return 0;
    }
    return 1;
}

static int dbg_stall(void)
{
    if (tlsr_dbg_stall(g_dev) != TLSR_OK) {
        ui_log("! stall failed: %s", tlsr_last_error());
        return 0;
    }
    g_dbg_halted = 1;
    return 1;
}

static int dbg_set_bp(uint32_t addr, int arm)
{
    if (tlsr_dbg_set_bp(g_dev, addr, arm) != TLSR_OK) {
        ui_log("! breakpoint write failed: %s", tlsr_last_error());
        return 0;
    }
    g_dbg_bp_armed = arm;
    g_dbg_bp_addr  = addr;
    return 1;
}

static int dbg_go_and_wait(uint32_t addr, unsigned ms)
{
    int hit;

    g_dbg_halted = 0;
    hit = tlsr_dbg_go_and_wait(g_dev, addr, ms, &g_dbg, core_log, NULL)
          == TLSR_OK;
    g_dbg_halted = hit;
    return hit;
}

static void job_dbg_pause(void)
{
    if (!need_target()) return;
    dbg_warn_once();
    if (dbg_stall() && dbg_read_regs())
        ui_log("core stalled, PC=0x%06X",
               (unsigned)(g_dbg.r[TLSR_DBG_IDX_PC] & 0xFFFFFF));
    dbg_publish();
}

static void job_dbg_continue(void)
{
    if (!need_target()) return;
    dbg_warn_once();
    if (tlsr_dbg_go(g_dev) == TLSR_OK) {
        g_dbg_halted = 0;
        ui_log(g_dbg_bp_armed ? "running (breakpoint armed at 0x%06X)"
                              : "running", (unsigned)(g_dbg_bp_addr & 0xFFFFFF));
        ui_log("  note: with the core running, SWire reads are unreliable, and "
               "the firmware may take the SWS pad - re-probe to get back in");
    } else {
        ui_log("! resume failed: %s", tlsr_last_error());
    }
    dbg_publish();
}

static void job_dbg_stop(void)
{
    if (!need_target()) return;
    if (tlsr_halt(g_dev) == TLSR_OK) {
        g_dbg_halted = 1;
        ui_log("core stopped (0x05 - the flash-safe halt, register file may "
               "read back as zero in this state)");
    } else {
        ui_log("! stop failed: %s", tlsr_last_error());
    }
    dbg_publish();
}

static void job_dbg_restart(void)
{
    if (!need_target()) return;
    if (tlsr_reboot(g_dev) == TLSR_OK) {
        g_dbg_halted = 0;
        g_dbg.valid = 0;
        ui_log("target reset and running");
    } else {
        ui_log("! restart failed: %s", tlsr_last_error());
    }
    dbg_publish();
}

static void job_dbg_step(void)
{
    if (!need_target()) return;
    dbg_warn_once();
    if (!g_dbg_halted && !dbg_stall()) return;
    if (tlsr_dbg_step(g_dev) != TLSR_OK) {
        ui_log("! step failed: %s", tlsr_last_error());
        return;
    }
    if (dbg_read_regs())
        ui_log("stepped, PC=0x%06X",
               (unsigned)(g_dbg.r[TLSR_DBG_IDX_PC] & 0xFFFFFF));
    dbg_publish();
}

/* Step over and step out both live in tlsr_debug.c, so the CLI runs exactly
 * the same heuristic.  Their bookkeeping here is only what the window needs:
 * whether the core ended up held, and whether a breakpoint is still armed. */
static void job_dbg_stepover(void)
{
    if (!need_target()) return;
    dbg_warn_once();
    if (tlsr_dbg_step_over(g_dev, DBG_WAIT_MS, &g_dbg, core_log, NULL)
        != TLSR_OK)
        ui_log("! step over: %s", tlsr_last_error());
    else
        g_dbg_halted = 1;
    dbg_publish();
}

static void job_dbg_stepout(void)
{
    if (!need_target()) return;
    dbg_warn_once();
    if (tlsr_dbg_step_out(g_dev, DBG_WAIT_MS, &g_dbg, core_log, NULL)
        != TLSR_OK)
        ui_log("! step out: %s", tlsr_last_error());
    else
        g_dbg_halted = 1;
    dbg_publish();
}

static void job_dbg_runcur(void)
{
    if (!need_target()) return;
    dbg_warn_once();
    ui_log("running to 0x%06X", (unsigned)(g_job.addr & 0xFFFFFF));
    if (dbg_set_bp(g_job.addr, 1))
        dbg_go_and_wait(g_job.addr, DBG_WAIT_MS);
    dbg_publish();
}

static void job_dbg_bptoggle(void)
{
    int arm;

    if (!need_target()) return;
    dbg_warn_once();
    arm = !(g_dbg_bp_armed && g_dbg_bp_addr == g_job.addr);
    if (dbg_set_bp(g_job.addr, arm))
        ui_log(arm ? "breakpoint armed at 0x%06X"
                   : "breakpoint cleared at 0x%06X",
               (unsigned)(g_job.addr & 0xFFFFFF));
    dbg_publish();
}

static void job_dbg_bpclear(void)
{
    if (!need_target()) return;
    if (dbg_set_bp(0, 0))
        ui_log("breakpoint block cleared");
    dbg_publish();
}

static void job_dbg_refresh(void)
{
    if (!need_target()) return;
    dbg_warn_once();
    if (dbg_read_regs())
        ui_log("register file read, PC=0x%06X SP=0x%06X LR=0x%06X",
               (unsigned)(g_dbg.r[TLSR_DBG_IDX_PC] & 0xFFFFFF),
               (unsigned)g_dbg.r[TLSR_DBG_IDX_SP],
               (unsigned)(g_dbg.r[TLSR_DBG_IDX_LR] & 0xFFFFFF));
    dbg_publish();
}

static void job_selftest(void)
{
    uint8_t echo[8];
    char h[64];
    int ok = 0;

    if (!g_dev) { ui_log("not connected"); return; }
    tlsr_power(g_dev, 1, TLSR_RAIL_BOTH);
    Sleep(200);
    if (tlsr_selftest(g_dev, echo, &ok) != TLSR_OK) {
        ui_log("! %s", tlsr_last_error());
        return;
    }
    ui_log("self-test: %s   echo %s", ok ? "PASS" : "FAIL",
           tlsr_hex(h, sizeof h, echo, 5));
    if (!ok)
        ui_log("  the bridge could not decode its own frame - check the 750R "
               "and the PA6/PA7 joints");
}

static void job_pintest(void)
{
    uint8_t lv[3];
    int ok = 0;

    if (!g_dev) { ui_log("not connected"); return; }
    tlsr_power(g_dev, 1, TLSR_RAIL_BOTH);
    Sleep(200);
    if (tlsr_pintest(g_dev, lv, &ok) != TLSR_OK) {
        ui_log("! %s", tlsr_last_error());
        return;
    }
    ui_log("pin test: PA7 low->PA6=%u (want 0), high->PA6=%u (want 1), "
           "released->PA6=%u  %s", lv[0], lv[1], lv[2], ok ? "PASS" : "FAIL");
}

/* Read memory or flash, then hexdump into the output box or save to a file. */
/* Bytes from the most recent range read, kept so "Dump range..." can save what
 * is already on screen instead of reading the target a second time.  Written
 * only by the worker, and read by the UI thread solely from the WM_APP+3 it
 * posts once it has finished -- so the two never touch it at the same time. */
static uint8_t *g_rng_buf;
static uint32_t g_rng_addr, g_rng_len;
static int      g_rng_region = -1;

static void cache_range(int region, uint32_t addr, const uint8_t *data,
                        uint32_t n)
{
    uint8_t *nb = (uint8_t *)realloc(g_rng_buf, n ? n : 1);
    if (!nb) { g_rng_region = -1; return; }   /* keep the old buffer, drop it */
    g_rng_buf = nb;
    memcpy(g_rng_buf, data, n);
    g_rng_addr   = addr;
    g_rng_len    = n;
    g_rng_region = region;
}

static int range_cached(int region, uint32_t addr, uint32_t n)
{
    return g_rng_buf && g_rng_region == region &&
           g_rng_addr == addr && g_rng_len == n;
}

static void show_hex(int out_id, uint32_t addr, const uint8_t *buf, uint32_t n)
{
    size_t cap = (size_t)((n / 16 + 2) * 80), used = 0;
    char *text = (char *)malloc(cap);
    uint32_t off;

    if (!text) { ui_log("! out of memory"); return; }
    for (off = 0; off < n; off += 16) {
        uint32_t i, line = (n - off > 16) ? 16 : (n - off);
        used += (size_t)snprintf(text + used, cap - used, "%08X  ",
                                 (unsigned)(addr + off));
        for (i = 0; i < 16; i++)
            used += (size_t)snprintf(text + used, cap - used,
                                     i < line ? "%02X " : "   ",
                                     i < line ? buf[off + i] : 0);
        used += (size_t)snprintf(text + used, cap - used, " |");
        for (i = 0; i < line; i++) {
            uint8_t c = buf[off + i];
            text[used++] = (c >= 32 && c < 127) ? (char)c : '.';
        }
        used += (size_t)snprintf(text + used, cap - used, "|\r\n");
    }
    text[used] = 0;
    SetWindowTextA(GetDlgItem(g_main, out_id), text);
    free(text);
}

static void job_read(void)
{
    uint8_t *buf;
    uint32_t addr = g_job.addr, n = g_job.len;
    int is_flash = (g_job.flag == REGION_FLASH);
    int out_id   = g_job.off ? IDC_F_OUT : IDC_M_OUT;

    if (!need_target()) return;
    buf = (uint8_t *)malloc(n);
    if (!buf) { ui_log("! out of memory"); return; }

    ui_log("> read %u bytes at 0x%06X ...", (unsigned)n, (unsigned)addr);
    set_stage(TLSR_STAGE_READ);
    narrate_begin("reading");
    if ((is_flash ? tlsr_flash_read(g_dev, addr, buf, n, narrate_progress, NULL)
                  : tlsr_read(g_dev, addr, buf, n)) != TLSR_OK) {
        ui_log("! %s", tlsr_last_error());
        free(buf);
        return;
    }

    if (g_job.path[0]) {
        FILE *f = fopen(g_job.path, "wb");
        if (!f) ui_log("! cannot write %s", g_job.path);
        else {
            fwrite(buf, 1, n, f);
            fclose(f);
            ui_log("  %u bytes -> %s", (unsigned)n, g_job.path);
        }
        free(buf);
        return;
    }

    show_hex(out_id, addr, buf, n);
    cache_range(g_job.flag, addr, buf, n);
    free(buf);
    ui_log("  read %u bytes at 0x%06X", (unsigned)n, (unsigned)addr);
}

/* Dump range: read the range first if it is not already on screen, then hand
 * back to the UI thread to ask for a filename.  The dialog cannot be opened
 * here -- it is modal on a window the UI thread owns. */
static void job_dump_range(void)
{
    uint8_t *buf;
    uint32_t addr = g_job.addr, n = g_job.len;

    if (range_cached(REGION_FLASH, addr, n)) {
        ui_log("> dump range 0x%06X..0x%06X: already read, saving that",
               (unsigned)addr, (unsigned)(addr + n - 1));
        PostMessageA(g_main, WM_APP + 3, 0, 0);
        return;
    }

    if (!need_target()) return;
    buf = (uint8_t *)malloc(n);
    if (!buf) { ui_log("! out of memory"); return; }

    ui_log("> dump range: reading %u bytes at 0x%06X first ...",
           (unsigned)n, (unsigned)addr);
    set_stage(TLSR_STAGE_READ);
    narrate_begin("reading");
    if (tlsr_flash_read(g_dev, addr, buf, n, narrate_progress, NULL)
        != TLSR_OK) {
        ui_log("! %s", tlsr_last_error());
        free(buf);
        return;                          /* no dialog on a failed read */
    }
    show_hex(IDC_F_OUT, addr, buf, n);
    cache_range(REGION_FLASH, addr, buf, n);
    free(buf);

    if (!range_cached(REGION_FLASH, addr, n)) {
        ui_log("! could not hold the range in memory to save it");
        return;
    }
    ui_log("  read %u bytes at 0x%06X", (unsigned)n, (unsigned)addr);
    PostMessageA(g_main, WM_APP + 3, 0, 0);
}

static void job_patch(void)
{
    uint8_t buf[256], back[256];
    char err[160];
    int n;

    if (!need_target()) { set_verdict(0); return; }
    n = parse_values(g_job.text, buf, sizeof buf, err, sizeof err);
    if (n <= 0) { ui_log("! %s", err); set_verdict(0); return; }

    set_stage(TLSR_STAGE_PROGRAM);
    if (tlsr_write(g_dev, g_job.addr, buf, (uint32_t)n) != TLSR_OK) {
        ui_log("! %s", tlsr_last_error());
        set_verdict(0);
        return;
    }
    ui_log("patched %d byte(s) at 0x%06X-0x%06X", n, (unsigned)g_job.addr,
           (unsigned)(g_job.addr + n - 1));

    if (!(g_job.flag & TLSR_PROG_VERIFY)) {
        ui_log("  verify skipped - the write was not read back");
        /* Nothing was checked, but nothing failed either: the write itself is
         * all there is to report on. */
        set_verdict(1);
        return;
    }
    set_stage(TLSR_STAGE_VERIFY);
    if (tlsr_read(g_dev, g_job.addr, back, (uint32_t)n) != TLSR_OK) {
        ui_log("! read-back failed: %s", tlsr_last_error());
        set_verdict(0);
        return;
    }
    {
        int same = memcmp(back, buf, (size_t)n) == 0;
        ui_log("  read-back %s", same ? "matches" : "DIFFERS");
        set_verdict(same);
    }
}

static void job_flash_dump(void)
{
    uint8_t *buf;
    uint32_t size;
    FILE *f;
    DWORD t0;

    if (!need_target()) return;
    size = g_info.flash_size ? g_info.flash_size : 512u * 1024u;
    buf = (uint8_t *)malloc(size);
    if (!buf) { ui_log("! out of memory"); return; }

    ui_log("> dump %u KB of flash to %s (expect roughly %u s) ...",
           (unsigned)(size / 1024), g_job.path, (unsigned)(size / 21500));
    set_stage(TLSR_STAGE_READ);
    narrate_begin("dumping");
    t0 = GetTickCount();
    if (tlsr_flash_read(g_dev, 0, buf, size, narrate_progress, NULL) != TLSR_OK) {
        ui_log("! %s", tlsr_last_error());
        free(buf);
        return;
    }
    f = fopen(g_job.path, "wb");
    if (!f) { ui_log("! cannot write %s", g_job.path); free(buf); return; }
    fwrite(buf, 1, size, f);
    fclose(f);
    free(buf);
    ui_log("  dumped %u bytes in %.1f s -> %s", (unsigned)size,
           (GetTickCount() - t0) / 1000.0, g_job.path);
}

static void job_flash_erase(void)
{
    if (!need_target()) { set_verdict(0); return; }
    ui_log("> erase %u byte(s) at 0x%06X ...",
           (unsigned)g_job.len, (unsigned)g_job.addr);
    set_stage(TLSR_STAGE_ERASE);
    narrate_begin("erasing");
    if (tlsr_flash_erase(g_dev, g_job.addr, g_job.len,
                         narrate_progress, core_log, NULL) != TLSR_OK) {
        ui_log("! %s", tlsr_last_error());
        set_verdict(0);
    } else {
        ui_log("  erased");
        set_verdict(1);
    }
}

static void job_flash_program(void)
{
    FILE *f;
    long fsize;
    uint8_t *blob;
    uint32_t n;

    if (!need_target()) { set_verdict(0); return; }
    f = fopen(g_job.path, "rb");
    if (!f) {
        ui_log("! cannot read %s", g_job.path);
        set_verdict(0);
        return;
    }
    fseek(f, 0, SEEK_END);
    fsize = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (fsize <= 0 || g_job.off >= (uint32_t)fsize) {
        ui_log("! offset 0x%X is past the end of the file (%ld bytes)",
               (unsigned)g_job.off, fsize);
        fclose(f);
        set_verdict(0);
        return;
    }
    n = g_job.len ? g_job.len : (uint32_t)fsize - g_job.off;
    if (g_job.off + n > (uint32_t)fsize)
        n = (uint32_t)fsize - g_job.off;

    blob = (uint8_t *)malloc(n);
    if (!blob) { fclose(f); ui_log("! out of memory"); set_verdict(0); return; }
    fseek(f, (long)g_job.off, SEEK_SET);
    if (fread(blob, 1, n, f) != n) {
        ui_log("! short read from %s", g_job.path);
        free(blob); fclose(f); set_verdict(0); return;
    }
    fclose(f);

    narrate_begin(g_job.flag ? "verifying" : "programming");
    if (g_job.flag) {
        uint32_t bad = 0;
        ui_log("> verify %u bytes at 0x%06X against the file ...",
               (unsigned)n, (unsigned)g_job.addr);
        set_stage(TLSR_STAGE_VERIFY);
        if (tlsr_flash_verify(g_dev, g_job.addr, blob, n, &bad,
                              narrate_progress, NULL) == TLSR_OK) {
            ui_log("  verify OK");
            set_verdict(1);
        } else {
            ui_log("! %s", tlsr_last_error());
            set_verdict(0);
        }
    } else {
        ui_log("> program %u bytes to flash 0x%06X (%s, %s) ...",
               (unsigned)n, (unsigned)g_job.addr,
               (g_job.stages & TLSR_PROG_ERASE)  ? "erase"  : "no erase",
               (g_job.stages & TLSR_PROG_VERIFY) ? "verify" : "no verify");
        /* The core announces erase / program / verify as it reaches them; this
         * covers the sector-preserving read that runs before the first of them. */
        set_stage(TLSR_STAGE_PROGRAM);
        if (tlsr_flash_program_ex(g_dev, g_job.addr, blob, n, g_job.stages,
                                  narrate_progress, core_log, NULL) == TLSR_OK) {
            ui_log("  programmed %u bytes%s", (unsigned)n,
                   (g_job.stages & TLSR_PROG_VERIFY) ? " and verified" : "");
            set_verdict(1);
        } else {
            ui_log("! %s", tlsr_last_error());
            set_verdict(0);
        }
    }
    free(blob);
}

/* Explicit activation with the frame count from the Bridge tab.  This is the
 * recovery to reach for when the target has gone quiet: power-cycle it, then
 * activate, because the SWire block only answers in a short window after
 * power-on and the burst has to still be running when that window opens. */
static void job_activate(void)
{
    uint16_t frames = (uint16_t)(g_job.len ? g_job.len : TLSR_ACTIVATE_FRAMES);

    if (!g_dev) { ui_log("not connected"); return; }
    ui_log("> activating with %u frames ...", (unsigned)frames);
    if (tlsr_activate(g_dev, frames) != TLSR_OK) {
        ui_log("! %s", tlsr_last_error());
        return;
    }
    ui_log("  activation burst sent - use Re-probe to see if the target answers");
}

static void job_cfg_get(void)
{
    tlsr_cfg c;
    uint8_t rails = 0;
    int i;

    if (!g_dev) { ui_log("not connected"); return; }
    if (tlsr_get_cfg(g_dev, &c, &rails) != TLSR_OK) {
        ui_log("! %s", tlsr_last_error());
        return;
    }
    {
        const uint8_t *p = (const uint8_t *)&c;
        for (i = 0; i < NUM_CFG; i++) {
            char b[16];
            snprintf(b, sizeof b, "%u", p[i]);
            SetWindowTextA(GetDlgItem(g_main, IDC_B_CFG0 + i), b);
        }
    }
    ui_log("timing: cell=%u low0=%u low1=%u thr=%u div=%u",
           c.cell, c.low0, c.low1, c.thr, c.spi_div);
}

static void job_cfg_set(void)
{
    tlsr_cfg c;
    uint8_t *p = (uint8_t *)&c;
    int i;

    if (!g_dev) { ui_log("not connected"); return; }
    for (i = 0; i < NUM_CFG; i++) {
        uint32_t v = 0;
        get_num(IDC_B_CFG0 + i, &v);
        p[i] = (uint8_t)v;
    }
    if (tlsr_set_cfg(g_dev, &c) != TLSR_OK)
        ui_log("! %s", tlsr_last_error());
    else
        ui_log("timing applied (not persistent - a replug restores defaults)");
}

/* ---------------------------------------------------------- tab handling -- */
static const int g_tab_target[] = {
    IDC_T_PROBE, IDC_T_PWRON, IDC_T_PWROFF, IDC_T_HALT, IDC_T_RUN,
    IDC_T_RESET, IDC_T_IDENT, IDC_T_CAPS, IDC_T_REGIONS, 0
};
static const int g_tab_memory[] = {
    IDC_M_REGION, IDC_M_MODE, IDC_M_ADDR, IDC_M_LEN, IDC_M_READ, IDC_M_SAVE,
    IDC_M_PATCH, IDC_M_WRITE, IDC_M_OUT, IDC_M_NOTE, IDC_M_PATCHLBL,
    IDC_M_L1, IDC_M_L2, IDC_M_L3,
    IDC_M_GRPR, IDC_M_GRPW, IDC_M_DOERASE, IDC_M_DOVERIFY, 0
};
static const int g_tab_flash[] = {
    IDC_F_MODE, IDC_F_ADDR, IDC_F_LEN, IDC_F_READ, IDC_F_DUMP, IDC_F_ERASE,
    IDC_F_FILE, IDC_F_BROWSE, IDC_F_OFF, IDC_F_PLEN, IDC_F_PADDR, IDC_F_PROG,
    IDC_F_VERIFY, IDC_F_OUT, IDC_F_NOTE, IDC_F_L1, IDC_F_L2, IDC_F_L3,
    IDC_F_L4, IDC_F_L5, IDC_F_L6,
    IDC_F_GRPR, IDC_F_GRPW, IDC_F_DOERASE, IDC_F_DOVERIFY, IDC_F_DUMPRNG, 0
};
static const int g_tab_bridge[] = {
    IDC_B_SELF, IDC_B_PIN, IDC_B_RAIL, IDC_B_ACT, IDC_B_GO, IDC_B_REFRESH,
    IDC_B_APPLY, IDC_B_L1, IDC_B_L2, IDC_B_HINT, 0
};
static const int g_tab_debug[] = {
    IDC_D_CONTINUE, IDC_D_PAUSE, IDC_D_STOP, IDC_D_RESTART,
    IDC_D_STEPINTO, IDC_D_STEPOVER, IDC_D_STEPOUT, IDC_D_STEPINST,
    IDC_D_RUNCUR, IDC_D_BPTOGGLE, IDC_D_BPCLEAR, IDC_D_REFRESH,
    IDC_D_ADDR, IDC_D_ADDRLBL, IDC_D_STATE, IDC_D_REGS, IDC_D_NOTE, 0
};

static void show_ids(const int *ids, int show)
{
    int i;
    for (i = 0; ids[i]; i++)
        ShowWindow(GetDlgItem(g_main, ids[i]), show ? SW_SHOW : SW_HIDE);
}

static void show_tab(int t)
{
    int i;
    show_ids(g_tab_target, t == 0);
    show_ids(g_tab_memory, t == 1);
    show_ids(g_tab_flash,  t == 2);
    show_ids(g_tab_bridge, t == 3);
    show_ids(g_tab_debug,  t == 4);
    for (i = 0; i < NUM_CFG; i++) {
        ShowWindow(GetDlgItem(g_main, IDC_B_CFG0 + i), t == 3 ? SW_SHOW : SW_HIDE);
        ShowWindow(GetDlgItem(g_main, IDC_B_CFGL0 + i), t == 3 ? SW_SHOW : SW_HIDE);
    }
}

/* ------------------------------------------------------------- resizing --- */
#define TAB_TOP     40
#define LOG_H      154

/* Where the PASS / FAIL box sits, and how tall it is.  The text is centred in
 * this box (SS_CENTERIMAGE), so moving the box is how the word moves.
 *
 * It deliberately runs down past TAB_TOP.  The tab control starts there, but
 * this is the far right of its strip -- well past the last tab -- so the only
 * thing underneath is empty background, and the extra room is what lets the
 * word be read from arm's length.  The tab control is sunk to the bottom of
 * the z-order in build_controls(), so it cannot paint over the label. */
#define VERDICT_Y   28
#define VERDICT_H   20
#define PAGE_LEFT   20
#define PAGE_TOP    78

/* Memory and Flash are each split into a framed Read half and a framed Write
 * half.  build_controls() and layout() both work from these, so the frames
 * cannot drift away from what they enclose.
 *
 * GRP_IN is the content inset: far enough inside the frame that the border
 * does not run through the first label. */
#define GRP_IN     (PAGE_LEFT + 14)

/* One gap size everywhere: below the tab strip, between the two frames, and
 * above the hex box.  PAGE_TOP - 14 is where the tab control's page area
 * begins, so the first frame is inset from it by the same amount as the rest. */
#define GRP_GAP      8

#define M_GRPR_Y   (PAGE_TOP - 14 + GRP_GAP)
#define M_GRPR_H    68
#define M_READ_Y   (M_GRPR_Y + 20)
#define M_GRPW_Y   (M_GRPR_Y + M_GRPR_H + GRP_GAP)
#define M_GRPW_H    52
#define M_WRITE_Y   M_GRPW_Y
#define M_OUT_Y    (M_GRPW_Y + M_GRPW_H + GRP_GAP)

#define F_GRPR_Y   (PAGE_TOP - 14 + GRP_GAP)
#define F_GRPR_H    68
#define F_READ_Y   (F_GRPR_Y + 20)
#define F_GRPW_Y   (F_GRPR_Y + F_GRPR_H + GRP_GAP)
#define F_GRPW_H   112
#define F_WRITE_Y   F_GRPW_Y
#define F_OUT_Y    (F_GRPW_Y + F_GRPW_H + GRP_GAP)

/* Everything that should grow with the window is positioned here rather than
 * at creation time, so a resize keeps the layout right.  In particular the
 * capability list runs to the right edge: its Notes column holds full
 * sentences, and a fixed width would clip them with no way to read them. */
static void layout(int cw, int ch)
{
    int right  = cw - PAGE_LEFT;
    int bottom = ch - LOG_H - 24;
    int i;

    MoveWindow(g_tabs, 8, TAB_TOP, cw - 16, ch - TAB_TOP - LOG_H - 16, TRUE);
    MoveWindow(g_log, 8, ch - LOG_H - 8, cw - 16, LOG_H, TRUE);
    /* The bar loses two pixels of height so the verdict fits underneath it
     * inside the existing top bar -- the tab strip still starts at TAB_TOP, so
     * no page lost any room to this.  The label is the same width as the bar
     * and right-aligned, which puts PASS/FAIL flush with the bar's right end. */
    MoveWindow(GetDlgItem(g_main, IDC_PROGRESS), cw - 196, 6, 180, 16, TRUE);
    MoveWindow(GetDlgItem(g_main, IDC_VERDICT), cw - 196, VERDICT_Y, 180,
               VERDICT_H, TRUE);

    /* --- Target: identity fixed, capabilities fill the rest -------------- */
    {
        int y = PAGE_TOP + 34;
        int regions_h = 108;
        int lists_h = bottom - y - regions_h - 10;
        int ident_w = 360;
        HWND caps = GetDlgItem(g_main, IDC_T_CAPS);
        int caps_w;

        if (lists_h < 80) lists_h = 80;
        caps_w = right - (PAGE_LEFT + ident_w + 10);
        if (caps_w < 220) caps_w = 220;

        MoveWindow(GetDlgItem(g_main, IDC_T_IDENT),
                   PAGE_LEFT, y, ident_w, lists_h, TRUE);
        MoveWindow(caps, PAGE_LEFT + ident_w + 10, y, caps_w, lists_h, TRUE);
        MoveWindow(GetDlgItem(g_main, IDC_T_REGIONS),
                   PAGE_LEFT, y + lists_h + 10, right - PAGE_LEFT,
                   regions_h, TRUE);

        /* Give the Notes column whatever is left after the first two. */
        ListView_SetColumnWidth(caps, 2,
                                caps_w - 200 - 40 - GetSystemMetrics(SM_CXVSCROLL));
    }

    /* --- Memory: framed Read half, framed Write half, then the dump box --- *
     * gw is the frame width and gr the inner right edge the action buttons
     * hug, so widening the window grows the fields rather than leaving a gap
     * in the middle. */
    {
        int gw = right - PAGE_LEFT;
        int gr = right - 14;
        int wb = gr - 80;                     /* Write button                */
        int cv = wb - 8 - 66;                 /* Verify checkbox             */
        int ce = cv - 8 - 62;                 /* Erase checkbox              */
        int ex = GRP_IN + 124;                /* Bytes edit                  */
        int ew = ce - 10 - ex;
        int oy = M_OUT_Y, oh;

        if (ew < 120) ew = 120;

        MoveWindow(GetDlgItem(g_main, IDC_M_GRPR), PAGE_LEFT, M_GRPR_Y,
                   gw, M_GRPR_H, TRUE);
        MoveWindow(GetDlgItem(g_main, IDC_M_GRPW), PAGE_LEFT, M_GRPW_Y,
                   gw, M_GRPW_H, TRUE);

        MoveWindow(GetDlgItem(g_main, IDC_M_SAVE), gr - 80,  M_READ_Y - 2, 80, 24, TRUE);
        MoveWindow(GetDlgItem(g_main, IDC_M_READ), gr - 156, M_READ_Y - 2, 70, 24, TRUE);
        MoveWindow(GetDlgItem(g_main, IDC_M_NOTE), GRP_IN, M_READ_Y + 30,
                   gr - GRP_IN, 18, TRUE);

        MoveWindow(GetDlgItem(g_main, IDC_M_PATCH), ex, M_WRITE_Y + 22, ew, 22, TRUE);
        MoveWindow(GetDlgItem(g_main, IDC_M_DOERASE),  ce, M_WRITE_Y + 24, 62, 20, TRUE);
        MoveWindow(GetDlgItem(g_main, IDC_M_DOVERIFY), cv, M_WRITE_Y + 24, 66, 20, TRUE);
        MoveWindow(GetDlgItem(g_main, IDC_M_WRITE),    wb, M_WRITE_Y + 20, 80, 24, TRUE);

        /* Never walk the box up over the Write frame -- the minimum window
         * height is what guarantees it room, so only floor the height. */
        oh = bottom - oy;
        if (oh < 40) oh = 40;
        MoveWindow(GetDlgItem(g_main, IDC_M_OUT), PAGE_LEFT, oy, gw, oh, TRUE);
    }

    /* --- Flash: same split; the write half holds the whole program flow --- */
    {
        int gw = right - PAGE_LEFT;
        int gr = right - 14;
        int vb = gr - 80;                     /* Verify button               */
        int pb = vb - 8 - 85;                 /* Program button              */
        int eb = pb - 8 - 110;                /* Erase sectors button        */
        int fx = GRP_IN + 35;                 /* File edit                   */
        int fw = (gr - 92 - 8) - fx;
        int oy = F_OUT_Y, oh;

        if (fw < 160) fw = 160;

        MoveWindow(GetDlgItem(g_main, IDC_F_GRPR), PAGE_LEFT, F_GRPR_Y,
                   gw, F_GRPR_H, TRUE);
        MoveWindow(GetDlgItem(g_main, IDC_F_GRPW), PAGE_LEFT, F_GRPW_Y,
                   gw, F_GRPW_H, TRUE);

        MoveWindow(GetDlgItem(g_main, IDC_F_DUMP),    gr - 95,  F_READ_Y - 2,  95, 24, TRUE);
        MoveWindow(GetDlgItem(g_main, IDC_F_DUMPRNG), gr - 218, F_READ_Y - 2, 115, 24, TRUE);
        MoveWindow(GetDlgItem(g_main, IDC_F_READ),    gr - 321, F_READ_Y - 2,  95, 24, TRUE);
        MoveWindow(GetDlgItem(g_main, IDC_F_NOTE), GRP_IN, F_READ_Y + 30,
                   gr - GRP_IN, 18, TRUE);

        MoveWindow(GetDlgItem(g_main, IDC_F_FILE), fx, F_WRITE_Y + 20, fw, 22, TRUE);
        MoveWindow(GetDlgItem(g_main, IDC_F_BROWSE), gr - 92, F_WRITE_Y + 18,
                   92, 24, TRUE);

        MoveWindow(GetDlgItem(g_main, IDC_F_ERASE),  eb, F_WRITE_Y + 78, 110, 24, TRUE);
        MoveWindow(GetDlgItem(g_main, IDC_F_PROG),   pb, F_WRITE_Y + 78, 85, 24, TRUE);
        MoveWindow(GetDlgItem(g_main, IDC_F_VERIFY), vb, F_WRITE_Y + 78, 80, 24, TRUE);

        oh = bottom - oy;
        if (oh < 40) oh = 40;
        MoveWindow(GetDlgItem(g_main, IDC_F_OUT), PAGE_LEFT, oy, gw, oh, TRUE);
    }

    /* --- Bridge: the explanatory note wraps to the window ----------------- */
    MoveWindow(GetDlgItem(g_main, IDC_B_HINT), PAGE_LEFT, PAGE_TOP + 146,
               right - PAGE_LEFT, 60, TRUE);

    /* --- Debugger: the register list fills whatever is left --------------- */
    MoveWindow(GetDlgItem(g_main, IDC_D_STATE), PAGE_LEFT + 330, PAGE_TOP + 46,
               right - (PAGE_LEFT + 330), 18, TRUE);
    MoveWindow(GetDlgItem(g_main, IDC_D_NOTE), PAGE_LEFT, PAGE_TOP + 70,
               right - PAGE_LEFT, 18, TRUE);
    {
        HWND rl = GetDlgItem(g_main, IDC_D_REGS);
        int h = bottom - (PAGE_TOP + 94);
        if (h < 60) h = 60;
        MoveWindow(rl, PAGE_LEFT, PAGE_TOP + 94, right - PAGE_LEFT, h, TRUE);
        ListView_SetColumnWidth(rl, 2,
                                (right - PAGE_LEFT) - 130 - 120 -
                                GetSystemMetrics(SM_CXVSCROLL));
    }

    /* Moving the tab control dirties the area the page controls occupy, so ask
     * every visible child to repaint itself once the new geometry is set. */
    RedrawWindow(g_main, NULL, NULL,
                 RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN);
    (void)i;
}

/* --------------------------------------------------------------- browsing -- */
static int browse(char *path, int save, const char *filter)
{
    OPENFILENAMEA o;
    memset(&o, 0, sizeof o);
    o.lStructSize = sizeof o;
    o.hwndOwner   = g_main;
    o.lpstrFilter = filter;
    o.lpstrFile   = path;
    o.nMaxFile    = MAX_PATH;
    o.Flags       = OFN_PATHMUSTEXIST | (save ? OFN_OVERWRITEPROMPT
                                              : OFN_FILEMUSTEXIST);
    o.lpstrDefExt = "bin";
    return save ? GetSaveFileNameA(&o) : GetOpenFileNameA(&o);
}

/* ------------------------------------------------------------- connection -- */
static void fill_ports(void)
{
    char names[32][16];
    int n = tlsr_list_ports(names, 32), i;
    HWND cb = GetDlgItem(g_main, IDC_PORT);

    SendMessageA(cb, CB_RESETCONTENT, 0, 0);
    for (i = 0; i < n; i++)
        SendMessageA(cb, CB_ADDSTRING, 0, (LPARAM)names[i]);
    if (n)
        SendMessageA(cb, CB_SETCURSEL, 0, 0);
}

static void do_connect(void)
{
    char port[32];

    if (g_dev) {
        tlsr_close(g_dev);
        g_dev = NULL;
        g_have_info = 0;
        EnterCriticalSection(&g_lock);
        g_rails = -1;
        LeaveCriticalSection(&g_lock);
        set_text(IDC_CONNECT, "Connect");
        set_text(IDC_STATUS, "disconnected");
        show_info();
        ui_log("disconnected");
        return;
    }
    get_text(IDC_PORT, port, sizeof port);
    if (!port[0]) {
        MessageBoxA(g_main, "Choose a COM port first.", APP_TITLE,
                    MB_OK | MB_ICONINFORMATION);
        return;
    }
    g_dev = tlsr_open(port);
    if (!g_dev) {
        ui_log("! %s", tlsr_last_error());
        MessageBoxA(g_main, tlsr_last_error(), APP_TITLE, MB_OK | MB_ICONERROR);
        return;
    }
    {
        char id[9]; uint16_t ver = 0; char s[96];
        if (tlsr_ping(g_dev, id, &ver) != TLSR_OK) {
            ui_log("! %s", tlsr_last_error());
            MessageBoxA(g_main, tlsr_last_error(), APP_TITLE, MB_OK | MB_ICONERROR);
            tlsr_close(g_dev);
            g_dev = NULL;
            return;
        }
        snprintf(s, sizeof s, "%s  fw %u.%u", port, ver >> 8, ver & 0xFF);
        set_text(IDC_STATUS, s);
        set_text(IDC_CONNECT, "Disconnect");
        ui_log("connected to %s: %s firmware %u.%u", port, id,
               ver >> 8, ver & 0xFF);
    }
    /* The core reports which stage of a program run it is in so the bar can be
     * coloured; it is called on the worker thread, which is why core_stage only
     * publishes and never paints. */
    tlsr_set_stage_cb(g_dev, core_stage, NULL);
    /* Deliberately NOT a probe: identifying the chip powers the target up, and
     * connecting to the bridge must not do that by itself. */
    submit(job_link_state);
}

/* ------------------------------------------------------------- PASS / FAIL -- */
/* How the last write ended, shown under the progress bar and then taken away
 * again.  It is deliberately not dismissed on a plain timer: a long program run
 * can finish while nobody is watching, and a result nobody saw is worse than
 * none.  The countdown therefore starts when the user comes back and touches
 * something.  GetLastInputInfo() is what makes "touches something" include
 * moving the mouse across a child control, which never sends this window a
 * message of its own. */
#define VERDICT_LINGER_MS 5000

static int      g_v_shown;      /* what the label currently says              */
static unsigned g_v_seq;        /* the verdict it was built from              */
static DWORD    g_v_input;      /* last system input when it appeared         */
static DWORD    g_v_hide_at;    /* 0 until the user has interacted            */

static DWORD last_input_time(void)
{
    LASTINPUTINFO li;
    li.cbSize = sizeof li;
    return GetLastInputInfo(&li) ? li.dwTime : 0;
}

static void verdict_tick(int verdict, unsigned seq)
{
    HWND lbl = GetDlgItem(g_main, IDC_VERDICT);

    if (seq != g_v_seq) {                       /* a new result arrived */
        g_v_seq     = seq;
        g_v_shown   = verdict;
        g_v_input   = last_input_time();
        g_v_hide_at = 0;
        SetWindowTextA(lbl, verdict == 1 ? "PASS" :
                            verdict == 2 ? "FAIL" : "");
        InvalidateRect(lbl, NULL, TRUE);
        ShowWindow(lbl, verdict ? SW_SHOW : SW_HIDE);
        return;
    }
    if (!g_v_shown)
        return;

    if (!g_v_hide_at) {
        DWORD t = last_input_time();
        /* Only input aimed at this application counts; typing in another
         * window is not the user acknowledging anything here. */
        if (t != g_v_input && GetForegroundWindow() == g_main)
            g_v_hide_at = GetTickCount() + VERDICT_LINGER_MS;
    } else if ((LONG)(GetTickCount() - g_v_hide_at) >= 0) {
        g_v_shown = 0;
        ShowWindow(lbl, SW_HIDE);
    }
}

/* ------------------------------------------------------------ window proc -- */
static void build_controls(void);

static LRESULT CALLBACK wnd_proc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    switch (m) {
    case WM_CREATE: {
        RECT rc;
        g_main = h;
        build_controls();
        fill_ports();
        show_tab(0);
        GetClientRect(h, &rc);
        layout(rc.right, rc.bottom);
        SetTimer(h, 1, 100, NULL);
        return 0;
    }

    case WM_TIMER: {
        char *chunk = NULL;
        size_t len = 0;
        int pct, rails, stage, verdict;
        unsigned vseq;

        EnterCriticalSection(&g_lock);
        if (g_loglen) {
            chunk = (char *)malloc(g_loglen + 1);
            if (chunk) {
                memcpy(chunk, g_logbuf, g_loglen);
                chunk[g_loglen] = 0;
                len = g_loglen;
            }
            g_loglen = 0;
        }
        pct     = g_prog_pct;
        rails   = g_rails;
        stage   = g_stage;
        verdict = g_verdict;
        vseq    = g_verdict_seq;
        LeaveCriticalSection(&g_lock);

        if (chunk && len) {
            int end = GetWindowTextLengthA(g_log);
            /* EM_REPLACESEL is ignored while the control is read-only, so the
             * flag comes off for the append and goes straight back on. */
            SendMessageA(g_log, EM_SETREADONLY, FALSE, 0);
            SendMessageA(g_log, EM_SETSEL, end, end);
            SendMessageA(g_log, EM_REPLACESEL, FALSE, (LPARAM)chunk);
            SendMessageA(g_log, EM_SETREADONLY, TRUE, 0);
            SendMessageA(g_log, EM_SCROLLCARET, 0, 0);
        }
        free(chunk);
        if (pct >= 0)
            SendMessageA(GetDlgItem(h, IDC_PROGRESS), PBM_SETPOS, pct, 0);

        /* Orange while sectors are erased, green while they are programmed,
         * blue while the result is read back. */
        {
            static int shown_stage = -1;
            if (stage != shown_stage) {
                shown_stage = stage;
                set_bar_stage(stage);
            }
        }
        verdict_tick(verdict, vseq);

        /* The rail state is whatever the worker last observed, so an operation
         * that powered the target up on our behalf is reflected here too --
         * this is the one place the button's face is decided, which is why it
         * follows a power-up the user never asked for as readily as a click. */
        {
            static int shown = -2;
            if (rails != shown) {
                HWND b = GetDlgItem(h, IDC_POWER);
                int  live = (g_dev && rails >= 0);
                char s[96];

                shown = rails;
                if (!live)
                    snprintf(s, sizeof s, "Target power: not connected");
                else if (rails == 0)
                    snprintf(s, sizeof s, "Target is off - click to power it up");
                else
                    snprintf(s, sizeof s, "Target is powered (%s) - click to "
                             "switch it off",
                             rails == TLSR_RAIL_PB0    ? "PB0" :
                             rails == TLSR_RAIL_MOSFET ? "MOSFET"
                                                       : "PB0+MOSFET");
                EnableWindow(b, live);
                SendMessageA(b, BM_SETIMAGE, IMAGE_ICON,
                             (LPARAM)((live && rails) ? g_ico_pwr_on
                                                      : g_ico_pwr_off));
                set_tip(b, s);
                InvalidateRect(b, NULL, TRUE);
            }
        }
        return 0;
    }

    /* The trough's outline is drawn here, in the parent, rather than by the
     * control.  An un-themed progress bar draws no frame of its own, and it
     * ignores WS_EX_CLIENTEDGE as well, so without this the bar is a bare grey
     * rectangle floating on the window.  One rectangle hugging the control is
     * all it takes, and it lands on parent pixels so nothing can clip it. */
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(h, &ps);
        HWND pb = GetDlgItem(h, IDC_PROGRESS);
        RECT r;

        if (pb && GetWindowRect(pb, &r)) {
            HPEN   pen = CreatePen(PS_SOLID, 1, RGB(160, 160, 160));
            HGDIOBJ op, ob;
            MapWindowPoints(NULL, h, (POINT *)&r, 2);
            op = SelectObject(dc, pen);
            ob = SelectObject(dc, GetStockObject(NULL_BRUSH));
            Rectangle(dc, r.left - 1, r.top - 1, r.right + 1, r.bottom + 1);
            SelectObject(dc, ob);
            SelectObject(dc, op);
            DeleteObject(pen);
        }
        EndPaint(h, &ps);
        return 0;
    }

    /* Green PASS, red FAIL.  Everything else keeps the default handling --
     * this message also arrives for the read-only log and hex boxes. */
    case WM_CTLCOLORSTATIC:
        if (GetDlgCtrlID((HWND)l) == IDC_VERDICT) {
            SetBkMode((HDC)w, TRANSPARENT);
            SetTextColor((HDC)w, g_v_shown == 2 ? RGB(200, 0, 0)
                                                : RGB(0, 150, 0));
            return (LRESULT)GetSysColorBrush(COLOR_BTNFACE);
        }
        break;

    case WM_APP + 2:                       /* worker touched the CPU state */
        dbg_update_enable();
        show_regs();
        return 0;

    /* The range is read and cached; ask where to put it.  This runs on the UI
     * thread because the save dialog is modal on this window. */
    case WM_APP + 3: {
        char path[MAX_PATH] = "";
        FILE *f;

        if (!g_rng_buf)
            return 0;
        if (!browse(path, 1, "Binary files\0*.bin\0All files\0*.*\0"))
            return 0;
        f = fopen(path, "wb");
        if (!f) {
            ui_log("! cannot write %s", path);
            return 0;
        }
        fwrite(g_rng_buf, 1, g_rng_len, f);
        fclose(f);
        ui_log("  %u bytes (0x%06X..0x%06X) -> %s", (unsigned)g_rng_len,
               (unsigned)g_rng_addr, (unsigned)(g_rng_addr + g_rng_len - 1),
               path);
        return 0;
    }

    case WM_APP + 1:                       /* worker finished a probe */
        show_info();
        /* Registers only become usable once the chip is identified. */
        InvalidateRect(GetDlgItem(h, IDC_M_REGION), NULL, TRUE);
        update_note(IDC_M_NOTE, cb_cur(IDC_M_REGION), cb_cur(IDC_M_MODE));
        update_note(IDC_F_NOTE, REGION_FLASH, cb_cur(IDC_F_MODE));
        return 0;

    /* Owner-drawn region combo: the Registers entry is greyed when the target
     * has not been identified, because reading it would only produce noise. */
    case WM_DRAWITEM: {
        LPDRAWITEMSTRUCT di = (LPDRAWITEMSTRUCT)l;
        char txt[64] = "";
        int enabled;

        if (di->CtlID != IDC_M_REGION || (int)di->itemID < 0)
            break;
        SendMessageA(di->hwndItem, CB_GETLBTEXT, di->itemID, (LPARAM)txt);
        enabled = region_supported((int)di->itemID);

        if ((di->itemState & ODS_SELECTED) && enabled)
            FillRect(di->hDC, &di->rcItem,
                     (HBRUSH)(COLOR_HIGHLIGHT + 1));
        else
            FillRect(di->hDC, &di->rcItem, (HBRUSH)(COLOR_WINDOW + 1));

        SetBkMode(di->hDC, TRANSPARENT);
        SetTextColor(di->hDC, GetSysColor(
            !enabled ? COLOR_GRAYTEXT :
            (di->itemState & ODS_SELECTED) ? COLOR_HIGHLIGHTTEXT : COLOR_WINDOWTEXT));
        {
            RECT r = di->rcItem;
            r.left += 4;
            DrawTextA(di->hDC, txt, -1, &r, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
        }
        return TRUE;
    }

    case WM_NOTIFY: {
        LPNMHDR n = (LPNMHDR)l;
        if (n->idFrom == IDC_TABS && n->code == TCN_SELCHANGE)
            show_tab(TabCtrl_GetCurSel(g_tabs));
        return 0;
    }

    case WM_SIZE:
        layout(LOWORD(l), HIWORD(l));
        return 0;

    case WM_GETMINMAXINFO: {
        MINMAXINFO *mm = (MINMAXINFO *)l;
        mm->ptMinTrackSize.x = 880;      /* below this the address row overlaps */
        mm->ptMinTrackSize.y = 560;  /* Flash write frame + a usable output box */
        return 0;
    }

    case WM_COMMAND: {
        int id = LOWORD(w), code = HIWORD(w);

        /* Live updates as the user types or changes a selection. */
        if (code == EN_CHANGE && id == IDC_M_ADDR) {
            update_patch_label();
            return 0;
        }
        if (code == CBN_SELCHANGE) {
            static int last_region = REGION_SRAM;
            if (id == IDC_M_REGION) {
                int sel = cb_cur(IDC_M_REGION);
                if (!region_supported(sel)) {
                    /* Greyed entries cannot be chosen; put the selection back
                     * and say why rather than silently ignoring the click. */
                    cb_sel(IDC_M_REGION, last_region);
                    ui_log("Registers are unavailable until the target has "
                           "been identified - press Connect or Re-probe");
                    return 0;
                }
                last_region = sel;
                update_note(IDC_M_NOTE, sel, cb_cur(IDC_M_MODE));
                return 0;
            }
            if (id == IDC_M_MODE) {
                int mode = cb_cur(IDC_M_MODE);
                convert_range_field(IDC_M_ADDR, IDC_M_LEN, mode);
                update_mode_label(IDC_M_L3, mode);
                update_note(IDC_M_NOTE, cb_cur(IDC_M_REGION), mode);
                return 0;
            }
            if (id == IDC_F_MODE) {
                int mode = cb_cur(IDC_F_MODE);
                convert_range_field(IDC_F_ADDR, IDC_F_LEN, mode);
                update_mode_label(IDC_F_L2, mode);
                update_note(IDC_F_NOTE, REGION_FLASH, mode);
                return 0;
            }
        }

        switch (id) {
        case IDC_RESCAN:  fill_ports(); return 0;
        case IDC_CONNECT: do_connect(); return 0;

        /* The icon says what the target is; the click asks for the opposite.
         * Same job and same rail selection as the Target tab's pair, so the
         * two routes cannot drift apart. */
        case IDC_POWER: {
            int on;
            EnterCriticalSection(&g_lock);
            on = (g_rails > 0);
            LeaveCriticalSection(&g_lock);
            g_job.flag = on ? 0 : 1;
            g_job.len  = (uint32_t)cb_cur(IDC_B_RAIL);
            submit(job_power);
            return 0;
        }

        case IDC_T_PROBE:  if (g_dev) submit(job_probe); return 0;
        case IDC_T_PWRON:
            g_job.flag = 1; g_job.len = (uint32_t)cb_cur(IDC_B_RAIL);
            submit(job_power); return 0;
        case IDC_T_PWROFF:
            g_job.flag = 0; g_job.len = (uint32_t)cb_cur(IDC_B_RAIL);
            submit(job_power); return 0;
        case IDC_T_HALT:   submit(job_halt);  return 0;
        case IDC_T_RUN:    submit(job_run);   return 0;
        case IDC_T_RESET:  submit(job_reset); return 0;

        /* --- debugger ------------------------------------------------- */
        case IDC_D_CONTINUE: submit(job_dbg_continue); return 0;
        case IDC_D_PAUSE:    submit(job_dbg_pause);    return 0;
        case IDC_D_STOP:     submit(job_dbg_stop);     return 0;
        case IDC_D_RESTART:  submit(job_dbg_restart);  return 0;
        case IDC_D_STEPINTO:
        case IDC_D_STEPINST: submit(job_dbg_step);     return 0;
        case IDC_D_STEPOVER: submit(job_dbg_stepover); return 0;
        case IDC_D_STEPOUT:  submit(job_dbg_stepout);  return 0;
        case IDC_D_BPCLEAR:  submit(job_dbg_bpclear);  return 0;
        case IDC_D_REFRESH:  submit(job_dbg_refresh);  return 0;

        case IDC_D_RUNCUR:
        case IDC_D_BPTOGGLE: {
            uint32_t a = 0;
            if (get_num(IDC_D_ADDR, &a)) {
                ui_log("! enter an address in the Debugger address box");
                return 0;
            }
            g_job.addr = a;
            submit(id == IDC_D_RUNCUR ? job_dbg_runcur : job_dbg_bptoggle);
            return 0;
        }

        case IDC_M_READ:
        case IDC_M_SAVE: {
            uint32_t a = 0, n = 0;
            char err[160];
            int region = cb_cur(IDC_M_REGION);
            if (resolve_range(IDC_M_ADDR, IDC_M_LEN, cb_cur(IDC_M_MODE),
                              &a, &n, err, sizeof err)) {
                ui_log("! %s", err);
                return 0;
            }
            g_job.addr = a; g_job.len = n; g_job.flag = region; g_job.off = 0;
            g_job.path[0] = 0;
            if (id == IDC_M_SAVE && !browse(g_job.path, 1,
                    "Binary files\0*.bin\0All files\0*.*\0"))
                return 0;
            submit(job_read);
            return 0;
        }
        case IDC_M_WRITE: {
            uint32_t a = 0;
            if (cb_cur(IDC_M_REGION) == REGION_FLASH) {
                MessageBoxA(g_main,
                    "Flash cannot be patched byte-wise: it must be erased in "
                    "4 KB sectors first.\n\nUse the Flash tab, which erases, "
                    "programs and verifies while preserving the rest of the "
                    "sector.", APP_TITLE, MB_OK | MB_ICONINFORMATION);
                return 0;
            }
            if (get_num(IDC_M_ADDR, &a)) { ui_log("! bad address"); return 0; }
            g_job.addr = a;
            g_job.flag = is_checked(IDC_M_DOVERIFY) ? TLSR_PROG_VERIFY : 0;
            get_text(IDC_M_PATCH, g_job.text, sizeof g_job.text);
            submit(job_patch);
            return 0;
        }

        case IDC_F_READ: {
            uint32_t a = 0, n = 0;
            char err[160];
            if (resolve_range(IDC_F_ADDR, IDC_F_LEN, cb_cur(IDC_F_MODE),
                              &a, &n, err, sizeof err)) {
                ui_log("! %s", err);
                return 0;
            }
            g_job.addr = a; g_job.len = n; g_job.flag = REGION_FLASH;
            g_job.off = 1;                     /* output goes to the Flash box */
            g_job.path[0] = 0;
            submit(job_read);
            return 0;
        }
        case IDC_F_DUMP:
            g_job.path[0] = 0;
            if (!browse(g_job.path, 1, "Binary files\0*.bin\0All files\0*.*\0"))
                return 0;
            submit(job_flash_dump);
            return 0;
        case IDC_F_DUMPRNG: {
            uint32_t a = 0, n = 0;
            char err[160];
            /* The filename is asked for afterwards, not now: if the range
             * still has to be read there is no point choosing a destination
             * for a read that may fail. */
            if (resolve_range(IDC_F_ADDR, IDC_F_LEN, cb_cur(IDC_F_MODE),
                              &a, &n, err, sizeof err)) {
                ui_log("! %s", err);
                return 0;
            }
            g_job.addr = a; g_job.len = n;
            submit(job_dump_range);
            return 0;
        }
        case IDC_F_ERASE: {
            uint32_t a = 0, n = 0;
            char err[160], msg[256];
            if (resolve_range(IDC_F_ADDR, IDC_F_LEN, cb_cur(IDC_F_MODE),
                              &a, &n, err, sizeof err)) {
                ui_log("! %s", err);
                return 0;
            }
            if (n < TLSR_FLASH_SECTOR) n = TLSR_FLASH_SECTOR;
            snprintf(msg, sizeof msg,
                     "Erase %u bytes (%u sector(s)) at 0x%06X?\n\n"
                     "This is destructive and cannot be undone.",
                     (unsigned)n, (unsigned)(n / TLSR_FLASH_SECTOR), (unsigned)a);
            if (MessageBoxA(g_main, msg, APP_TITLE,
                            MB_YESNO | MB_ICONWARNING) != IDYES)
                return 0;
            g_job.addr = a; g_job.len = n;
            submit(job_flash_erase);
            return 0;
        }
        case IDC_F_BROWSE: {
            char path[MAX_PATH] = "";
            if (browse(path, 0, "Binary files\0*.bin\0All files\0*.*\0"))
                set_text(IDC_F_FILE, path);
            return 0;
        }
        case IDC_F_PROG:
        case IDC_F_VERIFY: {
            uint32_t a = 0, off = 0, n = 0;
            char msg[320];
            get_text(IDC_F_FILE, g_job.path, sizeof g_job.path);
            if (!g_job.path[0]) { ui_log("! choose a .bin file first"); return 0; }
            get_num(IDC_F_PADDR, &a);
            get_num(IDC_F_OFF, &off);
            get_num(IDC_F_PLEN, &n);
            g_job.addr = a; g_job.off = off; g_job.len = n;
            g_job.flag = (id == IDC_F_VERIFY);
            g_job.stages = (is_checked(IDC_F_DOERASE)  ? TLSR_PROG_ERASE  : 0)
                         | (is_checked(IDC_F_DOVERIFY) ? TLSR_PROG_VERIFY : 0);
            if (id == IDC_F_PROG) {
                /* Spell out what the checkboxes changed, because skipping a
                 * stage is exactly the choice worth confirming twice. */
                snprintf(msg, sizeof msg,
                         "Program flash at 0x%06X from\n%s\n(file offset 0x%X)?"
                         "\n\n%s\n%s\n\nThis is destructive.",
                         (unsigned)a, g_job.path, (unsigned)off,
                         (g_job.stages & TLSR_PROG_ERASE)
                           ? "Erase: sectors are erased first, and the rest of "
                             "each touched sector is preserved."
                           : "Erase: SKIPPED. Nothing is erased and neighbours "
                             "are left alone, but flash can only clear bits - "
                             "this works solely on already-erased sectors.",
                         (g_job.stages & TLSR_PROG_VERIFY)
                           ? "Verify: the write is read back and compared."
                           : "Verify: SKIPPED. A bad write will not be "
                             "detected.");
                if (MessageBoxA(g_main, msg, APP_TITLE,
                                MB_YESNO | MB_ICONWARNING) != IDYES)
                    return 0;
            }
            submit(job_flash_program);
            return 0;
        }

        case IDC_B_SELF:    submit(job_selftest); return 0;
        case IDC_B_PIN:     submit(job_pintest);  return 0;
        case IDC_B_REFRESH: submit(job_cfg_get);  return 0;
        case IDC_B_APPLY:   submit(job_cfg_set);  return 0;
        case IDC_B_GO: {
            uint32_t frames = 0;
            if (get_num(IDC_B_ACT, &frames) || !frames || frames > 65535) {
                ui_log("! activation frames must be 1..65535");
                return 0;
            }
            g_job.len = frames;
            submit(job_activate);
            return 0;
        }
        }
        return 0;
    }

    case WM_CLOSE:
        if (g_dev) tlsr_close(g_dev);
        DestroyWindow(h);
        return 0;

    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcA(h, m, w, l);
}

/* ------------------------------------------------------- control creation -- */
static void build_controls(void)
{
    int y;
    TCITEMA ti;

    /* The tooltip control must exist before any icon button registers with it. */
    g_tip = CreateWindowExA(0, TOOLTIPS_CLASSA, NULL, WS_POPUP | TTS_ALWAYSTIP,
                            CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT,
                            CW_USEDEFAULT, g_main, NULL, g_inst, NULL);

    /* --- top bar ------------------------------------------------------- */
    mk_static("Port", 10, 12, 30, IDC_PORTLBL);
    mk_combo(45, 8, 95, IDC_PORT, 0);
    mk_button("Rescan", 148, 8, 66, IDC_RESCAN);
    mk_button("Connect", 220, 8, 80, IDC_CONNECT);
    mk_static("disconnected", 310, 12, 150, IDC_STATUS);
    /* Target power: shows the rail state and switches it, in the place the
     * "Device: ..." label used to sit.  A 32 px square sharing its top edge
     * with the row's 24 px buttons, so it lines up with Rescan and Connect and
     * nothing else in the top bar moves.  Being 8 px taller, it reaches y=40 --
     * exactly TAB_TOP, so its last row is 39 and it still clears the tab strip.
     * Both faces are loaded up front; WM_TIMER picks one. */
    g_ico_pwr_on  = (HICON)LoadImageA(g_inst, MAKEINTRESOURCEA(IDI_PWR_ON),
                                      IMAGE_ICON, 24, 24, LR_DEFAULTCOLOR);
    g_ico_pwr_off = (HICON)LoadImageA(g_inst, MAKEINTRESOURCEA(IDI_PWR_OFF),
                                      IMAGE_ICON, 24, 24, LR_DEFAULTCOLOR);
    mk_iconbtn(IDI_PWR_OFF, 466, 8, IDC_POWER, "Target power");
    EnableWindow(GetDlgItem(g_main, IDC_POWER), FALSE);
    /* PBM_SETBARCOLOR is ignored while the control is themed, and the manifest
     * asks for comctl32 v6, so theming comes off this one control -- otherwise
     * the bar stays system green whatever stage is running.
     *
     * That has a catch worth spelling out: an un-themed bar draws no frame of
     * its own and paints its unfilled part in the dialog colour, so at 0% it
     * vanished into the background completely.  A sunken client edge and a
     * track a shade darker than the window put back the bordered grey trough
     * the themed control drew for us. */
    {
        HWND pb = CreateWindowExA(WS_EX_CLIENTEDGE, PROGRESS_CLASSA, "",
                                  WS_CHILD | WS_CLIPSIBLINGS, 660, 6, 180, 16,
                                  g_main, (HMENU)(INT_PTR)IDC_PROGRESS,
                                  g_inst, NULL);
        unthemify(pb);
        SendMessageA(pb, PBM_SETRANGE32, 0, 100);
        SendMessageA(pb, PBM_SETBKCOLOR, 0, (LPARAM)RGB(230, 230, 230));
        set_bar_stage(TLSR_STAGE_IDLE);
    }

    /* Sits under the bar, hidden until an operation has something to report.
     * SS_CENTERIMAGE is what centres the text vertically in whatever height
     * layout() gives it; SS_RIGHT keeps it flush with the bar's right end. */
    {
        HWND v = ctl("STATIC", "", SS_RIGHT | SS_CENTERIMAGE,
                     660, VERDICT_Y, 180, VERDICT_H, IDC_VERDICT);
        SendMessageA(v, WM_SETFONT, (WPARAM)g_bold, TRUE);
    }

    /* --- tabs ---------------------------------------------------------- */
    g_tabs = ctl(WC_TABCONTROLA, "", 0, 8, TAB_TOP, 800, 360, IDC_TABS);
    ShowWindow(g_tabs, SW_SHOW);
    memset(&ti, 0, sizeof ti);
    ti.mask = TCIF_TEXT;
    ti.pszText = (LPSTR)"Target";  TabCtrl_InsertItem(g_tabs, 0, &ti);
    ti.pszText = (LPSTR)"Memory";  TabCtrl_InsertItem(g_tabs, 1, &ti);
    ti.pszText = (LPSTR)"Flash";   TabCtrl_InsertItem(g_tabs, 2, &ti);
    ti.pszText = (LPSTR)"Bridge";  TabCtrl_InsertItem(g_tabs, 3, &ti);
    ti.pszText = (LPSTR)"Debugger"; TabCtrl_InsertItem(g_tabs, 4, &ti);

    /* --- Target tab ---------------------------------------------------- */
    y = PAGE_TOP;
    mk_button("Re-probe",  PAGE_LEFT,       y, 90, IDC_T_PROBE);
    mk_button("Power on",  PAGE_LEFT + 96,  y, 90, IDC_T_PWRON);
    mk_button("Power off", PAGE_LEFT + 192, y, 90, IDC_T_PWROFF);
    mk_button("Halt core", PAGE_LEFT + 288, y, 90, IDC_T_HALT);
    mk_button("Run core",  PAGE_LEFT + 384, y, 90, IDC_T_RUN);
    mk_button("Reset+run", PAGE_LEFT + 480, y, 90, IDC_T_RESET);
    {
        HWND l = mk_list(0, 0, 10, 10, IDC_T_IDENT);      /* placed by layout() */
        lv_column(l, 0, "Property", 120);
        lv_column(l, 1, "Value", 230);
        l = mk_list(0, 0, 10, 10, IDC_T_CAPS);
        lv_column(l, 0, "Capability", 200);
        lv_column(l, 1, "?", 40);
        lv_column(l, 2, "Note", 300);
        l = mk_list(0, 0, 10, 10, IDC_T_REGIONS);
        lv_column(l, 0, "Region", 150);
        lv_column(l, 1, "Range", 220);
        lv_column(l, 2, "Access", 150);
    }

    /* --- Memory tab ------------------------------------------------------
     * Two framed halves: everything that only looks at the target, then
     * everything that changes it.  The frames are created last so they sit
     * behind their contents (see mk_group). */
    {
        int cx = GRP_IN;                 /* content left, inset into the frame */
        int r  = M_READ_Y;               /* read row                           */
        int w  = M_WRITE_Y + 22;         /* write row                          */

        mk_static("Region", cx, r + 4, 45, IDC_M_L1);
        mk_combo(cx + 48, r, 90, IDC_M_REGION, CBS_OWNERDRAWFIXED);
        cb_add(IDC_M_REGION, "Flash");
        cb_add(IDC_M_REGION, "SRAM");
        cb_add(IDC_M_REGION, "Registers");
        cb_sel(IDC_M_REGION, REGION_SRAM);
        mk_static("Address", cx + 146, r + 4, 50, IDC_M_L2);
        mk_edit("0x040000", cx + 198, r, 85, IDC_M_ADDR);
        mk_combo(cx + 291, r, 110, IDC_M_MODE, 0);
        cb_add(IDC_M_MODE, "Size (bytes)");
        cb_add(IDC_M_MODE, "End address");
        cb_sel(IDC_M_MODE, MODE_SIZE);
        mk_static("Size", cx + 409, r + 4, 100, IDC_M_L3);
        mk_edit("256", cx + 511, r, 85, IDC_M_LEN);
        mk_button("Read", 0, 0, 70, IDC_M_READ);        /* placed by layout() */
        mk_button("Save...", 0, 0, 80, IDC_M_SAVE);
        mk_static("", cx, r + 30, 500, IDC_M_NOTE);

        mk_static("Bytes ->", cx, w + 4, 120, IDC_M_PATCHLBL);
        mk_edit("", cx + 124, w, 300, IDC_M_PATCH);
        /* This tab writes SRAM and registers, which are directly writable --
         * there is no erase stage to skip.  The box is shown so both write
         * sections read the same, but it is disabled rather than pretending. */
        mk_check("Erase",  cx + 358, w + 2, 62, IDC_M_DOERASE, 1);
        EnableWindow(GetDlgItem(g_main, IDC_M_DOERASE), FALSE);
        add_tip(GetDlgItem(g_main, IDC_M_DOERASE),
                "Flash only. SRAM and registers are written directly, so "
                "there is nothing to erase. Programming flash is done on the "
                "Flash tab.");
        mk_check("Verify", cx + 426, w + 2, 66, IDC_M_DOVERIFY, 1);
        add_tip(GetDlgItem(g_main, IDC_M_DOVERIFY),
                "Read the bytes back after writing and compare them.");
        mk_button("Write", 0, 0, 80, IDC_M_WRITE);      /* placed by layout() */

        mk_out(PAGE_LEFT, M_OUT_Y, 770, 200, IDC_M_OUT);

        mk_group("Read",  PAGE_LEFT, M_GRPR_Y, 770, M_GRPR_H, IDC_M_GRPR);
        mk_group("Write", PAGE_LEFT, M_GRPW_Y, 770, M_GRPW_H, IDC_M_GRPW);
    }

    /* --- Flash tab ------------------------------------------------------- */
    {
        int cx = GRP_IN;
        int r  = F_READ_Y;
        int f  = F_WRITE_Y + 20;         /* file row                           */
        int o  = F_WRITE_Y + 50;         /* offsets row                        */
        int b  = F_WRITE_Y + 80;         /* checkboxes + action buttons row    */

        mk_static("Address", cx, r + 4, 55, IDC_F_L1);
        mk_edit("0x000000", cx + 58, r, 85, IDC_F_ADDR);
        mk_combo(cx + 151, r, 110, IDC_F_MODE, 0);
        cb_add(IDC_F_MODE, "Size (bytes)");
        cb_add(IDC_F_MODE, "End address");
        cb_sel(IDC_F_MODE, MODE_SIZE);
        mk_static("Size", cx + 269, r + 4, 100, IDC_F_L2);
        mk_edit("256", cx + 371, r, 85, IDC_F_LEN);
        /* Three buttons share this row, so the labels are trimmed and the
         * trailing "..." carries the usual meaning: opens a file dialog. */
        mk_button("Read range", 0, 0, 95, IDC_F_READ);    /* placed by layout() */
        mk_button("Dump range...", 0, 0, 115, IDC_F_DUMPRNG);
        mk_button("Dump whole", 0, 0, 95, IDC_F_DUMP);
        add_tip(GetDlgItem(g_main, IDC_F_DUMPRNG),
                "Save the address range above to a .bin file. Reads it from "
                "the target first if it is not already shown below.");
        add_tip(GetDlgItem(g_main, IDC_F_DUMP),
                "Save the whole flash to a .bin file.");
        mk_static("", cx, r + 30, 500, IDC_F_NOTE);

        mk_static("File", cx, f + 4, 30, IDC_F_L3);
        mk_edit("", cx + 35, f, 500, IDC_F_FILE);         /* width by layout() */
        mk_button("Browse...", 0, 0, 92, IDC_F_BROWSE);

        mk_static("File offset", cx, o + 4, 70, IDC_F_L4);
        mk_edit("0", cx + 72, o, 80, IDC_F_OFF);
        mk_static("Length (0=all)", cx + 162, o + 4, 90, IDC_F_L5);
        mk_edit("0", cx + 254, o, 80, IDC_F_PLEN);
        mk_static("-> flash address", cx + 344, o + 4, 105, IDC_F_L6);
        mk_edit("0x000000", cx + 450, o, 90, IDC_F_PADDR);

        mk_check("Erase",  cx, b + 2, 62, IDC_F_DOERASE, 1);
        add_tip(GetDlgItem(g_main, IDC_F_DOERASE),
                "Erase the touched sectors first, preserving the rest of each "
                "one. Unticking skips both the erase and the preserve step: "
                "flash can only clear bits, so that works solely where the "
                "sectors are already erased.");
        mk_check("Verify", cx + 68, b + 2, 66, IDC_F_DOVERIFY, 1);
        add_tip(GetDlgItem(g_main, IDC_F_DOVERIFY),
                "Read the programmed bytes back and compare them. Unticking "
                "makes it faster and means a bad write goes unnoticed.");
        mk_button("Erase sectors", 0, 0, 110, IDC_F_ERASE);  /* by layout() */
        mk_button("Program", 0, 0, 85, IDC_F_PROG);
        mk_button("Verify", 0, 0, 80, IDC_F_VERIFY);

        mk_out(PAGE_LEFT, F_OUT_Y, 770, 180, IDC_F_OUT);

        mk_group("Read",  PAGE_LEFT, F_GRPR_Y, 770, F_GRPR_H, IDC_F_GRPR);
        mk_group("Write", PAGE_LEFT, F_GRPW_Y, 770, F_GRPW_H, IDC_F_GRPW);
    }

    /* --- Bridge tab ---------------------------------------------------- */
    y = PAGE_TOP;
    mk_button("Self-test", PAGE_LEFT, y, 90, IDC_B_SELF);
    mk_button("Pin test", PAGE_LEFT + 96, y, 90, IDC_B_PIN);
    mk_static("Power rail", PAGE_LEFT + 200, y + 4, 70, IDC_B_L1);
    mk_combo(PAGE_LEFT + 272, y, 90, IDC_B_RAIL, 0);
    cb_add(IDC_B_RAIL, "both");
    cb_add(IDC_B_RAIL, "pb0");
    cb_add(IDC_B_RAIL, "mosfet");
    cb_sel(IDC_B_RAIL, 0);
    mk_static("Activation frames", PAGE_LEFT + 376, y + 4, 110, IDC_B_L2);
    mk_edit("600", PAGE_LEFT + 488, y, 70, IDC_B_ACT);
    mk_button("Activate", PAGE_LEFT + 566, y, 90, IDC_B_GO);
    {
        int i;
        for (i = 0; i < NUM_CFG; i++) {
            int cx = PAGE_LEFT + (i % 5) * 155;
            int cy = y + 44 + (i / 5) * 30;
            mk_static(g_cfg_names[i], cx, cy + 4, 80, IDC_B_CFGL0 + i);
            mk_edit("", cx + 84, cy, 55, IDC_B_CFG0 + i);
        }
    }
    mk_button("Refresh", PAGE_LEFT, y + 110, 90, IDC_B_REFRESH);
    mk_button("Apply", PAGE_LEFT + 96, y + 110, 90, IDC_B_APPLY);
    mk_static("Timing changes are not persistent: unplugging the bridge "
              "restores the measured defaults.  Sampling slower than the "
              "default makes reads intermittent rather than failing outright, "
              "which is a harder fault to spot.",
              PAGE_LEFT, y + 146, 760, IDC_B_HINT);

    /* --- Debugger tab -------------------------------------------------- */
    y = PAGE_TOP;
    {
        int x = PAGE_LEFT;
        const int P = 36, G = 14;      /* button pitch, extra gap per group   */

        mk_iconbtn(IDI_D_CONTINUE, x, y, IDC_D_CONTINUE,
                   "Continue - resume the core (0x0602 <- 0x84)");      x += P;
        mk_iconbtn(IDI_D_PAUSE, x, y, IDC_D_PAUSE,
                   "Pause - stall the core and read its registers "
                   "(0x0602 <- 0x06)");                                 x += P;
        mk_iconbtn(IDI_D_STOP, x, y, IDC_D_STOP,
                   "Stop - the flash-safe halt (0x0602 <- 0x05)");      x += P;
        mk_iconbtn(IDI_D_RESTART, x, y, IDC_D_RESTART,
                   "Restart - reset and run (0x0602 <- 0x88)");   x += P + G;

        mk_iconbtn(IDI_D_STEPINTO, x, y, IDC_D_STEPINTO,
                   "Step into - one instruction (0x0613 <- 0x80)");     x += P;
        mk_iconbtn(IDI_D_STEPOVER, x, y, IDC_D_STEPOVER,
                   "Step over - step once, and if that entered a call, "
                   "run to the return address");                        x += P;
        mk_iconbtn(IDI_D_STEPOUT, x, y, IDC_D_STEPOUT,
                   "Step out - run to the address in LR");              x += P;
        mk_iconbtn(IDI_D_STEPINST, x, y, IDC_D_STEPINST,
                   "Step one instruction");                       x += P + G;

        mk_iconbtn(IDI_D_RUNCUR, x, y, IDC_D_RUNCUR,
                   "Run to the address in the box");              x += P + G;

        mk_iconbtn(IDI_D_BPTOGGLE, x, y, IDC_D_BPTOGGLE,
                   "Toggle a hardware breakpoint at that address "
                   "(0x0614 <- addr | bit24)");                         x += P;
        mk_iconbtn(IDI_D_BPCLEAR, x, y, IDC_D_BPCLEAR,
                   "Clear the breakpoint block");                 x += P + G;

        mk_iconbtn(IDI_D_REFRESH, x, y, IDC_D_REFRESH,
                   "Re-read the CPU register file (0x0680)");
    }
    mk_static("Address", PAGE_LEFT, y + 46, 55, IDC_D_ADDRLBL);
    mk_edit("0x000000", PAGE_LEFT + 58, y + 42, 90, IDC_D_ADDR);
    mk_static("", PAGE_LEFT + 330, y + 46, 400, IDC_D_STATE);
    mk_static("", PAGE_LEFT, y + 70, 760, IDC_D_NOTE);   /* text set by
                                                          * dbg_update_enable */
    {
        HWND l = mk_list(PAGE_LEFT, y + 94, 770, 200, IDC_D_REGS);
        lv_column(l, 0, "Register", 130);
        lv_column(l, 1, "Hex", 120);
        lv_column(l, 2, "Decimal / note", 300);
    }

    /* --- log ----------------------------------------------------------- */
    g_log = CreateWindowExA(WS_EX_CLIENTEDGE, "EDIT", "",
                            WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS |
                            WS_VSCROLL | ES_MULTILINE |
                            ES_READONLY | ES_AUTOVSCROLL,
                            8, 410, 800, LOG_H, g_main, (HMENU)IDC_LOG,
                            g_inst, NULL);
    SendMessageA(g_log, WM_SETFONT, (WPARAM)g_mono, TRUE);

    /* The page controls are siblings of the tab control, and CreateWindowEx
     * puts each new child at the BOTTOM of the z-order -- so everything built
     * after the tab ended up behind it and the tab painted over the lot.  That
     * is what made every page look empty.  Sinking the tab control to the
     * bottom once puts the pages back on top; it cannot hide anything, because
     * the only sibling it overlaps is the page it is supposed to sit behind. */
    SetWindowPos(g_tabs, HWND_BOTTOM, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);

    /* Top-bar controls live outside the tab pages and are always visible. */
    ShowWindow(GetDlgItem(g_main, IDC_PORTLBL), SW_SHOW);
    ShowWindow(GetDlgItem(g_main, IDC_PORT), SW_SHOW);
    ShowWindow(GetDlgItem(g_main, IDC_RESCAN), SW_SHOW);
    ShowWindow(GetDlgItem(g_main, IDC_CONNECT), SW_SHOW);
    ShowWindow(GetDlgItem(g_main, IDC_STATUS), SW_SHOW);
    ShowWindow(GetDlgItem(g_main, IDC_POWER), SW_SHOW);
    ShowWindow(GetDlgItem(g_main, IDC_PROGRESS), SW_SHOW);

    update_note(IDC_M_NOTE, REGION_SRAM, MODE_SIZE);
    update_note(IDC_F_NOTE, REGION_FLASH, MODE_SIZE);
    update_patch_label();
    dbg_update_enable();
}

/* -------------------------------------------------------------- WinMain --- */
int WINAPI WinMain(HINSTANCE inst, HINSTANCE prev, LPSTR cmd, int show)
{
    WNDCLASSEXA wc;
    INITCOMMONCONTROLSEX ic;
    MSG msg;
    HWND h;

    (void)prev; (void)cmd;
    g_inst = inst;

    InitializeCriticalSection(&g_lock);
    g_job_evt = CreateEventA(NULL, FALSE, FALSE, NULL);
    CreateThread(NULL, 0, worker_main, NULL, 0, NULL);

    ic.dwSize = sizeof ic;
    ic.dwICC  = ICC_WIN95_CLASSES | ICC_TAB_CLASSES | ICC_PROGRESS_CLASS |
                ICC_LISTVIEW_CLASSES;
    InitCommonControlsEx(&ic);

    g_font = CreateFontA(-12, 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET,
                         OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                         CLEARTYPE_QUALITY, DEFAULT_PITCH, "Segoe UI");
    g_mono = CreateFontA(-12, 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET,
                         OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                         CLEARTYPE_QUALITY, FIXED_PITCH, "Consolas");
    /* PASS / FAIL is the one thing on screen that has to read from across a
     * bench, so it is both larger and heavier than the UI font.  It has the
     * band between the progress bar and the tab strip to itself. */
    g_bold = CreateFontA(-19, 0, 0, 0, FW_BOLD, 0, 0, 0, DEFAULT_CHARSET,
                         OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                         CLEARTYPE_QUALITY, DEFAULT_PITCH, "Segoe UI");

    memset(&wc, 0, sizeof wc);
    wc.cbSize        = sizeof wc;
    wc.lpfnWndProc   = wnd_proc;
    wc.hInstance     = inst;
    wc.hCursor       = LoadCursor(NULL, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    wc.lpszClassName = "TlsrProgrammerWnd";
    wc.hIcon         = LoadIcon(NULL, IDI_APPLICATION);
    RegisterClassExA(&wc);

    h = CreateWindowExA(0, wc.lpszClassName, APP_CAPTION, WS_OVERLAPPEDWINDOW,
                        CW_USEDEFAULT, CW_USEDEFAULT, 900, 640,
                        NULL, NULL, inst, NULL);
    if (!h)
        return 1;
    ShowWindow(h, show);
    UpdateWindow(h);

    ui_log("%s - connect to the bridge, then use the tabs.", APP_CAPTION);
    ui_log("The target board must be UNPLUGGED FROM MAINS if it is a mains "
           "device: such supplies are usually not isolated.");
    ui_log("Not every debug feature exists on this hardware.  Measured on a "
           "TLSR8253: the CPU debug block does not answer - the register file "
           "at 0x0680 and the PC latch at 0x06BC both read back as zero, and "
           "0x0602 <- 0x06 does not stall the core.  Single-step, breakpoints "
           "and run-to-cursor therefore cannot work and are greyed out; "
           "halt/resume/reset, memory, flash and the register probe are fine.");
    ui_log("Resuming the core can also cost you the SWire link: once the target "
           "firmware boots it may reconfigure the SWS pad as GPIO.  Recovery "
           "is a power cycle with an activation burst (Target -> Re-probe).");

    while (GetMessageA(&msg, NULL, 0, 0) > 0) {
        if (!IsDialogMessageA(h, &msg)) {
            TranslateMessage(&msg);
            DispatchMessageA(&msg);
        }
    }
    return 0;
}
