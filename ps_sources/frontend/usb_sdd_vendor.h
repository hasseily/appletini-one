/******************************************************************************
 * usb_sdd_vendor.h -- native Appletini SDD vendor device on USB0.
 *
 * The device exposes one vendor-specific interface with bulk OUT endpoint
 * 0x01 and bulk IN endpoint 0x81. Its wire protocol carries register messages,
 * 0x1004 event batches, and 0x1000 status messages.
 *
 * Windows binds WinUSB automatically via MS OS 2.0 descriptors (BOS +
 * platform capability + vendor-code descriptor set with a WINUSB
 * compatible ID and a DeviceInterfaceGUIDs registry property), so no
 * driver installation is needed. SuperDuperDisplay opens the device by
 * that interface GUID and does plain bulk transfers.
 ******************************************************************************/

#ifndef USB_SDD_VENDOR_H
#define USB_SDD_VENDOR_H

#include "xusbps.h"
#include "xusbps_ch9.h"

#ifdef __cplusplus
extern "C" {
#endif

/* pid.codes open-source VID + project PID. */
#define SDDV_VID                0x1209U
#define SDDV_PID                0xA271U

/* Single bidirectional data channel on EP1. */
#define SDDV_EP_DATA            1U

/* Vendor code carried in the BOS platform capability; Windows uses it to
 * request the MS OS 2.0 descriptor set (wIndex 0x07). */
#define SDDV_MS_VENDOR_CODE     0x20U
#define SDDV_MS_OS_20_DESC_SET  0x07U

/* Device interface GUID registered by WinUSB via the MS OS 2.0 registry
 * property; SuperDuperDisplay discovers the device with it:
 *   {F5A31C8E-7D3B-4E1C-9A64-52AA35C10B71}
 */

u32  XUsbPs_Ch9SetupDevDescReply_SddVendor(u8 *BufPtr, u32 BufLen);
u32  XUsbPs_Ch9SetupCfgDescReply_SddVendor(u8 *BufPtr, u32 BufLen);
u32  XUsbPs_Ch9SetupStrDescReply_SddVendor(u8 *BufPtr, u32 BufLen, u8 Index);
u32  XUsbPs_Ch9SetupBosDescReply_SddVendor(u8 *BufPtr, u32 BufLen);
void XUsbPs_SetConfiguration_SddVendor(XUsbPs *InstancePtr, int ConfigIdx);

/* Vendor request handler; returns 1 when the request was consumed. */
int  usb_sdd_vendor_req(XUsbPs *InstancePtr,
                        XUsbPs_SetupData *SetupData,
                        int *StatusOut);

/* Bring-up counters (defined in xusbps_ch9_sddvendor.c). */
extern u32 g_sddv_vendor_in_count;
extern u32 g_sddv_vendor_out_count;
extern u32 g_sddv_msos_reads;

/* Vendor-OUT data stage pending; EP0 DATA_RX completes the ACK. */
extern volatile u8 g_sddv_vendor_out_ack_owed;

#ifdef __cplusplus
}
#endif

#endif /* USB_SDD_VENDOR_H */
