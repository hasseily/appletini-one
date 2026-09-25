# Appletini copy/fill API 1.0

Firmware F1.1.2 introduced an ARM service for explicit 65C02 memory transfers,
using the vTW CPU hold, shadow-RAM access and PSRAM DMA. Stock F1.1.1 does
**not** implement this API.

Use F1.1.4 or later with its rebuilt FPGA image. The old DMA status register
could discard completion during unrelated reads or idle bus cycles. The ARM
service could then report `$67` even after the DMA finished. F1.1.4 keeps
DONE and ABORTED set until reset or the next DMA command. The API format and
register addresses stay the same, but updating only the ARM ELF is insufficient.

F1.1.3 also fixed a separate timer-conversion bug: its predecessor's timestamp
could move backward at two-second boundaries and cause false timeouts. That
fix alone did not resolve the reported Doom v11 startup crash `$67`. Host tests
cover the timer, and RTL tests reproduce lost completion through the real AXI
wrapper. The hardware retest on 2026-09-25 confirmed that Doom v11 works with
F1.1.4. The user did not observe a significant speedup.

The service requires an active virtual TransWarp CPU. It works at any vTW
speed, including TURBO, and does not change the speed. The native motherboard
CPU is unsupported. RamWorks must be enabled for extended-AUX endpoints.

## 1. Calling sequence

1. Identify Appletini's SmartPort controller in slot 7. Do not send private
   selector `$80` to an unrelated SmartPort controller.
2. Issue SmartPort **STATUS (`$00`), unit 0, selector `$80`** into a 32-byte
   buffer. Check success, signature `AMEM`, major version 1, descriptor size
   16, required feature bits and availability bit 0.
3. Build a length-prefixed command list using the format below.
4. Issue SmartPort **CONTROL (`$04`), unit 0, selector `$80`**, pointing to that
   list. The call is synchronous. Successful return means all operations
   completed and the CPU hold was released.
5. Optionally issue STATUS again to read the last result and ARM elapsed time.

On normal SmartPort entry, success returns `A=$00, C=0`; failure returns
`A=error, C=1`. Treat X and Y as clobbered. An unmodified F1.1.1 controller
returns `BADCTL=$21` for this selector. Applications may select their original
CPU implementation when the startup probe reports unsupported/unavailable.
Do **not** retry a failed execution using CPU loops: a hardware failure may
have partially modified memory.

### SmartPort parameter list

Both calls use this parameter block. Multibyte integers throughout this API
are little endian.

| Offset | Size | Value |
|---|---:|---|
| 0 | 1 | Parameter count: 3 |
| 1 | 1 | Controller unit: 0 |
| 2 | 2 | CPU pointer to STATUS output or CONTROL input |
| 4 | 1 | Selector: `$80` |
| 5 | 4 | Padding; emit zero in new callers (firmware ignores these bytes) |

Reserve the four padding bytes because the current Appletini SmartPort
ROM streams nine parameter bytes. The buffer pointer is a **CPU address**,
accessed by the ROM under the caller's mapping. The endpoints in a descriptor
are **physical addresses**, independent of RAMRD, RAMWRT and the selected AUX
bank. The service does not change the caller's Apple softswitch mapping.

The normal SmartPort call is `JSR entry`, immediately followed by a command
byte and a two-byte parameter-list pointer. Discover the entry using the
Appletini slot ROM's SmartPort entry offset (`$CnFF + 3`, within page `$Cn00`).
For the F1.1.1 slot-7 ROM this is `$C70D`.

