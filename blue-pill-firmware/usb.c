/* USB full-speed CDC-ACM device for STM32F103, interrupt driven.
 *
 * Deliberately small: one control endpoint, one bulk IN, one bulk OUT and a
 * (never used, but required by the CDC descriptor) interrupt IN endpoint.
 * Enumerates as VID 0483 / PID 5740 so Windows binds its in-box usbser.sys
 * driver with no .inf file.
 *
 * USB is interrupt driven rather than polled so that enumeration and host
 * traffic stay alive while a long DMA SWire transfer is running.
 */
#include "stm32f103.h"
#include "usb.h"

/* ------------------------------------------------------- PMA allocation --- */
/* Byte offsets inside the 512-byte packet memory. */
#define BTABLE_OFF   0x000
#define EP0_TX_OFF   0x020
#define EP0_RX_OFF   0x060
#define EP1_TX_OFF   0x0A0
#define EP2_RX_OFF   0x0E0
#define EP3_TX_OFF   0x120

#define EP0_SIZE     64
#define EPBULK_SIZE  64

/* buffer-descriptor table accessors (n = endpoint number) */
#define BT_ADDR_TX(n)   USB_PMA(BTABLE_OFF + (n) * 8 + 0)
#define BT_COUNT_TX(n)  USB_PMA(BTABLE_OFF + (n) * 8 + 2)
#define BT_ADDR_RX(n)   USB_PMA(BTABLE_OFF + (n) * 8 + 4)
#define BT_COUNT_RX(n)  USB_PMA(BTABLE_OFF + (n) * 8 + 6)

/* COUNT_RX blocksize encoding for a 64-byte buffer: BL_SIZE=1, NUM_BLOCK=1 */
#define COUNT_RX_64  ((1u << 15) | (1u << 10))

/* ------------------------------------------------------------- ring buf --- */
#define RX_RING_SZ 1024
static volatile uint8_t  rx_ring[RX_RING_SZ];
static volatile uint16_t rx_head, rx_tail;
static volatile uint8_t  rx_ep_armed;

static inline uint16_t ring_used(void)
{
    return (uint16_t)((rx_head - rx_tail) & (RX_RING_SZ - 1));
}
static inline uint16_t ring_free(void)
{
    return (uint16_t)(RX_RING_SZ - 1 - ring_used());
}

static volatile uint8_t usb_is_configured;

/* --------------------------------------------------------- PMA transfer --- */
static void pma_write(uint32_t off, const uint8_t *src, uint32_t n)
{
    uint32_t i = 0;
    for (; i + 1 < n; i += 2)
        USB_PMA(off + i) = (uint16_t)src[i] | ((uint16_t)src[i + 1] << 8);
    if (i < n)
        USB_PMA(off + i) = src[i];
}

static void pma_read(uint32_t off, uint8_t *dst, uint32_t n)
{
    uint32_t i = 0;
    for (; i + 1 < n; i += 2) {
        uint16_t w = USB_PMA(off + i);
        dst[i] = (uint8_t)w;
        dst[i + 1] = (uint8_t)(w >> 8);
    }
    if (i < n)
        dst[i] = (uint8_t)USB_PMA(off + i);
}

/* ------------------------------------------------------- EPnR accessors --- */
/* EPnR mixes plain rw bits, toggle-on-write bits (DTOG/STAT) and rc_w0 bits
 * (CTR_RX/CTR_TX).  Writing 0 to a toggle bit leaves it; writing 1 to a rc_w0
 * bit leaves it.  These helpers keep that straight. */
static inline void epr_toggle(uint32_t ep, uint16_t val, uint16_t mask)
{
    uint16_t v = USB_EPR(ep);
    uint16_t w = (uint16_t)((v & USB_EPR_RW_MASK) | USB_EP_CTR_RX | USB_EP_CTR_TX);
    w |= (uint16_t)((v ^ val) & mask);
    USB_EPR(ep) = w;
}
static inline void ep_stat_rx(uint32_t ep, uint32_t s)
{
    epr_toggle(ep, (uint16_t)(s << 12), USB_EP_STAT_RX);
}
static inline void ep_stat_tx(uint32_t ep, uint32_t s)
{
    epr_toggle(ep, (uint16_t)(s << 4), USB_EP_STAT_TX);
}
static inline void ep_clr_ctr_rx(uint32_t ep)
{
    USB_EPR(ep) = (uint16_t)((USB_EPR(ep) & USB_EPR_RW_MASK) | USB_EP_CTR_TX);
}
static inline void ep_clr_ctr_tx(uint32_t ep)
{
    USB_EPR(ep) = (uint16_t)((USB_EPR(ep) & USB_EPR_RW_MASK) | USB_EP_CTR_RX);
}

