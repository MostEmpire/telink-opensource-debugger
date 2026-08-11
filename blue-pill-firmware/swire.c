/* Telink SWire master for TLSR825x: SPI1 + DMA drive the wire, swire_codec
 * builds and parses the waveform.
 *
 * Wiring (fixed by the target board's 3-pad header):
 *   PA7  SPI1_MOSI --[750R]-- target SWS      drives the line
 *   PA6  SPI1_MISO ---------- target SWS      senses the line
 *   PB0  ------------------- target +3.3V     direct GPIO supply (weak)
 *   PB12 ---- P-MOSFET gate -- target +3.3V   high-side switch, ACTIVE LOW
 *                                             (10K external gate pull-up)
 *   PA5  SPI1_SCK                             not wired out, left as input
 *   GND  ------------------- target GND
 *
 * Both supply paths are asserted together by default: PB0 for continuity with
 * the original wiring, PB12 for the current the GPIO alone cannot deliver.
 *
 * Why SPI instead of bit-banging: the SWire cell is a few hundred nanoseconds,
 * so any interrupt landing mid-frame corrupts it.  Driving MOSI from DMA makes
 * the waveform immune to CPU jitter, and because SPI is full duplex the same
 * transfer samples MISO `cell` times per SWire cell -- the master reads the
 * line (its own echo and the slave's response alike) for free.
 *
 * The 750R is what lets one wire use two pins: the master drives high through
 * it, the slave can still pull the node low against ~4.4 mA, and PA6 always
 * sees the true line level.
 */
#include "stm32f103.h"
#include "protocol.h"
#include "swire.h"

#define SPI_TIMEOUT 2000000u

/* Defaults measured from a logic-analyser capture of a working TlsrTool
 * session (see swire_codec.h): 7-unit cells, 2/5 low ratio, 3 address bytes,
 * slave bytes framed exactly like master bytes.  222 ns/unit at 4.5 MHz SPI
 * gives 643 kbit/s, matching the capture. */
sw_cfg_t sw_cfg = {
    .spi_div    = 2,    /* 9 MHz sampling; cell=12 keeps ~750 kbit/s on the wire */
    .cell       = 12,
    .low0       = 3,
    .low1       = 8,
    .thr        = 6,
    .addr_bytes = 3,
    .slave_bits = 10,
    .slave_off  = 1,
    .slack      = 16,
};

static uint8_t tx_buf[SPI_BUF_SZ];
static uint8_t rx_buf[SPI_BUF_SZ];
static uint32_t last_rx_len;
static uint8_t pwr_rails;

/* ----------------------------------------------------------- SPI + DMA --- */
static void spi_setup(void)
{
    RCC_APB2ENR |= RCC_APB2ENR_SPI1EN;
    RCC_AHBENR  |= RCC_AHBENR_DMA1EN;

    SPI_CR1(SPI1_BASE) = 0;
    SPI_CR1(SPI1_BASE) = SPI_CR1_MSTR | SPI_CR1_SSM | SPI_CR1_SSI
                       | ((uint32_t)(sw_cfg.spi_div & 7u) << SPI_CR1_BR_Pos);
    SPI_CR2(SPI1_BASE) = SPI_CR2_RXDMAEN | SPI_CR2_TXDMAEN;
    SPI_CR1(SPI1_BASE) |= SPI_CR1_SPE;
}

static void spi_xfer(const uint8_t *tx, uint8_t *rx, uint32_t n)
{
    uint32_t guard = 0;

    DMA_CCR(2) = 0;
    DMA_CCR(3) = 0;
    DMA_IFCR = DMA_IFCR_CGIF(2) | DMA_IFCR_CGIF(3);

    /* channel 2 = SPI1_RX (peripheral -> memory) */
    DMA_CPAR(2)  = (uint32_t)&SPI_DR(SPI1_BASE);
    DMA_CMAR(2)  = (uint32_t)rx;
    DMA_CNDTR(2) = n;
    DMA_CCR(2)   = DMA_CCR_MINC | DMA_CCR_PL_HIGH | DMA_CCR_EN;

    /* channel 3 = SPI1_TX (memory -> peripheral) */
    DMA_CPAR(3)  = (uint32_t)&SPI_DR(SPI1_BASE);
    DMA_CMAR(3)  = (uint32_t)tx;
    DMA_CNDTR(3) = n;
    DMA_CCR(3)   = DMA_CCR_MINC | DMA_CCR_DIR_M2P | DMA_CCR_PL_HIGH | DMA_CCR_EN;

    while (!(DMA_ISR & DMA_ISR_TCIF(2)) && ++guard < SPI_TIMEOUT) {}
    guard = 0;
    while ((SPI_SR(SPI1_BASE) & SPI_SR_BSY) && ++guard < SPI_TIMEOUT) {}

    DMA_CCR(2) = 0;
    DMA_CCR(3) = 0;
    DMA_IFCR = DMA_IFCR_CGIF(2) | DMA_IFCR_CGIF(3);

    last_rx_len = n;
}

