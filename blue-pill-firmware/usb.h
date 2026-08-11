/* Minimal USB full-speed CDC-ACM device for STM32F103 (no HAL/CMSIS).
 * Presents a virtual COM port; the bridge command protocol rides on it. */
#ifndef USB_H
#define USB_H
#include <stdint.h>

void usb_init(void);
int  usb_configured(void);

/* Byte stream API.  usb_getc() blocks until a byte arrives. */
int  usb_getc_nb(void);                       /* -1 when the ring is empty */
uint8_t usb_getc(void);
void usb_write(const uint8_t *data, uint32_t len);

#endif /* USB_H */
