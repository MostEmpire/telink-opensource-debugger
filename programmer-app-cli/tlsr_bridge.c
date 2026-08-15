/*
 * tlsr_bridge.c - command framing and SWire register/SRAM access.
 *
 * Copyright (C) 2026  telink_project contributors
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * The bridge speaks a tiny request/response protocol:
 *
 *      host   -> 0x55 | CMD    | LEN_LO | LEN_HI | payload
 *      bridge -> 0x55 | STATUS | LEN_LO | LEN_HI | payload
 *
 * All the SWire waveform work happens on the bridge; this file only marshals
 * arguments and results.  The one piece of protocol knowledge that lives here
 * is the distinction between an incrementing read and a FIFO read, which the
 * flash layer depends on.
 */
#include <stdio.h>
#include <string.h>

#include "tlsr_core.h"
#include "tlsr_internal.h"

static const char *status_text(uint8_t st)
{
    switch (st) {
    case 0x00: return "OK";
    case 0x01: return "bad command";
    case 0x02: return "bad length";
    case 0x03: return "SWire timeout (target did not reply)";
    case 0x04: return "no sync (the bridge could not decode its own frame)";
    case 0x05: return "busy";
    case 0x06: return "target power is off";
    default:   return "unknown status";
    }
}

void tlsr_set_stage_cb(tlsr_dev *d, tlsr_stage_fn fn, void *user)
{
    if (!d)
        return;
    d->stage_cb   = fn;
    d->stage_user = user;
}

void tlsr_stage(tlsr_dev *d, int stage)
{
    if (d && d->stage_cb)
        d->stage_cb(d->stage_user, stage);
}

int tlsr_cmd(tlsr_dev *d, uint8_t cmd, const uint8_t *payload, uint16_t len,
             uint8_t *reply, uint16_t reply_max, uint16_t *reply_len,
             unsigned timeout_ms)
{
    uint8_t hdr[4];
    uint16_t n;
    int rc;

    if (!d) {
        tlsr_set_error("not connected");
        return TLSR_E_ARG;
    }

    hdr[0] = TLSR_BR_SYNC;
    hdr[1] = cmd;
    hdr[2] = (uint8_t)(len & 0xFF);
    hdr[3] = (uint8_t)(len >> 8);

    rc = tlsr_write_all(d, hdr, 4);
    if (rc != TLSR_OK)
        return rc;
    if (len) {
        rc = tlsr_write_all(d, payload, len);
        if (rc != TLSR_OK)
            return rc;
    }

    rc = tlsr_read_exact(d, hdr, 4, timeout_ms);
    if (rc != TLSR_OK)
        return rc;
    if (hdr[0] != TLSR_BR_SYNC) {
        /* Out of step with the bridge: drop whatever is queued so the next
         * command starts from a clean stream rather than compounding. */
        tlsr_purge(d);
        tlsr_set_error("bad reply header 0x%02X (expected 0x55)", hdr[0]);
        return TLSR_E_PROTO;
    }

    /* Take what the caller asked for and drain the rest.  Discarding rather
     * than failing keeps the byte stream in step, and means a firmware that
     * grows its reply (as CMD_PWR did when it started returning the rail mask)
     * does not break older callers. */
    n = (uint16_t)(hdr[2] | (hdr[3] << 8));
    {
        uint16_t take = (n < reply_max) ? n : reply_max;
        uint16_t left = (uint16_t)(n - take);

        if (take) {
            rc = tlsr_read_exact(d, reply, take, timeout_ms);
            if (rc != TLSR_OK)
                return rc;
        }
        while (left) {
            uint8_t scratch[64];
            uint16_t k = (left > sizeof scratch) ? (uint16_t)sizeof scratch : left;
            rc = tlsr_read_exact(d, scratch, k, timeout_ms);
            if (rc != TLSR_OK)
                return rc;
            left = (uint16_t)(left - k);
        }
        if (reply_len)
            *reply_len = take;
    }

    if (hdr[1] != TLSR_ST_OK) {
        tlsr_set_error("bridge rejected command 0x%02X: %s",
                       cmd, status_text(hdr[1]));
        return TLSR_E_BRIDGE;
    }
    return TLSR_OK;
}