/* Park MOSI high so the idle line is released rather than held low. */
static void spi_park_high(void)
{
    static const uint8_t ones[4] = { 0xFF, 0xFF, 0xFF, 0xFF };
    static uint8_t sink[4];

    spi_xfer(ones, sink, sizeof ones);
    last_rx_len = 0;
}

/* ------------------------------------------------------------- framing --- */
int sw_write(uint32_t addr, const uint8_t *data, uint32_t len)
{
    uint32_t mbytes = 1u + sw_cfg.addr_bytes + 1u + len + 1u;
    uint32_t nbytes = swc_frame_bytes(&sw_cfg, mbytes, 0u);
    uint32_t bit;

    if (nbytes > SPI_BUF_SZ)
        return -1;

    swc_fill(tx_buf, 0xFF, nbytes);
    bit = SW_LEAD_CELLS * sw_cfg.cell;
    bit = swc_put_header(&sw_cfg, tx_buf, bit, addr, 0x00);
    for (uint32_t i = 0; i < len; i++)
        bit = swc_put_byte(&sw_cfg, tx_buf, bit, data[i], 0);
    swc_put_byte(&sw_cfg, tx_buf, bit, 0xFF, 1);        /* STOP */

    spi_xfer(tx_buf, rx_buf, nbytes);
    return 0;
}

/* `decode_n` is separate from `len` so the self-test can send a real read frame
 * but only require its own echoed header to come back -- with no target on the
 * wire nothing will ever drive the data window. */
static int read_frame(uint32_t addr, uint32_t len, uint32_t decode_n,
                      uint8_t *out, uint8_t *echo)
{
    uint32_t hdr_bytes = 1u + sw_cfg.addr_bytes + 1u;
    uint32_t win_cells = len * sw_cfg.slave_bits + sw_cfg.slack;
    uint32_t nbytes = swc_frame_bytes(&sw_cfg, hdr_bytes + 1u, win_cells);
    uint32_t bit;

    if (nbytes > SPI_BUF_SZ)
        return -1;

    swc_fill(tx_buf, 0xFF, nbytes);
    bit = SW_LEAD_CELLS * sw_cfg.cell;
    bit = swc_put_header(&sw_cfg, tx_buf, bit, addr, 0x80);

    /* The master has to clock every response byte out.  For each one it drives
     * the byte's first cell as a '0' and then releases the wire for the
     * remaining nine, which the slave fills with 8 data bits and an end cell.
     *
     * This is visible in a capture of a working session: the first cell of each
     * slave byte has the *master's* period (7 samples at 6 MS/s) and low width,
     * while the cells after it carry the slave's slightly slower cadence
     * (period 10, '1' lows of 8).  Leave the window idle instead and the slave
     * never drives anything -- which is exactly what we were doing. */
    for (uint32_t i = 0; i < len; i++) {
        (void)swc_put_cell(&sw_cfg, tx_buf, bit, 0);
        bit += (uint32_t)sw_cfg.slave_bits * sw_cfg.cell;
    }
    bit += (uint32_t)sw_cfg.slack * sw_cfg.cell;        /* settle before STOP */

    swc_put_byte(&sw_cfg, tx_buf, bit, 0xFF, 1);        /* STOP */

    spi_xfer(tx_buf, rx_buf, nbytes);
    return swc_decode(&sw_cfg, rx_buf, nbytes * 8u, hdr_bytes, echo, decode_n, out);
}

int sw_read(uint32_t addr, uint32_t len, uint8_t *out, uint8_t *echo)
{
    return read_frame(addr, len, len, out, echo);
}

/* ---------------------------------------------------------- flash (MSPI) --
 * Doing the whole SPI-NOR transaction here rather than from the host turns a
 * ~10 USB round trip per block into one.  At 1.2 ms per round trip that was
 * most of the cost of dumping 512 KB.
 */
#define MSPI_DATA  0x000Cu
#define MSPI_CTRL  0x000Du
#define SWIRE_ID   0x00B3u