/* ------------------------------------------------------------ descriptors -- */
static const uint8_t desc_device[18] = {
    18, 0x01,
    0x00, 0x02,             /* bcdUSB 2.00                   */
    0x02, 0x00, 0x00,       /* class CDC (per-interface)     */
    EP0_SIZE,
    0x83, 0x04,             /* idVendor  0x0483 (ST)         */
    0x40, 0x57,             /* idProduct 0x5740 (VCP)        */
    0x00, 0x02,             /* bcdDevice 2.00                */
    1, 2, 3,                /* iManufacturer/iProduct/iSerial*/
    1
};

#define CONFIG_TOTAL 67
static const uint8_t desc_config[CONFIG_TOTAL] = {
    /* configuration */
    9, 0x02, CONFIG_TOTAL, 0x00, 2, 1, 0, 0x80, 250,
    /* interface 0: CDC communications */
    9, 0x04, 0, 0, 1, 0x02, 0x02, 0x00, 0,
    /*   header functional */
    5, 0x24, 0x00, 0x10, 0x01,
    /*   call management: no data-interface call mgmt */
    5, 0x24, 0x01, 0x00, 1,
    /*   abstract control management */
    4, 0x24, 0x02, 0x02,
    /*   union: control if 0, subordinate if 1 */
    5, 0x24, 0x06, 0, 1,
    /*   notification endpoint EP3 IN, interrupt */
    7, 0x05, 0x83, 0x03, 0x08, 0x00, 0xFF,
    /* interface 1: CDC data */
    9, 0x04, 1, 0, 2, 0x0A, 0x00, 0x00, 0,
    /*   EP1 IN bulk */
    7, 0x05, 0x81, 0x02, EPBULK_SIZE, 0x00, 0x00,
    /*   EP2 OUT bulk */
    7, 0x05, 0x02, 0x02, EPBULK_SIZE, 0x00, 0x00
};

static const uint8_t str_lang[4] = { 4, 0x03, 0x09, 0x04 };
static const uint8_t str_mfr[] = {
    30, 0x03, 't',0,'e',0,'l',0,'i',0,'n',0,'k',0,'_',0,
    'p',0,'r',0,'o',0,'j',0,'e',0,'c',0,'t',0
};
static const uint8_t str_prod[] = {
    32, 0x03, 'T',0,'L',0,'S',0,'R',0,' ',0,'S',0,'W',0,'S',0,
    ' ',0,'B',0,'r',0,'i',0,'d',0,'g',0,'e',0
};
static const uint8_t str_serial[] = {
    18, 0x03, 'T',0,'L',0,'S',0,'R',0,'S',0,'W',0,'S',0,'2',0
};

/* CDC line coding: accepted and ignored (a CDC host always sets it). */
static uint8_t line_coding[7] = { 0x00, 0xC2, 0x01, 0x00, 0x00, 0x00, 0x08 };

/* --------------------------------------------------------- EP0 state ------ */
typedef struct {
    uint8_t  bmRequestType, bRequest;
    uint16_t wValue, wIndex, wLength;
} setup_t;

static const uint8_t *ep0_ptr;
static uint16_t ep0_rem;
static uint8_t  ep0_zlp;          /* a terminating zero-length packet is due  */
static uint8_t  ep0_addr_pending; /* SET_ADDRESS takes effect after status    */
static uint8_t  ep0_out_stage;    /* an OUT data stage is in progress         */

static void ep0_send(const uint8_t *data, uint16_t len, uint16_t wLength)
{
    if (len > wLength)
        len = wLength;
    ep0_ptr = data;
    ep0_rem = len;
    /* a short packet must terminate the transfer; if the length is an exact
     * multiple of the packet size and shorter than requested, add a ZLP */
    ep0_zlp = (len < wLength) && (len % EP0_SIZE) == 0;

    uint16_t n = ep0_rem > EP0_SIZE ? EP0_SIZE : ep0_rem;
    pma_write(EP0_TX_OFF, ep0_ptr, n);
    BT_COUNT_TX(0) = n;
    ep0_ptr += n;
    ep0_rem = (uint16_t)(ep0_rem - n);
    ep_stat_tx(0, USB_EP_STAT_VALID);
}

static void ep0_send_zlp(void)
{
    ep0_rem = 0;
    ep0_zlp = 0;
    BT_COUNT_TX(0) = 0;
    ep_stat_tx(0, USB_EP_STAT_VALID);
}

