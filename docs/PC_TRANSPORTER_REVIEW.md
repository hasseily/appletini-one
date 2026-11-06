# Review of the PC Transporter study

Review date: 2026-11-06. Reviewed the full `PC_TRANSPORTER_STUDY.md` at Appletini revision `a5a0669`. Its SHA-256 was `97b9d0e10405e4b23042b79a16dbdd0e050f996631572d8d1f9cf5d7365789b7`. Line numbers below refer to that copy. This review leaves the study unchanged.

The proposed slot 5 choice and the split between an FPGA Apple interface and an ARM guest remain sound design directions. The study is not yet a register-level implementation specification. It treats some MAME comments as implemented behavior, and some implemented behavior as the original hardware contract. The original 2.0.5 software resolves part of the register uncertainty without needing a boot trace.

This review uses source inspection, static disassembly, and calculations. It does not establish an emulator boot, a physical-card result, or an Appletini timing bound.

## Evidence and provenance

MAME reference: commit [`ce12c0474a7f5786748327d0320179344bddffba`](https://github.com/mamedev/mame/commit/ce12c0474a7f5786748327d0320179344bddffba), fetched through the GitHub connector. This matters: a cached raw-file response exposed an older implementation without the new trap, page-window, and ISM work. Use the pinned files below instead of mutable `master` links.

- [PC Transporter implementation](https://github.com/mamedev/mame/blob/ce12c0474a7f5786748327d0320179344bddffba/src/devices/bus/a2bus/pc_xporter.cpp).
- [SWIM1/ISM implementation](https://github.com/mamedev/mame/blob/ce12c0474a7f5786748327d0320179344bddffba/src/devices/machine/swim1.cpp).
- [CGA implementation](https://github.com/mamedev/mame/blob/ce12c0474a7f5786748327d0320179344bddffba/src/devices/bus/isa/cga.cpp).
- [NEC instruction implementation](https://github.com/mamedev/mame/blob/ce12c0474a7f5786748327d0320179344bddffba/src/devices/cpu/nec/necinstr.hxx).

Downloaded the [archived 2.0.5 disk](https://www.cvxmelody.net/PC_Transporter_System_Disk_2.0.5_800K.zip), extracted the ProDOS files, and inspected the original bytes. The following hashes identify this evidence; the filename alone does not identify every earlier AE release.

| Input | Bytes | SHA-256 |
| --- | ---: | --- |
| Archive ZIP | — | `b0da1f3ea4b38709ee04351e0de93f499a4d6997b383e03cad1f704f61a40403` |
| ProDOS-order disk image | 819200 | `96f682de1f49d89279fb5ea2def1c89298eb7f72efa2b41e483c2c7beb81619d` |
| `AEPCBIOS` | 8192 | `b2ac846f17f5a16ca80c80b7fb608746c72e8a2c2918dcf10ffa51f890d1a886` |
| `AEPC.SYSTEM` | 33850 | `6985d0ce1cc828886615b44532d632bbef0d73eac0072e18d33c3393acd9410f` |
| `AEPC.DISKS` | 7168 | `9efab678d448509cfbbff6921b77f1a82c2f4219845305f8b890f629c7cb3bfc` |

Apple addresses below use the files' load addresses: `$2000` for `AEPC.SYSTEM` and `$1400` for `AEPC.DISKS`. File offsets remain the safer references if a loader relocates a routine. Mixed 65816 sections in `AEPC.SYSTEM` require the correct CPU mode; this review does not treat a linear 65C02 decode of the whole file as valid code.

## Findings that affect the design

### 1. P1: Do not use MAME's routing approximation as the hardware contract

Study lines 19, 64, 67–79, 111 and 123.

MAME's `port_routing()` reads `RAM[$BF400 + (port & ~1)]`, then selects a nibble. Entries therefore start at `$BF400`, `$BF402`, `$BF404`, and so on. The phrase “one byte per even/odd port pair” needs this two-byte stride. Compacting entries with `port / 2` would read the wrong controls.

The patent selects separate read and write controls within each nibble. For an even byte, write uses D0 for wait and D1 for notification; read uses D2 and D3. Odd bytes use D4–D7. Word cycles combine the two lanes. Waiting without a new notification and notification without waiting are both supported. [Designers' patent, control-byte and state tables](https://patents.google.com/patent/US4888680A/en).

The current MAME handler instead traps every nonzero nibble on writes and uses nibble bit 0 to choose its read retry path. For example, nibble `$1` makes MAME trap a read although its read-control bits are both zero under the patent's table. Its word handler also consults the starting port's nibble rather than combining both lanes.

**Required correction:** label this as MAME's approximation. Record the actual table bytes written by the original loader, decode them by direction and lane, and compare the resulting behavior before fixing the FPGA contract. The patent is evidence about the design; shipped-card behavior still needs confirmation.

### 2. P1: The “unknown” timer block has strong identification evidence

Study lines 36, 48, 101 and 123.

`AEPC.SYSTEM` programs `$CF28–$CF2B` in the pattern of an 8253/8254-compatible timer. This agrees with MAME's board description, which identifies a discrete NEC uPD71054, although its Apple register handlers do not connect this block to the timer device.

| Likely register | Original software evidence | Assessment |
| --- | --- | --- |
| `$CF28`: timer channel 0 | `$4244` writes `$12` to `$CF2B`; routines at `$4254` and `$4258` write/read `$CF28`. | Strong inference. `$12` selects channel 0, low byte, mode 1, binary. |
| `$CF29`: timer channel 1 | At `$6EB9`, write `$74` to `$CF2B`, then `$12`, `$00` to `$CF29`. File offset `$4EB9`. `AEPC.DISKS` repeats this at `$1500`, file offset `$0100`. | Strong inference. Channel 1, low/high bytes, mode 2, count 18. |
| `$CF2A`: timer channel 2 | At `$6F48`, write `$B0` to `$CF2B`, then `$A9`, `$04` to `$CF2A`. File offset `$4F48`. | Strong inference. Channel 2, low/high bytes, mode 0, count 1193. |
| `$CF2B`: timer control | The channel selections above match the three adjacent data registers. | Strong inference. Replace the “length and value” guess. |

The [Intel 8254 data sheet, control word format on printed page 6](https://www.cs.cmu.edu/~410/doc/8254.pdf#page=6) supports these control-byte decodes. At the study's 1.193 MHz clock, 1193 counts are about 1 ms. This is a calculated interpretation, not a clock measurement.

The same routine then writes `$01` to `$CF31`, polls bit 5 of `$CF31`, and clears `$01` through `$CF30`. Later it loads count 106 and repeats the poll. This strongly suggests timer gate/status behavior in the control block. MAME currently derives read bit 5 from its V30 HALT state and has no timer action on write bit 0. Do not infer read functions from the corresponding write bit names.

**Required correction:** add timer counters, latches, control words, gates and output-status wiring to the early compatibility work. Reproduce this calibration sequence before assuming that MAME's control reads can start the unmodified launcher. The physical clock, gate polarity, and status-bit mapping remain open.

### 3. P1: Word I/O and a stalled instruction need a precise completion contract

Study lines 100–101.

“A Transporter word access must trap once with the 16-bit flag” is too broad. The original V30 uses one bus cycle for an even-addressed word I/O access and two for an odd-addressed word access. Preserve the byte lanes and the separate bus transactions in the latter case. Verify the port-address limit and decode separately. [NEC V20/V30 user's manual, section 4, printed page 4-3](https://www.dosdays.co.uk/media/nec/NEC-V20-V30-Users-Manual.pdf).

The proposed instruction rewind is also incomplete for string I/O. In the vendored core, `INSB`/`INSW` write guest memory and update DI/CX around the port callback. Rewinding IP alone after that callback can leave a dummy write, advance the destination, or lose a repetition. A callback that reports “pending” must prevent architectural commit, or preserve enough state to resume the current transfer exactly once. Apply the same check to `OUTS`, both directions of DF, and interrupts/reset during a wait. [Current instruction handlers](../third_party/xtulator_cpu/cpu.c#L1977).

**Required correction:** distinguish one architectural instruction, one iteration, and one external bus transfer. Specify the suspension boundary before selecting or adapting the interpreter. Test aligned and unaligned word I/O plus repeated string I/O, with delayed replies.

### 4. P2: MAME's pointer does not wrap at 24 bits

Study lines 43 and 123.

MAME stores `m_offset` in a `uint32_t`. `$CF00–$CF02` expose and replace only its low 24 bits; `$CF03` increments the full value without a mask. `ram_r()` returns `$FF` at and above `$C0000`; `ram_w()` drops writes there. These are distinct limits. [Pointer type and RAM helpers](https://github.com/mamedev/mame/blob/ce12c0474a7f5786748327d0320179344bddffba/src/devices/bus/a2bus/pc_xporter.cpp#L207) · [Pointer access handlers](https://github.com/mamedev/mame/blob/ce12c0474a7f5786748327d0320179344bddffba/src/devices/bus/a2bus/pc_xporter.cpp#L791).

| Boundary | Current MAME behavior | Hardware question |
| --- | --- | --- |
| `$00FFFF` to `$010000` | Carries into the third exposed byte. | Carry through the bank byte. |
| `$0BFFFF` to `$0C0000` | Leaves populated RAM; following data reads return `$FF`. | Absent-RAM decode, independent of counter wrap. |
| `$0FFFFF` to `$100000` | Continues above 20 bits. | The patent describes a 20-bit counter. |
| `$FFFFFF` to `$01000000` | Visible pointer bytes read zero, but the internal address remains outside RAM. Rewriting all three exposed bytes preserves the hidden high byte. | This emulator artifact is not evidence of physical-card behavior. |

Original `AEPC.DISKS` at `$149F–$14BA` checks repeated pointer reads, increments through `$CF03`, and checks low-byte carry into the middle byte. It does not settle the 20- or 24-bit wrap case.

**Required correction:** say “three exposed address bytes backed by an unmasked 32-bit variable in this MAME revision.” Keep hardware width open. Add explicit boundary tests for read and write on both data ports, and page-window accesses across the end of RAM. Do not silently copy the hidden-byte artifact into RTL.

### 5. P2: Control readback and the port window need separate read/write maps

Study lines 41, 46, 63–65 and 92.

The claim that `$CF30` reads the control value follows MAME's header comment, but its read handler returns `(m_regs[0x330] & 0x7f) | m_pc_data_pending`. The control writes do not maintain that backing byte. Bit 7 therefore reports pending data in this implementation, not IRQ enable. `$CF31` combines stored low bits, HALT/RESET status, and transfer direction/width. [Actual read handlers](https://github.com/mamedev/mame/blob/ce12c0474a7f5786748327d0320179344bddffba/src/devices/bus/a2bus/pc_xporter.cpp#L859).

MAME also answers the IRQ masking question: `pending && enabled` drives Apple IRQ. Disabling IRQ retains the pending request; enabling it again raises IRQ. Acknowledging through `$CF32` clears pending. This resolves the model's behavior, while leaving hardware confirmation open. [IRQ handler](https://github.com/mamedev/mame/blob/ce12c0474a7f5786748327d0320179344bddffba/src/devices/bus/a2bus/pc_xporter.cpp#L1130).

`$C800–$CBFF` is not uniformly passive RAM in MAME. Apple accesses call the guest I/O address space. CGA installs handlers at ports `$3D0–$3DF`, so Apple `$CBD0–$CBDF` reaches device behavior. Keeping backing bytes in block RAM is useful, but those addresses still need device side effects and shared CGA state. [Apple port accesses](https://github.com/mamedev/mame/blob/ce12c0474a7f5786748327d0320179344bddffba/src/devices/bus/a2bus/pc_xporter.cpp#L761) · [CGA port installation](https://github.com/mamedev/mame/blob/ce12c0474a7f5786748327d0320179344bddffba/src/devices/bus/isa/cga.cpp#L334).

**Required correction:** give separate read and write meanings. Mark the disputed `$CF30/$CF31` bits explicitly, including the original timer-poll evidence above. Preserve device decoding inside the port window.

### 6. P2: The page-fill deadline is a requirement, not a demonstrated DDR capability

Study lines 42, 92 and 110.

Both the bank and page writes change each window immediately: `$CF05` and `$CF06` for `$CC00`; `$CF0E` and `$CF0F` for `$CE00`. Prefetch and tests must cover all four writes. The 512-byte window is only 256-byte aligned; it can span a bank boundary or the end of RAM.

For a 512-byte fill, 64 data beats at 133 MHz alone take about 481 ns. The existing HP3 interface uses AXI3 and a maximum burst length of 16, so it requires at least four bursts. That leaves about 239 ns of a 720 ns gross budget for all other delay: scheduling, initial DDR latency, arbitration, refresh, transfer gaps and staging. Burst addresses may overlap data, but the configuration provides no worst-case bound. [HP3 configuration](../scripts/create_project.tcl#L483).

**Required correction:** describe whole-window prefetch as a candidate to prove. Include bank-only updates and an immediate read of the last byte. If the bound fails, choose another backing or access design before claiming physical-slot support. The study's existing separation of simulation, implementation timing and pin measurements is appropriate.

## Focused results requested in this review

### NEC `0Fh` use: the supplied BIOS does not show a dependency

Study lines 100 and 123 can now record a bounded static result. The inspected 8192-byte `AEPCBIOS` contains twelve `$0F` bytes:

| File offset of `$0F` | Interpretation in the inspected file |
| --- | --- |
| `$0115` | Immediate in `AND AH,$0F` at `$0113`. |
| `$02EB` | Immediate in `AND DL,$0F` at `$02E9`. |
| `$0433` | Keyboard translation table data. The table starts at BIOS offset `$E3E8`; the keyboard handler selects it at `$E99E`. |
| `$065E` | Immediate in `MOV CX,$000F` at `$065D`. |
| `$0A09` | Displacement of `JB` at `$0A08`. |
| `$0BC8` | Immediate in `MOV AH,$0F` at `$0BC7`. |
| `$0D43` | Immediate in `MOV AH,$0F` at `$0D42`. |
| `$1068` | Immediate in `CMP AH,$0F` at `$1066`. |
| `$1499` | Displacement in the near `CALL` at `$1498`. |
| `$1AC6`, `$1AC8` | Character bitmap data in the font starting at `$1A6E`. The graphics code references BIOS offset `$FA6E`. |
| `$1E79` | Displacement of `JE` at `$1E78`. |

The reset jump at file `$1FF0` is `F000:E0F5`; BIOS offsets in the table use that layout. This inspection found no NEC-prefix instruction in the identified BIOS code. It does not prove anything about all DOS releases, utilities, drivers, downloaded code, or later patches. A byte search alone would falsely identify the table bytes as instructions; an execution trace remains the next check.

The current XTulator build selects `CPU_8086`: it interprets opcode `$0F` as `POP CS`. Selecting `CPU_V20` removes that decode but does not add the NEC group; the core falls back to its illegal-opcode path. Also, its `CPU_NO_SALC` setting makes `$D6` fall through to `XLAT`, rather than merely removing SALC. The 186 handlers exist in the source but the current build excludes them. [Configuration](../third_party/xtulator_cpu/cpuconf.h#L22) · [0F handler](../third_party/xtulator_cpu/cpu.c#L1296) · [186 handlers](../third_party/xtulator_cpu/cpu.c#L1879) · [D6 handler](../third_party/xtulator_cpu/cpu.c#L3045).

Keep the feasibility experiment, with an explicit CPU profile and diagnostics for unsupported instructions. Do not call that profile a complete V30. NEC bit-field `INS`/`EXT` in the `0Fh` group are different instructions from byte/word port-string `INS`/`OUTS`. Full V30 compatibility still requires the intended NEC instruction coverage, even if the first BIOS boot does not exercise it.

### Other unknown registers: useful evidence, incomplete bit maps

| Register | Static evidence | What remains open |
| --- | --- | --- |
| `$CF33` read | `AEPC.SYSTEM` at `$36AD` tests `$CF32` bit 6, then reads `$CF33` at `$36BA`. It rejects values at/above `$E0`, masks bit 7 for a key table, and distinguishes make/break through the saved sign bit. | Strong evidence for a keyboard scan-code input and a related ready/status bit. Read-to-clear behavior, buffering and IRQ effects need a trace. |
| `$CF33` write | At `$4B62–$4B6E`, software writes `$04` to either `$CF32` or `$CF33` based on the sign of A. | The write side participates in a control operation. Do not assume it writes keyboard data or behaves like the read side. |
| `$CF37` write | `AEPC.DISKS` sets the `$CC00` base to `$BF000`, writes `$03` to `$CF37`, then tries `$A5` and `$52` in `$CC00`. On failure it repeats with `$01`. File offsets `$00BC–$00F4`. `AEPC.SYSTEM` has a matching write/test helper at `$6E7D`. | This is part of card/RAM probing. The exact RAM enable, configuration or selection function remains unknown. Avoid naming a specific bit function yet. |

This evidence changes the work plan: keyboard scan-code input and the timer/control block belong in the first original-software milestone. `$CF37` needs at least an explicit probe-compatibility test. MAME's generic stored-byte fallback does not establish those functions.

### TransDrive: retain the ISM model and specify its side effects

The study correctly places this controller on the Apple side and separates it from a standard PC floppy-controller/DMA implementation. The pinned MAME source now provides several concrete details that the study leaves open. These describe that model; they still need comparison with original hardware.

| Register or signal | Behavior in the pinned model |
| --- | --- |
| `$CF20` / `$CF21` read | Pop FIFO entries as data or mark. Empty reads set an error; reading a mark as data also sets an error. |
| `$CF22` | Reading returns and clears the error register. Writing queues a CRC operation. |
| `$CF23` | Parameter RAM access advances its index modulo 16 on reads and writes. |
| `$CF26` | Reads mode. Writes clear selected mode bits and reset the parameter index. The card adapter prevents clearing ISM-select bit 6. |
| `$CF27` | Writes set selected mode bits, forcing bit 6 set. Reads return handshake, not the mode value. |
| Mode bits | 0: FIFO clear; 1/2: drive selects; 3: action; 4: write/read; 5: side; 6: ISM; 7: motor. |
| Drive and motor | Motor-on gates drive selection. Select values 1 and 2 select the two drives; 0 and 3 select none in the adapter. |
| Phase lines | Phase 0 drives direction; phase 1 drives step. Phase 2 reads low at track zero. |
| Handshake SENSE, bit 3 | The adapter sets it when a drive exists and `!wpt_r()` is true: writable, not protected. Preserve that polarity. |
| Side select | The adapter inverts the ISM head-select level for PC drives. |

Sources: [card adapter and signal wiring](https://github.com/mamedev/mame/blob/ce12c0474a7f5786748327d0320179344bddffba/src/devices/bus/a2bus/pc_xporter.cpp#L665) and [ISM register behavior](https://github.com/mamedev/mame/blob/ce12c0474a7f5786748327d0320179344bddffba/src/devices/machine/swim1.cpp#L187).

The card's model exposes PC 360 KiB 5.25-inch and 720 KiB 3.5-inch drive types. Its comments describe native Apple 3.5 Drive status signaling, but the adapter implements PC-style phase/SENSE behavior. Treat native Apple 3.5 Drive support as separate work. [Configured drive types](https://github.com/mamedev/mame/blob/ce12c0474a7f5786748327d0320179344bddffba/src/devices/bus/a2bus/pc_xporter.cpp#L296).

A sector image can supply ordinary media contents, but it does not supply the controller behavior. The implementation still needs track/sector ordering, marks, CRC, FIFO service and overrun/underrun, rotation, head/step timing, and write protection. At 250 kbit/s, a data byte takes about 32 microseconds; the stated 2-microsecond MFM half-cell is a different timing unit. Formatting and unusual tracks need a defined image-representation policy. Add read, write, verify, format, wrong-CRC, missing-sector, write-protected, no-drive and drive-switch cases to acceptance.

The source has no direct ISM-to-PC DMA/IRQ6 connection. That does not mean DOS sees no floppy ports or interrupt completion: Portware can implement guest-visible behavior and request a V30 interrupt through the Apple bridge. Keep physical wiring and guest service separate in the wording.

## Remaining full-document observations

- Line 36 says nothing drives the speaker level. MAME has a PIT output callback that calls `level_w()`, but the card file has no assignment to its `m_pc_spkrdata` gate. Describe the sound path as incomplete rather than absent.
- Replace mutable MAME links and loose code-address notes with the commit and input hashes above. Some comments conflict with their handlers; an unqualified “MAME models this” can hide that distinction.
- The baseline test note still matches the local source: the AD8088 source test expects config version `117U`, while the firmware defines `124U`. This review did not rerun software tests or builds because it changes no implementation. Preserve the prior test result as a dated baseline, not a new execution result.
- The first-host scope, separate RAM-disk ownership test, original Portware storage milestone, and separate Vivado/Vitis/package/physical-board gates remain appropriate. None of the new static evidence proves those gates pass.

## Next evidence to collect

1. Trace original timer initialization and `$CF30–$CF33` reads/writes, including the `$6F48` calibration routine and keyboard input. Record counters, gate transitions, status transitions, and Apple PCs.
2. Capture the original routing-table bytes and compare byte reads, byte writes, aligned words and odd words against the direction-specific controls. Include pending-request changes and IRQ masking.
3. Run explicit pointer and page-boundary diagnostics. Separate populated-memory limits from address-counter width.
4. Trace executed CPU opcodes through BIOS and a verified bootable DOS image. Keep the static BIOS result and the runtime instruction coverage as separate evidence.
5. Prove the arbitrary-address RAM deadline with all four page-base registers, then add the original-software boot and TransDrive tests. Resolve any model divergence before treating an emulator result as a hardware specification.
