/******************************************************************************
 * xusbps_ch9_sddvendor.c -- chapter-9 personality for the native Appletini
 * SDD vendor device. See usb_sdd_vendor.h.
 ******************************************************************************/

#include <string.h>

#include "xparameters.h"
#include "xusbps.h"
#include "xusbps_hw.h"
#include "xil_printf.h"
#include "xil_cache.h"

#include "xusbps_ch9.h"
#include "usb_sdd_vendor.h"

typedef struct {
    u8  bLength;
    u8  bDescriptorType;
    u16 bcdUSB;
    u8  bDeviceClass;
    u8  bDeviceSubClass;
    u8  bDeviceProtocol;
    u8  bMaxPacketSize0;
    u16 idVendor;
    u16 idProduct;
    u16 bcdDevice;
    u8  iManufacturer;
    u8  iProduct;
    u8  iSerialNumber;
    u8  bNumConfigurations;
} __attribute__((__packed__)) SDDV_STD_DEV_DESC;

typedef struct {
    u8  bLength;
    u8  bDescriptorType;
    u16 wTotalLength;
    u8  bNumInterfaces;
    u8  bConfigurationValue;
    u8  iConfiguration;
    u8  bmAttributes;
    u8  bMaxPower;
} __attribute__((__packed__)) SDDV_STD_CFG_DESC;

typedef struct {
    u8  bLength;
    u8  bDescriptorType;
    u8  bInterfaceNumber;
    u8  bAlternateSetting;
    u8  bNumEndPoints;
    u8  bInterfaceClass;
    u8  bInterfaceSubClass;
    u8  bInterfaceProtocol;
    u8  iInterface;
} __attribute__((__packed__)) SDDV_STD_IF_DESC;

typedef struct {
    u8  bLength;
    u8  bDescriptorType;
    u8  bEndpointAddress;
    u8  bmAttributes;
    u16 wMaxPacketSize;
    u8  bInterval;
} __attribute__((__packed__)) SDDV_STD_EP_DESC;

typedef struct {
    u8  bLength;
    u8  bDescriptorType;
    u16 wLANGID[1];
} __attribute__((__packed__)) SDDV_STD_STRING_DESC;

typedef struct {
    SDDV_STD_CFG_DESC stdCfg;
    SDDV_STD_IF_DESC  if0;
    SDDV_STD_EP_DESC  ep_out;   /* 0x01 bulk OUT */
    SDDV_STD_EP_DESC  ep_in;    /* 0x81 bulk IN  */
} __attribute__((__packed__)) SDDV_USB_CONFIG;

#define SDDV_DEVICE_DESC    0x01
#define SDDV_CONFIG_DESC    0x02
#define SDDV_STRING_DESC    0x03
#define SDDV_INTERFACE_DESC 0x04
#define SDDV_ENDPOINT_DESC  0x05

#define SDDV_EP0_MAXP       0x40

#ifndef be2les
#define be2les(x) ((u16)(x))
#endif

extern void usb_storage_debug_note_ep_prime(u8 EpNum, u8 Direction,
                                            int Status);

/* Bring-up visibility (printed by `sdd status`). */
u32 g_sddv_vendor_in_count;
u32 g_sddv_vendor_out_count;
u32 g_sddv_msos_reads;
volatile u8 g_sddv_vendor_out_ack_owed;

u32 XUsbPs_Ch9SetupDevDescReply_SddVendor(u8 *BufPtr, u32 BufLen)
{
    SDDV_STD_DEV_DESC deviceDesc = {
        sizeof(SDDV_STD_DEV_DESC),
        SDDV_DEVICE_DESC,
        be2les(0x0201),             /* bcdUSB 2.01: BOS descriptor present */
        0x00,                       /* device class: per interface */
        0x00,
        0x00,
        SDDV_EP0_MAXP,
        be2les(SDDV_VID),
        be2les(SDDV_PID),
        be2les(0x0100),
        0x01,                       /* iManufacturer */
        0x02,                       /* iProduct */
        0x03,                       /* iSerialNumber */
        0x01
    };

    if (!BufPtr || BufLen < sizeof(SDDV_STD_DEV_DESC)) {
        return 0;
    }
    memcpy(BufPtr, &deviceDesc, sizeof(SDDV_STD_DEV_DESC));
    return sizeof(SDDV_STD_DEV_DESC);
}

u32 XUsbPs_Ch9SetupCfgDescReply_SddVendor(u8 *BufPtr, u32 BufLen)
{
    SDDV_USB_CONFIG config = {
        {
            sizeof(SDDV_STD_CFG_DESC),
            SDDV_CONFIG_DESC,
            be2les(sizeof(SDDV_USB_CONFIG)),
            0x01,                   /* one interface */
            0x01,
            0x00,
            0xC0,                   /* self powered (the Apple powers us) */
            0x00
        },
        {
            sizeof(SDDV_STD_IF_DESC),
            SDDV_INTERFACE_DESC,
            0x00,
            0x00,
            0x02,
            0xFF, 0x00, 0x00,       /* vendor specific */
            0x02                    /* iInterface: product string */
        },
        {
            sizeof(SDDV_STD_EP_DESC),
            SDDV_ENDPOINT_DESC,
            0x00 | SDDV_EP_DATA,    /* 0x01 bulk OUT */
            0x02,
            be2les(0x200),
            0x00
        },
        {
            sizeof(SDDV_STD_EP_DESC),
            SDDV_ENDPOINT_DESC,
            0x80 | SDDV_EP_DATA,    /* 0x81 bulk IN */
            0x02,
            be2les(0x200),
            0x00
        }
    };

    if (!BufPtr || BufLen < sizeof(SDDV_USB_CONFIG)) {
        return 0;
    }
    memcpy(BufPtr, &config, sizeof(SDDV_USB_CONFIG));
    return sizeof(SDDV_USB_CONFIG);
}

