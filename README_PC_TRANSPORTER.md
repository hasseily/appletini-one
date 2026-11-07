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
- The compositor shows CGA while the guest runs and CGA video is active. CGA retrace status and AE Control Panel display changes still need work and tests with the original software.

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
| FPGA implementation | Passed. The full Vivado build gave +0.068 ns setup slack and +0.024 ns hold slack with no failing endpoints. It exported an XSA. The build used the standard limits and a 0.215 ns implementation margin. |

The worst setup path runs from an ARM register write through the vTW copy engine to a shadow RAM write enable. No Transporter path is near the limit. The result changes with placement. Four runs of this source used margins from 0.200 to 0.225 ns. Three met timing, at +0.068, +0.034 and +0.024 ns, and one missed by 0.081 ns. See run `20261107T033658Z-34f07ced-full` under `build/pct_timing_fix6_m215/.timing_runs`.

Released MAME 0.289 read `$CF31=$00` for 1,192,383 polls in the original calibration loop. In a separate experiment, a temporary timer model completed both polls and reached `AEPCBIOS`.

MAME at `ce12c0474a7f5786748327d0320179344bddffba` returned `$CF31=$21` on the first poll of both loops. It fetched the real BIOS reset vector and observed an Apple acknowledgment for port `$61`. Both builds recorded the same 1,024-byte routing table. Its SHA-256 is `75692784f1fe5e8aa4bf7bcf587f76fb40b5292ef9b9d8ba3504add5f9250b9a`.

With a 360 KiB FreeDOS disk, the pinned MAME build reached the kernel and drive enumeration by 45 emulated seconds. The capture ended before a command prompt appeared. The test disk SHA-256 is `b5c3a868a375952e52ed89feecfdb0a90c96af32bf0663e424a7bdd3f15b1a48`. [FreeDOS disk source](https://jorisvr.nl/article/freedos)

The Appletini board still needs a connector timing test and a complete DOS boot with the original Apple software. XSDB found no JTAG target during this run. Measure cold reads and bus deadlines under video, storage and ARM load. Verify `TRANSFER.EXE` in both directions and check that the data remains correct after restart. TransDrive and the original floppy media path remain open.
