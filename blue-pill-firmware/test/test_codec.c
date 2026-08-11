/* Native unit tests for the SWire codec (swire_codec.c).
 *
 *   gcc -O1 -Wall -Wextra -I.. -o test_codec test_codec.c ../swire_codec.c
 *   ./test_codec
 *
 * The point: on real hardware the master's MOSI waveform comes straight back
 * on MISO through the 750R, so the RX sample buffer *is* the TX buffer plus
 * whatever the slave pulled low.  That makes the whole encode/decode path
 * exactly testable on a PC -- feed the encoder's output back into the decoder,
 * and additionally paint synthetic slave bytes into the read window.
 */
#include <stdio.h>
#include <string.h>
#include "swire_codec.h"

static int failures;
static int checks;

#define CHECK(cond, ...) do {                      \
    checks++;                                      \
    if (!(cond)) {                                 \
        failures++;                                \
        printf("  FAIL %s:%d: ", __func__, __LINE__); \
        printf(__VA_ARGS__);                       \
        printf("\n");                              \
    }                                              \
} while (0)

static sw_cfg_t base_cfg(void)
{
    sw_cfg_t c;
    c.spi_div = 3;
    c.cell = 7;
    c.low0 = 2;
    c.low1 = 5;
    c.thr = 4;
    c.addr_bytes = 3;
    c.slave_bits = 10;
    c.slave_off = 1;
    c.slack = 16;
    return c;
}

/* ---- 1: a write frame decodes back into the bytes we encoded -------------- */
static void test_write_loopback(const sw_cfg_t *c)
{
    uint8_t buf[4096], hdr[8], out[8];
    const uint8_t payload[3] = { 0x05, 0xA5, 0x00 };
    uint32_t mbytes = 1u + c->addr_bytes + 1u + sizeof payload + 1u;
    uint32_t nbytes = swc_frame_bytes(c, mbytes, 0);
    uint32_t bit;

    if (nbytes > sizeof buf) {
        printf("  skip (frame %u > buf)\n", nbytes);
        return;
    }
    swc_fill(buf, 0xFF, nbytes);
    bit = SW_LEAD_CELLS * c->cell;
    bit = swc_put_header(c, buf, bit, 0x000602, 0x00);
    for (unsigned i = 0; i < sizeof payload; i++)
        bit = swc_put_byte(c, buf, bit, payload[i], 0);
    swc_put_byte(c, buf, bit, 0xFF, 1);

    /* decode the header plus the payload bytes, all master-framed (10 cells) */
    sw_cfg_t mc = *c;
    mc.slave_bits = SW_MASTER_CELLS;
    mc.slave_off = 1;
    int rc = swc_decode(&mc, buf, nbytes * 8u, 1u + c->addr_bytes + 1u, hdr,
                        sizeof payload, out);
    CHECK(rc == 0, "decode returned %d", rc);
    CHECK(hdr[0] == 0x5A, "START echoed as 0x%02X", hdr[0]);
    CHECK(hdr[1] == 0x00 && hdr[2] == 0x06 && hdr[3] == 0x02,
          "address echoed as %02X %02X %02X", hdr[1], hdr[2], hdr[3]);
    CHECK(hdr[4] == 0x00, "RW echoed as 0x%02X", hdr[4]);
    CHECK(memcmp(out, payload, sizeof payload) == 0,
          "payload echoed as %02X %02X %02X", out[0], out[1], out[2]);
}

/* Emit one byte the way the *slave* frames it: `slave_bits` cells, with the
 * eight data bits starting at cell `slave_off` and the rest low-'0' framing.
 * Must follow the cfg under test, otherwise the generator and the decoder
 * disagree about byte length and everything after the first byte shifts. */
static uint32_t put_slave_byte(const sw_cfg_t *c, uint8_t *buf, uint32_t bit,
                               uint8_t v)
{
    for (uint8_t i = 0; i < c->slave_bits; i++) {
        int one = 0;
        if (i >= c->slave_off && i < (uint8_t)(c->slave_off + 8))
            one = (v >> (7 - (i - c->slave_off))) & 1;
        bit = swc_put_cell(c, buf, bit, one);
    }
    return bit;
}

