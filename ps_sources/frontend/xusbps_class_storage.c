/******************************************************************************
* Copyright (C) 2010 - 2022 Xilinx, Inc.  All rights reserved.
* Copyright (C) 2023 - 2025 Advanced Micro Devices, Inc. All Rights Reserved.
* SPDX-License-Identifier: MIT
******************************************************************************/

/*****************************************************************************/
/**
 * @file xusbps_class_storage.c
 *
 * This file contains the implementation of the storage class code for the
 * example.
 *
 *<pre>
 * MODIFICATION HISTORY:
 *
 * Ver   Who  Date     Changes
 * ----- ---- -------- ---------------------------------------------------------
 * 1.00a wgr  10/10/10 First release
 * 2.1   kpc  4/28/14  Align DMA buffers to cache line boundary
 * 2.4   vak  4/01/19  Fixed IAR data_alignment warnings
 * 2.10  ka   8/21/25  Fixed GCC warnigns
 *</pre>
 ******************************************************************************/

/***************************** Include Files *********************************/

#include <string.h>

#include "xusbps.h"		/* USB controller driver */
#include "xusbps_endpoint.h"
#include "xusbps_hw.h"

#include "xusbps_ch9_storage.h"
#include "xusbps_ch9.h"
#include "xusbps_class_storage.h"
#include "xil_exception.h"
#include "xil_printf.h"
#include "xil_cache.h"
#include "xil_mmu.h"
#include "xiltimer.h"

/* #define CLASS_STORAGE_DEBUG */

#ifdef CLASS_STORAGE_DEBUG
#define printf xil_printf
#endif

/************************** Constant Definitions *****************************/

#define USB_CSW_SIGNATURE	0x53425355U
#define USB_CSW_RING_COUNT	8U
#define USB_WRITE_PIPE_DEPTH	16U
#define USB_WRITE_PIPE_INVALID	USB_WRITE_PIPE_DEPTH


/************************** Function Prototypes ******************************/
static void StorageNoteCbw(const USB_CBW *CBW, u32 BufferLen);
static void StorageNoteScsiCommand(u8 Opcode);
static int StorageEp1Send(XUsbPs *InstancePtr, const u8 *BufferPtr,
			  u32 BufferLen);
void usb_storage_debug_note_ch9_error(const XUsbPs_SetupData *SetupData);
void usb_storage_debug_note_ep0_stall(void);
void usb_storage_debug_note_ep_send_failure(u8 EpNum, u32 BufferLen,
					    int Status);
void usb_storage_debug_note_msc_reset(void);

/************************** Variable Definitions *****************************/

#ifdef __ICCARM__
#pragma pack(push, 1)
#endif
typedef struct {
	u32 dCSWSignature;
	u32 dCSWTag;
	u32 dCSWDataResidue;
	u8  bCSWStatus;
#ifdef __ICCARM__
} USB_CSW;
#pragma pack(pop)
#else
} __attribute__((__packed__)) USB_CSW;
#endif

typedef struct {
	u8 Data[USB_STORAGE_MAX_TRANSFER_BYTES];
	u32 Block;
	u32 Blocks;
	u32 Bytes;
	u32 Pending;
	u32 Reserved0;
	u32 Reserved1;
	u32 Reserved2;
	u32 Reserved3;
} usb_write_pipe_slot_t;

/* Pre-manufactured response to the SCSI Inquirey command.
 */
#ifdef __ICCARM__
#pragma data_alignment = 32
const static SCSI_INQUIRY scsiInquiry = {
	0x00,
	0x80,
	0x00,
	0x01,
	0x1f,
	0x00,
	0x00,
	0x00,
	{"Xilinx  "},		/* Vendor ID:  must be  8 characters long. */
	{"PS USB VirtDisk"},	/* Product ID: must be 16 characters long. */
	{"1.00"}		/* Revision:   must be  4 characters long. */
};

#pragma data_alignment = 32
static u8 MaxLUN = 0;

#pragma data_alignment = 32
static USB_CBW lastCBW;

#pragma data_alignment = 32
static USB_CSW statusCSW[USB_CSW_RING_COUNT];
static u32 statusCSWIndex;

#pragma data_alignment = 32
/* Local transmit buffer for simple replies. */
static u8 txBuffer[128];

#pragma data_alignment = 32
/* Shared transfer staging for READ multi-block replies. */
static u8 xferBuffer[USB_STORAGE_MAX_TRANSFER_BYTES];

#pragma data_alignment = 32
static usb_write_pipe_slot_t WritePipe[USB_WRITE_PIPE_DEPTH];
#else
static const SCSI_INQUIRY scsiInquiry ALIGNMENT_CACHELINE = {
	0x00,
	0x80,
	0x00,
	0x01,
	0x1f,
	0x00,
	0x00,
	0x00,
	{"Xilinx  "},		/* Vendor ID:  must be  8 characters long. */
	{"PS USB VirtDisk"},	/* Product ID: must be 16 characters long. */
	{"1.00"}		/* Revision:   must be  4 characters long. */
};
static u8 MaxLUN ALIGNMENT_CACHELINE = 0;

static USB_CBW lastCBW ALIGNMENT_CACHELINE;

static USB_CSW statusCSW[USB_CSW_RING_COUNT] ALIGNMENT_CACHELINE;
static u32 statusCSWIndex;

/* Local transmit buffer for simple replies. */
static u8 txBuffer[128] ALIGNMENT_CACHELINE;

/* SD->USB read staging. The SD ADMA2 engine writes xferBuffer and the USB
 * controller DMA reads it; both bypass the MMU, so to keep them coherent
 * without a CPU bounce copy the buffer is Normal Non-cacheable (marked at
 * runtime by StorageInitReadBuffer). Xil_SetTlbAttributes works at 1 MB
 * granularity, so the buffer is sized and aligned to a full 1 MB section to
 * keep neighbouring cacheable data unaffected; the driver only uses the first
 * USB_STORAGE_MAX_TRANSFER_BYTES of it.
 *
 * Non-cacheability is strictly the DMA coherency guard. Poll-context EP1
 * priming is synchronized separately by StorageWaitEp1InIdle. */
#define USB_STORAGE_XFER_REGION_BYTES 0x100000U
static u8 xferBuffer[USB_STORAGE_XFER_REGION_BYTES]
	__attribute__((aligned(USB_STORAGE_XFER_REGION_BYTES)));
static usb_write_pipe_slot_t WritePipe[USB_WRITE_PIPE_DEPTH] ALIGNMENT_CACHELINE;
#endif

static u8 requestSense[18] ALIGNMENT_CACHELINE = {
	0x70, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x0a,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00
};

static u32 writeBlock;
static u32 writeBlockCount;
static u32 writeBlockFill;
static int rxBytesLeft;
static int phase = USB_EP_STATE_COMMAND;
static int writeFailed;
/* BOT requires the device to deliver exactly one CSW per command, even
 * across a Bulk-IN stall + CLEAR_FEATURE(HALT) recovery. pendingCSW holds
 * the most recent CSW; cswPending means the host may not have received it
 * yet (set at build time, cleared when the next CBW arrives or on a
 * bus/MSC reset). The poll loop and clear-halt hook retry failed sends. */