int tlsr_ping(tlsr_dev *d, char id[9], uint16_t *version)
{
    uint8_t r[32];
    uint16_t n = 0;
    int rc;

    tlsr_purge(d);
    rc = tlsr_cmd(d, TLSR_CMD_PING, NULL, 0, r, sizeof r, &n, 1500);
    if (rc != TLSR_OK)
        return rc;
    if (n < 8 || memcmp(r, "TLSRSWS", 7) != 0) {
        tlsr_set_error("this port did not answer like a TLSR bridge "
                       "(is it the right COM port?)");
        return TLSR_E_PROTO;
    }
    if (id) {
        memcpy(id, r, 8);
        id[8] = 0;
    }
    if (version)
        *version = (uint16_t)(n >= 10 ? (r[8] | (r[9] << 8)) : 0);
    return TLSR_OK;
}

int tlsr_get_cfg(tlsr_dev *d, tlsr_cfg *cfg, uint8_t *rails)
{
    uint8_t r[32];
    uint16_t n = 0;
    int rc = tlsr_cmd(d, TLSR_CMD_GET_CFG, NULL, 0, r, sizeof r, &n, 1500);
    if (rc != TLSR_OK)
        return rc;
    if (n < 12) {
        tlsr_set_error("short configuration reply (%u bytes)", n);
        return TLSR_E_PROTO;
    }
    if (cfg) {
        cfg->spi_div    = r[0];
        cfg->cell       = r[1];
        cfg->low0       = r[2];
        cfg->low1       = r[3];
        cfg->thr        = r[4];
        cfg->addr_bytes = r[5];
        cfg->slave_bits = r[6];
        cfg->slave_off  = r[7];
        cfg->slack      = r[8];
        d->cfg = *cfg;
        d->cfg_valid = 1;
    }
    if (rails)
        *rails = r[11];
    return TLSR_OK;
}

int tlsr_set_cfg(tlsr_dev *d, const tlsr_cfg *cfg)
{
    uint8_t p[9];
    if (!cfg)
        return TLSR_E_ARG;
    p[0] = cfg->spi_div;  p[1] = cfg->cell;      p[2] = cfg->low0;
    p[3] = cfg->low1;     p[4] = cfg->thr;       p[5] = cfg->addr_bytes;
    p[6] = cfg->slave_bits; p[7] = cfg->slave_off; p[8] = cfg->slack;
    return tlsr_cmd(d, TLSR_CMD_SET_CFG, p, sizeof p, NULL, 0, NULL, 1500);
}

int tlsr_power(tlsr_dev *d, int on, uint8_t rails)
{
    uint8_t p[2];
    p[0] = on ? 1 : 0;
    p[1] = rails ? rails : TLSR_RAIL_BOTH;
    return tlsr_cmd(d, TLSR_CMD_PWR, p, 2, NULL, 0, NULL, 3000);
}

int tlsr_activate(tlsr_dev *d, uint16_t frames)
{
    uint8_t p[2];
    uint8_t r[8];
    uint16_t n = 0;
    if (!frames)
        frames = TLSR_ACTIVATE_FRAMES;
    p[0] = (uint8_t)(frames & 0xFF);
    p[1] = (uint8_t)(frames >> 8);
    /* The bridge power-cycles the target and then hammers the post-reset
     * window, so this takes a few hundred milliseconds. */
    return tlsr_cmd(d, TLSR_CMD_ACTIVATE, p, 2, r, sizeof r, &n, 20000);
}

int tlsr_selftest(tlsr_dev *d, uint8_t echo[8], int *ok)
{
    uint8_t r[16];
    uint16_t n = 0;
    int rc = tlsr_cmd(d, TLSR_CMD_SELFTEST, NULL, 0, r, sizeof r, &n, 5000);
    if (rc != TLSR_OK)
        return rc;
    if (n < 1) {
        tlsr_set_error("empty self-test reply");
        return TLSR_E_PROTO;
    }
    if (ok)
        *ok = r[0] ? 1 : 0;
    if (echo) {
        memset(echo, 0, 8);
        memcpy(echo, r + 1, (n - 1) > 8 ? 8 : (size_t)(n - 1));
    }
    return TLSR_OK;
}

