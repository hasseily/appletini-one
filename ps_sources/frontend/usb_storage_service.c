/******************************************************************************
* Copyright (C) 2010 - 2022 Xilinx, Inc.  All rights reserved.
* Copyright (C) 2023 - 2025 Advanced Micro Devices, Inc. All Rights Reserved.
* SPDX-License-Identifier: MIT
******************************************************************************/

/*****************************************************************************/
/**
* @file xusbps_intr_example.c
*
* This file contains an example of how to use the USB driver with the USB
* controller in DEVICE mode.
*
*
*<pre>
* MODIFICATION HISTORY:
*
* Ver   Who     Date     Changes
* ----- ------  -------- ----------------------------------------------------
* 1.00a wgr/nm  10/09/10 First release
* 1.01a nm      03/05/10 Included xpseudo_asm.h instead of xpseudo_asm_gcc.h
* 1.04a nm      02/05/13 Fixed CR# 696550.
*		         Added template code for Vendor request.
* 1.06a kpc		11/11/13 Fixed CR#759458, cacheInvalidate size should be
*				 ailgned to cache line size.
* 2.1   kpc    04/28/14 Cleanup and removed unused functions
* 2.4   vak    04/01/19 Fixed IAR data_alignment warnings
* 2.8   pm     07/07/23 Added support for system device-tree flow
* 2.10  ka     21/08/25 Fix GCC warnings
*</pre>
******************************************************************************/

/***************************** Include Files *********************************/

#include "xparameters.h"		/* XPAR parameters */
#include "xusbps.h"			/* USB controller driver */
#include "xusbps_endpoint.h"
#include "xusbps_hw.h"			/* USB controller register offsets */
#include "gic_init.h"
#include "usb_storage_service.h"
#include "usb_phy_init.h"
#include "xusbps_ch9.h"		/* Generic Chapter 9 handling code */
#include "xusbps_class_storage.h"	/* Storage class handling code */
#include "xil_exception.h"
#include "xpseudo_asm.h"
#include "xil_cache.h"
#include "xil_printf.h"
#include "sleep.h"
#include "xiltimer.h"
#include "usb_storage_backend.h"
#include <string.h>

#ifndef SDT
#include "xscugic.h"
#else
#include "xinterrupt_wrap.h"
#endif

/************************** Constant Definitions *****************************/
#define MEMORY_SIZE (64 * 1024)
#define USB_STORAGE_IRQ_MASK (XUSBPS_IXR_UR_MASK | XUSBPS_IXR_UI_MASK | \
			      XUSBPS_IXR_UE_MASK | XUSBPS_IXR_PC_MASK)
/* USB0 shares the SD card with SmartPort/Disk2/config through the same
 * disk/XSdPs layer. All SD I/O is main-loop context by construction (the
 * USB0 ISR never touches SD: READ/SYNC defer to the poll, the WRITE pipe
 * flushes from the poll), and usb_storage_sd.c enforces that with an
 * ISR/reentry guard (guard_rejects in `:usb` stats). Keep USB0 at the
 * default priority (0xA0, equal to SmartPort) anyway -- there is no reason
 * for it to preempt anyone. */
#define USB0_GIC_PRIORITY XINTERRUPT_DEFAULT_PRIORITY

/* Lazy SD re-attach cadence while the medium is not ready. A single
 * attach attempt against a missing card is quick (card-detect short
 * circuit), but rate-limit it so a cardless board doesn't stutter the
 * main loop. */
#define USB_STORAGE_REINIT_INTERVAL_TICKS (2U * COUNTS_PER_SECOND)
/* Windows reports eject failure if the device disconnects before it has
 * completed the START STOP UNIT CSW. Keep servicing USB briefly after the
 * class layer sees eject, then detach from the modal. */
#define USB_STORAGE_HOST_EJECT_GRACE_TICKS (COUNTS_PER_SECOND / 2U)
#ifndef XINTR_IS_LEVEL_TRIGGERED
#define XINTR_IS_LEVEL_TRIGGERED 0x03U
#endif
#ifdef __ICCARM__
#pragma data_alignment = 32
u8 Buffer[MEMORY_SIZE];
#else
u8 Buffer[MEMORY_SIZE] ALIGNMENT_CACHELINE;
#endif

/**************************** Type Definitions *******************************/

/***************** Macros (Inline Functions) Definitions *********************/
#ifdef SDT
#define USBPS_BASEADDR		XPS_USB0_BASEADDR /* USBPS base address */
#endif

/************************** Function Prototypes ******************************/
#ifndef SDT
static int UsbIntrExample(XScuGic *IntcInstancePtr, XUsbPs *UsbInstancePtr,
			  u16 UsbDeviceId, u16 UsbIntrId);
static void UsbDisableIntrSystem(XScuGic *IntcInstancePtr, u16 UsbIntrId);
static int UsbSetupIntrSystem(XScuGic *IntcInstancePtr,
			      XUsbPs *UsbInstancePtr, u16 UsbIntrId);
#else
static int UsbIntrExample(XUsbPs *UsbInstancePtr, UINTPTR BaseAddress);
static int UsbSetupSharedIntrSystem(XUsbPs *UsbInstancePtr, u32 UsbIntrId);
static void UsbDisableSharedIntrSystem(u32 UsbIntrId);
static u16 UsbDecodeGicIntrId(u32 UsbIntrId);
#endif

static void UsbIntrHandler(void *CallBackRef, u32 Mask);
static void XUsbPs_Ep0EventHandler(void *CallBackRef, u8 EpNum,
				   u8 EventType, void *Data);
static void XUsbPs_Ep1EventHandler(void *CallBackRef, u8 EpNum,
				   u8 EventType, void *Data);
static void UsbSoftDisconnect(XUsbPs *UsbInstancePtr);
static void UsbSoftConnect(XUsbPs *UsbInstancePtr);
static void UsbBuildDeviceConfig(XUsbPs_DeviceConfig *DeviceConfig);
static int UsbRegisterDeviceHandlers(XUsbPs *UsbInstancePtr);
static int UsbConfigureStorageDevice(XUsbPs *UsbInstancePtr,
				     uint32_t detached_delay_us);
static int UsbReconfigureAttachedDevice(void);
static int UsbQueueStorageReq(const u8 *BufferPtr, u32 BufferLen);
static int UsbProcessQueuedStorageReq(XUsbPs *UsbInstancePtr);
static void UsbResetStorageQueue(void);
static void UsbStorageSnapshotRegs(XUsbPs *UsbInstancePtr);
static void UsbStorageSnapshotEp1In(XUsbPs *UsbInstancePtr);
static void UsbSnapshotPhyAndUlpi(XUsbPs *UsbInstancePtr);
static void UsbNoteEpPrime(u8 EpNum, u8 Direction, int Status);
static void UsbResetLocalStorageState(void);
static void UsbResetEnumerationState(XUsbPs *UsbInstancePtr);
static void UsbRequestEp0Prime(void);
static void UsbRequestDeviceReconfigure(void);
static void UsbHandlePortChangeIrq(void);
void usb_storage_debug_note_setup(const XUsbPs_SetupData *SetupData);
void usb_storage_debug_note_ch9_error(const XUsbPs_SetupData *SetupData);
void usb_storage_debug_note_ep0_stall(void);
void usb_storage_debug_note_clear_feature_halt(u8 EpNum);
void usb_storage_debug_note_msc_reset(void);
void usb_storage_debug_note_set_address(u16 Address);
void usb_storage_debug_note_set_configuration(u16 Config);
void usb_storage_debug_note_descriptor(u8 DescType, u16 RequestLen,
				       u32 ReplyLen);
void usb_storage_debug_note_ep_send_failure(u8 EpNum, u32 BufferLen,
					    int Status);
