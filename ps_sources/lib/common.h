/******************************************************************************
 * Common Hardware Access Utilities
 *
 * @file    common.h
 * @brief   Low-level register access macros and hardware definitions
 *
 * OVERVIEW
 * ========
 * This module provides fundamental hardware register access macros that work
 * independently of the Xilinx BSP. These macros are used throughout the
 * PS application for direct hardware control, especially useful for
 * early boot code, minimal applications, or when BSP functions are unavailable.
 *
 * FEATURES
 * ========
 * - Direct memory-mapped register access
 * - Volatile pointer casting for hardware registers
 * - Type-safe address handling with uintptr_t
 * - BSP fallback definitions (XPAR_* parameters)
 * - Zero dependencies (pure C macros)
 *
 * REGISTER ACCESS SAFETY
 * ======================
 * The macros use proper volatile semantics to ensure:
 * 1. Compiler doesn't optimize away repeated reads/writes
 * 2. Memory accesses occur in the order written
 * 3. No register access is cached or reordered
 *
 * The uintptr_t cast ensures portability across 32-bit and 64-bit architectures,
 * though Zynq-7000 is strictly 32-bit.
 *
 * TYPICAL USAGE PATTERNS
 * ======================
 * Read-modify-write:
 * ```c
 * uint32_t val = REG_READ(CTRL_REG);
 * val |= ENABLE_BIT;
 * REG_WRITE(CTRL_REG, val);
 * ```
 *
 * Polling for status:
 * ```c
 * while (REG_READ(STATUS_REG) & BUSY_FLAG) {
 *     // Wait for operation to complete
 * }
 * ```
 *
 * Direct write:
 * ```c
 * REG_WRITE(DATA_REG, 0x12345678);
 * ```
 *
 * DEPENDENCIES
 * ============
 * - stdint.h (for uint32_t, uintptr_t) - included by caller
 * - None - this header is dependency-free
 *
 * USAGE EXAMPLES
 * ==============
 *
 * Example 1: UART Register Access
 * ```c
 * #include "common.h"
 *
 * #define UART0_BASE      0xE0000000U
 * #define UART_SR_OFFSET  0x2CU
 * #define UART_FIFO       0x30U
 * #define UART_SR_TXFULL  0x10U
 *
 * void uart_putc(char c) {
 *     // Wait for TX FIFO not full
 *     while (REG_READ(UART0_BASE + UART_SR_OFFSET) & UART_SR_TXFULL) {
 *         // Busy wait
 *     }
 *     // Write character to FIFO
 *     REG_WRITE(UART0_BASE + UART_FIFO, (uint32_t)c);
 * }
 * ```
 *
 * Example 2: Framebuffer Control
 * ```c
 * #include "common.h"
 *
 * #define FB_CONTROL_BASE 0x40000000U
 * #define FB_BASE_ADDR    (FB_CONTROL_BASE + 0x00)
 * #define FB_CONTROL_REG  (FB_CONTROL_BASE + 0x04)
 * #define FB_STATUS_REG   (FB_CONTROL_BASE + 0x08)
 *
 * void fb_swap_buffer(uint32_t new_addr) {
 *     // Update framebuffer address
 *     REG_WRITE(FB_BASE_ADDR, new_addr);
 *
 *     // Read back status
 *     uint32_t status = REG_READ(FB_STATUS_REG);
 *     printf("Frame count: %lu\n", status);
 * }
 * ```
 *
 * Example 3: GPIO Bit Manipulation
 * ```c
 * #include "common.h"
 *
 * #define GPIO_BASE       0xE000A000U
 * #define GPIO_DATA       (GPIO_BASE + 0x40)
 * #define GPIO_DIR        (GPIO_BASE + 0x44)
 * #define LED_BIT         (1U << 7)
 *
 * void led_init(void) {
 *     // Set LED pin as output
 *     uint32_t dir = REG_READ(GPIO_DIR);
 *     dir |= LED_BIT;
 *     REG_WRITE(GPIO_DIR, dir);
 * }
 *
 * void led_on(void) {
 *     uint32_t data = REG_READ(GPIO_DATA);
 *     data |= LED_BIT;
 *     REG_WRITE(GPIO_DATA, data);
 * }
 *
 * void led_off(void) {
 *     uint32_t data = REG_READ(GPIO_DATA);
 *     data &= ~LED_BIT;
 *     REG_WRITE(GPIO_DATA, data);
 * }
 * ```
 *
 * Example 4: SLCR (System Level Control Registers) Access
 * ```c
 * #include "common.h"
 *
 * #define SLCR_BASE       0xF8000000U
 * #define SLCR_UNLOCK     (SLCR_BASE + 0x008)
 * #define SLCR_LOCK       (SLCR_BASE + 0x004)
 * #define SLCR_UNLOCK_KEY 0xDF0DU
 * #define SLCR_LOCK_KEY   0x767BU
 *
 * void slcr_unlock(void) {
 *     REG_WRITE(SLCR_UNLOCK, SLCR_UNLOCK_KEY);
 * }
 *
 * void slcr_lock(void) {
 *     REG_WRITE(SLCR_LOCK, SLCR_LOCK_KEY);
 * }
 * ```
 *
 * COMPARISON WITH XIL_IO.H
 * =========================
 * The Xilinx BSP provides similar functions in xil_io.h:
 * - Xil_In32() ≈ REG_READ()
 * - Xil_Out32() ≈ REG_WRITE()
 *
 * Key differences:
 * 1. REG_READ/WRITE are simple macros (inline, zero overhead)
 * 2. Xil_In32/Out32 may include barriers or cache operations
 * 3. This header has no BSP dependency
 * 4. Both are functionally equivalent for uncached memory regions
 *
 * Use REG_READ/WRITE when:
 * - BSP is not available (early boot, standalone)
 * - Minimal code size is critical
 * - You need guaranteed inline expansion
 *
 * Use Xil_In32/Out32 when:
 * - BSP is available and you prefer consistency
 * - You need automatic cache coherency handling
 *
 * MEMORY ORDERING
 * ===============
 * These macros provide compiler barrier semantics via 'volatile', but do NOT
 * insert ARM memory barrier instructions (DMB/DSB). For multi-core or DMA
 * scenarios requiring strict ordering, you may need:
 * ```c
 * #include <arm_acle.h>
 * REG_WRITE(addr, val);
 * __dmb(0xF);  // Full data memory barrier
 * ```
 *
 * CACHE COHERENCY
 * ===============
 * These macros do NOT handle cache operations. When accessing shared memory
 * with PL (via AXI HP ports), ensure the region is marked non-cacheable or
 * manually flush/invalidate:
 * ```c
 * Xil_DCacheFlushRange(addr, size);  // Before PL reads PS writes
 * Xil_DCacheInvalidateRange(addr, size);  // Before PS reads PL writes
 * ```
 *
 * For framebuffers accessed by PL, mark region non-cacheable:
 * ```c
 * Xil_SetTlbAttributes(FB_BASE, 0x14de2);  // Device, non-cacheable
 * ```
 *
 * ALIGNMENT REQUIREMENTS
 * ======================
 * ARM Cortex-A9 requires aligned accesses for load/store instructions:
 * - REG_READ/WRITE assume 4-byte aligned addresses
 * - Unaligned access will fault (data abort exception)
 * - Always use aligned register addresses (multiple of 4)
 *
 * PERFORMANCE
 * ===========
 * - REG_READ: 1 CPU cycle (L1 cache hit) to ~100 cycles (MMIO access)
 * - REG_WRITE: 1 CPU cycle (L1 cache) to ~100 cycles (MMIO write)
 * - MMIO accesses (uncached peripherals) have ~10-20ns latency
 *
 * PORTABILITY NOTES
 * =================
 * - Designed for ARM Cortex-A9 (Zynq-7000 PS)
 * - Little-endian byte order assumed
 * - 32-bit register width assumed
 * - Not portable to Harvard architectures or systems with I/O space
 *
 * BSP PARAMETER FALLBACKS
 * =======================
 * This header provides fallback definitions for common XPAR_* parameters
 * when xparameters.h is not available. Includes:
 * - XPAR_XIICPS_0_BASEADDR: I2C0 base address (0xE0004000)
 *
 * Add more as needed:
 * ```c
 * #ifndef XPAR_XUARTPS_0_BASEADDR
 * #define XPAR_XUARTPS_0_BASEADDR 0xE0000000U
 * #endif
 * ```
 *
 * NOTES
 * =====
 * - Always use 'U' suffix for hex constants (unsigned)
 * - Check hardware documentation for register addresses and bit fields
 * - Zynq-7000 TRM Chapter 6 documents PS register map
 * - Use with caution - incorrect register writes can hang the system
 * - Atomic operations NOT provided (use locks for multi-core access)
 *
 * AUTHOR
 * ======
 # Rikkles
 * Part of the Appletini project
 ******************************************************************************/