static USB_CSW pendingCSW ALIGNMENT_CACHELINE;
static volatile u32 cswPending;   /* host is owed this CSW */
static volatile u32 cswQueued;    /* and it is believed queued in the ring */
static usb_storage_class_stats_t ClassStats;
static XTime writeCommandTick;
static XTime writeRxStartTick;
static volatile u32 writePipeHead;
static volatile u32 writePipeTail;
static volatile u32 writePipeQueued;
static u32 activeWriteSlot = USB_WRITE_PIPE_INVALID;
static volatile u32 asyncWriteFailed;
static volatile u32 hostEjectRequested;

/* SCSI sense bookkeeping. While media is absent, REQUEST SENSE must report
 * NOT READY / MEDIUM NOT PRESENT so the host keeps polling TEST UNIT READY
 * and mounts cleanly when media appears. */
static void StorageSetSense(u8 Key, u8 Asc, u8 Ascq)
{
	requestSense[2] = Key;
	requestSense[12] = Asc;
	requestSense[13] = Ascq;
	ClassStats.last_sense_key = Key;
	ClassStats.last_sense_asc = Asc;
}

#define SENSE_NOT_READY_NO_MEDIUM()	StorageSetSense(0x02, 0x3A, 0x00)
#define SENSE_MEDIUM_READ_ERROR()	StorageSetSense(0x03, 0x11, 0x00)
#define SENSE_MEDIUM_WRITE_ERROR()	StorageSetSense(0x03, 0x0C, 0x00)
#define SENSE_LBA_OUT_OF_RANGE()	StorageSetSense(0x05, 0x21, 0x00)
#define SENSE_INVALID_COMMAND()		StorageSetSense(0x05, 0x20, 0x00)
#define SENSE_CLEAR()			StorageSetSense(0x00, 0x00, 0x00)

static void stats_snapshot_state(void)
{
	ClassStats.phase = (u32)phase;
	ClassStats.rx_bytes_left = (rxBytesLeft > 0) ? (u32)rxBytesLeft : 0U;
}

static void stats_note_read(u32 Blocks, u32 Bytes, u64 Ticks, int Failed)
{
	ClassStats.read_cmds++;
	ClassStats.last_read_blocks = Blocks;
	ClassStats.last_read_ticks = Ticks;
	if (Blocks > ClassStats.max_read_blocks) {
		ClassStats.max_read_blocks = Blocks;
	}
	if (Ticks > ClassStats.max_read_ticks) {
		ClassStats.max_read_ticks = Ticks;
	}
	if (Failed != 0) {
		ClassStats.read_failures++;
	} else {
		ClassStats.read_bytes += Bytes;
		ClassStats.read_ticks += Ticks;
	}
}

static void stats_note_write_command(u32 Blocks, u32 Requested)
{
	ClassStats.write_cmds++;
	ClassStats.write_requested_bytes += Requested;
	ClassStats.last_write_blocks = Blocks;
	if (Blocks > ClassStats.max_write_blocks) {
		ClassStats.max_write_blocks = Blocks;
	}
	XTime_GetTime(&writeCommandTick);
	writeRxStartTick = 0;
}

static void stats_note_write_done(u32 Bytes, u64 RxTicks, u64 TotalTicks, int Failed)
{
	ClassStats.last_write_rx_ticks = RxTicks;
	ClassStats.last_write_total_ticks = TotalTicks;
	if (RxTicks > ClassStats.max_write_rx_ticks) {
		ClassStats.max_write_rx_ticks = RxTicks;
	}
	if (TotalTicks > ClassStats.max_write_total_ticks) {
		ClassStats.max_write_total_ticks = TotalTicks;
	}
	if (Failed != 0) {
		ClassStats.write_failures++;
	} else {
		ClassStats.write_committed_bytes += Bytes;
		ClassStats.write_rx_ticks += RxTicks;
		ClassStats.write_total_ticks += TotalTicks;
	}
}

static void stats_update_pending_slots(void)
{
	ClassStats.write_pending_slots = writePipeQueued;
	ClassStats.write_pipe_depth = USB_WRITE_PIPE_DEPTH;
	if (writePipeQueued > ClassStats.max_write_pending_slots) {
		ClassStats.max_write_pending_slots = writePipeQueued;
	}
}

static u32 WritePipeIrqSave(void)
{
	u32 Cpsr = mfcpsr();

	Xil_ExceptionDisableMask(XIL_EXCEPTION_IRQ);
	return Cpsr;
}

static void WritePipeIrqRestore(u32 Cpsr)
{
	if ((Cpsr & XIL_EXCEPTION_IRQ) == 0U) {
		Xil_ExceptionEnableMask(XIL_EXCEPTION_IRQ);
	}
}

static void ResetStorageState(void)
{
	writeBlock = 0;
	writeBlockCount = 0;
	writeBlockFill = 0;
	rxBytesLeft = 0;
	phase = USB_EP_STATE_COMMAND;
	writeFailed = 0;
	writeCommandTick = 0;
	writeRxStartTick = 0;
	activeWriteSlot = USB_WRITE_PIPE_INVALID;
	cswPending = 0U;
	cswQueued = 0U;
	if (writePipeQueued == 0U) {
		asyncWriteFailed = 0U;
	}
	SENSE_CLEAR();
}

void XUsbPs_StorageReset(void)
{
	ResetStorageState();
}

void XUsbPs_StorageInitReadBuffer(void)
{
	/* Drop any cached lines for the region, then switch its 1 MB MMU
	 * section to Normal Non-cacheable so the SD ADMA2 write and the USB
	 * controller DMA read of xferBuffer stay coherent without a bounce
	 * copy. Must run once at init, before the first SD-backed READ. */
	Xil_DCacheFlushRange((UINTPTR)xferBuffer, sizeof(xferBuffer));
	Xil_SetTlbAttributes((UINTPTR)xferBuffer, NORM_NONCACHE);
}

static u32 CbwRequestedBytes(const USB_CBW *CBW)
{
	return be2le(CBW->dCBWDataTransferLength);
}

static u32 TransferResidue(const USB_CBW *CBW, u32 SentBytes)
{
	u32 Requested = CbwRequestedBytes(CBW);

	return (Requested > SentBytes) ? (Requested - SentBytes) : 0;
}

static void StorageNoteCbw(const USB_CBW *CBW, u32 BufferLen)
{
	/* A new CBW proves the host received the previous CSW. */
	cswPending = 0U;
	cswQueued = 0U;
	ClassStats.cbw_packets++;
	ClassStats.last_cbw_len = BufferLen;
	ClassStats.last_cbw_signature = CBW->dCBWSignature;
	ClassStats.last_cbw_tag = CBW->dCBWTag;
	ClassStats.last_cbw_transfer_len = CbwRequestedBytes(CBW);
	ClassStats.last_cbw_flags = CBW->bmCBWFlags;
	ClassStats.last_cbw_lun = CBW->cCBWLUN;
	ClassStats.last_cbw_cb_len = CBW->bCBWCBLength;
	ClassStats.last_scsi_opcode = CBW->CBWCB[0];
}

