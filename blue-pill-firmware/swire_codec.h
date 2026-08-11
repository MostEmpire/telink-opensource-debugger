/* SWire waveform codec: the pure, hardware-free half of the SWire master.
 *
 * Split out from swire.c on purpose.  The cell encoding and the edge-resyncing
 * decoder are the parts no public source pins down correctly, and they are
 * painful to debug through a logic analyser -- so they are kept free of any
 * register access and unit-tested natively (see test/test_codec.c).
 *
 * The format here is not taken from documentation; it is what a logic-analyser
 * capture of pvvx's working TlsrTool session actually puts on the wire
 * (pulseview_activate_and_fread_capture.sr, 6 MS/s):
 *
 *   activation write : 5A | 00 06 02 | 00 | 05 | FF
 *   chip-id read     : 5A | 00 00 7E | 80 | 62 55 | FF   <- 0x5562 from the slave
 *
 *   - cell = 7 units; '0' = 2 units low then high, '1' = 5 units low then high
 *   - unit = 222 ns (SPI at 4.5 MHz), so a cell is 1.555 us -> 643 kbit/s
 *   - EVERY byte, master or slave, is 10 cells: cmd + 8 data MSB-first + end,
 *     where the end cell is encoded as a '0'
 *   - cmd = 1 only for the START (0x5A) and STOP (0xFF) bytes
 *   - address is 3 bytes, big endian; RW/ID is 0x00 write / 0x80 read
 *   - a read needs NO trigger pulse: the slave's bytes simply continue the
 *     same cadence immediately after the RW byte
 */
#ifndef SWIRE_CODEC_H
#define SWIRE_CODEC_H
#include <stdint.h>

/* Every timing/format parameter that could not be pinned down is still
 * runtime-settable, so a mismatch can be swept from the host rather than
 * needing a firmware rebuild. */
typedef struct {
    uint8_t spi_div;     /* 0..7  -> SPI clock = 72 MHz / 2^(div+1)          */
    uint8_t cell;        /* SPI bit times per SWire cell (measured: 7)       */
    uint8_t low0;        /* low sample count encoding a '0'  (measured: 2)   */
    uint8_t low1;        /* low sample count encoding a '1'  (measured: 5)   */
    uint8_t thr;         /* decode: low run >= thr  =>  '1'                  */
    uint8_t addr_bytes;  /* SWire address width (measured: 3)                */
    uint8_t slave_bits;  /* cells per slave byte (measured: 10, same as master) */
    uint8_t slave_off;   /* cell index of the data MSB in a slave byte (1)   */
    uint8_t slack;       /* spare cells granted to the slave response        */
} sw_cfg_t;

#define SW_MASTER_CELLS 10   /* cmd + 8 data + end cell */
#define SW_LEAD_CELLS   2    /* idle-high run so the first falling edge exists */

void     swc_fill(uint8_t *p, uint8_t v, uint32_t n);

/* Number of SPI bytes a frame occupies, including trailing idle padding. */
uint32_t swc_frame_bytes(const sw_cfg_t *c, uint32_t master_bytes,
                         uint32_t extra_cells);

/* All of these take and return a bit position inside `buf`, which must have
 * been pre-filled with 0xFF (idle high). */
uint32_t swc_put_cell(const sw_cfg_t *c, uint8_t *buf, uint32_t bit, int one);
uint32_t swc_put_byte(const sw_cfg_t *c, uint8_t *buf, uint32_t bit,
                      uint8_t v, int marker);
uint32_t swc_put_header(const sw_cfg_t *c, uint8_t *buf, uint32_t bit,
                        uint32_t addr, uint8_t rw);

/* Decode `hdr_bytes` master bytes (our own echo) followed by `n` slave bytes
 * out of `total_bits` MISO samples.  Returns 0, or -1 if the sample stream ran
 * out of cells before everything was decoded. */
int      swc_decode(const sw_cfg_t *c, const uint8_t *rx, uint32_t total_bits,
                    uint32_t hdr_bytes, uint8_t *hdr, uint32_t n, uint8_t *out);

#endif /* SWIRE_CODEC_H */
