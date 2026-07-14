/******************************************************************************
 * Zynq PS UART Output Library
 *
 * @file    uart.h
 * @brief   Lightweight UART output functions for debugging and logging
 *
 * OVERVIEW
 * ========
 * This module provides simple, direct-to-hardware UART output functions for
 * both UART0 and UART1 on the Zynq PS. Ideal for early boot debugging,
 * minimal BSP applications, or situations where printf() is too heavyweight.
 *
 * Note the COM ports will change randomly as you plug cards into the computer.
 *
 * HARDWARE CONFIGURATION
 * ======================
 * - UART0: Base 0xE0000000, connected to COM3 (CP2105 Enhanced Port)
 * - UART1: Base 0xE0001000, connected to COM4 (CP2105 Standard Port)
 * - Baud rate: configurable (default often 115200 from FSBL/BSP)
 * - Format: 8-N-1 (8 data bits, no parity, 1 stop bit)
 *
 * FEATURES
 * ========
 * - Direct register access (no BSP required)
 * - Optional mirroring: output to UART0 also goes to UART1
 * - Timeout protection prevents infinite loops on TX full
 * - Supports both UARTs independently
 * - Minimal code size and overhead
 *
 * MIRRORING
 * =========
 * When UART_MIRROR_BOTH is enabled (default in uart.c), any output sent to
 * UART0 is automatically duplicated to UART1. This allows monitoring on both
 * COM ports simultaneously.
 *
 * To disable mirroring, modify uart.c:
 * ```c
 * #define UART_MIRROR_BOTH 0
 * ```
 *
 * DEPENDENCIES
 * ============
 * - None (standalone, direct hardware access)
 * - Optional: XPAR_XUARTPS_0_BASEADDR from xparameters.h if available
 * - common.h for REG_READ/REG_WRITE macros (or define inline)
 *
 * USAGE
 * =====
 * Initialization is optional if FSBL has configured the UARTs, but the app can
 * also reprogram baud rate explicitly with uart_init_baud()/uart_init_both().
 *
 * Basic output:
 * ```c
 * uart_puts(UART0_BASE, "Hello World\r\n");
 * uart_puthex(UART0_BASE, 0xDEADBEEF);
 * uart_putdec(UART0_BASE, 42);
 * ```
 *
 * EXAMPLE
 * =======
 * ```c
 * #include "uart.h"
 *
 * int main(void) {
 *     // Print banner
 *     uart_puts(UART0_BASE, "\r\n");
 *     uart_puts(UART0_BASE, "=========================\r\n");
 *     uart_puts(UART0_BASE, "Appletini Boot Sequence\r\n");
 *     uart_puts(UART0_BASE, "=========================\r\n");
 *
 *     // Print register values
 *     uint32_t ctrl_reg = 0x12345678;
 *     uart_puts(UART0_BASE, "Control Register: 0x");
 *     uart_puthex(UART0_BASE, ctrl_reg);
 *     uart_puts(UART0_BASE, "\r\n");
 *
 *     // Print decimal counter
 *     for (int i = 0; i < 10; i++) {
 *         uart_puts(UART0_BASE, "Count: ");
 *         uart_putdec(UART0_BASE, i);
 *         uart_puts(UART0_BASE, "\r\n");
 *     }
 *
 *     return 0;
 * }
 * ```
 *
 * OUTPUT TO BOTH UARTS
 * ====================
 * ```c
 * // Send to UART0 (COM3) - also mirrored to UART1 if enabled
 * uart_puts(UART0_BASE, "This goes to COM3\r\n");
 *
 * // Send to UART1 (COM4) only
 * uart_puts(UART1_BASE, "This goes to COM4 only\r\n");
 * ```
 *
 * API FUNCTIONS
 * =============
 * - uart_putc_one(): Send char to one UART (no mirroring)
 * - uart_putc(): Send char (with mirroring if enabled)
 * - uart_puts(): Send null-terminated string
 * - uart_puthex(): Send 32-bit hex value (8 digits, uppercase)
 * - uart_putdec(): Send unsigned decimal number
 *
 * TIMEOUT BEHAVIOR
 * ================
 * If the UART TX FIFO is full, functions will retry for UART_TX_TIMEOUT
 * iterations (~100ms at 666 MHz CPU). If timeout expires, the character is
 * dropped silently to prevent deadlock.
 *
 * PERFORMANCE
 * ===========
 * At 115200 baud:
 * - Single character: ~87µs
 * - String "Hello\r\n": ~610µs
 * - 8-digit hex: ~700µs
 *
 * NOTES
 * =====
 * - Functions are blocking but have timeout protection
 * - No buffering - characters sent immediately
 * - No input (RX) support in this module
 * - Compatible with both standalone and BSP-based applications
 * - UART base addresses auto-detected from xparameters.h if available
 *
 * AUTHOR
 * ======
 * Part of the Appletini One project
 ******************************************************************************/

#ifndef UART_H
#define UART_H

#include <stdint.h>

#ifdef XPAR_XUARTPS_0_BASEADDR
#define UART0_BASE           XPAR_XUARTPS_0_BASEADDR
#else
#define UART0_BASE           0xE0000000U
#endif
#ifdef XPAR_XUARTPS_1_BASEADDR
#define UART1_BASE           XPAR_XUARTPS_1_BASEADDR
#else
#define UART1_BASE           0xE0001000U
#endif

void uart_putc_one(uint32_t base, char c);
void uart_putc(uint32_t base, char c);
void uart_puts(uint32_t base, const char *s);
void uart_puthex(uint32_t base, uint32_t val);
void uart_putdec(uint32_t base, uint32_t val);
int uart_getc_nonblock(uint32_t base, char *c);
void uart_init_baud(uint32_t base, uint32_t baud);
void uart_init_both(uint32_t baud);

#endif