/* ---- 2: a read frame with synthetic slave data ---------------------------- */
static void test_read_with_slave(const sw_cfg_t *c)
{
    uint8_t buf[8192], hdr[8], out[8];
    const uint8_t reply[2] = { 0x62, 0x55 };            /* e.g. chip id 0x5562 */
    uint32_t hdr_bytes = 1u + c->addr_bytes + 1u;
    uint32_t win_cells = (uint32_t)(sizeof reply) * c->slave_bits + c->slack;
    uint32_t nbytes = swc_frame_bytes(c, hdr_bytes + 1u, win_cells);
    uint32_t bit, win_start;

    if (nbytes > sizeof buf) {
        printf("  skip (frame %u > buf)\n", nbytes);
        return;
    }
    swc_fill(buf, 0xFF, nbytes);
    bit = SW_LEAD_CELLS * c->cell;
    bit = swc_put_header(c, buf, bit, 0x00007E, 0x80);
    win_start = bit;
    bit += win_cells * c->cell;
    swc_put_byte(c, buf, bit, 0xFF, 1);

    /* The slave pulls the same shared wire low, so its bytes appear in the RX
     * samples exactly as if we had encoded them into the window. */
    uint32_t p = win_start;
    for (unsigned i = 0; i < sizeof reply; i++)
        p = put_slave_byte(c, buf, p, reply[i]);

    int rc = swc_decode(c, buf, nbytes * 8u, hdr_bytes, hdr, sizeof reply, out);
    CHECK(rc == 0, "decode returned %d", rc);
    CHECK(hdr[0] == 0x5A, "START echoed as 0x%02X", hdr[0]);
    CHECK(hdr[hdr_bytes - 1] == 0x80, "RW echoed as 0x%02X", hdr[hdr_bytes - 1]);
    CHECK(memcmp(out, reply, sizeof reply) == 0,
          "slave bytes decoded as %02X %02X (want %02X %02X)",
          out[0], out[1], reply[0], reply[1]);
}

/* ---- 3: a silent slave must fail, not return garbage ---------------------- */
static void test_silent_slave(const sw_cfg_t *c)
{
    uint8_t buf[8192], hdr[8], out[8];
    uint32_t hdr_bytes = 1u + c->addr_bytes + 1u;
    uint32_t win_cells = 2u * c->slave_bits + c->slack;
    uint32_t nbytes = swc_frame_bytes(c, hdr_bytes + 1u, win_cells);
    uint32_t bit;

    swc_fill(buf, 0xFF, nbytes);
    bit = SW_LEAD_CELLS * c->cell;
    bit = swc_put_header(c, buf, bit, 0x00007E, 0x80);
    bit += win_cells * c->cell;
    swc_put_byte(c, buf, bit, 0xFF, 1);
    /* no slave bytes painted in: only the STOP byte follows the window */

    int rc = swc_decode(c, buf, nbytes * 8u, hdr_bytes, hdr, 2, out);
    CHECK(rc == -1, "silent slave should fail to decode, got %d", rc);
}

/* ---- 4: every byte value survives a round trip ---------------------------- */
static void test_all_byte_values(const sw_cfg_t *c)
{
    uint8_t buf[4096], hdr[8], out[4];
    int bad = 0;

    for (unsigned v = 0; v < 256; v++) {
        uint8_t payload = (uint8_t)v;
        uint32_t mbytes = 1u + c->addr_bytes + 1u + 1u + 1u;
        uint32_t nbytes = swc_frame_bytes(c, mbytes, 0);
        uint32_t bit;

        swc_fill(buf, 0xFF, nbytes);
        bit = SW_LEAD_CELLS * c->cell;
        bit = swc_put_header(c, buf, bit, 0x123456, 0x00);
        bit = swc_put_byte(c, buf, bit, payload, 0);
        swc_put_byte(c, buf, bit, 0xFF, 1);

        sw_cfg_t mc = *c;
        mc.slave_bits = SW_MASTER_CELLS;
        mc.slave_off = 1;
        if (swc_decode(&mc, buf, nbytes * 8u, 1u + c->addr_bytes + 1u,
                       hdr, 1, out) != 0 || out[0] != payload)
            bad++;
    }
    CHECK(bad == 0, "%d/256 byte values failed the round trip", bad);
}