void usb_storage_debug_note_ep_prime(u8 EpNum, u8 Direction, int Status);

/************************** Variable Definitions *****************************/

/* The instances to support the device drivers are global such that the
 * are initialized to zero each time the program runs.
 */
#ifndef SDT
static XScuGic IntcInstance;    /* The instance of the IRQ Controller */
#endif
static XUsbPs UsbInstance;	/* The instance of the USB Controller */
static XUsbPs_Local UsbLocalData;

static volatile int NumIrqs = 0;
static volatile int NeedEp0Prime = 0;
static volatile int NeedEndpointReset = 0;
static volatile int NeedFullReconfigure = 0;
static volatile uint8_t UsbConfiguredOnce = 0U;
static volatile uint8_t UsbSawPhysicalDisconnect = 0U;

/* Deferred-work ring: the EP1 OUT ISR cannot run polled SD I/O, so we
 * copy the packet out, release the dTD back to the hardware immediately
 * (the controller wires the dTDs as a circular all-active ring -- holding
 * any of them stalls the chain once HW wraps to it), and process the
 * copy later from usb_storage_service_poll. WRITE(10) packets can bypass
 * this ring when it is empty and the class state machine can consume them
 * without touching SD from the ISR. */
/* Two full maximum transfers plus margin. BOT is strictly sequential
 * (the host cannot issue CBW n+1 before CSW n), so steady-state depth is
 * bounded by one transfer (CBW + USB_STORAGE_MAX_TRANSFER_BLOCKS data
 * packets); the second transfer of headroom absorbs reset/retry overlap
 * so a drop -- which kills the BOT exchange -- is structurally
 * unreachable. */
#define USB_STORAGE_REQ_QUEUE_DEPTH	((2U * USB_STORAGE_MAX_TRANSFER_BLOCKS) + 16U)
#define USB_STORAGE_REQ_MAX_BYTES	512U

typedef struct {
	u32 BufferLen;
	u8 Data[USB_STORAGE_REQ_MAX_BYTES];
} UsbStorageReq;

static UsbStorageReq StorageReqQueue[USB_STORAGE_REQ_QUEUE_DEPTH];
static volatile u32 StorageReqHead = 0;
static volatile u32 StorageReqTail = 0;
static int UsbStorageStarted = 0;
static int UsbConnected = 0;
static uint8_t UsbLocallyDetached = 0U;
static XTime LastReinitTick = 0;
static uint8_t HostEjectPendingDetach = 0U;
static XTime HostEjectRequestTick = 0;
static usb_storage_service_stats_t StorageStats;

static void UsbStorageSnapshotRegs(XUsbPs *UsbInstancePtr)
{
	u32 BaseAddress;
	XUsbPs_Local *UsbLocalPtr;

	if (UsbInstancePtr == NULL) {
		return;
	}

	BaseAddress = UsbInstancePtr->Config.BaseAddress;
	if (BaseAddress == 0U) {
		return;
	}

	UsbLocalPtr = (XUsbPs_Local *)UsbInstancePtr->UserDataPtr;
	if (UsbLocalPtr != NULL) {
		StorageStats.current_config = UsbLocalPtr->CurrentConfig;
	}
	StorageStats.need_ep0_prime = (NeedEp0Prime != 0) ? 1U : 0U;
	StorageStats.last_usb_cmd = XUsbPs_ReadReg(BaseAddress,
						   XUSBPS_CMD_OFFSET);
	StorageStats.last_usb_sts = XUsbPs_ReadReg(BaseAddress,
						   XUSBPS_ISR_OFFSET);
	StorageStats.last_usb_intr = XUsbPs_ReadReg(BaseAddress,
						    XUSBPS_IER_OFFSET);
	StorageStats.last_deviceaddr = XUsbPs_ReadReg(BaseAddress,
						      XUSBPS_DEVICEADDR_OFFSET);
	StorageStats.last_eplistaddr = XUsbPs_ReadReg(BaseAddress,
						      XUSBPS_EPLISTADDR_OFFSET);
	StorageStats.last_usb_mode = XUsbPs_ReadReg(BaseAddress,
						    XUSBPS_MODE_OFFSET);
	StorageStats.last_portsc = XUsbPs_ReadReg(BaseAddress,
						  XUSBPS_PORTSCR1_OFFSET);
	StorageStats.last_otgsc = XUsbPs_ReadReg(BaseAddress,
						 XUSBPS_OTGCSR_OFFSET);
	StorageStats.last_epstat = XUsbPs_ReadReg(BaseAddress,
						  XUSBPS_EPSTAT_OFFSET);
	StorageStats.last_epprime = XUsbPs_ReadReg(BaseAddress,
						   XUSBPS_EPPRIME_OFFSET);
	StorageStats.last_eprdy = XUsbPs_ReadReg(BaseAddress,
						 XUSBPS_EPRDY_OFFSET);
	StorageStats.last_epcomplete = XUsbPs_ReadReg(BaseAddress,
						      XUSBPS_EPCOMPL_OFFSET);
	StorageStats.last_epcr0 = XUsbPs_ReadReg(BaseAddress,
						 XUSBPS_EPCR0_OFFSET);
	StorageStats.last_epcr1 = XUsbPs_ReadReg(BaseAddress,
						 XUSBPS_EPCR1_OFFSET);
	UsbStorageSnapshotEp1In(UsbInstancePtr);
}

static void UsbStorageSnapshotEp1In(XUsbPs *UsbInstancePtr)
{
	XUsbPs_EpIn *EpIn;

	if ((UsbInstancePtr == NULL) ||
	    (UsbInstancePtr->DeviceConfig.NumEndpoints <= 1U)) {
		return;
	}

	EpIn = &UsbInstancePtr->DeviceConfig.Ep[1].In;
	StorageStats.ep1in_dqh = (u32)EpIn->dQH;
	StorageStats.ep1in_dtds = (u32)EpIn->dTDs;
	StorageStats.ep1in_head = (u32)EpIn->dTDHead;
	StorageStats.ep1in_tail = (u32)EpIn->dTDTail;
	StorageStats.ep1in_requested_bytes = EpIn->RequestedBytes;
	StorageStats.ep1in_bytes_txed = EpIn->BytesTxed;
	StorageStats.ep1in_buffer_ptr = (u32)EpIn->BufferPtr;

	if (EpIn->dQH != NULL) {
		XUsbPs_dQHInvalidateCache(EpIn->dQH);
		StorageStats.ep1in_dqh_cfg =
			XUsbPs_ReaddQH(EpIn->dQH, XUSBPS_dQHCFG);
		StorageStats.ep1in_dqh_cptr =
			XUsbPs_ReaddQH(EpIn->dQH, XUSBPS_dQHCPTR);
		StorageStats.ep1in_dqh_next =
			XUsbPs_ReaddQH(EpIn->dQH, XUSBPS_dQHdTDNLP);
		StorageStats.ep1in_dqh_token =
			XUsbPs_ReaddQH(EpIn->dQH, XUSBPS_dQHdTDTOKEN);
	}
	if (EpIn->dTDHead != NULL) {
		XUsbPs_dTDInvalidateCache(EpIn->dTDHead);
		StorageStats.ep1in_head_next =
			XUsbPs_ReaddTD(EpIn->dTDHead, XUSBPS_dTDNLP);
		StorageStats.ep1in_head_token =
			XUsbPs_ReaddTD(EpIn->dTDHead, XUSBPS_dTDTOKEN);
		StorageStats.ep1in_head_buf =
			XUsbPs_ReaddTD(EpIn->dTDHead, XUSBPS_dTDBPTR0);
	}
	if (EpIn->dTDTail != NULL) {
		XUsbPs_dTDInvalidateCache(EpIn->dTDTail);
		StorageStats.ep1in_tail_next =
			XUsbPs_ReaddTD(EpIn->dTDTail, XUSBPS_dTDNLP);
		StorageStats.ep1in_tail_token =
			XUsbPs_ReaddTD(EpIn->dTDTail, XUSBPS_dTDTOKEN);
		StorageStats.ep1in_tail_buf =
			XUsbPs_ReaddTD(EpIn->dTDTail, XUSBPS_dTDBPTR0);
	}
}