static uint8_t fr_tmp[MAX_PAYLOAD];

static void reg8(uint32_t addr, uint8_t v)
{
    sw_write(addr, &v, 1);
}

/* Largest slave response that still fits the SPI buffer at the current cell
 * width, derived rather than hard-coded so a cfg change cannot overflow. */
static uint32_t max_read_bytes(void)
{
    uint32_t budget = ((SPI_BUF_SZ - 2u) * 8u) / sw_cfg.cell;
    uint32_t hdr = SW_LEAD_CELLS + 6u * SW_MASTER_CELLS + sw_cfg.slack;
    uint32_t n;

    if (budget <= hdr)
        return 1;
    n = (budget - hdr) / sw_cfg.slave_bits;
    if (n > MAX_PAYLOAD)
        n = MAX_PAYLOAD;
    return n ? n : 1;
}

static uint32_t max_write_bytes(void)
{
    uint32_t budget = ((SPI_BUF_SZ - 2u) * 8u) / sw_cfg.cell;
    uint32_t hdr = SW_LEAD_CELLS + 6u * SW_MASTER_CELLS;
    uint32_t n;

    if (budget <= hdr)
        return 1;
    n = (budget - hdr) / SW_MASTER_CELLS;
    if (n > MAX_PAYLOAD)
        n = MAX_PAYLOAD;
    return n ? n : 1;
}

/* One SWire read frame can carry max_read_bytes() slave bytes, and the first of
 * those is always stale.  A second frame inside the same CS-low transaction is
 * NOT a continuation: the MSPI advances a byte during that frame's address
 * phase and never returns it, so flash bytes go missing at every boundary
 * (measured: frame 0 yields offsets 0..119, frame 1 restarts at 121).
 *
 * So each sub-chunk is issued as its own complete READ transaction at its own
 * address.  Correct by construction, and still only one USB round trip for the
 * whole block because all of this runs on the bridge. */
int sw_flash_read(uint32_t addr, uint32_t len, uint8_t *out)
{
    uint8_t cmd[4], echo[8];
    uint32_t maxn = max_read_bytes();
    uint32_t done = 0, per;

    if (maxn < 2u)
        return -1;
    per = maxn - 1u;                       /* minus the stale first byte */

    while (done < len) {
        uint32_t want = len - done;
        uint32_t a = addr + done;
        int bad;

        if (want > per)
            want = per;

        reg8(MSPI_CTRL, 0x00);             /* CS low */
        reg8(SWIRE_ID, 0x80);              /* FIFO: stay on the data register */
        cmd[0] = 0x03;                     /* READ */
        cmd[1] = (uint8_t)(a >> 16);
        cmd[2] = (uint8_t)(a >> 8);
        cmd[3] = (uint8_t)a;
        sw_write(MSPI_DATA, cmd, 4);
        reg8(MSPI_CTRL, 0x08);             /* RD: shift data in */

        bad = read_frame(MSPI_DATA, want + 1u, want + 1u, fr_tmp, echo);

        reg8(SWIRE_ID, 0x00);
        reg8(MSPI_CTRL, 0x01);             /* CS high ends this transaction */

        if (bad < 0 || echo[0] != 0x5A)
            return -1;
        for (uint32_t i = 0; i < want; i++)
            out[done + i] = fr_tmp[1u + i];
        done += want;
    }
    return 0;
}

/* One page program (caller must not cross a 256-byte page), then poll WIP. */
int sw_flash_write(uint32_t addr, const uint8_t *data, uint32_t len)
{
    uint8_t cmd[4], echo[8];
    uint32_t maxn = max_write_bytes();
    uint32_t off = 0;
    uint32_t guard;

    reg8(MSPI_CTRL, 0x00);                 /* WREN */
    reg8(MSPI_DATA, 0x06);
    reg8(MSPI_CTRL, 0x01);

    reg8(MSPI_CTRL, 0x00);
    reg8(SWIRE_ID, 0x80);
    cmd[0] = 0x02;                         /* PAGE PROGRAM */
    cmd[1] = (uint8_t)(addr >> 16);
    cmd[2] = (uint8_t)(addr >> 8);
    cmd[3] = (uint8_t)addr;
    sw_write(MSPI_DATA, cmd, 4);
    while (off < len) {
        uint32_t n = len - off;
        if (n > maxn)
            n = maxn;
        sw_write(MSPI_DATA, data + off, n);
        off += n;
    }
    reg8(SWIRE_ID, 0x00);
    reg8(MSPI_CTRL, 0x01);                 /* CS high starts the program */

    for (guard = 0; guard < 200000u; guard++) {
        reg8(MSPI_CTRL, 0x00);
        reg8(MSPI_DATA, 0x05);             /* RDSR */
        reg8(MSPI_CTRL, 0x08);
        reg8(SWIRE_ID, 0x80);
        int bad = read_frame(MSPI_DATA, 2, 2, fr_tmp, echo);
        reg8(SWIRE_ID, 0x00);
        reg8(MSPI_CTRL, 0x01);
        if (bad < 0 || echo[0] != 0x5A)
            return -1;
        if (!(fr_tmp[1] & 0x01))           /* WIP clear */
            return 0;
    }
    return -1;                             /* never went ready */
}