static void ep0_stall(void)
{
    ep_stat_tx(0, USB_EP_STAT_STALL);
    ep_stat_rx(0, USB_EP_STAT_VALID);
}

static void configure_data_endpoints(void)
{
    /* EP1 IN bulk */
    USB_EPR(1) = USB_EP_TYPE_BULK | 1;
    BT_ADDR_TX(1) = EP1_TX_OFF;
    BT_COUNT_TX(1) = 0;
    ep_stat_tx(1, USB_EP_STAT_NAK);
    ep_stat_rx(1, USB_EP_STAT_DISABLED);

    /* EP2 OUT bulk */
    USB_EPR(2) = USB_EP_TYPE_BULK | 2;
    BT_ADDR_RX(2) = EP2_RX_OFF;
    BT_COUNT_RX(2) = COUNT_RX_64;
    ep_stat_rx(2, USB_EP_STAT_VALID);
    ep_stat_tx(2, USB_EP_STAT_DISABLED);
    rx_ep_armed = 1;

    /* EP3 IN interrupt (CDC notifications; never used) */
    USB_EPR(3) = USB_EP_TYPE_INTERRUPT | 3;
    BT_ADDR_TX(3) = EP3_TX_OFF;
    BT_COUNT_TX(3) = 0;
    ep_stat_tx(3, USB_EP_STAT_NAK);
    ep_stat_rx(3, USB_EP_STAT_DISABLED);
}

static void handle_setup(const setup_t *s)
{
    uint8_t type = (uint8_t)(s->bmRequestType & 0x60);

    if (type == 0x00) {                                   /* standard */
        switch (s->bRequest) {
        case 0x06: {                                      /* GET_DESCRIPTOR */
            uint8_t dt = (uint8_t)(s->wValue >> 8);
            uint8_t di = (uint8_t)(s->wValue & 0xFF);
            if (dt == 1) {
                ep0_send(desc_device, sizeof desc_device, s->wLength);
            } else if (dt == 2) {
                ep0_send(desc_config, sizeof desc_config, s->wLength);
            } else if (dt == 3) {
                const uint8_t *p = 0;
                uint16_t n = 0;
                if (di == 0)      { p = str_lang;   n = sizeof str_lang;   }
                else if (di == 1) { p = str_mfr;    n = sizeof str_mfr;    }
                else if (di == 2) { p = str_prod;   n = sizeof str_prod;   }
                else if (di == 3) { p = str_serial; n = sizeof str_serial; }
                if (p) ep0_send(p, n, s->wLength);
                else   ep0_stall();
            } else {
                ep0_stall();                              /* incl. qualifier */
            }
            return;
        }
        case 0x05:                                        /* SET_ADDRESS */
            ep0_addr_pending = (uint8_t)(s->wValue & 0x7F);
            ep0_send_zlp();
            return;
        case 0x09:                                        /* SET_CONFIGURATION */
            configure_data_endpoints();
            usb_is_configured = (s->wValue & 0xFF) ? 1 : 0;
            ep0_send_zlp();
            return;
        case 0x08: {                                      /* GET_CONFIGURATION */
            static uint8_t cfg;
            cfg = usb_is_configured;
            ep0_send(&cfg, 1, s->wLength);
            return;
        }
        case 0x00: {                                      /* GET_STATUS */
            static const uint8_t zero[2] = { 0, 0 };
            ep0_send(zero, 2, s->wLength);
            return;
        }
        case 0x01:                                        /* CLEAR_FEATURE */
        case 0x03:                                        /* SET_FEATURE */
        case 0x0B:                                        /* SET_INTERFACE */
            ep0_send_zlp();
            return;
        case 0x0A: {                                      /* GET_INTERFACE */
            static const uint8_t alt = 0;
            ep0_send(&alt, 1, s->wLength);
            return;
        }
        default:
            ep0_stall();
            return;
        }
    }

    if (type == 0x20) {                                   /* CDC class */
        switch (s->bRequest) {
        case 0x20:                                        /* SET_LINE_CODING */
            ep0_out_stage = 1;                            /* data arrives next */
            ep_stat_rx(0, USB_EP_STAT_VALID);
            return;
        case 0x21:                                        /* GET_LINE_CODING */
            ep0_send(line_coding, sizeof line_coding, s->wLength);
            return;
        case 0x22:                                        /* SET_CONTROL_LINE_STATE */
        case 0x23:                                        /* SEND_BREAK */
            ep0_send_zlp();
            return;
        default:
            ep0_stall();
            return;
        }
    }

    ep0_stall();
}