static void StorageNoteScsiCommand(u8 Opcode)
{
	ClassStats.scsi_cmds++;
	ClassStats.last_scsi_opcode = Opcode;
	switch (Opcode) {
		case USB_RBC_TEST_UNIT_READY:
			ClassStats.cmd_test_unit_ready++;
			break;
		case USB_RBC_INQUIRY:
			ClassStats.cmd_inquiry++;
			break;
		case USB_UFI_GET_CAP_LIST:
			ClassStats.cmd_get_cap_list++;
			break;
		case USB_RBC_READ_CAP:
			ClassStats.cmd_read_capacity++;
			break;
		case USB_RBC_MODE_SENSE:
			ClassStats.cmd_mode_sense++;
			break;
		case 0x03:
			ClassStats.cmd_request_sense++;
			break;
		case USB_RBC_READ:
			ClassStats.cmd_read10++;
			break;
		case USB_RBC_WRITE:
			ClassStats.cmd_write10++;
			break;
		case USB_RBC_MEDIUM_REMOVAL:
			ClassStats.cmd_medium_removal++;
			break;
		case USB_RBC_VERIFY:
			ClassStats.cmd_verify++;
			break;
		case 0x35:
			ClassStats.cmd_sync_cache++;
			break;
		case USB_RBC_STARTSTOP_UNIT:
			ClassStats.cmd_start_stop++;
			break;
		case USB_RBC_FORMAT:
			ClassStats.cmd_format++;
			break;
		case USB_RBC_MODE_SEL:
			ClassStats.cmd_mode_select++;
			break;
		default:
			ClassStats.cmd_unhandled++;
			break;
	}
}

static int StorageEp1Send(XUsbPs *InstancePtr, const u8 *BufferPtr,
			  u32 BufferLen)
{
	int Status;
	u32 Cpsr;

	/* XUsbPs_EpBufferSend (XUsbPs_EpQueueRequest) manipulates the EP1 IN
	 * dQH overlay, the ATDTW tripwire (USBCMD) and ENDPTPRIME with no
	 * internal locking. We call it from the main-loop poll context as well
	 * as from the USB ISR (WRITE fast-path), and the driver ISR itself
	 * advances the same EP1 IN dTD ring on every completion. A USB
	 * interrupt that preempts the priming sequence corrupts it and leaves
	 * the dTD active-but-never-serviced (EPRDY stuck, no TX completion).
	 * Mask IRQ around the call to make priming atomic w.r.t. the USB ISR.
	 * WritePipeIrqRestore only re-enables IRQ if it was enabled on entry,
	 * so this is safe when invoked from ISR context. */
	Cpsr = WritePipeIrqSave();
	Status = XUsbPs_EpBufferSend(InstancePtr, 1, (u8 *)BufferPtr,
				     BufferLen);
	WritePipeIrqRestore(Cpsr);
	ClassStats.ep1_in_sends++;
	ClassStats.last_ep1_send_len = BufferLen;
	ClassStats.last_ep1_send_status = (u32)Status;
	if (Status != XST_SUCCESS) {
		ClassStats.ep1_in_failures++;
	}
	return Status;
}

/* IRQ-guarded EP0 (control) send. Same priming race as StorageEp1Send:
 * XUsbPs_EpBufferSend's dQH/ATDTW/ENDPTPRIME sequence is not interrupt-safe
 * and the poll-context EP0 re-prime path touches ENDPTPRIME concurrently. */
static int StorageEp0Send(XUsbPs *InstancePtr, const u8 *BufferPtr,
			  u32 BufferLen)
{
	int Status;
	u32 Cpsr;

	Cpsr = WritePipeIrqSave();
	Status = XUsbPs_EpBufferSend(InstancePtr, 0, (u8 *)BufferPtr, BufferLen);
	WritePipeIrqRestore(Cpsr);
	return Status;
}

/* Poll-context EP1 IN drain.
 *
 * The deferred READ path primes EP1 IN from the main-loop poll context.
 * XUsbPs_EpQueueRequest chooses between an explicit ENDPTPRIME (when the
 * driver's dTD ring looks empty, dTDTail == dTDHead) and an ATDTW "append to
 * the in-flight chain" path (when it looks busy). The ring's tail pointer only
 * advances when the USB ISR reaps a TX completion, so if the previous command's
 * CSW completion is still unreaped at prime time, the ring looks busy, the
 * ATDTW path is taken, and it races the controller going idle: the freshly
 * appended data dTD is stranded Active-but-never-primed -- the READ(10) wedge
 * (tact=1, EPRDY set, host never drains). ISR-answered commands never hit this
 * because their ring is reaped in-order before they prime.
 *
 * Give the ISR a chance to catch up: with IRQ enabled, spin until the ring is
 * empty so the following prime takes the reliable explicit-ENDPTPRIME path.
 * The wait is bounded so a stalled host cannot hang the main loop; the prime
 * watchdog handles a descriptor stranded after timeout. */
static void StorageWaitEp1InIdle(XUsbPs *InstancePtr)
{
	XUsbPs_EpIn *Ep = &InstancePtr->DeviceConfig.Ep[1].In;
	XTime Start;
	XTime Now;

	/* Only meaningful when IRQ is enabled (the ISR must run to reap). If we
	 * are in a masked/ISR context the ring is already reaped in-order. */
	if ((mfcpsr() & XIL_EXCEPTION_IRQ) != 0U) {
		return;
	}

	XTime_GetTime(&Start);
	for (;;) {
		__asm__ volatile("" ::: "memory");
		if (Ep->dTDTail == Ep->dTDHead) {
			break;
		}
		XTime_GetTime(&Now);
		if ((Now - Start) > (5U * (COUNTS_PER_SECOND / 1000U))) {
			/* A stalled host must not hang the main loop. The prime
			 * watchdog detects and re-primes any stranded dTD. */
			ClassStats.ep1_idle_wait_timeouts++;
			break;
		}
	}
}

/* EP1 IN prime watchdog -- the structural backstop for the ENDPTPRIME vs
 * ATDTW race (see StorageWaitEp1InIdle). If a submitted dTD is sitting
 * Active while the controller shows EP1 IN neither primed (ENDPTPRIME) nor
 * armed (ENDPTSTAT), the controller will never service it: the host blocks
 * forever on the data/CSW and the mount dies. Detect that state from the
 * main-loop poll and re-issue the prime. The condition must persist for two
 * consecutive polls before recovery fires, which debounces the brief
 * hardware window between EPPRIME clearing and EPRDY latching on a healthy
 * prime. All checks run with IRQ masked so the ISR's ring reaping cannot
 * tear the view. */