static void UsbSnapshotPhyAndUlpi(XUsbPs *UsbInstancePtr)
{
	usb_phy_status_t Phy;
	u32 i;

	usb_phy_get_status(&Phy);
	StorageStats.last_slcr_usb0_clk_ctrl = Phy.usb0_clk_ctrl;
	StorageStats.last_slcr_usb1_clk_ctrl = Phy.usb1_clk_ctrl;
	StorageStats.last_slcr_usb_rst_ctrl = Phy.usb_rst_ctrl;
	for (i = 0U; i < 12U; ++i) {
		StorageStats.last_usb0_mio[i] = Phy.usb0_mio[i];
	}

	StorageStats.last_ulpi_view = 0U;
	if ((UsbInstancePtr != NULL) &&
	    (UsbInstancePtr->Config.BaseAddress != 0U)) {
		StorageStats.last_ulpi_view =
			XUsbPs_ReadReg(UsbInstancePtr->Config.BaseAddress,
				       XUSBPS_ULPIVIEW_OFFSET);
	}
}

static void UsbNoteEpPrime(u8 EpNum, u8 Direction, int Status)
{
	StorageStats.last_prime_ep = EpNum;
	StorageStats.last_prime_dir = Direction;
	StorageStats.last_prime_status = (u32)Status;
	if (EpNum == 0U) {
		StorageStats.ep0_prime_count++;
		if (Status != XST_SUCCESS) {
			StorageStats.ep0_prime_failures++;
		}
	} else if (EpNum == 1U) {
		StorageStats.ep1_prime_count++;
		if (Status != XST_SUCCESS) {
			StorageStats.ep1_prime_failures++;
		}
	}
}

/* IRQ-guarded endpoint prime. XUsbPs_EpPrime does a read-modify-write on
 * ENDPTPRIME with no internal locking. The EP0 re-prime below runs from the
 * main-loop poll context while the USB ISR can also issue primes, so a
 * preempting interrupt can drop the prime. Mask IRQ around it; the restore
 * only re-enables when IRQ was enabled on entry (safe from ISR context). */
static int UsbGuardedEpPrime(XUsbPs *InstancePtr, u8 EpNum, u8 Direction)
{
	u32 Cpsr = mfcpsr();
	int Status;

	Xil_ExceptionDisableMask(XIL_EXCEPTION_IRQ);
	Status = XUsbPs_EpPrime(InstancePtr, EpNum, Direction);
	if ((Cpsr & XIL_EXCEPTION_IRQ) == 0U) {
		Xil_ExceptionEnableMask(XIL_EXCEPTION_IRQ);
	}
	return Status;
}

static u32 UsbStorageQueueDepth(u32 Head, u32 Tail)
{
	if (Tail >= Head) {
		return Tail - Head;
	}
	return (USB_STORAGE_REQ_QUEUE_DEPTH - Head) + Tail;
}

static int UsbStorageQueueEmpty(void)
{
	return (StorageReqHead == StorageReqTail) ? 1 : 0;
}

static void UsbNoteStorageOut(u32 BufferLen)
{
	StorageStats.out_packets++;
	StorageStats.out_bytes += BufferLen;
}


int usb_storage_service_init(void)
{
	int Status;
	u32 BlockCount;

	if (UsbStorageStarted != 0) {
		return XST_SUCCESS;
	}

	xil_printf("USB mass-storage service: %s on USB0\r\n",
		   usb_storage_backend_name());

	/* Mark the SD->USB read staging buffer non-cacheable before any
	 * transfer can use it (the SD ADMA2 engine writes it and the USB DMA
	 * reads it; a cached buffer wedges the EP1 IN transfer). */
	XUsbPs_StorageInitReadBuffer();

	usb_phy_early_init();

	/* Start USB storage even without usable media. The host sees
	 * medium-not-present while the poll loop retries SD attachment, including
	 * after hot insertion. */
	Status = usb_storage_init();
	if (Status != XST_SUCCESS) {
		xil_printf("Storage backend init failed: %d (medium not "
			   "present; will keep retrying)\r\n", Status);
	} else {
		BlockCount = usb_storage_block_count();
		xil_printf("Storage capacity: blocks=%u (0x%08x) MB=%u\r\n",
			   (unsigned)BlockCount,
			   (unsigned)BlockCount,
			   (unsigned)(BlockCount / 2048U));
	}
	xil_printf("USB0 SD remote mount is available from the USB tab.\r\n");

#ifndef SDT
	Status = UsbIntrExample(&IntcInstance, &UsbInstance,
				XPAR_XUSBPS_0_DEVICE_ID, XPAR_XUSBPS_0_INTR);
#else
	Status = UsbIntrExample(&UsbInstance, USBPS_BASEADDR);
#endif
	if (Status != XST_SUCCESS) {
		return XST_FAILURE;
	}

	UsbStorageStarted = 1;
	StorageStats.starts++;
	UsbStorageSnapshotRegs(&UsbInstance);
	UsbSnapshotPhyAndUlpi(&UsbInstance);
	return XST_SUCCESS;
}

/* Attach to the host (Run/Stop + pullup). Called while the config menu's
 * USB0 SD-card remote-mount modal is active. */
void usb_storage_service_connect(void)
{
	int Status;

	if ((UsbStorageStarted == 0) || (UsbConnected != 0)) {
		return;
	}

	/* Replay the same controller reset/configure sequence used at boot.
	 * The XUsbPs endpoint rings and handler tables are driver-owned DMA
	 * state, and a detached/remounted host can leave them stale. */
	Status = UsbConfigureStorageDevice(&UsbInstance, 250000U);
	if (Status != XST_SUCCESS) {
		xil_printf("USB0 mass-storage reconnect configure failed: %d\r\n",
			   Status);
		return;
	}
	NeedFullReconfigure = 0;
	NeedEndpointReset = 0;
	UsbLocallyDetached = 0U;
	UsbSawPhysicalDisconnect = 0U;
	NeedEp0Prime = 0;
	StorageStats.need_ep0_prime = 0U;
	HostEjectPendingDetach = 0U;
	HostEjectRequestTick = 0;
	XUsbPs_StorageClearHostEjectRequested();

	/* Enable the interrupts. */
	XUsbPs_IntrEnable(&UsbInstance, USB_STORAGE_IRQ_MASK);

	/* Start the USB engine and present the device to the host. */
	XUsbPs_Start(&UsbInstance);
	UsbSoftConnect(&UsbInstance);
	Status = UsbGuardedEpPrime(&UsbInstance, 0, XUSBPS_EP_DIRECTION_OUT);
	UsbNoteEpPrime(0, XUSBPS_EP_DIRECTION_OUT, Status);

	UsbConnected = 1;
	xil_printf("USB0 mass-storage device attached\r\n");
}