static void ep0_handler(uint16_t epr)
{
    if (epr & USB_EP_CTR_RX) {
        uint16_t count = (uint16_t)(BT_COUNT_RX(0) & 0x3FF);
        uint8_t setup = (epr & USB_EP_SETUP) ? 1 : 0;
        ep_clr_ctr_rx(0);

        if (setup) {
            uint8_t raw[8];
            setup_t s;
            pma_read(EP0_RX_OFF, raw, 8);
            s.bmRequestType = raw[0];
            s.bRequest      = raw[1];
            s.wValue        = (uint16_t)(raw[2] | (raw[3] << 8));
            s.wIndex        = (uint16_t)(raw[4] | (raw[5] << 8));
            s.wLength       = (uint16_t)(raw[6] | (raw[7] << 8));
            ep0_out_stage = 0;
            handle_setup(&s);
            /* re-arm for the next SETUP / OUT status stage */
            ep_stat_rx(0, USB_EP_STAT_VALID);
        } else if (ep0_out_stage) {
            uint8_t buf[EP0_SIZE];
            if (count > sizeof buf) count = sizeof buf;
            pma_read(EP0_RX_OFF, buf, count);
            if (count >= sizeof line_coding) {
                for (uint32_t i = 0; i < sizeof line_coding; i++)
                    line_coding[i] = buf[i];
            }
            ep0_out_stage = 0;
            ep0_send_zlp();                                /* status stage */
            ep_stat_rx(0, USB_EP_STAT_VALID);
        } else {
            /* OUT status stage of a control-IN transfer */
            ep_stat_rx(0, USB_EP_STAT_VALID);
        }
    }

    if (epr & USB_EP_CTR_TX) {
        ep_clr_ctr_tx(0);
        if (ep0_addr_pending) {
            USB_DADDR = (uint16_t)(USB_DADDR_EF | ep0_addr_pending);
            ep0_addr_pending = 0;
        }
        if (ep0_rem) {
            uint16_t n = ep0_rem > EP0_SIZE ? EP0_SIZE : ep0_rem;
            pma_write(EP0_TX_OFF, ep0_ptr, n);
            BT_COUNT_TX(0) = n;
            ep0_ptr += n;
            ep0_rem = (uint16_t)(ep0_rem - n);
            ep_stat_tx(0, USB_EP_STAT_VALID);
        } else if (ep0_zlp) {
            ep0_zlp = 0;
            BT_COUNT_TX(0) = 0;
            ep_stat_tx(0, USB_EP_STAT_VALID);
        }
    }
}

static void ep2_out_handler(void)
{
    uint16_t count = (uint16_t)(BT_COUNT_RX(2) & 0x3FF);
    uint8_t buf[EPBULK_SIZE];

    if (count > sizeof buf)
        count = sizeof buf;
    pma_read(EP2_RX_OFF, buf, count);
    ep_clr_ctr_rx(2);

    for (uint16_t i = 0; i < count; i++) {
        if (ring_free() == 0)
            break;                                   /* drop: host overran us */
        rx_ring[rx_head] = buf[i];
        rx_head = (uint16_t)((rx_head + 1) & (RX_RING_SZ - 1));
    }

    if (ring_free() >= EPBULK_SIZE) {
        ep_stat_rx(2, USB_EP_STAT_VALID);
        rx_ep_armed = 1;
    } else {
        rx_ep_armed = 0;                             /* NAK until drained */
    }
}

static void usb_reset(void)
{
    rx_head = rx_tail = 0;
    rx_ep_armed = 0;
    usb_is_configured = 0;
    ep0_rem = 0;
    ep0_zlp = 0;
    ep0_addr_pending = 0;
    ep0_out_stage = 0;

    USB_BTABLE = BTABLE_OFF;

    USB_EPR(0) = USB_EP_TYPE_CONTROL | 0;
    BT_ADDR_TX(0) = EP0_TX_OFF;
    BT_COUNT_TX(0) = 0;
    BT_ADDR_RX(0) = EP0_RX_OFF;
    BT_COUNT_RX(0) = COUNT_RX_64;
    ep_stat_rx(0, USB_EP_STAT_VALID);
    ep_stat_tx(0, USB_EP_STAT_NAK);

    USB_DADDR = USB_DADDR_EF;                        /* address 0, enabled */
}

