/* TLSR SWire bridge: USB CDC command protocol <-> SPI-generated SWire master.
 *
 *   PA7  MOSI --[750R]-- target SWS
 *   PA6  MISO ---------- target SWS
 *   PB0  -------------- target +3.3V (direct GPIO supply)
 *   PB12 -- P-MOSFET gate, active low, 10K external pull-up to +3.3V
 *   GND  -------------- target GND
 *   PA11/PA12 = USB D-/D+ (the Blue Pill's own micro-USB socket)
 *   PC13 = on-board LED, lit once the host has configured the CDC interface
 */
#include "stm32f103.h"
#include "protocol.h"
#include "swire.h"
#include "usb.h"

#define RAW_CHUNK 1024
#define ACT_DEFAULT_COUNT 600

static uint8_t cmdbuf[CMD_BUF_SZ];
static uint8_t outbuf[RAW_CHUNK];

static void led_init(void)
{
    RCC_APB2ENR |= RCC_APB2ENR_IOPCEN;
    gpio_cfg(GPIOC_BASE, 13, GPIO_OUT_PP_50);
    GPIO_BSRR(GPIOC_BASE) = (1u << 13);        /* off (active low) */
}

static void led_update(void)
{
    if (usb_configured())
        GPIO_BRR(GPIOC_BASE) = (1u << 13);
    else
        GPIO_BSRR(GPIOC_BASE) = (1u << 13);
}

static uint8_t get_byte(void)
{
    int c;
    while ((c = usb_getc_nb()) < 0)
        led_update();
    return (uint8_t)c;
}

static void reply(uint8_t status, const uint8_t *data, uint32_t len)
{
    uint8_t hdr[4] = { BR_SYNC, status, (uint8_t)len, (uint8_t)(len >> 8) };

    usb_write(hdr, sizeof hdr);
    if (len)
        usb_write(data, len);
}

static uint32_t be24(const uint8_t *p)
{
    return ((uint32_t)p[0] << 16) | ((uint32_t)p[1] << 8) | p[2];
}

/* Clamp a proposed configuration to something the encoder/decoder can express
 * rather than letting a typo on the host wedge the bridge. */
static int cfg_valid(const sw_cfg_t *c)
{
    if (c->spi_div > 7) return 0;
    if (c->cell < 4 || c->cell > MAX_CELL) return 0;
    if (c->low0 < 1 || c->low0 >= c->cell) return 0;
    if (c->low1 <= c->low0 || c->low1 >= c->cell) return 0;
    if (c->thr <= c->low0 || c->thr > c->low1) return 0;
    if (c->addr_bytes < 1 || c->addr_bytes > 4) return 0;
    if (c->slave_bits < 8 || c->slave_bits > 16) return 0;
    if ((uint32_t)c->slave_off + 8u > c->slave_bits) return 0;
    if (c->slack > 64) return 0;
    return 1;
}

static void do_get_cfg(void)
{
    uint32_t rawlen;
    uint8_t *p = outbuf;

    (void)sw_last_raw(&rawlen);
    *p++ = sw_cfg.spi_div;
    *p++ = sw_cfg.cell;
    *p++ = sw_cfg.low0;
    *p++ = sw_cfg.low1;
    *p++ = sw_cfg.thr;
    *p++ = sw_cfg.addr_bytes;
    *p++ = sw_cfg.slave_bits;
    *p++ = sw_cfg.slave_off;
    *p++ = sw_cfg.slack;
    *p++ = (uint8_t)FW_VERSION;
    *p++ = (uint8_t)(FW_VERSION >> 8);
    *p++ = sw_power_rails();               /* 0 = off; bit0 PB0, bit1 MOSFET */
    *p++ = (uint8_t)rawlen;
    *p++ = (uint8_t)(rawlen >> 8);
    *p++ = (uint8_t)GPIO_IDR(GPIOA_BASE);
    *p++ = (uint8_t)(GPIO_IDR(GPIOA_BASE) >> 8);
    *p++ = (uint8_t)GPIO_IDR(GPIOB_BASE);
    *p++ = (uint8_t)(GPIO_IDR(GPIOB_BASE) >> 8);
    reply(ST_OK, outbuf, (uint32_t)(p - outbuf));
}

static void do_read(const uint8_t *buf, uint32_t len)
{
    uint32_t addr, n;
    uint8_t echo[8];

    if (len < 5) {
        reply(ST_BAD_LEN, 0, 0);
        return;
    }
    addr = be24(buf);
    n = (uint32_t)buf[3] | ((uint32_t)buf[4] << 8);
    if (n == 0 || n > MAX_PAYLOAD) {
        reply(ST_BAD_LEN, 0, 0);
        return;
    }
    if (!sw_powered()) {
        reply(ST_NO_POWER, 0, 0);
        return;
    }
    if (sw_read(addr, n, outbuf, echo) < 0) {
        reply(ST_SWS_TIMEOUT, 0, 0);
        return;
    }
    /* The master's own START byte comes back on MISO; if it did not decode,
     * the sampling parameters are wrong and the payload is meaningless. */
    if (echo[0] != 0x5A) {
        reply(ST_NOSYNC, 0, 0);
        return;
    }
    reply(ST_OK, outbuf, n);
}