uint8_t usb_storage_service_disconnect(void)
{
	u32 budget;

	if ((UsbStorageStarted == 0) || (UsbConnected == 0)) {
		return 0U;
	}

	budget = USB_STORAGE_REQ_QUEUE_DEPTH;
	while ((budget != 0U) && UsbProcessQueuedStorageReq(&UsbInstance)) {
		XUsbPs_StoragePoll();
		budget--;
	}
	(void)XUsbPs_StorageFlushPending();
	XUsbPs_StoragePoll();

	UsbLocallyDetached = 1U;
	UsbSoftDisconnect(&UsbInstance);
	usleep(250000);
	UsbConfiguredOnce = 0U;
	UsbSawPhysicalDisconnect = 0U;
	NeedFullReconfigure = 0;
	NeedEndpointReset = 0;
	NeedEp0Prime = 0;
	StorageStats.need_ep0_prime = 0U;
	HostEjectPendingDetach = 0U;
	HostEjectRequestTick = 0;
	XUsbPs_StorageClearHostEjectRequested();
	UsbConnected = 0;
	xil_printf("USB0 mass-storage device detached\r\n");
	return 1U;
}

void usb_storage_service_poll(void)
{
	u32 budget;

	if (UsbStorageStarted == 0) {
		return;
	}

	if (NeedFullReconfigure != 0) {
		(void)UsbReconfigureAttachedDevice();
	}

	/* Drain the request queue each poll so packet handling does not inherit
	 * main-loop latency. SD writes remain batched per CBW.
	 *
	 * Keep the ring large enough for at least one maximum WRITE(10):
	 * one CBW plus USB_STORAGE_MAX_TRANSFER_BLOCKS 512-byte OUT packets.
	 * Dropping a packet is fatal because the BOT DATA phase then waits
	 * forever for bytes the host already sent. */
	budget = USB_STORAGE_REQ_QUEUE_DEPTH;
	while ((budget != 0U) && UsbProcessQueuedStorageReq(&UsbInstance)) {
		XUsbPs_StoragePoll();
		budget--;
	}
	XUsbPs_StoragePoll();

	/* Heal a stranded EP1 IN prime (ENDPTPRIME vs ATDTW race backstop)
	 * and re-send any CSW the host is still owed after a send failure
	 * or ring recovery — BOT dies without its CSW. */
	XUsbPs_StoragePrimeWatchdog(&UsbInstance);
	XUsbPs_StorageRetryCsw(&UsbInstance);

	/* Lazy medium recovery: while the SD card is not usable the host
	 * sees medium-not-present (TEST UNIT READY fails with NOT READY
	 * sense); keep attempting a quick re-attach so the disk mounts as
	 * soon as the card is available. */
	if (usb_storage_medium_ready() == 0) {
		XTime Now;

		XTime_GetTime(&Now);
		if ((LastReinitTick == 0) ||
		    ((Now - LastReinitTick) >=
		     USB_STORAGE_REINIT_INTERVAL_TICKS)) {
			LastReinitTick = Now;
			if (usb_storage_try_reinit() == XST_SUCCESS) {
				u32 BlockCount = usb_storage_block_count();

				xil_printf("USB storage: medium attached, "
					   "blocks=%u MB=%u\r\n",
					   (unsigned)BlockCount,
					   (unsigned)(BlockCount / 2048U));
			}
		}
	}

	if (NeedEp0Prime) {
		int Status;

		if (NeedEndpointReset != 0) {
			UsbResetEnumerationState(&UsbInstance);
			NeedEndpointReset = 0;
		}
		XUsbPs_Start(&UsbInstance);
		Status = UsbGuardedEpPrime(&UsbInstance, 0,
					   XUSBPS_EP_DIRECTION_OUT);
		UsbNoteEpPrime(0, XUSBPS_EP_DIRECTION_OUT, Status);
		NeedEp0Prime = 0;
		StorageStats.need_ep0_prime = 0U;
	}
}

int usb_storage_service_needs_attention(void)
{
	if (UsbStorageStarted == 0) {
		return 0;
	}
	if (NeedEp0Prime != 0) {
		return 1;
	}
	if (!UsbStorageQueueEmpty()) {
		return 1;
	}
	return XUsbPs_StorageHasPending();
}

uint8_t usb_storage_service_consume_host_eject_request(void)
{
	XTime Now;

	if (XUsbPs_StorageHostEjectRequested() == 0) {
		HostEjectPendingDetach = 0U;
		HostEjectRequestTick = 0;
		return 0U;
	}
	XTime_GetTime(&Now);
	if (HostEjectPendingDetach == 0U) {
		HostEjectPendingDetach = 1U;
		HostEjectRequestTick = Now;
		return 0U;
	}
	if ((Now - HostEjectRequestTick) < USB_STORAGE_HOST_EJECT_GRACE_TICKS) {
		return 0U;
	}
	XUsbPs_StorageClearHostEjectRequested();
	HostEjectPendingDetach = 0U;
	HostEjectRequestTick = 0;
	return 1U;
}

/*****************************************************************************/
/**
 *
 * This function does a minimal DEVICE mode setup on the USB device and driver
 * as a design example. The purpose of this function is to illustrate how to
 * set up a USB flash disk emulation system.
 *
 *
 * @param	IntcInstancePtr is a pointer to the instance of the INTC driver.
 * @param	UsbInstancePtr is a pointer to the instance of USB driver.
 * @param	UsbDeviceId is the Device ID of the USB Controller and is the
 * 		XPAR_<USB_instance>_DEVICE_ID value from xparameters.h.
 * @param	UsbIntrId is the Interrupt Id and is typically
 * 		XPAR_<INTC_instance>_<USB_instance>_IP2INTC_IRPT_INTR value
 * 		from xparameters.h.
 *
 * @return
 * 		- XST_SUCCESS if successful
 * 		- XST_FAILURE on error
 *
 ******************************************************************************/
#ifndef SDT
static int UsbIntrExample(XScuGic *IntcInstancePtr, XUsbPs *UsbInstancePtr,
			  u16 UsbDeviceId, u16 UsbIntrId)
#else
static int UsbIntrExample(XUsbPs *UsbInstancePtr, UINTPTR BaseAddress)
#endif
{
	int	Status;
	int	ReturnStatus = XST_FAILURE;
	XUsbPs_Config		*UsbConfigPtr;

	/* Initialize the USB driver so that it's ready to use,
	 * specify the controller ID that is generated in xparameters.h
	 */
#ifndef SDT
	UsbConfigPtr = XUsbPs_LookupConfig(UsbDeviceId);
#else
	UsbConfigPtr = XUsbPs_LookupConfig(BaseAddress);
#endif
	if (NULL == UsbConfigPtr) {
		goto out;
	}


	/* We are passing the physical base address as the third argument
	 * because the physical and virtual base address are the same in our
	 * example.  For systems that support virtual memory, the third
	 * argument needs to be the virtual base address.
	 */
	Status = XUsbPs_CfgInitialize(UsbInstancePtr,
				      UsbConfigPtr,
				      UsbConfigPtr->BaseAddress);
	if (XST_SUCCESS != Status) {
		goto out;
	}

	/* Set up the interrupt subsystem.
	 */
#ifndef SDT
	Status = UsbSetupIntrSystem(IntcInstancePtr,
				    UsbInstancePtr,
				    UsbIntrId);
#else
	Status = UsbSetupSharedIntrSystem(UsbInstancePtr, UsbConfigPtr->IntrId);
#endif
	if (XST_SUCCESS != Status) {
		goto out;
	}

	Status = UsbConfigureStorageDevice(UsbInstancePtr, 250000U);
	if (XST_SUCCESS != Status) {
		goto out;
	}

	/* Controller, endpoints, handlers and IRQ routing are configured,
	 * but the device stays DETACHED (no Run/Stop, no pullup): attaching
	 * is deferred to usb_storage_service_connect(), called only while the
	 * USB tab's SD-card remote-mount modal is active. Attaching during
	 * normal boot/init lets the host enumerate and issue commands while
	 * the firmware is busy with Apple-side services. */
	xil_printf("USB0 mass-storage device configured (attach deferred)\r\n");
	ReturnStatus = XST_SUCCESS;

out:
	if (ReturnStatus == XST_SUCCESS) {
		return ReturnStatus;
	}

	/* Clean up. It's always safe to disable interrupts and clear the
	 * handlers, even if they have not been enabled/set. The same is true
	 * for disabling the interrupt subsystem.
	 */
	XUsbPs_Stop(UsbInstancePtr);
	XUsbPs_IntrDisable(UsbInstancePtr, XUSBPS_IXR_ALL);
#ifndef SDT
	UsbDisableIntrSystem(IntcInstancePtr, UsbIntrId);
#else
	UsbDisableSharedIntrSystem(UsbConfigPtr->IntrId);
#endif
	(int) XUsbPs_IntrSetHandler(UsbInstancePtr, NULL, NULL, 0);

	return ReturnStatus;
}