void USB_LP_CAN1_RX0_IRQHandler(void)
{
    uint16_t istr;

    while ((istr = USB_ISTR) & (USB_ISTR_CTR | USB_ISTR_RESET |
                                USB_ISTR_SUSP | USB_ISTR_WKUP)) {
        if (istr & USB_ISTR_RESET) {
            USB_ISTR = (uint16_t)~USB_ISTR_RESET;
            usb_reset();
            continue;
        }
        if (istr & USB_ISTR_SUSP) {
            USB_ISTR = (uint16_t)~USB_ISTR_SUSP;
            continue;
        }
        if (istr & USB_ISTR_WKUP) {
            USB_ISTR = (uint16_t)~USB_ISTR_WKUP;
            continue;
        }
        if (istr & USB_ISTR_CTR) {
            uint32_t ep = istr & USB_ISTR_EP_ID;
            uint16_t epr = USB_EPR(ep);
            if (ep == 0) {
                ep0_handler(epr);
            } else if (ep == 1) {
                ep_clr_ctr_tx(1);
            } else if (ep == 2) {
                if (epr & USB_EP_CTR_RX)
                    ep2_out_handler();
                if (epr & USB_EP_CTR_TX)
                    ep_clr_ctr_tx(2);
            } else {
                if (epr & USB_EP_CTR_RX) ep_clr_ctr_rx(ep);
                if (epr & USB_EP_CTR_TX) ep_clr_ctr_tx(ep);
            }
        }
    }
}

void usb_init(void)
{
    RCC_APB2ENR |= RCC_APB2ENR_IOPAEN;

    /* Force the host to re-enumerate after a warm reset: hold D+ low briefly.
     * (The Blue Pill wires its 1.5k pull-up permanently to PA12.) */
    gpio_cfg(GPIOA_BASE, 12, GPIO_OUT_PP_50);
    GPIO_BRR(GPIOA_BASE) = (1u << 12);
    delay_ms(20);
    gpio_cfg(GPIOA_BASE, 12, GPIO_IN_FLOAT);
    gpio_cfg(GPIOA_BASE, 11, GPIO_IN_FLOAT);

    RCC_APB1ENR |= RCC_APB1ENR_USBEN;
    RCC_APB1RSTR |= RCC_APB1RSTR_USBRST;
    delay_us(10);
    RCC_APB1RSTR &= ~RCC_APB1RSTR_USBRST;

    USB_CNTR = USB_CNTR_FRES;          /* exit power-down, hold reset */
    delay_us(10);
    USB_CNTR = 0;                      /* release reset */
    USB_ISTR = 0;
    USB_CNTR = USB_CNTR_CTRM | USB_CNTR_RESETM | USB_CNTR_SUSPM | USB_CNTR_WKUPM;

    nvic_enable(IRQ_USB_LP);
}

int usb_configured(void)
{
    return usb_is_configured;
}

int usb_getc_nb(void)
{
    if (rx_head == rx_tail)
        return -1;
    uint8_t c = rx_ring[rx_tail];
    rx_tail = (uint16_t)((rx_tail + 1) & (RX_RING_SZ - 1));

    /* re-arm the OUT endpoint once the ring has room again */
    if (!rx_ep_armed && ring_free() >= EPBULK_SIZE) {
        __asm volatile ("cpsid i");
        if (!rx_ep_armed) {
            ep_stat_rx(2, USB_EP_STAT_VALID);
            rx_ep_armed = 1;
        }
        __asm volatile ("cpsie i");
    }
    return c;
}

uint8_t usb_getc(void)
{
    int c;
    while ((c = usb_getc_nb()) < 0) {
        __asm volatile ("wfi");
    }
    return (uint8_t)c;
}

void usb_write(const uint8_t *data, uint32_t len)
{
    uint32_t sent = 0;

    if (!usb_is_configured)
        return;

    while (sent < len) {
        uint32_t n = len - sent;
        if (n > EPBULK_SIZE)
            n = EPBULK_SIZE;
        /* wait for the previous packet to be collected by the host */
        while ((USB_EPR(1) & USB_EP_STAT_TX) == (USB_EP_STAT_VALID << 4)) {
            if (!usb_is_configured)
                return;
        }
        pma_write(EP1_TX_OFF, data + sent, n);
        BT_COUNT_TX(1) = (uint16_t)n;
        ep_stat_tx(1, USB_EP_STAT_VALID);
        sent += n;
    }

    /* terminate with a ZLP when the last packet exactly filled the endpoint,
     * so the host's driver flushes instead of waiting on its read timeout */
    if (len && (len % EPBULK_SIZE) == 0) {
        while ((USB_EPR(1) & USB_EP_STAT_TX) == (USB_EP_STAT_VALID << 4)) {
            if (!usb_is_configured)
                return;
        }
        BT_COUNT_TX(1) = 0;
        ep_stat_tx(1, USB_EP_STAT_VALID);
    }
}