static const char *sddv_string(u8 Index)
{
    switch (Index) {
        case 1: return "Appletini";
        case 2: return "Appletini SDD Stream";
        case 3: return "TINI0001";
        default: return NULL;
    }
}

u32 XUsbPs_Ch9SetupStrDescReply_SddVendor(u8 *BufPtr, u32 BufLen, u8 Index)
{
    u32 i;
    const char *String;
    u32 StringLen;
    u32 DescLen;
    u8 TmpBuf[128];
    SDDV_STD_STRING_DESC *StringDesc;

    if (!BufPtr) {
        return 0;
    }

    StringDesc = (SDDV_STD_STRING_DESC *)TmpBuf;

    if (Index == 0) {
        StringDesc->bLength = 4;
        StringDesc->bDescriptorType = SDDV_STRING_DESC;
        StringDesc->wLANGID[0] = be2les(0x0409);
    } else {
        String = sddv_string(Index);
        if (String == NULL) {
            return 0;
        }
        StringLen = strlen(String);
        StringDesc->bLength = (u8)(StringLen * 2 + 2);
        StringDesc->bDescriptorType = SDDV_STRING_DESC;
        for (i = 0; i < StringLen; i++) {
            StringDesc->wLANGID[i] = be2les((u16)String[i]);
        }
    }
    DescLen = StringDesc->bLength;
    if (DescLen > BufLen) {
        return 0;
    }
    memcpy(BufPtr, StringDesc, DescLen);
    return DescLen;
}

/* ------------------------------------------------------------------ */
/* BOS + MS OS 2.0 descriptor set (automatic WinUSB binding)           */
/* ------------------------------------------------------------------ */

/* MS OS 2.0 descriptor set:
 *   set header (10) + compatible ID "WINUSB" (20) +
 *   registry property DeviceInterfaceGUIDs (14 + 42 + 80 = 136)
 * total 166 bytes. */
#define SDDV_MSOS_SET_LEN 166U

static void sddv_utf16(u8 *dst, const char *src)
{
    while (*src) {
        *dst++ = (u8)*src++;
        *dst++ = 0x00;
    }
    *dst++ = 0x00;
    *dst = 0x00;
}

static u32 sddv_fill_msos_set(u8 *buf)
{
    u8 *p = buf;

    memset(buf, 0, SDDV_MSOS_SET_LEN);

    /* Set header */
    p[0] = 10; p[1] = 0;            /* wLength */
    p[2] = 0x00; p[3] = 0x00;       /* MS_OS_20_SET_HEADER_DESCRIPTOR */
    p[4] = 0x00; p[5] = 0x00; p[6] = 0x03; p[7] = 0x06; /* Win 8.1+ */
    p[8] = (u8)(SDDV_MSOS_SET_LEN & 0xFF);
    p[9] = (u8)(SDDV_MSOS_SET_LEN >> 8);
    p += 10;

    /* Compatible ID: WINUSB */
    p[0] = 20; p[1] = 0;            /* wLength */
    p[2] = 0x03; p[3] = 0x00;       /* MS_OS_20_FEATURE_COMPATIBLE_ID */
    memcpy(&p[4], "WINUSB\0\0", 8);
    /* sub-compatible ID stays zero */
    p += 20;

    /* Registry property: DeviceInterfaceGUIDs (REG_MULTI_SZ) */
    p[0] = 136; p[1] = 0;           /* wLength */
    p[2] = 0x04; p[3] = 0x00;       /* MS_OS_20_FEATURE_REG_PROPERTY */
    p[4] = 0x07; p[5] = 0x00;       /* REG_MULTI_SZ */
    p[6] = 42; p[7] = 0;            /* wPropertyNameLength */
    sddv_utf16(&p[8], "DeviceInterfaceGUIDs");
    p[8 + 42] = 80; p[9 + 42] = 0;  /* wPropertyDataLength */
    /* MULTI_SZ: GUID string + double NUL (second NUL from memset) */
    sddv_utf16(&p[8 + 42 + 2], "{F5A31C8E-7D3B-4E1C-9A64-52AA35C10B71}");

    return SDDV_MSOS_SET_LEN;
}