static void do_set_cfg(const uint8_t *buf, uint32_t len)
{
    sw_cfg_t c = sw_cfg;

    if (len >= 1) c.spi_div    = buf[0];
    if (len >= 2) c.cell       = buf[1];
    if (len >= 3) c.low0       = buf[2];
    if (len >= 4) c.low1       = buf[3];
    if (len >= 5) c.thr        = buf[4];
    if (len >= 6) c.addr_bytes = buf[5];
    if (len >= 7) c.slave_bits = buf[6];
    if (len >= 8) c.slave_off  = buf[7];
    if (len >= 9) c.slack      = buf[8];

    if (!cfg_valid(&c)) {
        reply(ST_BAD_LEN, 0, 0);
        return;
    }
    sw_cfg = c;
    sw_apply_cfg();
    reply(ST_OK, 0, 0);
}

static void do_activate(const uint8_t *buf, uint32_t len)
{
    uint16_t count = ACT_DEFAULT_COUNT;
    uint32_t addr = 0x0602;
    uint8_t data = 0x05;                   /* CPU stop */
    uint16_t sent;

    if (len >= 2)
        count = (uint16_t)(buf[0] | (buf[1] << 8));
    if (len >= 6) {
        addr = be24(buf + 2);
        data = buf[5];
    }
    if (count == 0 || count > 20000) {
        reply(ST_BAD_LEN, 0, 0);
        return;
    }
    sent = sw_activate(count, addr, data);
    outbuf[0] = (uint8_t)sent;
    outbuf[1] = (uint8_t)(sent >> 8);
    reply(ST_OK, outbuf, 2);
}

static void do_get_raw(const uint8_t *buf, uint32_t len)
{
    uint32_t rawlen, off, n;
    const uint8_t *raw = sw_last_raw(&rawlen);

    if (len < 4) {
        reply(ST_BAD_LEN, 0, 0);
        return;
    }
    off = (uint32_t)buf[0] | ((uint32_t)buf[1] << 8);
    n   = (uint32_t)buf[2] | ((uint32_t)buf[3] << 8);
    if (off >= rawlen) {
        reply(ST_OK, 0, 0);
        return;
    }
    if (n > RAW_CHUNK)
        n = RAW_CHUNK;
    if (off + n > rawlen)
        n = rawlen - off;
    reply(ST_OK, raw + off, n);
}

static void do_selftest(void)
{
    uint8_t echo[8];
    int rc = sw_selftest(echo, sizeof echo);
    uint32_t hdr = 1u + sw_cfg.addr_bytes + 1u;

    outbuf[0] = (rc == 0) ? 1 : 0;
    for (uint32_t i = 0; i < hdr && i < sizeof echo; i++)
        outbuf[1 + i] = echo[i];
    reply(ST_OK, outbuf, 1 + hdr);
}

