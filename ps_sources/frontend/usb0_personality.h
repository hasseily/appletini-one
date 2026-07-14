/******************************************************************************
 * usb0_personality.h -- runtime selection of the USB0 device identity.
 *
 * USB0 presents at most one device personality at a time:
 *   - STORAGE: the SD-card mass-storage bridge, attached only during the
 *     explicit remote-mount modal state.
 *   - SDD: FTDI FT601 emulation streaming Apple bus events to a locally
 *     running SuperDuperDisplay (see usb_sdd_service.c).
 *
 * The chapter-9 layer (xusbps_ch9.c) resolves its descriptor/class/config
 * callbacks against the canonical XUsbPs_* symbol names; this module owns
 * those symbols and forwards to the personality selected at the moment the
 * device (re-)enumerates. Switching personalities is only done while USB0
 * is soft-disconnected or detached, so the host never observes a mid-flight
 * identity change.
 ******************************************************************************/

#ifndef USB0_PERSONALITY_H
#define USB0_PERSONALITY_H

#include "xusbps.h"
#include "xusbps_ch9.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    USB0_PERSONALITY_STORAGE = 0,
    USB0_PERSONALITY_SDD = 1,
} usb0_personality_t;

usb0_personality_t usb0_personality_get(void);
void usb0_personality_set(usb0_personality_t p);

/* Called by xusbps_ch9.c for vendor requests before its standard Chapter 9
 * handling; returns 1 when the active personality consumed the request. */
int usb0_personality_vendor_req(XUsbPs *InstancePtr,
                                XUsbPs_SetupData *SetupData,
                                int *StatusOut);

/* BOS descriptor for GET_DESCRIPTOR(0x0F); returns 0 when the active
 * personality has none (ch9 stalls the request). */
u32 usb0_personality_bos_desc(u8 *BufPtr, u32 BufLen);

#ifdef __cplusplus
}
#endif

#endif /* USB0_PERSONALITY_H */