void XUsbPs_StoragePrimeWatchdog(XUsbPs *InstancePtr)
{
	static u32 WedgeStreak;
	static u32 FullStreak;
	XUsbPs_EpIn *Ep;
	u32 Prime;
	u32 Stat;
	u32 Token;
	u32 Cpsr;
	const u32 Ep1InMask = 0x00020000U;	/* ENDPTPRIME/STAT PETB bit 17 */

	if ((InstancePtr == NULL) ||
	    (InstancePtr->Config.BaseAddress == 0U) ||
	    (InstancePtr->DeviceConfig.NumEndpoints <= 1U)) {
		return;
	}

	Cpsr = WritePipeIrqSave();
	Ep = &InstancePtr->DeviceConfig.Ep[1].In;
	if (Ep->dTDTail == NULL) {
		WedgeStreak = 0U;
		FullStreak = 0U;
		WritePipeIrqRestore(Cpsr);
		return;
	}

	Prime = XUsbPs_ReadReg(InstancePtr->Config.BaseAddress,
			       XUSBPS_EPPRIME_OFFSET);
	Stat = XUsbPs_ReadReg(InstancePtr->Config.BaseAddress,
			      XUSBPS_EPRDY_OFFSET);
	XUsbPs_dTDInvalidateCache(Ep->dTDTail);
	Token = XUsbPs_ReaddTD(Ep->dTDTail, XUSBPS_dTDTOKEN);

	if (Ep->dTDTail == Ep->dTDHead) {
		/* head==tail is AMBIGUOUS in this driver: empty, or
		 * completely full of unreaped active dTDs. The tail token
		 * disambiguates. A full ring with the endpoint neither
		 * primed nor armed is the terminal wedge observed on
		 * hardware (every send fails XST_USB_NO_DESC_AVAILABLE,
		 * the host clear-halts forever, no disk appears). The
		 * queued responses are dead history at this point — the
		 * host has already timed out on them — so recover by
		 * flushing the endpoint and reinitializing the ring, then
		 * let the CSW-retry path supply the one CSW still owed. */
		WedgeStreak = 0U;
		if (((Token & 0x80U) != 0U) &&
		    ((Prime & Ep1InMask) == 0U) &&
		    ((Stat & Ep1InMask) == 0U)) {
			FullStreak++;
			if (FullStreak >= 2U) {
				XUsbPs_EpFlush(InstancePtr, 1,
					       XUSBPS_EP_DIRECTION_IN);
				(void)XUsbPs_ReconfigureEp(InstancePtr,
					&InstancePtr->DeviceConfig, 1,
					XUSBPS_EP_DIRECTION_IN, 0);
				cswQueued = 0U;	/* queued copy destroyed */
				ClassStats.ring_full_recoveries++;
				FullStreak = 0U;
			}
		} else if (((Token & 0x80U) == 0U) && (Ep->dQH != NULL)) {
			/* Second wedge shape (root-caused live 2026-07-03 via
			 * JTAG): ring tokens read EMPTY, but the dQH transfer
			 * overlay holds an ACTIVE token with the endpoint
			 * neither primed nor armed. Cause: the driver's
			 * ATDTW/prime-skip dance concluded the endpoint was
			 * already armed and never wrote ENDPTPRIME, so the
			 * queued transfer strands in the overlay. A manual
			 * ENDPTPRIME write over JTAG armed the endpoint
			 * immediately (ENDPTSTAT bit set) with the queued
			 * transfer intact -- so the recovery is exactly
			 * that: re-issue the prime. Do NOT flush+reconfigure
			 * here; that destroys the queued transfer, re-enters
			 * the same trap on the CSW retry, and the unbounded
			 * loop starves the main loop (observed as a full
			 * card freeze). */
			u32 QhToken;

			XUsbPs_dQHInvalidateCache(Ep->dQH);
			QhToken = XUsbPs_ReaddQH(Ep->dQH, XUSBPS_dQHdTDTOKEN);
			if (((QhToken & 0x80U) != 0U) &&
			    ((Prime & Ep1InMask) == 0U) &&
			    ((Stat & Ep1InMask) == 0U)) {
				FullStreak++;
				if (FullStreak >= 2U) {
					(void)XUsbPs_EpPrime(InstancePtr, 1,
						XUSBPS_EP_DIRECTION_IN);
					ClassStats.prime_watchdog_recoveries++;
					FullStreak = 0U;
				}
			} else {
				FullStreak = 0U;
			}
		} else {
			FullStreak = 0U;
		}
		WritePipeIrqRestore(Cpsr);
		return;
	}
	FullStreak = 0U;

	if (((Token & 0x80U) != 0U) &&
	    ((Prime & Ep1InMask) == 0U) && ((Stat & Ep1InMask) == 0U)) {
		WedgeStreak++;
		if (WedgeStreak >= 2U) {
			(void)XUsbPs_EpPrime(InstancePtr, 1,
					     XUSBPS_EP_DIRECTION_IN);
			ClassStats.prime_watchdog_recoveries++;
			WedgeStreak = 0U;
		}
	} else {
		WedgeStreak = 0U;
	}
	WritePipeIrqRestore(Cpsr);
}

static USB_CSW *BuildStatus(const USB_CBW *CBW, u32 Residue, u8 Status)
{
	USB_CSW *StatusPtr = &statusCSW[statusCSWIndex];

	statusCSWIndex = (statusCSWIndex + 1U) % USB_CSW_RING_COUNT;

	StatusPtr->dCSWSignature = USB_CSW_SIGNATURE;
	StatusPtr->dCSWTag = CBW->dCBWTag;
	StatusPtr->dCSWDataResidue = Residue;
	StatusPtr->bCSWStatus = Status;

	return StatusPtr;
}

static void SendStatus(XUsbPs *InstancePtr, const USB_CBW *CBW,
		       u32 Residue, u8 Status)
{
	USB_CSW *StatusPtr = BuildStatus(CBW, Residue, Status);
	int SendResult;

	ClassStats.csw_sends++;
	ClassStats.last_status_residue = Residue;
	ClassStats.last_status_code = Status;

	/* Track the CSW we owe the host before attempting the send, so a
	 * failed queue (e.g. wedged ring, XST_USB_NO_DESC_AVAILABLE) can be
	 * retried from the poll loop / clear-halt recovery instead of being
	 * silently dropped — the BOT exchange dies without its CSW. The
	 * pending state clears when the next CBW arrives (the host only
	 * advances after receiving the CSW) or on a bus/MSC reset. */
	pendingCSW = *StatusPtr;
	cswPending = 1U;

	SendResult = StorageEp1Send(InstancePtr, (u8 *)StatusPtr,
				    sizeof(*StatusPtr));
	if (SendResult == XST_SUCCESS) {
		cswQueued = 1U;
	} else {
		cswQueued = 0U;
		ClassStats.csw_send_failures++;
	}
}

/* Retry an owed CSW that is NOT believed queued (after a send failure or
 * an EP1 IN ring recovery that destroyed the queued copy). Retrying a
 * successfully-queued CSW would duplicate it, so cswQueued gates this.
 * Safe from poll or ISR context; StorageEp1Send masks IRQ internally. */
void XUsbPs_StorageRetryCsw(XUsbPs *InstancePtr)
{
	if ((cswPending == 0U) || (cswQueued != 0U)) {
		return;
	}
	if (StorageEp1Send(InstancePtr, (u8 *)&pendingCSW,
			   sizeof(pendingCSW)) == XST_SUCCESS) {
		cswQueued = 1U;
		ClassStats.csw_retries++;
	}
}

/* Complete a COMMAND-phase command that moves NO data. BOT 6.7.2: when
 * the host expects a data-IN stage (dCBWDataTransferLength > 0 with the
 * IN direction flag) and the device has nothing to send, the device must
 * STALL the Bulk-IN pipe and supply the CSW after the host clears the
 * halt. Queuing the 13-byte CSW into a shorter host data stage overruns the
 * scheduled transfer. Park the CSW until the CLEAR_FEATURE(HALT) hook or poll
 * retry can deliver it. */