static void UsbBuildDeviceConfig(XUsbPs_DeviceConfig *DeviceConfig)
{
	/* Endpoint 0 is the default control endpoint. Endpoint 1 is the BOT
	 * bulk pair used by the mass-storage class. */
	memset(DeviceConfig, 0, sizeof(*DeviceConfig));

	DeviceConfig->EpCfg[0].Out.Type		= XUSBPS_EP_TYPE_CONTROL;
	DeviceConfig->EpCfg[0].Out.NumBufs	= 2;
	DeviceConfig->EpCfg[0].Out.BufSize	= 64;
	DeviceConfig->EpCfg[0].Out.MaxPacketSize = 64;
	DeviceConfig->EpCfg[0].In.Type		= XUSBPS_EP_TYPE_CONTROL;
	DeviceConfig->EpCfg[0].In.NumBufs	= 2;
	DeviceConfig->EpCfg[0].In.MaxPacketSize	= 64;

	DeviceConfig->EpCfg[1].Out.Type		= XUSBPS_EP_TYPE_BULK;
	DeviceConfig->EpCfg[1].Out.NumBufs	= 16;
	DeviceConfig->EpCfg[1].Out.BufSize	= 512;
	DeviceConfig->EpCfg[1].Out.MaxPacketSize = 512;
	DeviceConfig->EpCfg[1].In.Type		= XUSBPS_EP_TYPE_BULK;
	DeviceConfig->EpCfg[1].In.NumBufs	= 16;
	DeviceConfig->EpCfg[1].In.MaxPacketSize	= 512;

	DeviceConfig->NumEndpoints = 2;
}

static int UsbRegisterDeviceHandlers(XUsbPs *UsbInstancePtr)
{
	int Status;

	Status = XUsbPs_IntrSetHandler(UsbInstancePtr, UsbIntrHandler,
				       UsbInstancePtr, USB_STORAGE_IRQ_MASK);
	if (Status != XST_SUCCESS) {
		return Status;
	}

	Status = XUsbPs_EpSetHandler(UsbInstancePtr, 0,
				     XUSBPS_EP_DIRECTION_OUT,
				     XUsbPs_Ep0EventHandler, UsbInstancePtr);
	if (Status != XST_SUCCESS) {
		return Status;
	}

	Status = XUsbPs_EpSetHandler(UsbInstancePtr, 1,
				     XUSBPS_EP_DIRECTION_OUT,
				     XUsbPs_Ep1EventHandler, UsbInstancePtr);
	if (Status != XST_SUCCESS) {
		return Status;
	}

	return XUsbPs_EpSetHandler(UsbInstancePtr, 1,
				   XUSBPS_EP_DIRECTION_IN,
				   XUsbPs_Ep1EventHandler, UsbInstancePtr);
}

static int UsbConfigureStorageDevice(XUsbPs *UsbInstancePtr,
				     uint32_t detached_delay_us)
{
	int Status;
	u8 *MemPtr;
	XUsbPs_DeviceConfig DeviceConfig;

	if (UsbInstancePtr == NULL) {
		return XST_FAILURE;
	}

	XUsbPs_IntrDisable(UsbInstancePtr, XUSBPS_IXR_ALL);

	UsbBuildDeviceConfig(&DeviceConfig);
	MemPtr = (u8 *)&Buffer[0];
	memset(MemPtr, 0, MEMORY_SIZE);
	Xil_DCacheFlushRange((unsigned int)MemPtr, MEMORY_SIZE);
	DeviceConfig.DMAMemPhys = (u32)MemPtr;

	memset(&UsbLocalData, 0, sizeof(UsbLocalData));
	UsbInstancePtr->UserDataPtr = &UsbLocalData;
	UsbInstancePtr->CurrentAltSetting = XUSBPS_DEFAULT_ALT_SETTING;
	UsbInstancePtr->IsConfigDone = 0U;
	UsbConfiguredOnce = 0U;

	Status = XUsbPs_ConfigureDevice(UsbInstancePtr, &DeviceConfig);
	if (Status != XST_SUCCESS) {
		return Status;
	}

	UsbResetLocalStorageState();
	UsbSoftDisconnect(UsbInstancePtr);
	if (detached_delay_us != 0U) {
		usleep(detached_delay_us);
	}

	return UsbRegisterDeviceHandlers(UsbInstancePtr);
}

static int UsbReconfigureAttachedDevice(void)
{
	int Status;
	int PrimeStatus;

	if ((UsbConnected == 0) || (UsbLocallyDetached != 0U)) {
		NeedFullReconfigure = 0;
		return XST_SUCCESS;
	}

	XUsbPs_IntrDisable(&UsbInstance, XUSBPS_IXR_ALL);
	UsbSoftDisconnect(&UsbInstance);
	usleep(250000);

	Status = UsbConfigureStorageDevice(&UsbInstance, 0U);
	if (Status != XST_SUCCESS) {
		NeedFullReconfigure = 0;
		NeedEndpointReset = 1;
		NeedEp0Prime = 1;
		StorageStats.need_ep0_prime = 1U;
		XUsbPs_IntrEnable(&UsbInstance, USB_STORAGE_IRQ_MASK);
		XUsbPs_Start(&UsbInstance);
		xil_printf("USB0 mass-storage reconfigure failed: %d\r\n",
			   Status);
		return Status;
	}

	NeedFullReconfigure = 0;
	NeedEndpointReset = 0;
	NeedEp0Prime = 0;
	StorageStats.need_ep0_prime = 0U;
	UsbLocallyDetached = 0U;
	UsbSawPhysicalDisconnect = 0U;

	XUsbPs_IntrEnable(&UsbInstance, USB_STORAGE_IRQ_MASK);
	XUsbPs_Start(&UsbInstance);
	UsbSoftConnect(&UsbInstance);
	PrimeStatus = UsbGuardedEpPrime(&UsbInstance, 0,
					XUSBPS_EP_DIRECTION_OUT);
	UsbNoteEpPrime(0, XUSBPS_EP_DIRECTION_OUT, PrimeStatus);
	xil_printf("USB0 mass-storage device reattached\r\n");
	return XST_SUCCESS;
}

void *usb_storage_service_usb_instance(void)
{
	return &UsbInstance;
}

static void UsbSoftDisconnect(XUsbPs *UsbInstancePtr)
{
	u32 BaseAddress = UsbInstancePtr->Config.BaseAddress;
	u32 Otg;

	XUsbPs_WriteReg(BaseAddress, XUSBPS_CMD_OFFSET, 0);
	Otg = XUsbPs_ReadReg(BaseAddress, XUSBPS_OTGCSR_OFFSET);
	XUsbPs_WriteReg(BaseAddress, XUSBPS_OTGCSR_OFFSET,
			Otg & ~(XUSBPS_OTGSC_OT_MASK | XUSBPS_OTGSC_DP_MASK));
	XUsbPs_WriteReg(BaseAddress, XUSBPS_ISR_OFFSET, XUSBPS_IXR_ALL);
	StorageStats.soft_disconnects++;
}