int tlsr_pintest(tlsr_dev *d, uint8_t levels[3], int *ok)
{
    uint8_t r[8];
    uint16_t n = 0;
    int rc = tlsr_cmd(d, TLSR_CMD_PINTEST, NULL, 0, r, sizeof r, &n, 5000);
    if (rc != TLSR_OK)
        return rc;
    if (n < 4) {
        tlsr_set_error("short pin-test reply");
        return TLSR_E_PROTO;
    }
    if (ok)
        *ok = r[0] ? 1 : 0;
    if (levels) {
        levels[0] = r[1];
        levels[1] = r[2];
        levels[2] = r[3];
    }
    return TLSR_OK;
}

int tlsr_get_raw(tlsr_dev *d, uint8_t *out, uint32_t max, uint32_t *len)
{
    uint32_t off = 0;
    for (;;) {
        uint8_t p[4];
        uint16_t got = 0;
        uint32_t want = max - off;
        int rc;

        if (want == 0)
            break;
        if (want > 1024)
            want = 1024;
        p[0] = (uint8_t)(off & 0xFF);
        p[1] = (uint8_t)(off >> 8);
        p[2] = (uint8_t)(want & 0xFF);
        p[3] = (uint8_t)(want >> 8);
        rc = tlsr_cmd(d, TLSR_CMD_GET_RAW, p, 4, out + off,
                      (uint16_t)want, &got, 3000);
        if (rc != TLSR_OK)
            return rc;
        if (got == 0)
            break;                 /* end of the captured buffer */
        off += got;
    }
    if (len)
        *len = off;
    return TLSR_OK;
}

/* ------------------------------------------------- SWire register / SRAM ---- */
/* A plain read: the target auto-increments its address across the burst. */
int tlsr_read(tlsr_dev *d, uint32_t addr, uint8_t *out, uint32_t n)
{
    uint32_t done = 0;
    while (done < n) {
        uint8_t p[5];
        uint16_t got = 0, want = (uint16_t)((n - done > TLSR_CHUNK)
                                            ? TLSR_CHUNK : (n - done));
        uint32_t a = addr + done;
        int rc;

        p[0] = (uint8_t)(a >> 16);
        p[1] = (uint8_t)(a >> 8);
        p[2] = (uint8_t)a;
        p[3] = (uint8_t)(want & 0xFF);
        p[4] = (uint8_t)(want >> 8);
        rc = tlsr_cmd(d, TLSR_CMD_SWS_READ, p, 5, out + done, want, &got, 5000);
        if (rc != TLSR_OK)
            return rc;
        if (got != want) {
            tlsr_set_error("short read at 0x%06X: wanted %u, got %u",
                           (unsigned)a, want, got);
            return TLSR_E_PROTO;
        }
        done += got;
    }
    return TLSR_OK;
}

int tlsr_write(tlsr_dev *d, uint32_t addr, const uint8_t *data, uint32_t n)
{
    uint32_t done = 0;
    while (done < n) {
        uint8_t p[3 + TLSR_CHUNK];
        uint32_t chunk = (n - done > TLSR_CHUNK) ? TLSR_CHUNK : (n - done);
        uint32_t a = addr + done;
        int rc;

        p[0] = (uint8_t)(a >> 16);
        p[1] = (uint8_t)(a >> 8);
        p[2] = (uint8_t)a;
        memcpy(p + 3, data + done, chunk);
        rc = tlsr_cmd(d, TLSR_CMD_SWS_WRITE, p, (uint16_t)(chunk + 3),
                      NULL, 0, NULL, 5000);
        if (rc != TLSR_OK)
            return rc;
        done += chunk;
    }
    return TLSR_OK;
}

int tlsr_read8(tlsr_dev *d, uint32_t addr, uint8_t *out)
{
    return tlsr_read(d, addr, out, 1);
}

int tlsr_write8(tlsr_dev *d, uint32_t addr, uint8_t val)
{
    return tlsr_write(d, addr, &val, 1);
}

/* ------------------------------------------------------------ core control -- */
int tlsr_halt(tlsr_dev *d)
{
    return tlsr_write8(d, TLSR_REG_CPU_CTRL, TLSR_CPU_HALT);
}