u32 XUsbPs_Ch9SetupBosDescReply_SddVendor(u8 *BufPtr, u32 BufLen)
{
    /* BOS header (5) + MS OS 2.0 platform capability (28) */
    static const u8 bos[33] = {
        /* BOS */
        0x05, 0x0F, 33, 0x00, 0x01,
        /* Platform capability, MS OS 2.0 UUID
         * D8DD60DF-4589-4CC7-9CD2-659D9E648A9F */
        0x1C, 0x10, 0x05, 0x00,
        0xDF, 0x60, 0xDD, 0xD8, 0x89, 0x45, 0xC7, 0x4C,
        0x9C, 0xD2, 0x65, 0x9D, 0x9E, 0x64, 0x8A, 0x9F,
        0x00, 0x00, 0x03, 0x06,          /* Windows 8.1+ */
        (u8)(SDDV_MSOS_SET_LEN & 0xFF),
        (u8)(SDDV_MSOS_SET_LEN >> 8),
        SDDV_MS_VENDOR_CODE,
        0x00                              /* bAltEnumCode */
    };

    if (!BufPtr || BufLen < sizeof(bos)) {
        return 0;
    }
    memcpy(BufPtr, bos, sizeof(bos));
    return sizeof(bos);
}

void XUsbPs_SetConfiguration_SddVendor(XUsbPs *InstancePtr, int ConfigIdx)
{
    int Status;

    Xil_AssertVoid(InstancePtr != NULL);

    if (ConfigIdx != 1) {
        return;
    }

    /* Identical sequence to the storage personality: EP1 bulk both
     * directions, flush + ring reinit for clean re-enumeration, prime
     * the OUT side. */
    XUsbPs_EpEnable(InstancePtr, SDDV_EP_DATA, XUSBPS_EP_DIRECTION_OUT);
    XUsbPs_EpEnable(InstancePtr, SDDV_EP_DATA, XUSBPS_EP_DIRECTION_IN);
    XUsbPs_SetBits(InstancePtr, XUSBPS_EPCR1_OFFSET,
                   XUSBPS_EPCR_TXT_BULK_MASK |
                   XUSBPS_EPCR_RXT_BULK_MASK |
                   XUSBPS_EPCR_TXR_MASK |
                   XUSBPS_EPCR_RXR_MASK);

    XUsbPs_EpFlush(InstancePtr, SDDV_EP_DATA, XUSBPS_EP_DIRECTION_IN);
    XUsbPs_EpFlush(InstancePtr, SDDV_EP_DATA, XUSBPS_EP_DIRECTION_OUT);
    (void)XUsbPs_ReconfigureEp(InstancePtr, &InstancePtr->DeviceConfig,
                               SDDV_EP_DATA, XUSBPS_EP_DIRECTION_IN, 0);
    (void)XUsbPs_ReconfigureEp(InstancePtr, &InstancePtr->DeviceConfig,
                               SDDV_EP_DATA, XUSBPS_EP_DIRECTION_OUT, 0);

    Status = XUsbPs_EpPrime(InstancePtr, SDDV_EP_DATA,
                            XUSBPS_EP_DIRECTION_OUT);
    usb_storage_debug_note_ep_prime(SDDV_EP_DATA,
                                    XUSBPS_EP_DIRECTION_OUT, Status);
}

/* ------------------------------------------------------------------ */
/* Vendor requests                                                     */
/* ------------------------------------------------------------------ */

int usb_sdd_vendor_req(XUsbPs *InstancePtr,
                       XUsbPs_SetupData *SetupData,
                       int *StatusOut)
{
    static u8 Reply[256] __attribute__((aligned(32)));
    u32 ReplyLen = 0;
    int Direction = SetupData->bmRequestType & (1 << 7);

    /* No logging here: this runs in the USB ISR, and a UART print blocks
     * interrupts for milliseconds per request. The g_sddv_* counters
     * (printed by `sdd status`) are the diagnostic surface. */

    if (Direction) {
        g_sddv_vendor_in_count++;
        memset(Reply, 0, sizeof(Reply));
        if ((SetupData->bRequest == SDDV_MS_VENDOR_CODE) &&
            (SetupData->wIndex == SDDV_MS_OS_20_DESC_SET)) {
            g_sddv_msos_reads++;
            ReplyLen = sddv_fill_msos_set(Reply);
        } else {
            ReplyLen = SetupData->wLength;
            if (ReplyLen > sizeof(Reply)) {
                ReplyLen = sizeof(Reply);
            }
        }
        if (ReplyLen > SetupData->wLength) {
            ReplyLen = SetupData->wLength;
        }
        *StatusOut = XUsbPs_EpBufferSend(InstancePtr, 0, Reply, ReplyLen);
        return 1;
    }

    g_sddv_vendor_out_count++;
    /* Vendor OUT: never wait for the data stage in the ISR (see the
     * FT601 postmortem); prime and let EP0 DATA_RX finish the ACK. */
    if (SetupData->wLength > 0) {
        g_sddv_vendor_out_ack_owed = 1U;
        XUsbPs_EpPrime(InstancePtr, 0, XUSBPS_EP_DIRECTION_OUT);
        *StatusOut = XST_SUCCESS;
        return 1;
    }
    *StatusOut = XUsbPs_EpBufferSend(InstancePtr, 0, NULL, 0);
    return 1;
}
