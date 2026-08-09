# Virtual ALF AD8088 Plus

Appletini can expose one of two mutually exclusive processor cards in slot 5:
the existing PCPI-compatible Z80 Appli-Card or a virtual 640 KB ALF AD8088
Plus. Physical slot 5 must be empty. The Plus layout is a compatibility
superset of the original Processor Card and includes the documented AD128K
address range.

## Configuration

Open **Config > Slot 5 Processor**, enable slot 5, and select either:

- `Z80 Appli-Card`
- `ALF AD8088 Plus (640K)`

Changing the selection resets the selected coprocessor. Virtual TransWarp and
the AD8088 cannot run together because both own the Apple bus. The menu blocks
a live conflict, and startup turns vTW off if an old profile enables both. The
PL also refuses AD8088 memory cycles whenever vTW is enabled. AD8088 bus cycles
are deferred while Disk II is in a timing-critical motor window.

## Implementation

The PL implements the sixteen slot-5 I/O ports at `$C0D0-$C0DF`, including the
documented port-0 bit-7 handshake. Single-byte accesses use the normal DMA
path. Monitor fill and copy commands move four bytes during one DMA hold, then
release the Apple bus before the next batch. The service runs batches for up
to one millisecond per main-loop pass, so a large boot copy finishes promptly
without one long DMA hold. This keeps each hold below the 6502's 10 microsecond
register-retention limit. Each batch has a sequence number, so old completion
state cannot finish a new request. If Disk II or vTW owns the bus, the command
stays pending and retries later; it never reports success without moving the
data. A transaction stopped by such a window carries a latched blocked
verdict in BUS_STATUS, so the PS still classifies it as retryable when the
window has closed before completion is read. A running 8088 program stalls
through blocked windows like added wait states instead of failing
mid-instruction. A timeout finishes the current data phase and uses the
normal parked release before READY returns. Reset, mode change, and slot disable remain hard
boundaries that release DMA, address, and data drive.

The PS runs an 8088-instruction-compatible XTulator CPU core plus a clean-room
monitor in a dedicated 1 MB DDR arena, separate from the Z80 card's memory.
The emulated physical memory map matches the 640 KB configuration expected by
AD8088 Plus software such as Reboot Camp '83:

- `$00000-$0FFFF`: 64 KB local RAM
- `$10000-$1FFFF`: 64 KB Apple shared-memory window
- `$20000-$2FFFF`: 64 KB expansion RAM
- `$30000-$3FFFF`: unpopulated gap (reads `$FF`)
- `$40000-$BFFFF`: 512 KB expansion RAM; `$40000-$5FFFF` remains the
  original AD128K-compatible range
- `$C0000-$FEFFF`: unpopulated ROM space (reads `$FF`)

The monitor implements integer commands 29-32, floating-point commands 33-47,
user commands 48-247, SEQUENCE (`$FB`), RANDOM (`$FC`), SET MEMORY (`$FD`),
MOVE DATA (`$FE`), and far CALL (`$FF`). Original-ROM graphics/MET commands
1-28 are currently accepted as no-ops. This is a clean-room compatibility
implementation, not a copy of the original ALF PROM, so software that depends
on those original graphics routines is outside the current compatibility
scope.

## Test disks

Run `python scripts/build_ad8088_test_disks.py` to generate:

- `software/AD8088_Test.dsk` -- bootable DOS 3.3 image
- `software/AD8088_Test.po` -- bootable 800 KB ProDOS image

Both auto-run the same visible test. It checks the mailbox, an 8088 integer
monitor command, fill/copy through the AD128K-compatible range, and execution
of real 8088 code. The last stage makes the 8088 write
`8088 EXECUTED THIS LINE` directly into the Apple text page through the
shared-memory window.

The Reboot Camp disk at `software/AD8088 MSDOS.hdv` follows the manual's READY
check before each port write and waits for READY after each long monitor
command. It reads back the copied IO.SYS header before it starts the 8088 and
does not rely on a fixed delay for the 24,763-byte boot copy.

UART `8088 status` reports monitor state, command/error counts, the actual 8088
instruction count, and average instructions/second since the preceding status
request. Take two readings while an 8088 workload is active to measure its
effective speed. `8088 dump <address> <length>` displays emulated card RAM.

The vendored XTulator CPU source and its GPL-2.0-or-later notice are under
`third_party/xtulator_cpu/`.

Protocol and memory-map behavior were implemented from the
[ALF AD8088 Processor Card manual](https://mirrors.apple2.org.za/Apple%20II%20Documentation%20Project/Interface%20Cards/CPU/ALF%20AD8088/Manuals/ALF%20AD8088%20Processor%20Card%20Manual.pdf).