static void SendStatusNoData(XUsbPs *InstancePtr, const USB_CBW *CBW,
			     u8 Status)
{
	USB_CSW *StatusPtr;
	u32 Requested = CbwRequestedBytes(CBW);
	int SendResult;

	StatusPtr = BuildStatus(CBW, Requested, Status);
	ClassStats.csw_sends++;
	ClassStats.last_status_residue = Requested;
	ClassStats.last_status_code = Status;
	pendingCSW = *StatusPtr;
	cswPending = 1U;
	cswQueued = 0U;

	if ((Requested != 0U) && ((CBW->bmCBWFlags & 0x80U) != 0U)) {
		ClassStats.ep1_protocol_stalls++;
		XUsbPs_EpStall(InstancePtr, 1, XUSBPS_EP_DIRECTION_IN);
		/* CSW delivered after the host's clear-halt. */
		return;
	}

	SendResult = StorageEp1Send(InstancePtr, (u8 *)&pendingCSW,
				    sizeof(pendingCSW));
	if (SendResult == XST_SUCCESS) {
		cswQueued = 1U;
	} else {
		ClassStats.csw_send_failures++;
	}
}

static void SendDataAndStatus(XUsbPs *InstancePtr, const USB_CBW *CBW,
			      const void *Data, u32 Length, u32 Residue)
{
	int Status;
	u32 Cpsr;

	if (Length == 0) {
		SendStatus(InstancePtr, CBW, Residue, 0);
		return;
	}

	/* The data prime and the following CSW prime MUST be atomic with
	 * respect to the USB ISR. XUsbPs_EpBufferSend manipulates the EP1 IN
	 * dQH overlay + ATDTW tripwire + ENDPTPRIME, and the driver ISR advances
	 * the same dTD ring on every TX completion. StorageEp1Send masks IRQ
	 * around each individual send, but when this runs from the poll loop
	 * (READ / READ CAPACITY / anything deferred), IRQ is briefly UNMASKED in
	 * the gap between the data send and the CSW send -- and a USB interrupt
	 * landing in that gap corrupts the in-flight data dTD and leaves it
	 * active-but-never-serviced (EPRDY stuck, host never drains; observed as
	 * the 512-byte READ(10) data wedge with datast=0 yet tact=1). Commands
	 * answered from the ISR are already atomic because the ISR runs with IRQ
	 * masked; hold IRQ masked across the whole data+CSW sequence so the poll
	 * path gets the same guarantee. The inner per-send masking nests safely:
	 * WritePipeIrqRestore only re-enables IRQ if it was enabled on entry,
	 * which it is not once we have masked here. */
	Cpsr = WritePipeIrqSave();

	ClassStats.data_in_sends++;
	ClassStats.last_data_in_len = Length;
	Status = StorageEp1Send(InstancePtr, Data, Length);
	ClassStats.last_data_in_status = (u32)Status;
	if (Status != XST_SUCCESS) {
		ClassStats.data_in_failures++;
		SendStatus(InstancePtr, CBW, CbwRequestedBytes(CBW), 1);
		WritePipeIrqRestore(Cpsr);
		return;
	}

	SendStatus(InstancePtr, CBW, Residue, 0);
	WritePipeIrqRestore(Cpsr);
}

static u32 LimitToRequested(const USB_CBW *CBW, u32 Available)
{
	u32 Requested = CbwRequestedBytes(CBW);

	if ((Requested != 0U) && (Requested < Available)) {
		return Requested;
	}

	return Available;
}

static int WritePipeHasFreeSlot(void)
{
	u32 Active = (activeWriteSlot < USB_WRITE_PIPE_DEPTH) ? 1U : 0U;

	return ((writePipeQueued + Active) < USB_WRITE_PIPE_DEPTH) ? 1 : 0;
}

static int WritePipeFlushOne(void)
{
	usb_write_pipe_slot_t *Slot;
	int Status;
	u32 Cpsr;

	if (writePipeQueued == 0U) {
		return XST_SUCCESS;
	}

	Slot = &WritePipe[writePipeHead];
	if (Slot->Pending == 0U) {
		asyncWriteFailed = 1U;
		ClassStats.write_async_failures++;
		Cpsr = WritePipeIrqSave();
		writePipeHead = (writePipeHead + 1U) % USB_WRITE_PIPE_DEPTH;
		writePipeQueued--;
		stats_update_pending_slots();
		WritePipeIrqRestore(Cpsr);
		return XST_FAILURE;
	}

	Status = usb_storage_write(Slot->Block, Slot->Blocks, Slot->Data);
	if (Status != XST_SUCCESS) {
		asyncWriteFailed = 1U;
		ClassStats.write_async_failures++;
	}

	Slot->Pending = 0U;
	Cpsr = WritePipeIrqSave();
	writePipeHead = (writePipeHead + 1U) % USB_WRITE_PIPE_DEPTH;
	writePipeQueued--;
	stats_update_pending_slots();
	WritePipeIrqRestore(Cpsr);
	return Status;
}

int XUsbPs_StorageFlushPending(void)
{
	int Status = XST_SUCCESS;

	while (writePipeQueued != 0U) {
		if (WritePipeFlushOne() != XST_SUCCESS) {
			Status = XST_FAILURE;
		}
	}

	return (asyncWriteFailed != 0U) ? XST_FAILURE : Status;
}

void XUsbPs_StoragePoll(void)
{
	(void)WritePipeFlushOne();
}

int XUsbPs_StorageHasPending(void)
{
	return ((writePipeQueued != 0U) ||
		(activeWriteSlot < USB_WRITE_PIPE_DEPTH) ||
		(phase != USB_EP_STATE_COMMAND) ||
		(rxBytesLeft != 0)) ? 1 : 0;
}

int XUsbPs_StorageHostEjectRequested(void)
{
	return (hostEjectRequested != 0U) ? 1 : 0;
}

void XUsbPs_StorageClearHostEjectRequested(void)
{
	hostEjectRequested = 0U;
}

static int WritePipeAllocSlot(int AllowFlush)
{
	while (!WritePipeHasFreeSlot()) {
		if ((AllowFlush == 0) ||
		    (WritePipeFlushOne() != XST_SUCCESS)) {
			return -1;
		}
	}

	activeWriteSlot = writePipeTail;
	WritePipe[activeWriteSlot].Pending = 0U;
	return (int)activeWriteSlot;
}

static void WritePipeReleaseActive(void)
{
	if (activeWriteSlot < USB_WRITE_PIPE_DEPTH) {
		WritePipe[activeWriteSlot].Pending = 0U;
	}
	activeWriteSlot = USB_WRITE_PIPE_INVALID;
}

static void WritePipeCommitActive(u32 Block, u32 Blocks, u32 Bytes)
{
	usb_write_pipe_slot_t *Slot;
	u32 Cpsr;

	if (activeWriteSlot >= USB_WRITE_PIPE_DEPTH) {
		return;
	}

	Slot = &WritePipe[activeWriteSlot];
	Slot->Block = Block;
	Slot->Blocks = Blocks;
	Slot->Bytes = Bytes;
	Slot->Pending = 1U;
	Cpsr = WritePipeIrqSave();
	writePipeTail = (writePipeTail + 1U) % USB_WRITE_PIPE_DEPTH;
	writePipeQueued++;
	stats_update_pending_slots();
	activeWriteSlot = USB_WRITE_PIPE_INVALID;
	WritePipeIrqRestore(Cpsr);
}

