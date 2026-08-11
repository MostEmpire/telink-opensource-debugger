/* Vector table, reset handler and 72 MHz / 48 MHz-USB clock init. */
#include "stm32f103.h"

extern uint32_t _sidata, _sdata, _edata, _sbss, _ebss, _estack;
extern int main(void);

void Reset_Handler(void);
void Default_Handler(void) { while (1) {} }
void USB_LP_CAN1_RX0_IRQHandler(void);

#define WEAK __attribute__((weak, alias("Default_Handler")))
void NMI_Handler(void) WEAK;
void HardFault_Handler(void) WEAK;

typedef void (*vector_t)(void);

__attribute__((section(".isr_vector"), used))
vector_t const g_vectors[] = {
    (vector_t)&_estack,
    Reset_Handler,
    NMI_Handler,
    HardFault_Handler,
    /* MemManage, BusFault, UsageFault, 4x reserved, SVC, DebugMon,
     * reserved, PendSV, SysTick  -> indices 4..15 */
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    /* IRQ 0..19: WWDG .. USB_HP_CAN1_TX */
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    /* IRQ 20 */
    USB_LP_CAN1_RX0_IRQHandler,
};

static void clock_init(void)
{
    RCC_CR |= RCC_CR_HSEON;
    while (!(RCC_CR & RCC_CR_HSERDY)) {}

    /* 2 wait states + prefetch for 72 MHz */
    FLASH_ACR = (FLASH_ACR & ~0x7u) | 0x2u | (1u << 4);

    /* PLL = HSE(8 MHz) x9 = 72 MHz; AHB/1, APB1/2 = 36 MHz, APB2/1 = 72 MHz.
     * Bit 22 (USBPRE) left at 0 => USB clock = PLL / 1.5 = 48 MHz. */
    RCC_CFGR = (0x7u << 18)      /* PLLMUL = x9  */
             | (1u << 16)        /* PLLSRC = HSE */
             | (0x4u << 8);      /* PPRE1  = /2  */
    RCC_CR |= RCC_CR_PLLON;
    while (!(RCC_CR & RCC_CR_PLLRDY)) {}
    RCC_CFGR = (RCC_CFGR & ~0x3u) | 0x2u;
    while (((RCC_CFGR >> 2) & 0x3u) != 0x2u) {}

    /* DWT cycle counter backs delay_us()/delay_ms() */
    DEMCR |= DEMCR_TRCENA;
    DWT_CYCCNT = 0;
    DWT_CTRL |= DWT_CTRL_CYCCNTENA;
}

void Reset_Handler(void)
{
    uint32_t *src = &_sidata, *dst = &_sdata;

    while (dst < &_edata) *dst++ = *src++;
    for (dst = &_sbss; dst < &_ebss;) *dst++ = 0;

    clock_init();
    main();
    while (1) {}
}
