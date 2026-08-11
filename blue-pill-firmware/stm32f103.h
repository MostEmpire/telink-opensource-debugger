/* Bare-metal register definitions for STM32F103C8 "Blue Pill" (Cortex-M3).
 * Only what the USB SWire bridge needs; no CMSIS/HAL so it builds with just
 * arm-none-eabi-gcc. */
#ifndef STM32F103_H
#define STM32F103_H
#include <stdint.h>

#define REG32(a) (*(volatile uint32_t *)(a))
#define REG16(a) (*(volatile uint16_t *)(a))

#define CPU_HZ 72000000u

/* ---------------------------------------------------------------- RCC ---- */
#define RCC_BASE      0x40021000u
#define RCC_CR        REG32(RCC_BASE + 0x00)
#define RCC_CFGR      REG32(RCC_BASE + 0x04)
#define RCC_APB2RSTR  REG32(RCC_BASE + 0x0C)
#define RCC_APB1RSTR  REG32(RCC_BASE + 0x10)
#define RCC_AHBENR    REG32(RCC_BASE + 0x14)
#define RCC_APB2ENR   REG32(RCC_BASE + 0x18)
#define RCC_APB1ENR   REG32(RCC_BASE + 0x1C)

#define RCC_CR_HSEON        (1u << 16)
#define RCC_CR_HSERDY       (1u << 17)
#define RCC_CR_PLLON        (1u << 24)
#define RCC_CR_PLLRDY       (1u << 25)

#define RCC_AHBENR_DMA1EN   (1u << 0)

#define RCC_APB2ENR_AFIOEN  (1u << 0)
#define RCC_APB2ENR_IOPAEN  (1u << 2)
#define RCC_APB2ENR_IOPBEN  (1u << 3)
#define RCC_APB2ENR_IOPCEN  (1u << 4)
#define RCC_APB2ENR_SPI1EN  (1u << 12)

#define RCC_APB2RSTR_SPI1RST (1u << 12)
#define RCC_APB1RSTR_USBRST  (1u << 23)
#define RCC_APB1ENR_USBEN    (1u << 23)

/* -------------------------------------------------------------- FLASH ---- */
#define FLASH_ACR     REG32(0x40022000u + 0x00)

/* --------------------------------------------------------------- GPIO ---- */
#define GPIOA_BASE    0x40010800u
#define GPIOB_BASE    0x40010C00u
#define GPIOC_BASE    0x40011000u
#define GPIO_CRL(p)   REG32((p) + 0x00)
#define GPIO_CRH(p)   REG32((p) + 0x04)
#define GPIO_IDR(p)   REG32((p) + 0x08)
#define GPIO_ODR(p)   REG32((p) + 0x0C)
#define GPIO_BSRR(p)  REG32((p) + 0x10)
#define GPIO_BRR(p)   REG32((p) + 0x14)

/* CNF|MODE nibble values for CRL/CRH */
#define GPIO_IN_FLOAT     0x4u
#define GPIO_IN_PULL      0x8u
#define GPIO_OUT_PP_50    0x3u
#define GPIO_OUT_OD_50    0x7u
#define GPIO_AF_PP_50     0xBu
#define GPIO_AF_OD_50     0xFu

/* set the 4-bit config nibble of pin `pin` (0..7 => CRL, 8..15 => CRH) */
static inline void gpio_cfg(uint32_t port, uint32_t pin, uint32_t cfg)
{
    if (pin < 8) {
        uint32_t v = GPIO_CRL(port);
        v &= ~(0xFu << (pin * 4));
        v |= (cfg & 0xFu) << (pin * 4);
        GPIO_CRL(port) = v;
    } else {
        uint32_t v = GPIO_CRH(port);
        v &= ~(0xFu << ((pin - 8) * 4));
        v |= (cfg & 0xFu) << ((pin - 8) * 4);
        GPIO_CRH(port) = v;
    }
}

/* --------------------------------------------------------------- SPI1 ---- */
#define SPI1_BASE     0x40013000u
#define SPI_CR1(s)    REG32((s) + 0x00)
#define SPI_CR2(s)    REG32((s) + 0x04)
#define SPI_SR(s)     REG32((s) + 0x08)
#define SPI_DR(s)     REG32((s) + 0x0C)

#define SPI_CR1_CPHA      (1u << 0)
#define SPI_CR1_CPOL      (1u << 1)
#define SPI_CR1_MSTR      (1u << 2)
#define SPI_CR1_BR_Pos    3
#define SPI_CR1_SPE       (1u << 6)
#define SPI_CR1_LSBFIRST  (1u << 7)
#define SPI_CR1_SSI       (1u << 8)
#define SPI_CR1_SSM       (1u << 9)
#define SPI_CR1_DFF       (1u << 11)

#define SPI_CR2_RXDMAEN   (1u << 0)
#define SPI_CR2_TXDMAEN   (1u << 1)

#define SPI_SR_RXNE       (1u << 0)
#define SPI_SR_TXE        (1u << 1)
#define SPI_SR_BSY        (1u << 7)

