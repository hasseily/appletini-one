# PC Transporter test mode

The PC Transporter test mode uses virtual slot 5. It provides a guest CPU, shared RAM, Apple I/O registers, CGA output and USB keyboard input. Use the test mode to collect startup and bus timing data. [Design study](docs/PC_TRANSPORTER_STUDY.md)

## Setup

1. Use a matching Appletini FPGA build and ARM frontend.
2. Use an enhanced Apple //e with physical slot 5 empty.
3. Select **PC Transporter (test)** under **Processor card**. Enable slot 5.
4. Boot your original AE system disk. Use the 2.0.5 disk identified in the study for comparison with the reference trace.
5. Enter `pct status` in the Appletini UART console. Record the status, transfer, guest address and fault counters.

The ProDOS hard disk file must contain a bootable DOS volume. Use a copy for tests that write data. The distributed utility volume and a DOS boot disk serve different purposes. [Media details](docs/PC_TRANSPORTER_STUDY.md#storage-and-operating-modes)

| `pct status` bits | Meaning |
| --- | --- |
| 0 | Slot 5 interface enabled |
| 1, 2 and 3 | Guest reset, halt and INT, respectively |
| 4 and 5 | Apple IRQ enabled and transfer pending |
| 6 | All queued RAM writes completed |
| 7 | The memory bridge detected a fault |
| 15:8 and 23:16 | Acknowledgment and reset sequence numbers |
| 31:24 | Guest interrupt request sequence number |

## Implementation choices

- The FPGA uses the existing C8 ownership and slot isolation logic. The pointer and page windows address the reserved DDR section at `0x3F900000`. The ARM maps this section without caching.
- The Apple and guest share one backing store for `$BF000–$BF3FF` and the `$C800` port window. Confirm this alias on original hardware.
- Disk II has priority when both clients request HP3. The bridge checks the early Apple address against the later sample before it uses prefetched data. A response that misses the deadline cannot drive a later bus cycle.
- The FPGA counts late reads, dropped writes and DDR errors. The ARM stops the guest when it detects a memory fault. It also reports an unsupported guest opcode and its address.
- The candidate pointer wraps at 24 bits. The candidate routing logic uses the patent's direction bits and combines the two word lanes with OR. Confirm both choices on original hardware.
- Port window and port RAM reads reply three fabric clocks after the address sample. Other reads reply one clock after it. The physical bus drives card data 16 clocks after that sample, so these delays do not change Apple bus timing.
- The generic control bytes use distributed RAM with one write port. An Apple write to one of these bytes reaches the RAM one clock after its commit. A timer read returns the count from one clock earlier.
- The timer model covers the Apple calibration sequence and basic binary counter operations. Timer channel 0 sets the `$CF32` bit 5 service flag. Confirm the timer connections and IRQ timing against the card.
- The guest uses a separate XTulator profile with 186-class I/O instructions. The NEC `0Fh` instruction group remains open.
- A queued key requests Apple service through `$CF32` bit 6. A `$CF33` read clears the ready flag. Confirm the read and IRQ behavior on original hardware.
- A guest INT remains pending in `$CF30` bit 6 until the CPU accepts it or the Apple clears it. The FPGA checks the interrupt and reset sequence numbers before it accepts a CPU reply.
- The compositor shows CGA while the guest runs and CGA video is active. Graphics use the CRTC's displayed columns, displayed rows, and scanlines per row. CPU1 captures each active line for modes that reuse a CGA memory bank within a frame. The compositor can use a complete capture while the next frame is being collected.

## Tests

Run the host and RTL tests from the repository root.

```powershell
python -B scripts/test_pc_transporter.py
python -B scripts/test_axisimple_wrapper.py
python -B scripts/test_applicard_card.py
python -B scripts/test_ad8088_card.py
```

On Windows, the simulators can require access to the Xilinx compiler and its temporary files. Use `--host-only` or `--rtl-only` to run one part of the Transporter suite.

Use the 65C02 bus probe before running the AE software on an enhanced //e. It checks the slot and C8 response, pointer carry and wrap, both page windows, and reads after writes. It uses test bytes in card RAM. Run it in a fresh session with the guest in reset. [Probe setup and result codes](software/pc_transporter_probe/README.md)

Use the MAME reference collector with your own ROM and disk files. Use the MAME source revision listed in the study for a source-matched comparison.

```powershell
python scripts/pc_transporter_mame_trace.py --mame <mame.exe> --rompath <ROM-dir> --disk <2.0.5.zip> --out <new-dir> --seconds 20 --key-time 5 --keys 1
python scripts/test_pc_transporter.py --host-only --bios <AEPCBIOS> --routing <new-dir>/routing.bin
```

The host fixture loads the original BIOS and the routing bytes from the reference trace. It checks the reset jump, the `$3D8` and `$61` write requests, and the first blocking read at `$62`. It supplies synthetic replies. This test covers only early BIOS execution.

The candidate uses the captured routing bits for port `$3D8`. MAME handles this port in its CGA device. Compare both behaviors with an original card trace.

## Results

| Test | Result |
| --- | --- |
| Original BIOS in the Appletini host fixture | Reached the first blocking Portware read. The fixture checked the exact BIOS and routing file hashes. |
| Guest and CGA host tests | Passed. They cover reset, request replies, aligned and split word I/O, REP, and CGA text and graphics. The AD8088 CPU test passed with both guest profiles linked. |
| FPGA simulation | The bridge passed with the repository soft-switch manager and a synthetic memory backend. The tests cover C8, the port RAM alias, late replies and CGA writes. They also cover calibration, timer and keyboard service, and guest interrupt acknowledgment. |
| GP0 and HP3 simulation | The Transporter MMIO address and the existing Disk II MMIO bench passed. The shared DDR bench checked request priority and reply ownership. |
| 65C02 bus probe | The probe passed with a software register fixture. Eight injected faults gave the expected codes. Pinned MAME booted the ProDOS disk and ran the probe. The probe stopped at its idle check because MAME returned `$00` from `$CF31`, with the reset bit clear. |
| Cortex-A9 build | F1.3.7 built the FSBL, BSPs, CPU1 and CPU0 from the selected XSA. The package audit checked the FPGA and all ARM data. The firmware manifest and CRC checks passed. |
| FPGA implementation | Passed. The full Vivado build gave +0.182 ns setup slack and +0.036 ns hold slack with no failing endpoints. It exported an XSA. The build used the standard limits and a 0.215 ns implementation margin. |

The live ONE//e data byte carries every card reply. SuperSprite, AppliCard and the Mockingboard now read the byte that the bus captures before `data_en`, as vTW and PSRAM do. These cards read data only at `data_en`, so their behavior does not change. An SSI-263 read still takes D6–D0 from the live byte.

Four runs of this source used margins from 0.200 to 0.230 ns. Two met timing, at +0.182 and +0.094 ns. Two missed: one by 0.003 ns on the DIR_D pin, and one by 0.031 ns in the vTW CPU. In both passing runs a direction pin sets the worst path, and every fabric path has at least +0.22 ns. No Transporter path is near the limit. The result changes with placement. See `build/pct_dataphase_m215/.timing_runs/20261107T063008Z-10645c4b-full`.

On F1.3.7, `PCTTEST.PO` reached its first V30 tests in ONE//e at 1 MHz. The live console reported 1,284,044 late RAM reads, zero dropped writes and zero DDR errors. The guest stopped at `FFFF:0000` with zero illegal instructions. The ONE//e bus trace recorded `$FF` on reads from `$CE00`, where the suite waits for the guest's `$A5` marker. The exact first guest program passed in the host fixture.

F1.3.7 rejects early read requests while `/DMA` is low. ONE//e presents the vTW CPU as a virtual DMA master, so the Transporter has to start its DDR reads at the later address sample. Its 12-clock timeout then expires before the virtual bus captures the reply.

The ONE//e fix uses the early address that the virtual bus has already captured, even when vTW owns that bus. A read that starts at phase 73 can publish its reply through phase 122. The virtual bus captures the byte at phase 123. The physical bus keeps its 12-clock timeout and rejects early requests from physical DMA masters.

The regression uses the real ONE//e bus and HP3 bridge with a mock DDR target. It captures `$A5` on the last safe reply and counts one miss on the next clock. The first two Vivado runs with this fix missed timing, at −0.006 and −0.182 ns. The data-phase change above closed timing. The fix still needs a board run.

If an earlier RAM transaction blocks the ONE//e address sample, the Transporter now retries the early read when the transaction completes. It can use the rest of the window before the later address sample. The regression holds a write past the early sample, then checks that a read of a different byte still gets its DDR reply before the bus captures data.

`CGADEMO2.PO` reported `E6` because its slot scan expects `$03` and `$00` at offsets `$05` and `$07`, and `$AE $50 $43 $54` at offsets `$F5`–`$F8`. `PCTTEST.PO` checks the same bytes. The FPGA now returns those bytes for its selected slot. The demo scan passes with these replies in simulation.

The board then detected the card and option 1 reported `E9`. The demo starts the V30 and waits for vertical retrace on `$CF31` bit 3, then counts line edges on bit 0. Both bits had returned stored control data. The FPGA now uses the programmed CGA timing for these reads, including during mode changes. Option 1 also reuses VRAM during scanout to draw a 768×240 picture. The current compositor draws a 640×200 image from VRAM once per output frame.

With retrace working, the board reported `EA` and left mailbox bytes `01 02 A5 B0` at `$CE00`–`$CE03`. The V30 had started but missed the first image copy deadline. It needs 106 interpreter slices at the 64-unit budget in about 6.76 ms. The ARM now runs short bursts at foreground checkpoints and between groups of CGA rows. `pct status` reports slice count, longest slice time, and longest interval between slice starts for the latest guest run.

The next board run finished that copy and left mailbox bytes `02 01 A5 F0`, which identifies the next-frame deadline for command 2. The ARM uses native 16-bit loads and stores for aligned words in guest RAM, including CGA VRAM. `pct status` also retains the guest's resume address and CX from the last slice when the demo resets the guest after an error.

A clean board retry completed three animation frames. On the fourth it stopped at `2000:051F` with `CX=0072`, leaving 114 words in the final copy. It recorded zero FPGA read misses. The CGA renderer now gives the guest a short burst before drawing and after every four rows. The status output also records where the longest gap began and ended, how often a poll found pending Apple writes, and how long elapsed between the last slice and the observed stop.

The four-row build completed 116 frames before the next `EA`. It left 44 words in the final copy and logged a 1.632 ms gap between foreground slices. The once-per-second RTC and temperature reads block on I²C and can take about that long. The frontend now runs guest slices while it waits for those transfers. `pct status` counts the slices run during I²C waits.

The first run after an update reported `E8` with 82 late Apple reads and zero V30 slices. After a card reboot, option 1 reached frame 42 and reported `EA` with one late read and a latched fault. The mailbox contained the guest's completed acknowledgment, and the longest slice gap fell to 293 µs. ONE//e now waits at data capture for a pending Transporter read, up to 512 clocks beyond normal capture. It publishes one data phase after the reply or timeout. The physical Apple bus keeps its original deadline. `pct status` counts held reads and reports the longest hold in fabric clocks.

On the next board run, option 1 kept running but showed scrambled and off-center monochrome graphics. The demo programs 768×240 at 96 bytes per line and reuses the first 3,328 bytes of each 8 KiB bank during scanout. A frame snapshot can therefore show the lower picture data in the top 68 lines. The graphics renderer now uses the programmed stride and centers the picture in the output. The scanline capture reads each row before the demo reuses its bytes.

The board then displayed the correct image. Apple text stayed visible around it until opening and closing the Appletini menu forced a redraw. The compositor now clears the old picture when it switches between Apple video and CGA. Option 1 polls the Apple keyboard at `$C000` once per frame. The demo leaves Apple IRQs masked, so its USB keys now also reach the ONE//e keyboard. Portware enables Apple IRQs when it starts the guest and keeps exclusive PC input during brief IRQ masks.

Option 1 redraws the same artwork every frame. Option 2 loads only 16 KiB and keeps the V30 halted, so the bottom repeats part of the top. The board initially stayed on Apple text because the compositor required a running V30. The compositor now shows graphics when the Apple has enabled them and its video RAM writes have finished. The original disk replay confirmed the wrap begins at pixel 256 on lines 170 and 171, counting from zero. After installing the fix and rebooting, the board showed the wrapped picture when entering option 2 from either the Apple menu or option 1. Esc returned to the Apple menu.

The 2.0 launcher draws its Mouse and IIGS LPT1 warnings into CGA text while
holding the V30. The board had both prompts in video RAM, and a key advanced
from the first to the second while the screen stayed blank. The frontend now
shows the launcher's blink-enabled CGA text after its writes finish. CGADEMO2
clears blink when it returns to its Apple menu.

Holding Space during launch enters the 2.0 Control Panel. The board drew
the full menu in 80-column CGA text with blink off while the V30 stayed
halted. The frontend now shows that mode as well. Menu keys still go to
the Apple keyboard while the guest is halted.

The Control Panel selects a menu number with a full-height CGA cursor. The
board stored the normal `1` glyph under that cursor. The renderer uses the
CGA frame counter to blink the cursor and reveal the selected number.

Reboot and Run PC Transporter clears a stopped V30 after an unsupported
opcode. The opcode count stays available in `pct status`. Apple-bus memory
faults still stop the guest across Transporter reboots.

The FreeDOS CGA Demo disk adds `CGAMOVE.COM` to the clock-sync image. Run
`CGAMOVE` from `C:\>` to see four color swatches, fixed screen edges, and a
bouncing checkerboard in 320×200 CGA mode. The palette flips once at each
bounce, including corners. Press Esc to return to DOS.
The PC Transporter MAME build booted from C:, ran `APLCLOCK`, verified
71 palette changes through the first corner bounce, and accepted another
DOS command after Esc.
The [demo source](software/pc_transporter/cgamove.s) includes build commands.

The slot-defaults disk saves `SSCLPT1` in slot 1 and `MOUSECOM` in slot 2
through the Transporter Control Panel. MAME reopened both assignments, booted
FreeDOS from C:, and reported 640 KiB of conventional memory. Enable the
Super Serial Card and MouseCard in Appletini to use those slot assignments.

The FreeDOS EMS disk loads `PCTEMS.SYS` from `CONFIG.SYS` and skips the
timed FreeDOS startup prompt. The LIM EMS 3.2
driver exposes 4 MiB as 256 pages through four 16 KiB windows at `D0000h`.
FreeDOS still sees 640 KiB of conventional RAM. Run `EMSTEST` from C: to
check allocation, page contents, map save and restore, and release. The
[driver source](software/pc_transporter/pctems.s) includes build commands.
MAME booted the disk with a model of the EMS ports and ran `EMSTEST`. Native
tests checked the firmware implementation, including frame edges and reset.

With PC Transporter selected, **Slot 5 Processor → Resource usage → Maximum**
raises the foreground guest CPU budget from 250 µs to 1 ms per poll.
The video and I²C budgets stay at their Standard values. Both settings
check FPGA reset, halt, faults, and pending I/O between 64-unit slices.
`pct status` reports the selected mode as `cpu_speed`.

The EMS and Tests disk opens `PCTMENU` after `APLCLOCK`. Options 1–3 run
`PCTBENCH`, `EMSTEST`, and `CGAMOVE` and then return to the menu. Option 4
returns to DOS. Type `PCTMENU` to open it again.

Run the speed test once in Standard and once in Maximum. Compare CPU
iterations/s and RAM KiB/s separately. Higher scores mean more work per
second. If the BIOS timer stops, the test switches to stopwatch mode and
marks each fixed workload with START and DONE. `PCTBENCH /M` also selects
that mode. MAME checked all four choices and both benchmark timing paths.

On the board, dev11 reported 477,102 iterations/s and 2,662 KiB/s in both
settings. The two workloads took 40 and 14 BIOS ticks respectively in each
run. The UART confirmed Maximum was active and recorded zero Apple-bus memory
faults. The precision benchmark uses the ARM's elapsed timer on dev12 and
keeps the BIOS timer and stopwatch options for earlier builds.
Maximum on dev12 also fetches aligned 32-bit blocks of conventional RAM for
instruction decoding. It discards each block before the next instruction so
guest or Apple writes are visible on the next fetch. The RAM stays uncached.

On dev12 the benchmark clock could jump when the ARM counter crossed a
boundary. The board reported `4294959 ms` for one CPU run after the clock
moved backward and the unsigned elapsed time wrapped. Dev13 fixes the
clock conversion and uses the same precision benchmark disk.

In the dev13 board retest, Standard took 2,900–3,150 ms for the CPU loop
and about 900 ms for the 2 MiB RAM copy. Maximum took 1,940 ms and 810 ms.
That gives 1.49–1.62× the CPU-loop throughput and about 1.11× the RAM-copy
throughput on these workloads.

Released MAME 0.289 read `$CF31=$00` for 1,192,383 polls in the original calibration loop. In a separate experiment, a temporary timer model completed both polls and reached `AEPCBIOS`.

MAME at `ce12c0474a7f5786748327d0320179344bddffba` returned `$CF31=$21` on the first poll of both loops. It fetched the real BIOS reset vector and observed an Apple acknowledgment for port `$61`. Both builds recorded the same 1,024-byte routing table. Its SHA-256 is `75692784f1fe5e8aa4bf7bcf587f76fb40b5292ef9b9d8ba3504add5f9250b9a`.

With a 360 KiB FreeDOS disk, the pinned MAME build reached the kernel and drive enumeration by 45 emulated seconds. The capture ended before a command prompt appeared. The test disk SHA-256 is `b5c3a868a375952e52ed89feecfdb0a90c96af32bf0663e424a7bdd3f15b1a48`. [FreeDOS disk source](https://jorisvr.nl/article/freedos)

The Appletini board still needs a connector timing test and a complete DOS boot with the original Apple software. Measure cold reads and bus deadlines under video, storage and ARM load. Verify `TRANSFER.EXE` in both directions and check that the data remains correct after restart. TransDrive and the original floppy media path remain open.