static int StorageStartWrite(XUsbPs *InstancePtr, const USB_CBW *CBW,
			     int AllowFlush)
{
	u32 Block = htonl(((SCSI_READ_WRITE *)CBW->CBWCB)->block);
	u32 Blocks = htons(((SCSI_READ_WRITE *)CBW->CBWCB)->length);
	u32 CapacityBlocks = usb_storage_block_count();
	u32 Requested = CbwRequestedBytes(CBW);
	u32 TransferBytes = Blocks * USB_STORAGE_BLOCK_SIZE;
	int Invalid = 0;
	int Slot = -1;

	if (Requested != 0U) {
		if (usb_storage_medium_ready() == 0) {
			SENSE_NOT_READY_NO_MEDIUM();
			Invalid = 1;
		} else if ((Blocks == 0U) ||
		    (Blocks > USB_STORAGE_MAX_TRANSFER_BLOCKS) ||
		    (Requested != TransferBytes) ||
		    (Block >= CapacityBlocks) ||
		    (Blocks > (CapacityBlocks - Block))) {
			SENSE_LBA_OUT_OF_RANGE();
			Invalid = 1;
		}
		if (Invalid == 0) {
			Slot = WritePipeAllocSlot(AllowFlush);
			if (Slot < 0) {
				if (AllowFlush == 0) {
					return 0;
				}
				SENSE_MEDIUM_WRITE_ERROR();
				Invalid = 1;
			}
		}
	}

	stats_note_write_command(Blocks, Requested);
#ifdef CLASS_STORAGE_DEBUG
	printf("SCSI: WRITE block %u count %u\n",
	       (unsigned)Block, (unsigned)Blocks);
#endif
	if (Requested == 0U) {
		SendStatus(InstancePtr, CBW, 0, 0);
		return 1;
	}

	writeBlock = Block;
	writeBlockCount = Blocks;
	writeBlockFill = 0;
	writeFailed = Invalid;
	lastCBW = *CBW;
	rxBytesLeft = Requested;
	phase = USB_EP_STATE_DATA;
	return 1;
}

static void StorageHandleWriteData(XUsbPs *InstancePtr, const u8 *BufferPtr,
				   u32 BufferLen)
{
	usb_write_pipe_slot_t *Slot = NULL;
	u32 TransferBytes;

	TransferBytes = (BufferLen > (u32)rxBytesLeft) ?
			(u32)rxBytesLeft : BufferLen;
	if ((TransferBytes != 0U) && (writeRxStartTick == 0)) {
		XTime_GetTime(&writeRxStartTick);
	}

	if (!writeFailed && (activeWriteSlot < USB_WRITE_PIPE_DEPTH)) {
		Slot = &WritePipe[activeWriteSlot];
	}
	if (!writeFailed && (Slot == NULL)) {
		writeFailed = 1;
	}
	if (!writeFailed &&
	    ((writeBlockFill + TransferBytes) > sizeof(Slot->Data))) {
		writeFailed = 1;
		ClassStats.write_overflows++;
	}
	if (!writeFailed && (TransferBytes != 0U)) {
		memcpy(&Slot->Data[writeBlockFill], BufferPtr, TransferBytes);
	}
	writeBlockFill += TransferBytes;
	ClassStats.write_received_bytes += TransferBytes;
	rxBytesLeft -= TransferBytes;

	if (rxBytesLeft <= 0) {
		u32 Whole = writeBlockFill / USB_STORAGE_BLOCK_SIZE;
		u32 Rem = writeBlockFill % USB_STORAGE_BLOCK_SIZE;
		XTime RxDoneTick;
		XTime DoneTick;
		u64 RxTicks;

		if ((Rem != 0U) || (Whole != writeBlockCount)) {
			writeFailed = 1;
		}
		XTime_GetTime(&RxDoneTick);
		RxTicks = (writeRxStartTick != 0) ?
			  (u64)(RxDoneTick - writeRxStartTick) : 0U;
		if (!writeFailed) {
			WritePipeCommitActive(writeBlock, Whole,
					      Whole * USB_STORAGE_BLOCK_SIZE);
		} else {
			WritePipeReleaseActive();
		}
		XTime_GetTime(&DoneTick);
		stats_note_write_done(Whole * USB_STORAGE_BLOCK_SIZE,
				      RxTicks,
				      (u64)(DoneTick - writeCommandTick),
				      writeFailed);
		SendStatus(InstancePtr, &lastCBW, 0, writeFailed ? 1U : 0U);
		phase = USB_EP_STATE_COMMAND;
	}
}

