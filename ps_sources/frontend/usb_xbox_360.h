/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef USB_XBOX_360_H
#define USB_XBOX_360_H

#include <stdint.h>

/* Run after CherryUSB has reaped completed URBs. */
void usb_xbox_360_poll(void);
void usb_xbox_360_stop(void);
void usb_xbox_360_dump_status(uint32_t uart_base);

#endif /* USB_XBOX_360_H */
