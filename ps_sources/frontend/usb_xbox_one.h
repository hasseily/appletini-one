#ifndef USB_XBOX_ONE_H
#define USB_XBOX_ONE_H

#include <stdint.h>

/* Run after CherryUSB has reaped completed URBs. */
void usb_xbox_one_poll(void);
void usb_xbox_one_stop(void);
void usb_xbox_one_dump_status(uint32_t uart_base);

#endif