static void UsbSoftConnect(XUsbPs *UsbInstancePtr)
{
	XUsbPs_SetBits(UsbInstancePtr, XUSBPS_OTGCSR_OFFSET,
		       XUSBPS_OTGSC_OT_MASK);
	StorageStats.soft_connects++;
}

static int UsbQueueStorageReq(const u8 *BufferPtr, u32 BufferLen)
{
	u32 Tail = StorageReqTail;
	u32 NextTail = (Tail + 1U) % USB_STORAGE_REQ_QUEUE_DEPTH;
	UsbStorageReq *Req;
	u32 Depth;

	if ((BufferLen > USB_STORAGE_REQ_MAX_BYTES) ||
	    (NextTail == StorageReqHead)) {
		StorageStats.queue_drops++;
		return XST_FAILURE;
	}

	Req = &StorageReqQueue[Tail];
	Req->BufferLen = BufferLen;
	if (BufferLen != 0U) {
		memcpy(Req->Data, BufferPtr, BufferLen);
	}

	StorageReqTail = NextTail;
	StorageStats.queued_packets++;
	StorageStats.queued_bytes += BufferLen;
	Depth = UsbStorageQueueDepth(StorageReqHead, NextTail);
	StorageStats.last_queue_depth = Depth;
	if (Depth > StorageStats.queue_high_water) {
		StorageStats.queue_high_water = Depth;
	}
	return XST_SUCCESS;
}

static int UsbProcessQueuedStorageReq(XUsbPs *UsbInstancePtr)
{
	u32 Head = StorageReqHead;
	UsbStorageReq *Req;

	if (Head == StorageReqTail) {
		return 0;
	}

	Req = &StorageReqQueue[Head];
	XUsbPs_HandleStorageReq(UsbInstancePtr, 1, Req->Data, Req->BufferLen);
	StorageReqHead = (Head + 1U) % USB_STORAGE_REQ_QUEUE_DEPTH;
	StorageStats.processed_packets++;
	StorageStats.processed_bytes += Req->BufferLen;
	StorageStats.last_queue_depth =
		UsbStorageQueueDepth(StorageReqHead, StorageReqTail);
	return 1;
}

static void UsbResetStorageQueue(void)
{
	StorageReqHead = 0;
	StorageReqTail = 0;
	StorageStats.last_queue_depth = 0;
}

static void UsbResetLocalStorageState(void)
{
	UsbResetStorageQueue();
	XUsbPs_StorageReset();
}

static void UsbResetEnumerationState(XUsbPs *UsbInstancePtr)
{
	u32 BaseAddress;
	XUsbPs_Local *UsbLocalPtr;

	if (UsbInstancePtr == NULL) {
		return;
	}

	UsbResetLocalStorageState();
	UsbLocalPtr = (XUsbPs_Local *)UsbInstancePtr->UserDataPtr;
	if (UsbLocalPtr != NULL) {
		UsbLocalPtr->CurrentConfig = 0U;
	}
	UsbInstancePtr->CurrentAltSetting = XUSBPS_DEFAULT_ALT_SETTING;
	UsbInstancePtr->IsConfigDone = 0U;
	UsbConfiguredOnce = 0U;

	BaseAddress = UsbInstancePtr->Config.BaseAddress;
	if (BaseAddress == 0U) {
		return;
	}

	XUsbPs_WriteReg(BaseAddress, XUSBPS_DEVICEADDR_OFFSET, 0U);
	XUsbPs_WriteReg(BaseAddress, XUSBPS_EPSTAT_OFFSET,
			XUsbPs_ReadReg(BaseAddress, XUSBPS_EPSTAT_OFFSET));
	XUsbPs_WriteReg(BaseAddress, XUSBPS_EPCOMPL_OFFSET,
			XUsbPs_ReadReg(BaseAddress, XUSBPS_EPCOMPL_OFFSET));
	XUsbPs_WriteReg(BaseAddress, XUSBPS_EPFLUSH_OFFSET, XUSBPS_EP_ALL_MASK);

	XUsbPs_EpFlush(UsbInstancePtr, 0, XUSBPS_EP_DIRECTION_OUT);
	XUsbPs_EpFlush(UsbInstancePtr, 0, XUSBPS_EP_DIRECTION_IN);
	XUsbPs_ClrBits(UsbInstancePtr, XUSBPS_EPCR0_OFFSET,
		       XUSBPS_EPCR_RXS_MASK | XUSBPS_EPCR_TXS_MASK);
	XUsbPs_SetBits(UsbInstancePtr, XUSBPS_EPCR0_OFFSET,
		       XUSBPS_EPCR_RXR_MASK | XUSBPS_EPCR_TXR_MASK);
	(void)XUsbPs_ReconfigureEp(UsbInstancePtr,
				   &UsbInstancePtr->DeviceConfig, 0,
				   XUSBPS_EP_DIRECTION_OUT, 0);
	(void)XUsbPs_ReconfigureEp(UsbInstancePtr,
				   &UsbInstancePtr->DeviceConfig, 0,
				   XUSBPS_EP_DIRECTION_IN, 0);

	if (UsbInstancePtr->DeviceConfig.NumEndpoints > 1U) {
		XUsbPs_EpDisable(UsbInstancePtr, 1,
				 XUSBPS_EP_DIRECTION_OUT |
				 XUSBPS_EP_DIRECTION_IN);
		XUsbPs_EpFlush(UsbInstancePtr, 1, XUSBPS_EP_DIRECTION_OUT);
		XUsbPs_EpFlush(UsbInstancePtr, 1, XUSBPS_EP_DIRECTION_IN);
		XUsbPs_ClrBits(UsbInstancePtr, XUSBPS_EPCR1_OFFSET,
			       XUSBPS_EPCR_RXS_MASK | XUSBPS_EPCR_TXS_MASK);
		XUsbPs_SetBits(UsbInstancePtr, XUSBPS_EPCR1_OFFSET,
			       XUSBPS_EPCR_RXR_MASK | XUSBPS_EPCR_TXR_MASK);
		(void)XUsbPs_ReconfigureEp(UsbInstancePtr,
					   &UsbInstancePtr->DeviceConfig, 1,
					   XUSBPS_EP_DIRECTION_OUT, 0);
		(void)XUsbPs_ReconfigureEp(UsbInstancePtr,
					   &UsbInstancePtr->DeviceConfig, 1,
					   XUSBPS_EP_DIRECTION_IN, 0);
	}
}

static void UsbRequestEp0Prime(void)
{
	if (UsbLocallyDetached != 0U) {
		return;
	}
	NeedEndpointReset = 1;
	NeedEp0Prime = 1;
	StorageStats.need_ep0_prime = 1U;
}

static void UsbRequestDeviceReconfigure(void)
{
	if (UsbLocallyDetached != 0U) {
		return;
	}
	NeedFullReconfigure = 1;
	NeedEndpointReset = 0;
	NeedEp0Prime = 1;
	StorageStats.need_ep0_prime = 1U;
}