int tlsr_run(tlsr_dev *d)
{
    return tlsr_write8(d, TLSR_REG_CPU_CTRL, TLSR_CPU_RUN);
}

int tlsr_reboot(tlsr_dev *d)
{
    return tlsr_write8(d, TLSR_REG_CPU_CTRL, TLSR_CPU_REBOOT);
}

/* An un-activated SWire slave does not stay silent -- it returns noise, and a
 * single read can easily land on a value that is neither 0x0000 nor 0xFFFF.
 * Requiring two identical reads rejects that, which a simple range check does
 * not: a stale session once reported a confident-looking 0xAA64. */
static int chip_id_stable(tlsr_dev *d, uint16_t *out)
{
    uint8_t a[2], b[2];
    uint16_t id;

    if (tlsr_read(d, TLSR_REG_CHIP_ID, a, 2) != TLSR_OK)
        return 0;
    if (tlsr_read(d, TLSR_REG_CHIP_ID, b, 2) != TLSR_OK)
        return 0;
    if (a[0] != b[0] || a[1] != b[1])
        return 0;
    id = (uint16_t)(a[0] | (a[1] << 8));
    if (id == 0x0000 || id == 0xFFFF)
        return 0;
    if (out)
        *out = id;
    return 1;
}

int tlsr_ensure_target(tlsr_dev *d, tlsr_log_fn log, void *user)
{
    uint8_t rails = 0;
    uint16_t chip = 0;
    int rc;

    rc = tlsr_get_cfg(d, NULL, &rails);
    if (rc != TLSR_OK)
        return rc;

    if (!rails) {
        TLSR_LOG(log, user, "target was off - powering up");
        rc = tlsr_power(d, 1, TLSR_RAIL_BOTH);
        if (rc != TLSR_OK)
            return rc;
        Sleep(200);
        d->activated = 0;          /* a fresh power-up needs activating */
    }

    /* Activate at least once per connection.  Power being on is not enough:
     * the target may have been left powered but un-activated by a previous
     * session, and it will happily return noise in that state. */
    if (d->activated && chip_id_stable(d, &chip))
        return TLSR_OK;

    TLSR_LOG(log, user, "activating target");
    rc = tlsr_activate(d, TLSR_ACTIVATE_FRAMES);
    if (rc != TLSR_OK)
        return rc;

    if (!chip_id_stable(d, &chip)) {
        TLSR_LOG(log, user, "no stable answer - retrying with a longer burst");
        rc = tlsr_activate(d, TLSR_ACTIVATE_FRAMES * 4);
        if (rc != TLSR_OK)
            return rc;
        if (!chip_id_stable(d, &chip)) {
            tlsr_set_error("target did not respond with a stable chip id. "
                           "Check the SWS wiring, and that the outlet is "
                           "unplugged from mains and powered by the bridge.");
            d->activated = 0;
            return TLSR_E_TARGET;
        }
    }
    d->activated = 1;
    return TLSR_OK;
}

/* --------------------------------------------------------------- utilities -- */
char *tlsr_hex(char *buf, size_t buflen, const uint8_t *data, size_t n)
{
    size_t i, off = 0;
    if (!buflen)
        return buf;
    buf[0] = 0;
    for (i = 0; i < n; i++) {
        int w = snprintf(buf + off, buflen - off, i ? " %02X" : "%02X", data[i]);
        if (w < 0 || (size_t)w >= buflen - off)
            break;
        off += (size_t)w;
    }
    return buf;
}

static int tlsr_hexval(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

int tlsr_parse_hex(const char *text, uint8_t *out, size_t max)
{
    size_t count = 0;
    const char *p = text;

    while (*p && count < max) {
        int hi, lo;
        while (*p == ' ' || *p == ',' || *p == '\t')
            p++;
        if (p[0] == '0' && (p[1] == 'x' || p[1] == 'X'))
            p += 2;
        if (!*p)
            break;
        hi = tlsr_hexval(*p++);
        if (hi < 0)
            return -1;
        lo = tlsr_hexval(*p);
        if (lo < 0) {
            /* a lone nibble, e.g. "5" meaning 0x05 */
            out[count++] = (uint8_t)hi;
            continue;
        }
        p++;
        out[count++] = (uint8_t)((hi << 4) | lo);
    }
    return (int)count;
}
