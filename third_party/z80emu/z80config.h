/* z80config.h -- Appletini Applicard configuration for z80emu.
 *
 * All optional behaviors stay at upstream defaults:
 *  - little-endian host (Cortex-A9), so no Z80_BIG_ENDIAN
 *  - full undocumented-flag emulation (ZEXALL-clean)
 *  - no instruction catching: the Applicard has no interrupt sources wired
 *    (the CTC socket is unpopulated), so HALT simply burns the slice budget
 *    inside Z80Emulate and the service's idle governor handles the common
 *    "polling the status port" wait instead.
 */

#ifndef __Z80CONFIG_INCLUDED__
#define __Z80CONFIG_INCLUDED__

/* #define Z80_BIG_ENDIAN */
/* #define Z80_DOCUMENTED_FLAGS_ONLY */
/* #define Z80_CATCH_HALT */
/* #define Z80_CATCH_DI */
/* #define Z80_CATCH_EI */
/* #define Z80_CATCH_RETI */
/* #define Z80_CATCH_RETN */
/* #define Z80_CATCH_ED_UNDEFINED */
/* #define Z80_PREFIX_FAILSAFE */
/* #define Z80_FALSE_CONDITION_FETCH */
/* #define Z80_HANDLE_SELF_MODIFYING_CODE */
/* #define Z80_MASK_IM2_VECTOR_ADDRESS */

#endif