int main(void)
{
    led_init();
    sw_init();
    usb_init();

    for (;;) {
        uint32_t len;
        uint8_t cmd;

        if (get_byte() != BR_SYNC)
            continue;                      /* resync on the frame marker */
        cmd = get_byte();
        len = get_byte();
        len |= (uint32_t)get_byte() << 8;

        if (len > CMD_BUF_SZ) {
            while (len--)
                (void)get_byte();
            reply(ST_BAD_LEN, 0, 0);
            continue;
        }
        for (uint32_t i = 0; i < len; i++)
            cmdbuf[i] = get_byte();

        switch (cmd) {
        case CMD_PING: {
            static const char id[] = FW_IDENT;
            for (uint32_t i = 0; i < 8; i++)
                outbuf[i] = (uint8_t)id[i];
            outbuf[8] = (uint8_t)FW_VERSION;
            outbuf[9] = (uint8_t)(FW_VERSION >> 8);
            reply(ST_OK, outbuf, 10);
            break;
        }

        case CMD_SYNC:
            if (len >= 1)
                sw_cfg.spi_div = cmdbuf[0] & 7u;
            sw_apply_cfg();
            reply(ST_OK, 0, 0);
            break;

        case CMD_SET_SPEED:
            if (len < 1 || cmdbuf[0] > 7) {
                reply(ST_BAD_LEN, 0, 0);
                break;
            }
            sw_cfg.spi_div = cmdbuf[0];
            sw_apply_cfg();
            reply(ST_OK, 0, 0);
            break;

        case CMD_SWS_WRITE:
            if (len < 4) {
                reply(ST_BAD_LEN, 0, 0);
            } else if (!sw_powered()) {
                reply(ST_NO_POWER, 0, 0);
            } else if (sw_write(be24(cmdbuf), cmdbuf + 3, len - 3) < 0) {
                reply(ST_BAD_LEN, 0, 0);
            } else {
                reply(ST_OK, 0, 0);
            }
            break;

        case CMD_SWS_READ:
            do_read(cmdbuf, len);
            break;

        case CMD_RESET: {
            uint8_t mode = (len >= 1) ? cmdbuf[0] : RESET_SOFT;
            if (mode == RESET_PWR_PULSE) {
                sw_power(0);
                delay_ms(80);
                sw_power(1);
                reply(ST_OK, 0, 0);
            } else if (mode == RESET_PWR_ACT) {
                sw_activate(ACT_DEFAULT_COUNT, 0x0602, 0x05);
                reply(ST_OK, 0, 0);
            } else if (!sw_powered()) {
                reply(ST_NO_POWER, 0, 0);
            } else {
                uint8_t v = 0x20;          /* reg 0x006F <- 0x20: soft reset */
                sw_write(0x006F, &v, 1);
                reply(ST_OK, 0, 0);
            }
            break;
        }

        case CMD_PWR: {
            uint8_t rails;
            if (len < 1) {
                reply(ST_BAD_LEN, 0, 0);
                break;
            }
            rails = (len >= 2) ? (uint8_t)(cmdbuf[1] & PWR_RAIL_BOTH)
                               : (uint8_t)PWR_RAIL_BOTH;
            if (rails == 0)                /* an empty mask means "both" */
                rails = PWR_RAIL_BOTH;
            sw_power_mask(cmdbuf[0] ? 1 : 0, rails);
            outbuf[0] = sw_power_rails();
            reply(ST_OK, outbuf, 1);
            break;
        }

        case CMD_SET_CFG:
            do_set_cfg(cmdbuf, len);
            break;

        case CMD_GET_CFG:
            do_get_cfg();
            break;

        case CMD_ACTIVATE:
            do_activate(cmdbuf, len);
            break;

        case CMD_GET_RAW:
            do_get_raw(cmdbuf, len);
            break;

        case CMD_SELFTEST:
            do_selftest();
            break;

        case CMD_ACT_READ: {
            uint16_t n, used;
            uint32_t rd_addr, rd_len;
            uint8_t echo[8];
            if (len < 6) { reply(ST_BAD_LEN, 0, 0); break; }
            if (!sw_powered()) { reply(ST_NO_POWER, 0, 0); break; }
            n = (uint16_t)(cmdbuf[0] | (cmdbuf[1] << 8));
            rd_addr = be24(cmdbuf + 2);
            rd_len = cmdbuf[5];
            if (n == 0 || n > 20000 || rd_len == 0 || rd_len > MAX_PAYLOAD) {
                reply(ST_BAD_LEN, 0, 0);
                break;
            }
            used = sw_activate_read(n, 0x0602, 0x05, rd_addr, rd_len,
                                    outbuf + 2, echo);
            outbuf[0] = (uint8_t)used;
            outbuf[1] = (uint8_t)(used >> 8);
            reply(ST_OK, outbuf, 2 + (used == 0xFFFF ? 0 : rd_len));
            break;
        }

        case CMD_FLASH_RD: {
            uint32_t addr, n;
            if (len < 5) { reply(ST_BAD_LEN, 0, 0); break; }
            if (!sw_powered()) { reply(ST_NO_POWER, 0, 0); break; }
            addr = be24(cmdbuf);
            n = (uint32_t)cmdbuf[3] | ((uint32_t)cmdbuf[4] << 8);
            if (n == 0 || n > RAW_CHUNK) { reply(ST_BAD_LEN, 0, 0); break; }
            if (sw_flash_read(addr, n, outbuf) < 0) reply(ST_SWS_TIMEOUT, 0, 0);
            else reply(ST_OK, outbuf, n);
            break;
        }

        case CMD_FLASH_WR: {
            uint32_t addr;
            if (len < 4) { reply(ST_BAD_LEN, 0, 0); break; }
            if (!sw_powered()) { reply(ST_NO_POWER, 0, 0); break; }
            addr = be24(cmdbuf);
            if (sw_flash_write(addr, cmdbuf + 3, len - 3) < 0)
                reply(ST_SWS_TIMEOUT, 0, 0);
            else
                reply(ST_OK, 0, 0);
            break;
        }

        case CMD_PINTEST: {
            uint8_t pins[3];
            int rc = sw_pintest(pins);
            outbuf[0] = (rc == 0) ? 1 : 0;
            outbuf[1] = pins[0];
            outbuf[2] = pins[1];
            outbuf[3] = pins[2];
            reply(ST_OK, outbuf, 4);
            break;
        }

        default:
            reply(ST_BAD_CMD, 0, 0);
            break;
        }
    }
}