/*****************************************************************************/
/**
* This function handles Reduced Block Command (RBC) requests from the host.
*
* @param	InstancePtr is a pointer to XUsbPs instance of the controller.
* @param	EpNum is the number of the endpoint on which the RBC was received.
* @param	BufferPtr is the data buffer containing the RBC or data.
* @param	BufferLen is the length of the data buffer.
*
* @return	None.
*
* @note		None.
*
******************************************************************************/
void XUsbPs_HandleStorageReq(XUsbPs *InstancePtr, u8 EpNum,
			     u8 *BufferPtr, u32 BufferLen)
{
	USB_CBW	*CBW;
	u32	Block;
	u32	Blocks;
	u32	CapacityBlocks;
	u32	Requested;
	u32	TransferBytes;

	(void)EpNum;

	/* COMMAND phase. */
	if (USB_EP_STATE_COMMAND == phase) {
		CBW = (USB_CBW *) BufferPtr;
		StorageNoteCbw(CBW, BufferLen);
		StorageNoteScsiCommand(CBW->CBWCB[0]);

		switch (CBW->CBWCB[0]) {
			case USB_RBC_INQUIRY:
#ifdef CLASS_STORAGE_DEBUG
				printf("SCSI: INQUIRY\n");
#endif
				TransferBytes = LimitToRequested(CBW,
								 sizeof(scsiInquiry));
				SendDataAndStatus(InstancePtr, CBW, &scsiInquiry,
						  TransferBytes,
						  TransferResidue(CBW, TransferBytes));
				break;


			case USB_UFI_GET_CAP_LIST: {
					SCSI_CAP_LIST	*CapList;

					CapList = (SCSI_CAP_LIST *) txBuffer;
					memset(CapList, 0, sizeof(*CapList));
#ifdef CLASS_STORAGE_DEBUG
					printf("SCSI: CAPLIST\n");
#endif
					CapList->listLength	= 8;
					CapList->descCode	= 3;
					CapList->numBlocks	= htonl(usb_storage_block_count());
					CapList->blockLength	= htons(USB_STORAGE_BLOCK_SIZE);
					TransferBytes = LimitToRequested(CBW,
									 sizeof(SCSI_CAP_LIST));
					SendDataAndStatus(InstancePtr, CBW, txBuffer,
							  TransferBytes,
							  TransferResidue(CBW,
									  TransferBytes));
					break;
				}

			case USB_RBC_READ_CAP: {
					SCSI_READ_CAPACITY	*Cap;

					Cap = (SCSI_READ_CAPACITY *) txBuffer;
					memset(Cap, 0, sizeof(*Cap));
#ifdef CLASS_STORAGE_DEBUG
					printf("SCSI: READCAP\n");
#endif
					if (usb_storage_medium_ready() == 0) {
						/* Fail the command instead of
						 * reporting a 0-block disk; the
						 * host retries after TUR says
						 * the medium arrived. */
						SENSE_NOT_READY_NO_MEDIUM();
						SendStatusNoData(InstancePtr,
								 CBW, 1);
						break;
					}
					TransferBytes = usb_storage_block_count();
					Cap->numBlocks = htonl(TransferBytes ?
							       TransferBytes - 1U : 0U);
					Cap->blockSize = htonl(USB_STORAGE_BLOCK_SIZE);
					TransferBytes = LimitToRequested(CBW,
									 sizeof(SCSI_READ_CAPACITY));
					SendDataAndStatus(InstancePtr, CBW, txBuffer,
							  TransferBytes,
							  TransferResidue(CBW,
									  TransferBytes));
					break;
				}

			case USB_RBC_READ:
				Block = htonl(((SCSI_READ_WRITE *) CBW->CBWCB)->block);
				Blocks = htons(((SCSI_READ_WRITE *) CBW->CBWCB)->length);
				CapacityBlocks = usb_storage_block_count();
				TransferBytes =
					Blocks * USB_STORAGE_BLOCK_SIZE;
				XTime ReadT0;
				XTime ReadT1;
				int ReadFailed;
#ifdef CLASS_STORAGE_DEBUG
				printf("SCSI: READ block %u count %u\n",
				       (unsigned)Block, (unsigned)Blocks);
#endif
				if (Blocks == 0U) {
					stats_note_read(Blocks, 0U, 0U, 0);
					SendStatusNoData(InstancePtr, CBW, 0);
					break;
				}
				if (usb_storage_medium_ready() == 0) {
					SENSE_NOT_READY_NO_MEDIUM();
					stats_note_read(Blocks, 0U, 0U, 1);
					SendStatusNoData(InstancePtr, CBW, 1);
					break;
				}
				if ((Blocks > USB_STORAGE_MAX_TRANSFER_BLOCKS) ||
				    (Block >= CapacityBlocks) ||
				    (Blocks > (CapacityBlocks - Block))) {
					SENSE_LBA_OUT_OF_RANGE();
					stats_note_read(Blocks, 0U, 0U, 1);
					SendStatusNoData(InstancePtr, CBW, 1);
					break;
				}
				if (XUsbPs_StorageFlushPending() != XST_SUCCESS) {
					SENSE_MEDIUM_WRITE_ERROR();
					stats_note_read(Blocks, 0U, 0U, 1);
					SendStatusNoData(InstancePtr, CBW, 1);
					break;
				}
				XTime_GetTime(&ReadT0);
				if (usb_storage_read(Block, Blocks, xferBuffer) !=
				    XST_SUCCESS) {
					XTime_GetTime(&ReadT1);
					SENSE_MEDIUM_READ_ERROR();
					stats_note_read(Blocks, 0U,
							(u64)(ReadT1 - ReadT0), 1);
					SendStatusNoData(InstancePtr, CBW, 1);
					break;
				}
				XTime_GetTime(&ReadT1);
				TransferBytes = LimitToRequested(CBW, TransferBytes);
				ReadFailed = 0;
				stats_note_read(Blocks, TransferBytes,
						(u64)(ReadT1 - ReadT0), ReadFailed);
				/* Let the USB ISR reap any prior EP1 IN completion
				 * so the prime below takes the explicit-ENDPTPRIME
				 * path, not the racy ATDTW append that wedges the
				 * deferred READ (see StorageWaitEp1InIdle). */
				StorageWaitEp1InIdle(InstancePtr);
				/* xferBuffer is non-cacheable (StorageInitReadBuffer):
				 * the SD ADMA2 write and the USB DMA read are coherent
				 * with no bounce copy. */
				SendDataAndStatus(InstancePtr, CBW, xferBuffer,
						  TransferBytes,
						  TransferResidue(CBW, TransferBytes));
				break;

			case USB_RBC_MODE_SENSE:
#ifdef CLASS_STORAGE_DEBUG
				printf("SCSI: MODE SENSE\n");
#endif
				txBuffer[0] = 3;	/* mode data length */
				txBuffer[1] = 0;	/* medium type */
				txBuffer[2] = 0;	/* dev-specific (not WP) */
				txBuffer[3] = 0;	/* block desc length */
				TransferBytes = LimitToRequested(CBW, 4);
				SendDataAndStatus(InstancePtr, CBW, txBuffer,
						  TransferBytes,
						  TransferResidue(CBW, TransferBytes));
				break;

			case 0x5A:	/* MODE SENSE(10) — macOS probes this
					 * (write-protect check) with a small
					 * data stage; must answer with data,
					 * not a bare CSW. */
#ifdef CLASS_STORAGE_DEBUG
				printf("SCSI: MODE SENSE(10)\n");
#endif
				memset(txBuffer, 0, 8);
				txBuffer[1] = 6;	/* mode data length lo */
				TransferBytes = LimitToRequested(CBW, 8);
				SendDataAndStatus(InstancePtr, CBW, txBuffer,
						  TransferBytes,
						  TransferResidue(CBW, TransferBytes));
				break;


			case USB_RBC_TEST_UNIT_READY:
			case USB_RBC_VERIFY:
#ifdef CLASS_STORAGE_DEBUG
				printf("SCSI: TEST UNIT READY\n");
#endif
				if (usb_storage_medium_ready() == 0) {
					/* Honest not-ready: the host keeps
					 * polling TUR and mounts as soon as
					 * the SD card comes up (lazy re-init
					 * runs from the service poll). */
					SENSE_NOT_READY_NO_MEDIUM();
					SendStatus(InstancePtr, CBW, 0, 1);
					break;
				}
				if (asyncWriteFailed != 0U) {
					SENSE_MEDIUM_WRITE_ERROR();
					SendStatus(InstancePtr, CBW, 0, 1);
					break;
				}
				SendStatus(InstancePtr, CBW, 0, 0);
				break;

			case USB_RBC_MEDIUM_REMOVAL:
#ifdef CLASS_STORAGE_DEBUG
				printf("SCSI: PREVENT/ALLOW MEDIUM REMOVAL\n");
#endif
				SendStatus(InstancePtr, CBW, 0, 0);
				break;

			case 0x35:	/* Sync Cache */
#ifdef CLASS_STORAGE_DEBUG
				printf("SCSI: SYNC CACHE\n");
#endif
				SendStatus(InstancePtr, CBW, 0,
					   (XUsbPs_StorageFlushPending() ==
					    XST_SUCCESS) ? 0U : 1U);
				break;


			case USB_RBC_WRITE:
				(void)StorageStartWrite(InstancePtr, CBW, 1);
				break;


			case USB_RBC_STARTSTOP_UNIT: {
					u8 immed;
					u8 start;
					int flushStatus;

					immed = ((SCSI_START_STOP *) CBW->CBWCB)->immed;
					start = ((SCSI_START_STOP *) CBW->CBWCB)->start;
#ifdef CLASS_STORAGE_DEBUG
					printf("SCSI: START/STOP unit: immed %02x start %02x\n",
					       immed, start);
#endif
					(void)immed;
					flushStatus = XUsbPs_StorageFlushPending();
					if (flushStatus == XST_SUCCESS &&
					    (start & 0x03U) == 0x02U) {
						hostEjectRequested = 1U;
					}
					SendStatus(InstancePtr, CBW, 0,
						   (flushStatus == XST_SUCCESS) ?
						    0U : 1U);
					break;
				}

			case USB_RBC_FORMAT:
			case USB_RBC_MODE_SEL:
				Requested = CbwRequestedBytes(CBW);
				if (Requested == 0U) {
					SendStatus(InstancePtr, CBW, 0, 0);
					break;
				}

				lastCBW = *CBW;
				rxBytesLeft = Requested;
				phase = USB_EP_STATE_DATA;
				break;

			case 0x03:	/* Request Sense */
				Requested = CbwRequestedBytes(CBW);
				TransferBytes = (Requested < sizeof(requestSense)) ?
						Requested : sizeof(requestSense);
				/* Send a snapshot from txBuffer so the live
				 * sense can be cleared (sense reports once)
				 * without mutating an in-flight DMA buffer. */
				memcpy(txBuffer, requestSense,
				       sizeof(requestSense));
				SENSE_CLEAR();
				SendDataAndStatus(InstancePtr, CBW, txBuffer,
						  TransferBytes,
						  TransferResidue(CBW, TransferBytes));
				break;

			/* Commands that we do not support for this example. */
			case 0x5e:	/* Persistent Reserve In */
			case 0x5f:	/* Persistent Reserve Out */
			case 0x17:	/* Release */
			case 0x16:	/* Reserve */
			case 0x3b:	/* Write Buffer */
#ifdef CLASS_STORAGE_DEBUG
				printf("SCSI: Got unhandled command %02x\n", CBW->CBWCB[0]);
#endif
			default:
				SENSE_INVALID_COMMAND();
				SendStatusNoData(InstancePtr, CBW, 1);
				break;
		}
	}
	/* DATA phase.
	 */
	else if (USB_EP_STATE_DATA == phase) {
		switch (lastCBW.CBWCB[0]) {
			case USB_RBC_WRITE:
				StorageHandleWriteData(InstancePtr, BufferPtr,
						       BufferLen);
				break;

			case USB_RBC_FORMAT:
			case USB_RBC_MODE_SEL:
				TransferBytes = (BufferLen > (u32)rxBytesLeft) ?
						(u32)rxBytesLeft : BufferLen;
				rxBytesLeft -= TransferBytes;

				if (rxBytesLeft <= 0) {
					SendStatus(InstancePtr, &lastCBW, 0, 0);
					phase = USB_EP_STATE_COMMAND;
				}
				break;
		}
	}
}