/* ------------------------------------------------------- power / control -- */
#define PB0_BIT   (1u << 0)
#define PB12_BIT  (1u << 12)

/* P-MOSFET high-side switch on PB12, active low.
 *
 * Off state is an input with the internal pull-up enabled, backing up the
 * external 10K from the gate to +3.3V -- so the FET is held off whenever the
 * Blue Pill is unprogrammed, in reset, or mid-boot.  On STM32F1 the pull
 * direction in input mode comes from ODR, hence the BSRR/BRR before the mode
 * change rather than after. */
static void mosfet_on(void)
{
    GPIO_BRR(GPIOB_BASE) = PB12_BIT;              /* gate low = FET conducts */
    gpio_cfg(GPIOB_BASE, 12, GPIO_OUT_PP_50);
}

static void mosfet_off(void)
{
    /* Drive the gate high while still an output: the FET is off immediately
     * and hard, before the pin is released to the pull-ups. */
    GPIO_BSRR(GPIOB_BASE) = PB12_BIT;
    gpio_cfg(GPIOB_BASE, 12, GPIO_IN_PULL);       /* ODR=1 selects pull-up */
}

void sw_power_mask(int on, uint8_t rails)
{
    if (on) {
        /* PB0 goes high *before* the FET conducts.  The reverse order would
         * briefly park a driven-low GPIO on a rail the FET is feeding. */
        if (rails & PWR_RAIL_PB0)
            GPIO_BSRR(GPIOB_BASE) = PB0_BIT;
        if (rails & PWR_RAIL_MOSFET)
            mosfet_on();
        gpio_cfg(GPIOA_BASE, 7, GPIO_AF_PP_50);   /* drive the line again */
        pwr_rails = rails & PWR_RAIL_BOTH;
        delay_ms(2);
    } else {
        /* Open-drain first: never source current into an unpowered target
         * through its SWS pad's ESD diode. */
        gpio_cfg(GPIOA_BASE, 7, GPIO_AF_OD_50);
        /* FET off before PB0 is pulled low, otherwise PB0 shorts the target
         * rail to ground through the conducting MOSFET. */
        mosfet_off();
        delay_us(200);
        GPIO_BRR(GPIOB_BASE) = PB0_BIT;
        pwr_rails = 0;
    }
}

void sw_power(int on)
{
    sw_power_mask(on, PWR_RAIL_BOTH);
}

int sw_powered(void)
{
    return pwr_rails != 0;
}

uint8_t sw_power_rails(void)
{
    return pwr_rails;
}

uint16_t sw_activate(uint16_t count, uint32_t addr, uint8_t data)
{
    uint16_t i;

    sw_power(0);
    delay_ms(80);                          /* let the target rail discharge */
    /* Power on inline rather than via sw_power(): no settle delay, because the
     * SWire slave only accepts activation in the window right after power-up,
     * before firmware reconfigures the pad. */
    GPIO_BSRR(GPIOB_BASE) = PB0_BIT;
    mosfet_on();
    gpio_cfg(GPIOA_BASE, 7, GPIO_AF_PP_50);
    pwr_rails = PWR_RAIL_BOTH;
    for (i = 0; i < count; i++)
        sw_write(addr, &data, 1);
    return i;
}

/* Spam activation writes and try the read *between* them, without ever going
 * back to the host.  The capture of a working TlsrTool session shows exactly
 * this: chip-id reads interleaved into the middle of the activation burst, so
 * whatever window opens is not necessarily still open a USB round trip later. */