static void UsbHandlePortChangeIrq(void)
{
	u32 PortSc;
	uint8_t CablePresent;

	UsbResetLocalStorageState();

	if (UsbInstance.Config.BaseAddress == 0U) {
		UsbRequestEp0Prime();
		return;
	}

	PortSc = XUsbPs_ReadReg(UsbInstance.Config.BaseAddress,
				XUSBPS_PORTSCR1_OFFSET);
	CablePresent = ((PortSc & XUSBPS_PORTSCR_CCS_MASK) != 0U) ? 1U : 0U;
	if (CablePresent == 0U) {
		UsbSawPhysicalDisconnect = 1U;
		UsbConfiguredOnce = 0U;
		NeedEndpointReset = 0;
		NeedEp0Prime = 0;
		StorageStats.need_ep0_prime = 0U;
		return;
	}

	if (UsbSawPhysicalDisconnect != 0U) {
		UsbRequestDeviceReconfigure();
	} else {
		UsbRequestEp0Prime();
	}
}

void usb_storage_service_get_stats(usb_storage_service_stats_t *stats)
{
	if (stats != NULL) {
		UsbStorageSnapshotRegs(&UsbInstance);
		UsbSnapshotPhyAndUlpi(&UsbInstance);
		*stats = StorageStats;
	}
}

void usb_storage_service_reset_stats(void)
{
	memset(&StorageStats, 0, sizeof(StorageStats));
	UsbStorageSnapshotRegs(&UsbInstance);
	UsbSnapshotPhyAndUlpi(&UsbInstance);
}

/*****************************************************************************/
/**
 *
 * This function is the handler which performs processing for the USB driver.
 * It is called from an interrupt context such that the amount of processing
 * performed should be minimized.
 *
 * This handler provides an example of how to handle USB interrupts and
 * is application specific.
 *
 * @param	CallBackRef is the Upper layer callback reference passed back
 *		when the callback function is invoked.
 * @param 	Mask is the Interrupt Mask.
 * @param	CallBackRef is the User data reference.
 *
 * @return
 * 		- XST_SUCCESS if successful
 * 		- XST_FAILURE on error
 *
 * @note	None.
 *
 ******************************************************************************/
static void UsbIntrHandler(void *CallBackRef, u32 Mask)
{
	(void)CallBackRef;

	NumIrqs++;
	StorageStats.irq_count++;
	StorageStats.last_irq_mask = Mask;
	if (Mask & XUSBPS_IXR_UI_MASK) {
		StorageStats.ui_irqs++;
	}
	if (Mask & XUSBPS_IXR_UE_MASK) {
		StorageStats.ue_irqs++;
	}
	if (Mask & XUSBPS_IXR_PC_MASK) {
		StorageStats.pc_irqs++;
		UsbHandlePortChangeIrq();
	}
	if (Mask & XUSBPS_IXR_UR_MASK) {
		StorageStats.reset_irqs++;
		UsbResetLocalStorageState();
		UsbRequestEp0Prime();
	}
}


/*****************************************************************************/
/**
* This function is registered to handle callbacks for endpoint 0 (Control).
*
* It is called from an interrupt context such that the amount of processing
* performed should be minimized.
*
*
* @param	CallBackRef is the reference passed in when the function
*		was registered.
* @param	EpNum is the Number of the endpoint on which the event occurred.
* @param	EventType is type of the event that occurred.
*
* @return	None.
*
******************************************************************************/
static void XUsbPs_Ep0EventHandler(void *CallBackRef, u8 EpNum,
				   u8 EventType, void *Data)
{
	XUsbPs			*InstancePtr;
	int			Status;
	XUsbPs_SetupData	SetupData;
	u8	*BufferPtr;
	u32	BufferLen;
	u32	Handle;

	(void)Data;

	Xil_AssertVoid(NULL != CallBackRef);

	InstancePtr = (XUsbPs *) CallBackRef;

	switch (EventType) {

		/* Handle the Setup Packets received on Endpoint 0. */
		case XUSBPS_EP_EVENT_SETUP_DATA_RECEIVED:
			StorageStats.ep0_setup_events++;
			Status = XUsbPs_EpGetSetupData(InstancePtr, EpNum, &SetupData);
			if (XST_SUCCESS == Status) {
				usb_storage_debug_note_setup(&SetupData);
				/* Handle the setup packet. */
				(int) XUsbPs_Ch9HandleSetupPacket(InstancePtr,
								  &SetupData);
			} else {
				StorageStats.ep0_rx_failures++;
			}
			break;

		/* We get data RX events for 0 length packets on endpoint 0. We receive
		 * and immediately release them again here, but there's no action to be
		 * taken.
		 */
		case XUSBPS_EP_EVENT_DATA_RX:
			StorageStats.ep0_data_rx_events++;
			/* Get the data buffer. */
			Status = XUsbPs_EpBufferReceive(InstancePtr, EpNum,
							&BufferPtr, &BufferLen, &Handle);
			if (XST_SUCCESS == Status) {
				/* Return the buffer. */
				XUsbPs_EpBufferRelease(Handle);
			} else {
				StorageStats.ep0_rx_failures++;
			}
			break;

		default:
			StorageStats.ep0_other_events++;
			StorageStats.last_ep0_event = EventType;
			break;
	}
}


/*****************************************************************************/
/**
* This function is registered to handle callbacks for endpoint 1 (Bulk data).
*
* It is called from an interrupt context such that the amount of processing
* performed should be minimized.
*
*
* @param	CallBackRef is the reference passed in when the function was
*		registered.
* @param	EpNum is the Number of the endpoint on which the event occurred.
* @param	EventType is type of the event that occurred.
*
* @return	None.
*
* @note 	None.
*
******************************************************************************/
static void XUsbPs_Ep1EventHandler(void *CallBackRef, u8 EpNum,
				   u8 EventType, void *Data)
{
	XUsbPs *InstancePtr;
	int Status;
	u8	*BufferPtr;
	u32	BufferLen;
	u32 InavalidateLen;
	u32	Handle;

	(void)Data;

	Xil_AssertVoid(NULL != CallBackRef);

	InstancePtr = (XUsbPs *) CallBackRef;

	switch (EventType) {
		case XUSBPS_EP_EVENT_DATA_RX:
			StorageStats.ep1_rx_events++;
			StorageStats.last_ep1_event = EventType;
			/* Get the data buffer.*/
			Status = XUsbPs_EpBufferReceive(InstancePtr, EpNum,
							&BufferPtr, &BufferLen, &Handle);
			if (XST_SUCCESS == Status) {
				/* Invalidate the Buffer Pointer */
				InavalidateLen =  BufferLen;
				if (BufferLen % 32) {
					InavalidateLen = (BufferLen / 32) * 32 + 32;
				}

				Xil_DCacheInvalidateRange((unsigned int)BufferPtr,
							  InavalidateLen);
				(void)EpNum;
				UsbNoteStorageOut(BufferLen);
				if (UsbStorageQueueEmpty() &&
				    XUsbPs_StorageTryFastRx(InstancePtr,
							    BufferPtr,
							    BufferLen)) {
					StorageStats.fast_packets++;
					StorageStats.fast_bytes += BufferLen;
				} else {
					(void)UsbQueueStorageReq(BufferPtr,
								 BufferLen);
				}
				/* Release the dTD immediately. The OUT dTDs are
				 * a circular all-active ring; holding any of
				 * them stops HW once it wraps around to the
				 * held one, silently stalling the endpoint. */
				XUsbPs_EpBufferRelease(Handle);
			} else {
				StorageStats.ep1_rx_failures++;
			}
			break;

		case XUSBPS_EP_EVENT_DATA_TX:
			StorageStats.ep1_tx_events++;
			StorageStats.last_ep1_event = EventType;
			break;

		default:
			StorageStats.ep1_other_events++;
			StorageStats.last_ep1_event = EventType;
			break;
	}
}

