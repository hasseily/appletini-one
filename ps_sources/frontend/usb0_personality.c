/******************************************************************************
 * usb0_personality.c -- see usb0_personality.h.
 ******************************************************************************/

#include "usb0_personality.h"
#include "xusbps_ch9_storage.h"
#include "xusbps_class_storage.h"
#include "usb_sdd_vendor.h"
#include "usb_sdd_service.h"

static volatile usb0_personality_t g_usb0_personality =
    USB0_PERSONALITY_STORAGE;

usb0_personality_t usb0_personality_get(void)
{
    return g_usb0_personality;
}

void usb0_personality_set(usb0_personality_t p)
{
    g_usb0_personality = p;
}

u32 XUsbPs_Ch9SetupDevDescReply(u8 *BufPtr, u32 BufLen)
{
    if (g_usb0_personality == USB0_PERSONALITY_SDD) {
        return XUsbPs_Ch9SetupDevDescReply_SddVendor(BufPtr, BufLen);
    }
    return XUsbPs_Ch9SetupDevDescReply_Storage(BufPtr, BufLen);
}

u32 XUsbPs_Ch9SetupCfgDescReply(u8 *BufPtr, u32 BufLen)
{
    if (g_usb0_personality == USB0_PERSONALITY_SDD) {
        return XUsbPs_Ch9SetupCfgDescReply_SddVendor(BufPtr, BufLen);
    }
    return XUsbPs_Ch9SetupCfgDescReply_Storage(BufPtr, BufLen);
}

u32 XUsbPs_Ch9SetupStrDescReply(u8 *BufPtr, u32 BufLen, u8 Index)
{
    if (g_usb0_personality == USB0_PERSONALITY_SDD) {
        return XUsbPs_Ch9SetupStrDescReply_SddVendor(BufPtr, BufLen, Index);
    }
    return XUsbPs_Ch9SetupStrDescReply_Storage(BufPtr, BufLen, Index);
}

u32 usb0_personality_bos_desc(u8 *BufPtr, u32 BufLen)
{
    if (g_usb0_personality == USB0_PERSONALITY_SDD) {
        return XUsbPs_Ch9SetupBosDescReply_SddVendor(BufPtr, BufLen);
    }
    /* Storage personality reports bcdUSB 2.00; a host should never ask.
     * Returning 0 makes ch9 stall the request. */
    return 0;
}

void XUsbPs_SetConfiguration(XUsbPs *InstancePtr, int ConfigIdx)
{
    if (g_usb0_personality == USB0_PERSONALITY_SDD) {
        XUsbPs_SetConfiguration_SddVendor(InstancePtr, ConfigIdx);
        usb_sdd_service_note_configured();
        return;
    }
    XUsbPs_SetConfiguration_Storage(InstancePtr, ConfigIdx);
}

void XUsbPs_ClassReq(XUsbPs *InstancePtr, XUsbPs_SetupData *SetupData)
{
    if (g_usb0_personality == USB0_PERSONALITY_SDD) {
        /* Vendor-class device: no class requests exist. */
        XUsbPs_EpStall(InstancePtr, 0,
                       XUSBPS_EP_DIRECTION_IN | XUSBPS_EP_DIRECTION_OUT);
        (void)SetupData;
        return;
    }
    XUsbPs_ClassReq_Storage(InstancePtr, SetupData);
}

int usb0_personality_vendor_req(XUsbPs *InstancePtr,
                                XUsbPs_SetupData *SetupData,
                                int *StatusOut)
{
    if (g_usb0_personality == USB0_PERSONALITY_SDD) {
        return usb_sdd_vendor_req(InstancePtr, SetupData, StatusOut);
    }
    return 0;
}