uint16_t sw_activate_read(uint16_t count, uint32_t wr_addr, uint8_t wr_data,
                          uint32_t rd_addr, uint32_t rd_len,
                          uint8_t *out, uint8_t *echo)
{
    uint16_t i;

    for (i = 0; i < count; i++) {
        sw_write(wr_addr, &wr_data, 1);
        if ((i & 0x1F) == 0x1F) {
            if (read_frame(rd_addr, rd_len, rd_len, out, echo) == 0
                && echo[0] == 0x5A)
                return i;                  /* frames needed before it answered */
        }
    }
    return 0xFFFF;                         /* never answered */
}

int sw_selftest(uint8_t *echo, uint32_t echo_len)
{
    uint32_t hdr_bytes = 1u + sw_cfg.addr_bytes + 1u;
    uint8_t data = 0;
    uint8_t forced = 0;
    int rc;

    if (echo_len < hdr_bytes)
        return -1;

    /* Force push-pull so the loopback works even with the target powered off. */
    if (!pwr_rails) {
        gpio_cfg(GPIOA_BASE, 7, GPIO_AF_PP_50);
        forced = 1;
    }
    /* A read frame aimed at the chip-id register: harmless to a live target.
     * Only the echoed header is decoded, so this passes on a bare bridge with
     * nothing connected to the SWS pad. */
    (void)data;
    rc = read_frame(0x007E, 1, 0, &data, echo);
    if (forced)
        gpio_cfg(GPIOA_BASE, 7, GPIO_AF_OD_50);

    if (rc < 0)
        return -1;
    if (echo[0] != 0x5A)                   /* our own START did not survive */
        return -1;
    return 0;
}

/* Hardware continuity check, independent of SPI and of all SWire timing.
 *
 * PA7 is taken away from the SPI peripheral and driven as a plain GPIO while
 * PA6 is read back.  If the 750R and both solder joints are good, PA6 must
 * follow PA7.  This separates "the wire is not connected" from "the protocol
 * timing is wrong", which the self-test alone cannot do.
 *
 * Run it with target power off: a powered target can hold the line itself. */
int sw_pintest(uint8_t *out)
{
    gpio_cfg(GPIOA_BASE, 7, GPIO_OUT_PP_50);

    GPIO_BRR(GPIOA_BASE) = (1u << 7);
    delay_us(200);
    out[0] = (uint8_t)((GPIO_IDR(GPIOA_BASE) >> 6) & 1u);      /* expect 0 */

    GPIO_BSRR(GPIOA_BASE) = (1u << 7);
    delay_us(200);
    out[1] = (uint8_t)((GPIO_IDR(GPIOA_BASE) >> 6) & 1u);      /* expect 1 */

    gpio_cfg(GPIOA_BASE, 7, GPIO_IN_FLOAT);                    /* released */
    delay_us(200);
    out[2] = (uint8_t)((GPIO_IDR(GPIOA_BASE) >> 6) & 1u);

    gpio_cfg(GPIOA_BASE, 7, pwr_rails ? GPIO_AF_PP_50 : GPIO_AF_OD_50);
    spi_park_high();

    return (out[0] == 0 && out[1] == 1) ? 0 : -1;
}

const uint8_t *sw_last_raw(uint32_t *len)
{
    *len = last_rx_len;
    return rx_buf;
}

void sw_apply_cfg(void)
{
    spi_setup();
    spi_park_high();
}

void sw_init(void)
{
    RCC_APB2ENR |= RCC_APB2ENR_IOPAEN | RCC_APB2ENR_IOPBEN;

    /* Both power paths off until the host asks for them.  PB12 is set up
     * first so the FET is positively held off before anything else runs. */
    mosfet_off();                                 /* PB12: input + pull-up */
    gpio_cfg(GPIOB_BASE, 0, GPIO_OUT_PP_50);
    GPIO_BRR(GPIOB_BASE) = PB0_BIT;
    pwr_rails = 0;

    /* PA5 (SCK) is not wired to the target and the SWire slave never sees it.
     * It is still configured as the SPI alternate function rather than left as
     * an input: a master SPI shifts from its internal baud generator either
     * way, but routing SCK out removes any doubt about that and gives a scope
     * a clean clock to trigger on while debugging the waveform. */
    gpio_cfg(GPIOA_BASE, 5, GPIO_AF_PP_50);
    gpio_cfg(GPIOA_BASE, 6, GPIO_IN_FLOAT);       /* MISO */
    gpio_cfg(GPIOA_BASE, 7, GPIO_AF_OD_50);       /* MOSI, safe while off */

    spi_setup();
    spi_park_high();
}