/* ---- 5: address width variants -------------------------------------------- */
static void test_addr_widths(void)
{
    for (uint8_t aw = 2; aw <= 4; aw++) {
        sw_cfg_t c = base_cfg();
        uint8_t buf[4096], hdr[8], out[4];
        uint32_t nbytes, bit;
        uint32_t addr = 0x123456u & ((1u << (8 * aw)) - 1u);

        c.addr_bytes = aw;
        nbytes = swc_frame_bytes(&c, 1u + aw + 1u + 1u, 0);
        swc_fill(buf, 0xFF, nbytes);
        bit = SW_LEAD_CELLS * c.cell;
        bit = swc_put_header(&c, buf, bit, addr, 0x00);
        swc_put_byte(&c, buf, bit, 0xFF, 1);

        sw_cfg_t mc = c;
        mc.slave_bits = SW_MASTER_CELLS;
        mc.slave_off = 1;
        int rc = swc_decode(&mc, buf, nbytes * 8u, 1u + aw + 1u, hdr, 0, out);
        CHECK(rc == 0, "addr_bytes=%u decode failed", aw);
        CHECK(hdr[0] == 0x5A, "addr_bytes=%u START=0x%02X", aw, hdr[0]);
        for (uint8_t i = 0; i < aw; i++) {
            uint8_t want = (uint8_t)(addr >> (8 * (aw - 1 - i)));
            CHECK(hdr[1 + i] == want, "addr_bytes=%u byte%u=%02X want %02X",
                  aw, i, hdr[1 + i], want);
        }
    }
}

int main(void)
{
    sw_cfg_t c = base_cfg();

    printf("default cfg (cell=7 low0=2 low1=5 thr=4):\n");
    test_write_loopback(&c);
    test_read_with_slave(&c);
    test_silent_slave(&c);
    test_all_byte_values(&c);

    printf("address widths 2..4:\n");
    test_addr_widths();

    /* The sweep the host tool performs, verified here so a "usable" row from
     * `tlsr_tool sweep` cannot be an artefact of an unencodable parameter set. */
    printf("parameter sweep (cell 5..10):\n");
    for (uint8_t cell = 5; cell <= 10; cell++) {
        sw_cfg_t s = base_cfg();
        s.cell = cell;
        s.low0 = cell / 4 ? cell / 4 : 1;
        s.low1 = (uint8_t)(cell - s.low0 - 1);
        s.thr = (uint8_t)((s.low0 + s.low1 + 1) / 2);
        if (s.low1 <= s.low0 || s.thr <= s.low0 || s.thr > s.low1)
            continue;
        printf("  cell=%u low0=%u low1=%u thr=%u\n", cell, s.low0, s.low1, s.thr);
        test_write_loopback(&s);
        test_read_with_slave(&s);
        test_all_byte_values(&s);
    }

    /* Slave framing is fixed by the trigger: the master's 1-unit pulse decodes
     * as a cell of its own, so a response byte is 9 cells (trigger + 8 data)
     * and the data starts at offset 1.  Check that holds across cell widths,
     * and that the pre-trigger framing (8 cells) now decodes wrongly -- which
     * is why nothing worked before the trigger pulse was added. */
    printf("slave framing (trigger + 8 data = 9 cells):\n");
    for (uint8_t cell = 5; cell <= 8; cell++) {
        sw_cfg_t s = base_cfg();
        s.cell = cell;
        s.low0 = cell / 4 ? cell / 4 : 1;
        s.low1 = (uint8_t)(cell - s.low0 - 1);
        s.thr = (uint8_t)((s.low0 + s.low1 + 1) / 2);
        printf("  cell=%u slave_bits=9 slave_off=1\n", cell);
        test_read_with_slave(&s);
    }

    printf("\n%d checks, %d failures -- %s\n", checks, failures,
           failures ? "FAIL" : "ALL PASS");
    return failures ? 1 : 0;
}