/* Decide whether a COMMAND-phase SCSI opcode must be deferred to the polling
 * loop. The only commands that may block in caller (ISR) context are the ones
 * that touch the SD card: READ does a synchronous block read, and SYNC CACHE /
 * START-STOP flush the async write pipe (synchronous block writes). Everything
 * else -- all of the enumeration/status commands (INQUIRY, READ CAPACITY,
 * MODE SENSE, TEST UNIT READY, REQUEST SENSE, GET CAP LIST, ...) -- only
 * touches RAM and is answered immediately, keeping USB bring-up independent
 * of main-loop scheduling latency. WRITE is handled separately: it has
 * a dedicated async path that primes the response without blocking. */
static int StorageOpcodeNeedsDefer(u8 Opcode)
{
	switch (Opcode) {
		case USB_RBC_READ:		/* SD block read (blocking) */
		case 0x35:			/* SYNC CACHE: flushes write pipe */
		case USB_RBC_STARTSTOP_UNIT:	/* may flush write pipe */
			return 1;
		default:
			return 0;
	}
}

/* Attempt to fully service an EP1 OUT packet in the caller's context (the USB
 * ISR). Returns 1 if handled, 0 if it must be queued for the polling loop.
 *
 * The caller only invokes this when the deferral queue is empty, so handling a
 * packet here can never reorder it ahead of an already-queued command -- and
 * BOT is strictly sequential (the host will not issue the next CBW until it has
 * our CSW), so a command answered here is never racing a later one. */
int XUsbPs_StorageTryFastRx(XUsbPs *InstancePtr, u8 *BufferPtr, u32 BufferLen)
{
	USB_CBW *CBW;

	if (USB_EP_STATE_COMMAND == phase) {
		if (BufferLen < sizeof(USB_CBW)) {
			return 0;
		}
		CBW = (USB_CBW *)BufferPtr;

		if (CBW->CBWCB[0] == USB_RBC_WRITE) {
			/* WRITE keeps its dedicated async path: start the
			 * transfer without flushing (AllowFlush=0) so the ISR
			 * never blocks on SD I/O; the block commit happens
			 * later in the poll loop. */
			StorageNoteCbw(CBW, BufferLen);
			StorageNoteScsiCommand(CBW->CBWCB[0]);
			return StorageStartWrite(InstancePtr, CBW, 0);
		}

		if (StorageOpcodeNeedsDefer(CBW->CBWCB[0])) {
			/* Blocks on the SD card -- let the poll loop run it. */
			return 0;
		}

		/* Enumeration / status command: no SD I/O. Answer it now.
		 * XUsbPs_HandleStorageReq records the CBW stats itself. */
		XUsbPs_HandleStorageReq(InstancePtr, 1, BufferPtr, BufferLen);
		return 1;
	}

	if (USB_EP_STATE_DATA == phase) {
		if (lastCBW.CBWCB[0] == USB_RBC_WRITE) {
			StorageHandleWriteData(InstancePtr, BufferPtr,
					       BufferLen);
			return 1;
		}
		/* FORMAT / MODE SELECT OUT data: light, drain immediately. */
		XUsbPs_HandleStorageReq(InstancePtr, 1, BufferPtr, BufferLen);
		return 1;
	}

	return 0;
}

void XUsbPs_StorageGetStats(usb_storage_class_stats_t *stats)
{
	if (stats != NULL) {
		stats_update_pending_slots();
		stats_snapshot_state();
		*stats = ClassStats;
	}
}

void XUsbPs_StorageResetStats(void)
{
	memset(&ClassStats, 0, sizeof(ClassStats));
	stats_update_pending_slots();
}


/*****************************************************************************/
/**
* This function handles a Storage Class Setup request from the host.
*
* @param	InstancePtr is a pointer to XUsbPs instance of the controller.
* @param	SetupData is the setup data structure containing the setup
*		request.
*
* @return	None.
*
* @note		None.
*
******************************************************************************/
void XUsbPs_ClassReq_Storage(XUsbPs *InstancePtr, XUsbPs_SetupData *SetupData)
{
	int Status;

	Xil_AssertVoid(InstancePtr != NULL);
	Xil_AssertVoid(SetupData   != NULL);


	switch (SetupData->bRequest) {

		case XUSBPS_CLASSREQ_MASS_STORAGE_RESET:
			usb_storage_debug_note_msc_reset();
			ResetStorageState();
			Status = StorageEp0Send(InstancePtr, NULL, 0);
			if (Status != XST_SUCCESS) {
				usb_storage_debug_note_ep_send_failure(0, 0, Status);
			}
			break;

		case XUSBPS_CLASSREQ_GET_MAX_LUN:
			Status = StorageEp0Send(InstancePtr, &MaxLUN, 1);
			if (Status != XST_SUCCESS) {
				usb_storage_debug_note_ep_send_failure(0, 1, Status);
			}
			break;

		default:
			usb_storage_debug_note_ch9_error(SetupData);
			usb_storage_debug_note_ep0_stall();
			XUsbPs_EpStall(InstancePtr, 0, XUSBPS_EP_DIRECTION_IN);
			break;
	}
}
