/* Forced include (/FI) for every translation unit of the host render
 * harness. Neuters GCC-isms for MSVC and redirects the card-control
 * register window into a fake array BEFORE card_control_regs.h defines
 * APPLE_DEBUG_BASE. Build 32-bit x86 so hardware-address arithmetic on
 * uint32_t stays pointer-sized. */
#ifndef HARNESS_PREFIX_H
#define HARNESS_PREFIX_H

#include <stdint.h>
#include <stddef.h>
#include <string.h>

#define __attribute__(x)

extern unsigned char g_fake_card_regs[8192];
#define APPLE_DEBUG_BASE ((uint32_t)(uintptr_t)g_fake_card_regs)

#endif
