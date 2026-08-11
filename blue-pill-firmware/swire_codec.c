/* SWire waveform codec -- see swire_codec.h.  No hardware access in here.
 *
 * Encoding (pvvx TlsrTools825x/SWireSTM32, PROTOCOL.md section 1):
 *   one SWire cell = `cell` SPI bit times, line idles high
 *   '0' = low for `low0` samples, then high    (short low)
 *   '1' = low for `low1` samples, then high    (long low)
 *   master byte = 10 cells: marker + 8 data bits MSB-first + end cell
 *                 marker = 1 for the START/STOP framing bytes, 0 otherwise
 *   frame       = START 0x5A, addr[addr_bytes], RW (0x00 write / 0x80 read),
 *                 data..., STOP 0xFF
 */
#include "swire_codec.h"

void swc_fill(uint8_t *p, uint8_t v, uint32_t n)
{
    while (n--)
        *p++ = v;
}

uint32_t swc_frame_bytes(const sw_cfg_t *c, uint32_t master_bytes,
                         uint32_t extra_cells)
{
    uint32_t bits = (SW_LEAD_CELLS + master_bytes * SW_MASTER_CELLS + extra_cells)
                    * c->cell;
    return (bits + 7u) / 8u + 2u;          /* +2: trailing idle-high padding */
}

uint32_t swc_put_cell(const sw_cfg_t *c, uint8_t *buf, uint32_t bit, int one)
{
    uint32_t p = bit;
    uint32_t left = one ? c->low1 : c->low0;

    /* Clear the low run a byte at a time.  Bit-by-bit was a load/mask/store per
     * sample: encoding one 230-byte frame writes ~12600 bits, which made the
     * *write* path cost more than the decode it was supposed to feed. */
    while (left) {
        uint32_t bi = p >> 3;
        uint32_t j = p & 7u;
        uint32_t n = 8u - j;
        uint8_t mask;

        if (n > left)
            n = left;
        /* bits [j, j+n) of this byte, MSB first */
        mask = (uint8_t)((0xFFu >> j) & (uint8_t)(0xFFu << (8u - j - n)));
        buf[bi] &= (uint8_t)~mask;
        p += n;
        left -= n;
    }
    return bit + c->cell;
}

uint32_t swc_put_byte(const sw_cfg_t *c, uint8_t *buf, uint32_t bit,
                      uint8_t v, int marker)
{
    bit = swc_put_cell(c, buf, bit, marker);
    for (int i = 7; i >= 0; i--)
        bit = swc_put_cell(c, buf, bit, (v >> i) & 1);
    return swc_put_cell(c, buf, bit, 0);   /* end cell, encoded as a '0' */
}

uint32_t swc_put_header(const sw_cfg_t *c, uint8_t *buf, uint32_t bit,
                        uint32_t addr, uint8_t rw)
{
    bit = swc_put_byte(c, buf, bit, 0x5A, 1);
    for (int i = (int)c->addr_bytes - 1; i >= 0; i--)
        bit = swc_put_byte(c, buf, bit, (uint8_t)(addr >> (8 * i)), 0);
    return swc_put_byte(c, buf, bit, rw, 0);
}

/* ------------------------------------------------------------- decoder --- */
/* Edge-resynchronising: every cell is located by its own falling edge and
 * classified by how long the line stays low.  A constant sampling skew, or a
 * slow drift between master and slave, therefore does not accumulate. */
typedef struct { const uint8_t *buf; uint32_t pos, end; } sampler_t;

static inline int smp(const sampler_t *s, uint32_t p)
{
    return (s->buf[p >> 3] >> (7u - (p & 7u))) & 1;
}

/* Locate the next cell.  Returns its low-run length and start sample, or -1
 * when the stream has no further low sample.
 *
 * Scans a byte at a time wherever it can.  A frame is ~75% idle-high, and the
 * naive per-sample loop cost ~63 CPU cycles per sample -- for a 100-byte read
 * that was ~11 ms of decoding against 1.4 ms of actual DMA, making the decoder,
 * not the wire or USB, the limit on throughput.
 *
 * We always enter with the line high (the previous cell ended on a high
 * sample), so the next zero bit *is* a falling edge and can be searched for
 * directly. */