void usb_storage_debug_note_setup(const XUsbPs_SetupData *SetupData)
{
	if (SetupData == NULL) {
		return;
	}
	StorageStats.last_setup_bm_request_type = SetupData->bmRequestType;
	StorageStats.last_setup_b_request = SetupData->bRequest;
	StorageStats.last_setup_w_value = SetupData->wValue;
	StorageStats.last_setup_w_index = SetupData->wIndex;
	StorageStats.last_setup_w_length = SetupData->wLength;
	if ((SetupData->bmRequestType & XUSBPS_REQ_TYPE_MASK) ==
	    XUSBPS_CMD_CLASSREQ) {
		StorageStats.class_requests++;
	}
}

void usb_storage_debug_note_ch9_error(const XUsbPs_SetupData *SetupData)
{
	(void)SetupData;
	StorageStats.setup_errors++;
}

void usb_storage_debug_note_ep0_stall(void)
{
	StorageStats.ep0_stalls++;
}

void usb_storage_debug_note_clear_feature_halt(u8 EpNum)
{
	StorageStats.clear_feature_halt++;
	/* Per BOT, after the host clears a Bulk-IN halt the device must
	 * (re)supply the CSW for the current command. Gated internally on
	 * an owed-but-not-queued CSW so a successfully queued one is never
	 * duplicated. */
	if ((EpNum & 0x0FU) == 1U) {
		XUsbPs_StorageRetryCsw(&UsbInstance);
	}
}

void usb_storage_debug_note_msc_reset(void)
{
	StorageStats.msc_resets++;
}

void usb_storage_debug_note_set_address(u16 Address)
{
	(void)Address;
	StorageStats.set_address_requests++;
}

void usb_storage_debug_note_set_configuration(u16 Config)
{
	StorageStats.set_configuration_requests++;
	StorageStats.current_config = Config & 0xFFU;
	UsbConfiguredOnce = (Config != 0U) ? 1U : 0U;
}

void usb_storage_debug_note_descriptor(u8 DescType, u16 RequestLen,
				       u32 ReplyLen)
{
	StorageStats.last_desc_type = DescType;
	StorageStats.last_desc_request_len = RequestLen;
	StorageStats.last_desc_reply_len = ReplyLen;
	switch (DescType) {
		case XUSBPS_TYPE_DEVICE_DESC:
			StorageStats.get_descriptor_device++;
			break;
		case XUSBPS_TYPE_CONFIG_DESC:
			StorageStats.get_descriptor_config++;
			break;
		case XUSBPS_TYPE_STRING_DESC:
			StorageStats.get_descriptor_string++;
			break;
		case XUSBPS_TYPE_DEVICE_QUALIFIER:
			StorageStats.get_descriptor_qualifier++;
			break;
		default:
			StorageStats.get_descriptor_other++;
			break;
	}
}

void usb_storage_debug_note_ep_send_failure(u8 EpNum, u32 BufferLen,
					    int Status)
{
	StorageStats.last_send_ep = EpNum;
	StorageStats.last_send_len = BufferLen;
	StorageStats.last_send_status = (u32)Status;
	if (EpNum == 0U) {
		StorageStats.ep0_send_failures++;
	}
}

void usb_storage_debug_note_ep_prime(u8 EpNum, u8 Direction, int Status)
{
	UsbNoteEpPrime(EpNum, Direction, Status);
}

/*****************************************************************************/
/**
*
* This function setups the interrupt system such that interrupts can occur for
* the USB controller. This function is application specific since the actual
* system may or may not have an interrupt controller. The USB controller could
* be directly connected to a processor without an interrupt controller.  The
* user should modify this function to fit the application.
*
* @param	IntcInstancePtr is a pointer to instance of the Intc controller.
* @param	UsbInstancePtr is a pointer to instance of the USB controller.
* @param	UsbIntrId is the Interrupt Id and is typically
* 		XPAR_<INTC_instance>_<USB_instance>_VEC_ID value
* 		from xparameters.h
*
* @return
* 		- XST_SUCCESS if successful
* 		- XST_FAILURE on error
*
******************************************************************************/
#ifndef SDT
static int UsbSetupIntrSystem(XScuGic *IntcInstancePtr,
			      XUsbPs *UsbInstancePtr, u16 UsbIntrId)
{
	int Status;
	XScuGic_Config *IntcConfig;

	/*
	 * Initialize the interrupt controller driver so that it is ready to
	 * use.
	 */
	IntcConfig = XScuGic_LookupConfig(XPAR_SCUGIC_SINGLE_DEVICE_ID);
	if (NULL == IntcConfig) {
		return XST_FAILURE;
	}
	Status = XScuGic_CfgInitialize(IntcInstancePtr, IntcConfig,
				       IntcConfig->CpuBaseAddress);
	if (Status != XST_SUCCESS) {
		return XST_FAILURE;
	}

	Xil_ExceptionInit();
	/*
	 * Connect the interrupt controller interrupt handler to the hardware
	 * interrupt handling logic in the processor.
	 */
	Xil_ExceptionRegisterHandler(XIL_EXCEPTION_ID_IRQ_INT,
				     (Xil_ExceptionHandler)XScuGic_InterruptHandler,
				     IntcInstancePtr);
	/*
	 * Connect the device driver handler that will be called when an
	 * interrupt for the device occurs, the handler defined above performs
	 * the specific interrupt processing for the device.
	 */
	Status = XScuGic_Connect(IntcInstancePtr, UsbIntrId,
				 (Xil_ExceptionHandler)XUsbPs_IntrHandler,
				 (void *)UsbInstancePtr);
	if (Status != XST_SUCCESS) {
		return Status;
	}
	/*
	 * Enable the interrupt for the device.
	 */
	XScuGic_Enable(IntcInstancePtr, UsbIntrId);

	/*
	 * Enable interrupts in the Processor.
	 */
	Xil_ExceptionEnableMask(XIL_EXCEPTION_IRQ);

	return XST_SUCCESS;
}

/*****************************************************************************/
/**
*
* This function disables the interrupts that occur for the USB controller.
*
* @param	IntcInstancePtr is a pointer to instance of the INTC driver.
* @param	UsbIntrId is the Interrupt Id and is typically
* 		XPAR_<INTC_instance>_<USB_instance>_VEC_ID value
* 		from xparameters.h
*
* @return	None
*
* @note		None.
*
******************************************************************************/
static void UsbDisableIntrSystem(XScuGic *IntcInstancePtr, u16 UsbIntrId)
{
	/* Disconnect and disable the interrupt for the USB controller. */
	XScuGic_Disconnect(IntcInstancePtr, UsbIntrId);
}
#endif

#ifdef SDT
static u16 UsbDecodeGicIntrId(u32 UsbIntrId)
{
	return (u16)(XGet_IntrId(UsbIntrId) + XGet_IntrOffset(UsbIntrId));
}

static int UsbSetupSharedIntrSystem(XUsbPs *UsbInstancePtr, u32 UsbIntrId)
{
	XScuGic *Gic = gic_get();
	u16 GicIntrId = UsbDecodeGicIntrId(UsbIntrId);
	int Status;

	if (Gic == NULL) {
		return XST_FAILURE;
	}

	XScuGic_SetPriorityTriggerType(Gic, GicIntrId,
				       USB0_GIC_PRIORITY,
				       XINTR_IS_LEVEL_TRIGGERED);

	Status = XScuGic_Connect(Gic, GicIntrId,
				 (Xil_ExceptionHandler)XUsbPs_IntrHandler,
				 UsbInstancePtr);
	if (Status != XST_SUCCESS) {
		return Status;
	}

	XScuGic_Enable(Gic, GicIntrId);
	return XST_SUCCESS;
}

static void UsbDisableSharedIntrSystem(u32 UsbIntrId)
{
	XScuGic *Gic = gic_get();

	if (Gic != NULL) {
		XScuGic_Disconnect(Gic, UsbDecodeGicIntrId(UsbIntrId));
	}
}
#endif