/* --------------------------------------------------------------- DMA1 ---- */
#define DMA1_BASE     0x40020000u
#define DMA_ISR       REG32(DMA1_BASE + 0x00)
#define DMA_IFCR      REG32(DMA1_BASE + 0x04)
/* channels are 1-based */
#define DMA_CCR(n)    REG32(DMA1_BASE + 0x08 + 20u * ((n) - 1))
#define DMA_CNDTR(n)  REG32(DMA1_BASE + 0x0C + 20u * ((n) - 1))
#define DMA_CPAR(n)   REG32(DMA1_BASE + 0x10 + 20u * ((n) - 1))
#define DMA_CMAR(n)   REG32(DMA1_BASE + 0x14 + 20u * ((n) - 1))

#define DMA_CCR_EN        (1u << 0)
#define DMA_CCR_TCIE      (1u << 1)
#define DMA_CCR_DIR_M2P   (1u << 4)
#define DMA_CCR_MINC      (1u << 7)
#define DMA_CCR_PL_HIGH   (2u << 12)
#define DMA_ISR_TCIF(n)   (1u << (4u * ((n) - 1) + 1))
#define DMA_IFCR_CGIF(n)  (1u << (4u * ((n) - 1)))

/* ---------------------------------------------------------------- USB ---- */
#define USB_BASE      0x40005C00u
#define USB_PMA_BASE  0x40006000u
#define USB_EPR(n)    REG16(USB_BASE + 4u * (n))
#define USB_CNTR      REG16(USB_BASE + 0x40)
#define USB_ISTR      REG16(USB_BASE + 0x44)
#define USB_FNR       REG16(USB_BASE + 0x48)
#define USB_DADDR     REG16(USB_BASE + 0x4C)
#define USB_BTABLE    REG16(USB_BASE + 0x50)

#define USB_CNTR_FRES     (1u << 0)
#define USB_CNTR_PDWN     (1u << 1)
#define USB_CNTR_RESETM   (1u << 10)
#define USB_CNTR_SUSPM    (1u << 11)
#define USB_CNTR_WKUPM    (1u << 12)
#define USB_CNTR_CTRM     (1u << 15)

#define USB_ISTR_EP_ID    0x000Fu
#define USB_ISTR_DIR      (1u << 4)
#define USB_ISTR_SOF      (1u << 9)
#define USB_ISTR_RESET    (1u << 10)
#define USB_ISTR_SUSP     (1u << 11)
#define USB_ISTR_WKUP     (1u << 12)
#define USB_ISTR_CTR      (1u << 15)

#define USB_DADDR_EF      (1u << 7)

/* EPnR bit fields */
#define USB_EP_CTR_RX     (1u << 15)
#define USB_EP_DTOG_RX    (1u << 14)
#define USB_EP_STAT_RX    (3u << 12)
#define USB_EP_SETUP      (1u << 11)
#define USB_EP_TYPE       (3u << 9)
#define USB_EP_KIND       (1u << 8)
#define USB_EP_CTR_TX     (1u << 7)
#define USB_EP_DTOG_TX    (1u << 6)
#define USB_EP_STAT_TX    (3u << 4)
#define USB_EP_EA         0x000Fu

#define USB_EP_TYPE_BULK      (0u << 9)
#define USB_EP_TYPE_CONTROL   (1u << 9)
#define USB_EP_TYPE_ISO       (2u << 9)
#define USB_EP_TYPE_INTERRUPT (3u << 9)

#define USB_EP_STAT_DISABLED  0u
#define USB_EP_STAT_STALL     1u
#define USB_EP_STAT_NAK       2u
#define USB_EP_STAT_VALID     3u

/* Bits that are plain read/write in EPnR (everything else is toggle or rc_w0) */
#define USB_EPR_RW_MASK   (USB_EP_TYPE | USB_EP_KIND | USB_EP_EA)

/* The packet memory is 16-bit wide but mapped at a 32-bit stride on the APB,
 * so a PMA byte offset `o` lives at USB_PMA_BASE + o*2. */
#define USB_PMA(off)  REG16(USB_PMA_BASE + 2u * (off))

/* ------------------------------------------------------ Cortex-M3 core ---- */
#define NVIC_ISER0    REG32(0xE000E100u)
#define SCB_AIRCR     REG32(0xE000ED0Cu)
#define DEMCR         REG32(0xE000EDFCu)
#define DEMCR_TRCENA  (1u << 24)
#define DWT_CTRL      REG32(0xE0001000u)
#define DWT_CYCCNT    REG32(0xE0001004u)
#define DWT_CTRL_CYCCNTENA (1u << 0)

#define IRQ_USB_LP    20u   /* USB_LP_CAN1_RX0 */

static inline void nvic_enable(uint32_t irq)
{
    REG32(0xE000E100u + 4u * (irq >> 5)) = 1u << (irq & 31u);
}

static inline void delay_cycles(uint32_t cycles)
{
    uint32_t t0 = DWT_CYCCNT;
    while ((DWT_CYCCNT - t0) < cycles) {}
}
static inline void delay_us(uint32_t us)   { delay_cycles(us * (CPU_HZ / 1000000u)); }
static inline void delay_ms(uint32_t ms)   { while (ms--) delay_us(1000); }

#endif /* STM32F103_H */