static int next_cell(sampler_t *s, uint32_t *low_out, uint32_t *start_out)
{
    const uint8_t *buf = s->buf;
    uint32_t p = s->pos;
    const uint32_t end = s->end;
    uint32_t low = 0;

    /* Each step lands on the next transition in O(1) per byte using a
     * count-leading-zeros, instead of testing samples one at a time.  Bit j of
     * a byte is sample (bi*8 + j), MSB first, so masking off the bits already
     * consumed and taking the CLZ of the result gives the transition directly.
     * Byte-at-a-time skipping alone was not enough: inside the slave's reply
     * the line alternates every few samples, so there are almost no runs of
     * eight to skip, and the decoder still cost ~31 cycles/sample. */

    /* --- seek the next low sample --------------------------------------- */
    for (;;) {
        uint32_t bi = p >> 3;
        uint8_t m;

        if (p >= end) {
            s->pos = end;
            return -1;
        }
        m = (uint8_t)(~buf[bi]) & (uint8_t)(0xFFu >> (p & 7u));
        if (m) {                           /* a low sample lives in this byte */
            p = (bi << 3) + (uint32_t)__builtin_clz((uint32_t)m << 24);
            break;
        }
        p = (bi + 1u) << 3;
    }
    if (p >= end) {
        s->pos = end;
        return -1;
    }
    *start_out = p;

    /* --- measure the low run: seek the next high sample ------------------ */
    for (;;) {
        uint32_t bi = p >> 3;
        uint8_t m;

        if (p >= end)
            break;
        m = buf[bi] & (uint8_t)(0xFFu >> (p & 7u));
        if (m) {                           /* the run ends inside this byte */
            uint32_t hp = (bi << 3) + (uint32_t)__builtin_clz((uint32_t)m << 24);
            low += hp - p;
            p = hp;
            break;
        }
        low += 8u - (p & 7u);
        p = (bi + 1u) << 3;
    }
    if (p > end) {
        low -= p - end;
        p = end;
    }

    s->pos = p;
    *low_out = low;
    return 0;
}

static int next_bit(const sw_cfg_t *c, sampler_t *s)
{
    uint32_t low, start;

    if (next_cell(s, &low, &start) < 0)
        return -1;
    return (low >= c->thr) ? 1 : 0;
}

int swc_decode(const sw_cfg_t *c, const uint8_t *rx, uint32_t total_bits,
               uint32_t hdr_bytes, uint8_t *hdr, uint32_t n, uint8_t *out)
{
    sampler_t s;

    s.buf = rx;
    /* Skip the first couple of samples.  The very first MISO sample of an SPI
     * transfer can be shifted in before the line is meaningfully driven, and a
     * single spurious high->low there would insert a bogus cell and shift the
     * whole frame by one.  The frame opens with SW_LEAD_CELLS of idle high, so
     * there is plenty of margin to throw these away. */
    s.pos = (total_bits > 4u) ? 2u : 0u;
    s.end = total_bits;

    for (uint32_t i = 0; i < hdr_bytes; i++) {
        uint32_t v = 0;
        for (int b = 0; b < SW_MASTER_CELLS; b++) {
            int bit = next_bit(c, &s);
            if (bit < 0)
                return -1;
            v = (v << 1) | (uint32_t)bit;
        }
        if (hdr)
            hdr[i] = (uint8_t)((v >> 1) & 0xFFu);      /* drop cmd + end */
    }

    /* Slave bytes need their own threshold.  The target clocks its reply from
     * its own oscillator, so its cells are not the master's width -- measured
     * on hardware the slave ran at ~4.5 samples/cell against our 7, which makes
     * a fixed threshold read its '1' bits as '0'.  Derive the threshold from
     * the slave's actual cadence instead: cells are evenly spaced within a
     * byte, a '0' is low for low0/cell of that span and a '1' for low1/cell,
     * so half the cell period separates them cleanly at any rate. */
    for (uint32_t i = 0; i < n; i++) {
        uint32_t lows[16], starts[16];
        uint32_t period = 0, thr;
        uint32_t v = 0;

        for (uint8_t b = 0; b < c->slave_bits; b++) {
            if (next_cell(&s, &lows[b], &starts[b]) < 0)
                return -1;
        }
        /* Skip cell 0: the master drives that one, at its own rate. */
        for (uint8_t b = 2; b < c->slave_bits; b++) {
            uint32_t d = starts[b] - starts[b - 1];
            if (period == 0 || d < period)
                period = d;
        }
        thr = period / 2u;
        if (thr < 1u)
            thr = c->thr;                  /* degenerate: fall back to config */

        for (uint8_t b = 0; b < c->slave_bits; b++)
            v = (v << 1) | (lows[b] >= thr ? 1u : 0u);

        out[i] = (uint8_t)((v >> (c->slave_bits - c->slave_off - 8)) & 0xFFu);
    }
    return 0;
}