**ROM workspace:** this ROM writes `$07F8` under the caller's mapping during
entry (MAIN in the example's required mapping). Save/restore that
byte only if neither the source nor destination depends on its original/new
value. For a whole-MAIN snapshot, restoring it after the call is insufficient:
the source would already contain the ROM workspace byte. Doom uses the raw
FIFO transport described in section 7 to avoid this write.

## 2. Capability and result block (32 bytes)

STATUS returns the following block, even when vTW is currently unavailable.
The availability field describes whether the accelerated CPU is active and
this service has no sticky unsafe failure; a particular CONTROL may still
fail, for example if RamWorks is disabled.

| Offset | Size | Meaning |
|---|---:|---|
| 0 | 4 | ASCII `AMEM` |
| 4 | 1 | Major version: 1 |
| 5 | 1 | Minor version: 0 |
| 6 | 1 | Descriptor size: 16 |
| 7 | 1 | Maximum descriptor count: 16 |
| 8 | 2 | Feature bits: bit 0 COPY, bit 1 FILL, bit 2 PRIVATE; v1 = `$0007` |
| 10 | 2 | Lowest allowed endpoint address: `$0200` |
| 12 | 2 | Exclusive endpoint limit: `$C000` |
| 14 | 1 | Highest supported logical AUX bank: 126 |
| 15 | 1 | Bit 0: active vTW available; other bits zero |
| 16 | 4 | Current ARM microsecond counter, low 32 bits |
| 20 | 2 | Maximum hardware DMA transaction: 512 bytes; not an API length limit |
| 22 | 1 | Last CONTROL result code |
| 23 | 1 | Fully completed descriptors in the last CONTROL |
| 24 | 4 | Last CONTROL elapsed microseconds |
| 28 | 4 | Confirmed completed bytes in the last CONTROL |

Elapsed time includes validation, hold acquisition, transfers and release. It
excludes command submission/response transport. STATUS does not clear results.
The timestamp wraps about every 71.6 minutes; unsigned 32-bit subtraction is
valid for shorter intervals. Result counters are global to this controller:
read them before another caller executes a command.

## 3. CONTROL buffer

The first word is a payload length **excluding the length word itself**.
For `N` descriptors, write `8 + 16*N`, with `1 <= N <= 16`.
The complete caller buffer occupies `10 + 16*N` bytes, at most 266 bytes.

| Payload offset | Size | Value |
|---|---:|---|
| 0 | 4 | ASCII `AMEM` |
| 4 | 1 | Major version: 1 |
| 5 | 1 | Descriptor count: 1–16 |
| 6 | 1 | Header flags: 0 |
| 7 | 1 | Reserved: 0 |
| 8 | 16*N | Descriptors in execution order |

Payload length must match the count exactly. Unknown flags, operations,
versions or nonzero reserved bytes are rejected. All descriptors, including
their PRIVATE requirements, are checked before any destination is written.
Hold acquisition can synchronize existing pending work even for a rejected
PRIVATE request; this does not execute any of the requested writes.

### Descriptor (16 bytes)

| Offset | Size | Meaning |
|---|---:|---|
| 0 | 1 | Operation: 1 COPY, 2 FILL |
| 1 | 1 | Flags: bit 0 PRIVATE, other bits zero |
| 2 | 1 | Source space: 0 MAIN, 1 AUX |
| 3 | 1 | Source logical bank |
| 4 | 2 | Source address |
| 6 | 1 | Destination space: 0 MAIN, 1 AUX |
| 7 | 1 | Destination logical bank |
| 8 | 2 | Destination address |
| 10 | 2 | Byte count, nonzero |
| 12 | 1 | FILL value; must be zero for COPY |
| 13 | 3 | Reserved: zero |

For FILL, all four source bytes (offsets 2–5) must be zero. COPY transfers the
specified byte count unchanged. FILL repeats its one-byte value.

### Endpoint rules

| Space/bank | Storage |
|---|---|
| MAIN, bank 0 | Accelerated MAIN shadow RAM |
| AUX, bank 0 | Accelerated base-AUX shadow RAM |
| AUX, banks 1–126 | RamWorks PSRAM; logical bank `n` maps to physical bank `n+1` |

MAIN with a nonzero bank, AUX bank 127, or any other space is invalid. Bank 127
is intentionally excluded by this API's safe DMA address range, even if a
different CPU access path exposes it.

Each endpoint must start at or above `$0200`, and `address + length` must be
at most `$C000`, without wrapping. The largest single descriptor is therefore
`$BE00` (48,640) bytes at `$0200`. Zero page, stack, language-card RAM, ROM and
I/O cannot be endpoints. Arbitrary byte alignment is supported; bytes outside
the destination interval are preserved.

COPY rejects any overlap in the same physical bank, including an identical
source/destination interval. It is a copy operation, not `memmove`. Use a
separate temporary region and ordered descriptors for an overlapping move.
Later descriptors may read earlier destinations, and destinations of separate
descriptors may overlap: they execute strictly in list order.

## 4. PRIVATE is an explicit working-memory contract

**Every destination in MAIN or base AUX requires flag bit 0 (`PRIVATE`).**
Extended-AUX destinations may use flags 0; PRIVATE is also accepted there.

A PRIVATE write updates authoritative CPU memory and maintains the relevant
CPU cache coherence. It deliberately emits **no renderer capture records and
no motherboard video replay writes**. The caller is declaring that the
destination is private working memory. Do not use this path to update a
currently displayed framebuffer, physical display memory or data that an
external bus consumer must see. Before exposing such data, republish the
entire required range through the application's normal supported display or
I/O path. A video-mode change alone is not a publish operation.

This rule applies to all MAIN/base-AUX addresses, not just conventional video
ranges: renderer and physical visibility depend on mode and consumer. Version
1 has no visible-video-copy flag. Doom opts in for its phase arenas and its
internal column buffer; its SHR screen blit remains on the existing path.

The existing CPU hold first drains pending mirror/cache work. This prevents
an older queued write from overwriting a newly restored private region.
The broad F1.1.1 flush rules remain unchanged in this branch.

## 5. Ownership, failures and timing

Submit from code, stack and data that survive the operation. The command
list is captured before execution, but the caller's return path, response
buffer and any later instructions must remain valid. To replace most MAIN
memory, keep the transport and continuation in language-card RAM, as Doom
does. The API cannot protect a caller from overwriting its own program.

One CPU hold covers the entire list. Transfers run in bounded internal
chunks: 504 useful bytes, with aligned DMA transactions of at most 512 bytes.
That avoids the existing FPGA DMA engine's ten-bit length truncation.
Successful release happens only after accepted transfers complete. Reset,
loss of session or hold ownership cancels DMA and drains it before release.
If completion cannot be proved, the service does not release any surviving
CPU hold and marks itself unsafe. Reset/session teardown may already have
released the hold in hardware; a normal error return may be impossible. Recover by
restarting the Appletini firmware, not by resubmitting the list.

| Error | Name | Meaning |
|---|---|---|
| `$00` | OK | All descriptors completed |
| `$21` | BADCTL / unsupported | Old firmware or unsupported selector/unit |
| `$60` | UNAVAILABLE | Active vTW/RamWorks prerequisite missing |
| `$61` | BAD_HEADER | Signature, version, count, flags or payload length invalid |
| `$62` | BAD_DESCRIPTOR | Operation, flags, unused fields or reserved bytes invalid |
| `$63` | RANGE | Unsupported endpoint/bank, zero count or address overflow |
| `$64` | OVERLAP | COPY source/destination overlap in the same bank |
| `$65` | PRIVATE_REQUIRED | MAIN/base-AUX destination lacks PRIVATE |
| `$66` | BUSY | Another owner is using the service, hold or DMA |
| `$67` | IO | Transfer/hold timeout or hardware error |
| `$68` | SESSION_LOST | Apple reset or accelerated execution session disappeared |
| `$69` | UNSAFE | Abort/drain/release could not be confirmed |

Validation failures execute no destination writes. Runtime failures are
**not atomic**: earlier descriptors and chunks remain written, and the failing
chunk may also have been partly written. STATUS progress counts only confirmed
completed chunks and complete descriptors. It is diagnostic, not a safe retry
offset. A lost/reset session gets no stale SmartPort response.

The CPU is stopped during these operations. Long holds can merge periodic
VBL interrupts, so VBL IRQ counts and phase samples can undercount elapsed
time and transfer cost. Use ARM timestamps or external wall time for speed
measurements. Do not infer hardware acceleration from the simulator's timing:
its API model verifies memory and transport behavior, not DMA speed.

## 6. ca65 example

`software/memory_api/memory_api.inc` provides constants and command builders.
`software/memory_api/example.s` shows STATUS probing and a two-command list:

```asm
operations:
        AMEM_BEGIN 2
        ; Save 8 KiB MAIN working memory to extended AUX bank 125.
        AMEM_COPY_RECORD AMEM_MAIN, 0, $6000, AMEM_AUX, 125, $6000, $2000, 0
        ; Clear a private 4 KiB MAIN working region.
        AMEM_FILL_RECORD AMEM_MAIN, 0, $9000, $1000, $00, AMEM_PRIVATE
```

The list's length word is `$0028`, and its total size is 42 bytes. To restore
the saved region, reverse the endpoints and set PRIVATE:

```asm
        AMEM_BEGIN 1
        AMEM_COPY_RECORD AMEM_AUX, 125, $6000, AMEM_MAIN, 0, $6000, $2000, AMEM_PRIVATE
```

Assemble the example from this repository's root:

```sh
ca65 --cpu 65c02 -I software/memory_api -o memory_api_example.o software/memory_api/example.s
```

This produces a relocatable example object, not a bootable program. Place its
CODE, RODATA, BSS and stack outside the overwritten regions in your program's
linker configuration. The example assumes Appletini in slot 7, active vTW,
MAIN zero page/stack, RAMRD/RAMWRT off and peripheral slot ROM visible. It
preserves processor status and MAIN `$07F8`; X/Y may be clobbered.

## 7. Raw Appletini FIFO transport

This is Appletini-specific. Use it only after identifying the Appletini slot
ROM. Preserve processor flags, disable interrupts while owning C8 (unless
every enabled handler is known to preserve C8 ownership and transport storage),
clear decimal mode and arrange the caller's mapping so Appletini's peripheral ROM
is accessible. Access `$CFFF` to release an old C8 selection, then read the
Appletini slot-7 ROM to select its C8 space. Do not select another slot until
the response is consumed. Restore flags and mapping on return.

FIFO registers:

| Address | Role |
|---|---|
| `$CFF0` | Data: write pushes one byte, read peeks current byte |
| `$CFF1` | Control/status; bit 7 indicates response ready |
| `$CFF2` | Write pops one response byte; write value ignored |

Start with no command in flight and the preceding response fully consumed.
There is no Apple-side FIFO reset command: every write to CONTROL executes
a request. Push the SmartPort command byte, then the nine parameter bytes.
For CONTROL, append the list's
two-byte length and its payload. Write `$02` to CONTROL to execute the
SmartPort command, then poll CONTROL bit 7. Pop the first response byte:
the SmartPort result. Each pop is `LDA $CFF0` followed by `STA $CFF2`;
reading `$CFF2` does not consume anything. For successful STATUS, pop a two-byte returned length
followed by that many data bytes (32 for API 1.0). CONTROL returns only its
result byte. Finish by releasing C8 at `$CFFF`.

The parameter pointer bytes are retained in this framing for compatibility;
the raw transport itself supplies the CONTROL data and receives STATUS data
via the FIFO. One maximum-size CONTROL submits 276 bytes before execute:
1 command + 9 parameters + 2 length + 264 payload. A raw helper must implement
its own return-code/carry convention and workspace preservation. On timeout
after execute, memory may have changed; stop the operation, never silently
fall back to CPU copying.

## 8. Build and validation

### Doom v11 crash on F1.1.3

A hardware dump after the reported startup crash showed:

| Item | Value | Meaning |
|---|---|---|
| Last API error | `$67` | Transfer failed |
| Completed bytes | `$0BD0` (3024) | Six 504-byte chunks confirmed |
| Elapsed time | `$2E40` (11840 us) | Consistent with the 10 ms transfer deadline |
| Hold status, `$40000270` | `$00000001` | One flush completed; no busy or held bit |
| Shadow READ4 status, `$40000284` | `$80000372` | Idle and ready; 882 four-byte reads completed |

Doom's first API copy saves MAIN `$0200` onward to AUX bank 122 at the
same address. The API maps that bank to physical `$7B0200`. Each 504-byte
source chunk needs 126 READ4 operations: 882 reads match seven source chunks,
while the API confirmed only six destination chunks. This is consistent with
a timeout during the seventh PSRAM DMA write, followed by a clean CPU release.
It excludes an initial hold timeout, which would leave zero completed bytes
and take at least 100 ms.

The DMA register dump confirmed MC_ADDR `$007B0DD0`, DDR_ADDR `$005CB900`
(the F1.1.3 bounce buffer), LENGTH_RW `$800001F8` and STATUS `$00000000`.
Those are the expected seventh-chunk parameters, with neither BUSY nor DONE
set. This matches the completion-loss fault reproduced in the RTL test.
The dump does not prove whether that chunk reached PSRAM before cleanup.
The user retested the same Doom v11 disk with the full F1.1.4 image on
2026-09-25 and confirmed that it works, with no significant speedup observed.
This confirms the startup fix; it is not a measured performance comparison.

### Mac checks

```sh
python3 scripts/test_memory_api.py
python3 scripts/test_memory_api_hw.py
ca65 --cpu 65c02 -I software/memory_api -o /tmp/memory_api_example.o software/memory_api/example.s
```

With Vivado tools on PATH, `python scripts/test_ps_dma_command.py` checks
completion retention through the real AXI wrapper, the DMA register contract
and abort draining. The old RTL fails the completion-retention test.

The native C tests exercise the real parser/executor with a memory backend,
including unaligned boundaries, ordered dependencies, full-list validation,
private-memory requirements and partial failures. The second script also
checks the real hardware backend against fake MMIO/DMA, and compiles the
backend, DMA helper and SmartPort dispatch for syntax using minimal BSP
declarations. They cannot validate FPGA timing or the PC BSP/toolchain.
See the Doom repository's profiling document
for the hardware A/B procedure.

### PC FPGA and firmware build

Use the repository's supported Vitis environment, with `XILINX_VITIS` set.
From the repository root, rebuild the FPGA to include the DMA completion fix.
The build exports `project\appletini_yarz_top.xsa` and the matching bitstream at
`project\appletini_yarz.runs\impl_1\appletini_yarz_top.bit`.

```bat
vivado -mode batch -source scripts/build_and_export_xsa.tcl
vitis -s scripts\create_vitis_workspace.py
scripts\make_firmware_bin.bat FIRMWARE-AMEM.BIN
```

The Vitis command **recreates `vitis_workspace` and terminates existing Vitis
IDE/server and Java processes**, as that existing script specifies. Save any
workspace work and close applications that depend on those processes first.
It builds the platform/FSBL, core-1 frontend and core-0 frontend with the new
sources. The packaging command combines FSBL, the rebuilt bitstream and frontend
ELF, then appends the firmware manifest. For a different bitstream location:

```bat
scripts\make_firmware_bin.bat FIRMWARE-AMEM.BIN vitis_workspace\appletini_platform\export\appletini_platform\sw\boot\fsbl.elf C:\path\to\F1.1.4.bit vitis_workspace\frontend\build\frontend.elf
```

Copy the resulting image onto the card's SD volume under the required name
`FIRMWARE.BIN`, then use the normal firmware update procedure. The packaged
bitstream must include the F1.1.4 DMA status fix. Hardware acceptance still
requires copy/readback tests on spare regions, reset/error checks and Doom
captures against the same disk on stock and modified firmware. No speedup is
claimed until those measurements exist.