#ifndef COMMON_H
#define COMMON_H

/*==========================================================================
 * BSP Parameter Fallbacks
 *
 * Provide default values for common XPAR_* definitions when xparameters.h
 * is not available. These match the standard Zynq-7000 PS register map.
 *==========================================================================*/

#ifndef XPAR_XIICPS_0_BASEADDR
#define XPAR_XIICPS_0_BASEADDR 0xE0004000U
#endif

/*==========================================================================
 * Hardware Register Access Macros
 *
 * REG_READ(addr)  - Read 32-bit value from memory-mapped register
 * REG_WRITE(addr, val) - Write 32-bit value to memory-mapped register
 *
 * Both macros use volatile pointers to ensure:
 * - Compiler doesn't optimize away register accesses
 * - Memory operations occur in source code order
 * - No caching of register values
 *
 * The uintptr_t cast provides type safety when converting addresses
 * (typically uint32_t literals) to pointers.
 *==========================================================================*/

/**
 * @brief Read 32-bit value from hardware register
 *
 * @param addr  Physical address of register (must be 4-byte aligned)
 * @return      32-bit value read from register
 *
 * Example:
 *   uint32_t status = REG_READ(0xE0000000 + 0x2C);
 */
#define REG_READ(addr)       (*(volatile uint32_t *)(uintptr_t)(addr))

/**
 * @brief Write 32-bit value to hardware register
 *
 * @param addr  Physical address of register (must be 4-byte aligned)
 * @param val   32-bit value to write
 *
 * Example:
 *   REG_WRITE(0xE0000000 + 0x30, 0x41);  // Write 'A' to UART FIFO
 */
#define REG_WRITE(addr, val) (*(volatile uint32_t *)(uintptr_t)(addr) = (uint32_t)(val))

/* MMIO rules for PL control registers (e.g. FB_CONTROL_BASE on GP0):
 * 1) Use only REG_READ/REG_WRITE (or Xil_In32/Xil_Out32) on register space.
 * 2) Never use memcpy/memset on MMIO register windows.
 * 3) Map register windows as MMIO/Device in MMU setup.
 * 4) For write-then-poll flows, add barriers if you introduce out-of-order risks.
 */

#endif
