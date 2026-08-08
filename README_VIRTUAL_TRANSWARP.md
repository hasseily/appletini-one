# Virtual TransWarp — Design Document

## Fixed Enhanced-//e acceleration

Accelerating presents a **fixed Enhanced //e on every host**, //e or II+.

- **Embedded CPU ROM.** The 16 KB Enhanced //e ROM (`docs/Apple2e_Enhanced.rom`
  → generated `ps_sources/frontend/apple2e_cpu_rom_data.c` via
  `scripts/gen_apple2e_cpu_rom_c.py`) is loaded into the shadow ROM region on
  every takeover (`VTW_ST_LOAD_ROM`).
- **One translate path.** The core always uses the //e MMU model. A
  II+ running the //e ROM drives the //e switches, which the private tracker
  follows.
- **Synthesized //e status reads.** `$C011–$C01F` are served internally from
  the tracked switch state without a bus cycle — a II+ has no //e IOU to
  answer them (`X_STATUS_DONE`). `$C019` RDVBLBAR uses the native VBL boundary
  at `192:0`; hardware validation showed that a vTW may be cycle accurate but
  it doesn't have the correct VBL timing. It fluctuates generally one cycle
  early.
  RDVBLBAR bit7 uses the //e AppleWin convention (1=active display, 0=vblank).
- Character ROM is the user's responsibility (Video ROM config); the built-in
  default is already a //e set. Functional coverage: `tb_vtw_video`.

Components:
- `hdl/apple/w65c02_core.sv` — cycle-exact W65C02S core (2.54 M
  SingleStepTests vectors, Klaus Dormann functional/extended/decimal/
  interrupt suites, 266 directed checks; routed standalone at 133 MHz,
  WNS +0.631 ns).
- `globals.sv::translate_apple_addr` — the //e banking translator as a
  shared function over an explicit `TranslateState`: the vTW's private
  switch copy and the motherboard tracker use one source of truth.
- `hdl/apple/vtw_shadow.sv` — 144 KB dual-port shadow (main 64K, aux 64K,
  ROM 16K; LC folded by the translator's bit-12 remap). Port B = ARM.
- `hdl/apple/vtw_bus_engine.sv` — sync-cycle FSM, 512-deep posted write
  queue (flush-before-video-window + bank-steer flush ordering),
  parked-cycle driver, PHI1-disciplined /DMA with full bus release during
  Apple RES# windows (§2 RES# row). Drives addresses at the wrapper's
  `TAP_DRIVE_ADDR` strobe (`ab_read.drive_en`, fall+8).
- `hdl/apple/vtw_core_top.sv` — core socket + private `soft_switch_manager`
  instance (synthetic serve_en pulses) + routing + CE pacing (full / divided
  / cycle-locked 1 MHz on `data_en`) + $C074 decode + ARM boot-copy port.
- `apple_top.sv` — vtw_core_top as the arbiter's 11th client; effective
  enable = config bit AND machine ∈ {//e, II/II+}, latched for the whole
  session so CTRL-RESET cannot tear a running session down. Accelerated
  sessions are always an Enhanced //e (no per-host flavor); `video_vbl`
  feeds the core's synthesized `$C019`;
  AxiSimple window 0x6C–0x7F (CTRL, auto-incrementing shadow port, ARM
  sync-cycle command/status, status/counters, bus-trail rings).
- `ps_sources/frontend/vtw_service.c` — session sequencer: waits for the
  boot ROM's machine report (//e or II/II+), takes the bus, asserts RES#
  (~100 ms) so the takeover can never land mid-protocol, invalidates
  $03F3/F4 (forces the autostart cold path; the shadow persists across
  sessions), loads the embedded Enhanced //e ROM into the shadow via the
  port-B window (`VTW_ST_LOAD_ROM`), releases the core.
- UART `vtw [status|on|off|speed full|1mhz|div <n>]` + config-menu
  "TransWarp" tab (the slot-3 presentation): enable checkbox dimmed on a
  positive non-//e identification; presets MAX / 26 / 13 / 7 / 3.6
  / 2.6 / 1 MHz cycle-exact (divided-mode dividers 5/10/19/37/51 against the
  133.333 MHz fabric clock). Persisted keys `vtw.enabled`,
  `vtw.speed.mode`, `vtw.pace.divider`, `vtw.c074.ignore`, and
  `vtw.disk2.acceleration.disabled`. RAM-tab interplay: under
  acceleration the machine always has 8MB (lives in the shadow);
- SmartPort transport state (FIFOs, exec/ready, control) clears on Apple
  RES#, on the card and via the PS sweep + hw-EXEC-gated poll
  (`tb_smartport_reset` covers it).

Operational notes:
- IRQ/NMI are sampled at the late-PHI0 data snap with a 2-consecutive-
  sample filter: bus-drive ground bounce otherwise forges IRQ edges, and
  an unenhanced //e vectors them through its internal handler.
- Parked I/O-page addresses are sanitized to $FFFF: our parked cycles are
  clean, fully-decodable replays, and a replayed $C0xx read would
  re-execute read side effects every idle cycle.
- PAL/international //e: INTCXROM=1 with PC in internal $C2xx at an idle
  prompt is the machine's NORMAL keyboard-firmware wait state (mid-KEYIN
  via the $FBB4 trampoline), not a fault signature.

## 1. Goal and governing constraint

Add a virtual accelerator to the Appletini that makes the host Apple //e run
at configurable speed (well beyond 3.58 MHz), presented in the config menu as
a slot-3 device.

**Governing constraint: the bus contract is the physical Applied Engineering
TransWarp's, verbatim.** The Appletini's capture, serving, soft-switch, and
renderer paths are hardened and hardware-validated against exactly that
master behavior — full shadow, sparse bus traffic, held /DMA.
By emitting the same *contract* (which cycles appear on the bus and what they
mean), the virtual TransWarp requires **zero changes** to every other
subsystem, and every test we ran against the physical card doubles as its
acceptance suite. Speed is the only sanctioned deviation.

One deliberate electrical improvement that does not change the contract: our
bus cycles are driven with ideal 6502 timing (address/R-W asserted early in
PHI1, held through the whole cycle, bus actively parked between accesses).
The physical TW's late assertions and early releases are the root of its
compatibility problems; ours is the best-behaved master on the platform,
which also means our own snooping paths observe our cycles perfectly.

## 2. Bus contract (mirrors the physical TransWarp)

| Access class | Bus behavior |
|---|---|
| /DMA | Asserted (open-drain low) from Apple reset-release when the vTW is enabled; held for the whole session **except during Apple RES# windows, where the bus is fully released so the MMU/IOU process the reset under stock conditions (see RES# row)**. Transitions only during PHI1 (Apple IIe Tech Note #2). Motherboard 6502 sleeps; handback is via CTRL-RESET with the vTW disabled, exactly like `$C074 = 3` on the real card. //e and II/II+ (the `machine_inh_allowed` interlock covers both; a IIgs is refused). On a II/II+ the core's translator neutralizes the //e-only MMU switches (no aux, no internal $CX ROM, slot 3 owns $C3xx) — acceleration only, the machine's memory is what it is; the language card is tracked either way (slot-0 $C08x protocol, LC image lives in the shadow). |
| $C000–$CFFF | Real bus cycles, one per access, synchronized to PHI0, except for the Appletini's private SmartPort path and safe even reads of its virtual Disk II switches. Normal bus accesses stall the core for sync plus one Apple cycle. Disk II odd reads and all Disk II writes stay on this path; Q7 write mode also holds the whole core at 1 MHz. |
| Writes to $0400–$0BFF and $2000–$5FFF | Written through to motherboard RAM as posted bus writes (keeps a CRT/composite display live, matching AAL's documented TW behavior), for main (bank 0) AND base aux (bank 1): the bus cycle carries only the Apple address, and the motherboard's own switch state — which mirrors the core's, since it tracks the same $C0xx cycles — steers it into the right bank. A bank-steer flush rule (sync write to $C000–$C007 or any $C054–$C057 access flushes the posted queue) keeps queued writes retiring under the switch regime they were issued in; without it the capture shadow's aux banks never update and 80-col/DHGR render empty. Posted queue drains on idle bus cycles; the core does not stall unless the queue fills. |
| All other memory traffic | Invisible. Reads and writes hit the BRAM shadow only; motherboard RAM is deliberately stale outside the video windows (manual: "The Apple's memory is used only for video display"). |
| RamWorks banks (aux beyond base 64K) | When the Appletini provides aux memory (RAM tab on, no physical aux card — the same gate as the motherboard-side serving), the vTW serves all 8 MB from PSRAM **at accelerator speed**: the private translator emits bank-qualified addresses from its own $C071/$C073 tracking, and a single-line (8-byte) write-allocate cache turns sequential traffic into one PSRAM line op per 8 bytes, admitted through `psram_simple`'s existing bounded background window (priority above PS-DMA, below the Apple write queue — serve-deadline analysis unchanged). Dirty lines are written back on eviction and whenever the core drops into reset, so RamWorks contents survive CTRL-RESET and handback; the cache invalidates between sessions. While the vTW owns the bus, `psram_simple` suppresses INH read-serving (parked aux-routed replays would otherwise consume every background-admission slot and starve this port — the accelerator's real aux traffic never uses the bus), and the boot ROM's aux-slot probe report is ignored during a session (the probe reads back the shadow, not the slot). With a physical aux card (or RAM tab off) the translator never leaves banks 0/1: banked software sees clean 64K-card aliasing and correctly sizes the machine as having no RamWorks. This is better than the physical TW, which passes all aux traffic to the bus at 1 MHz. |
| Idle cycles | Bus actively parked: last real address re-driven with R/W = 1 (I/O-page addresses sanitized to $FFFF so read side effects are never replayed). No floating, no decay. |
| $C074 | Decoded by the vTW (motherboard-space register, same as the real card): write 0 = full speed, 1 = cycle-locked 1 MHz, 3 = off-until-reset. RocketChip-compatible software expects this. The optional `Ignore $C074 Speed Switch` setting discards every value and clears any value latched before it was enabled. This keeps the chosen menu or USB speed in force, but can break timed I/O and copy protection. |
| RES# | Filtered RES# resets the core through the shadow's reset vector (autostart ROM). **The bus is released for the entire reset window** — /DMA deasserted, nothing driven — **and the motherboard 6502 then runs 80 stock cycles after RES# rises before the bus is re-taken.** Both halves are required by the //e MMU: it does not process a reset while a DMA master holds the bus, and its soft-switch clear is only completed by the 6502's own post-reset activity (reset sequence + $FFFC/$FFFD vector fetch). 80 cycles covers the vector fetch and INIT's display-mode switches (snooped by the tracker) while ending well before the reset handler's first MMU write (the $FA77 trampoline's `STA $C007`, ~cycle 160); the 6502's remaining traffic is zero-page/stack writes to real RAM, which is stale under the vTW anyway. The vTW core boots from shadow in parallel and stalls on its first $C0xx sync access until the engine re-owns the bus. CTRL-RESET with vTW disabled simply never re-takes: the machine returns to the motherboard 6502 cold. |
| IRQ/NMI | The sampled slot pins (already in `ab_read`) wire directly to the core's IRQ#/NMI# inputs. Vector fetches go through the shadow with correct LC banking. Strictly better than the real TW's power-on reboot stub, with the same observable contract for a booted OS. |

## 3. Architecture — fabric 65C02

```
PL (fabric)                                        CPU0 (ARM)
+------------------------------------------+      +---------------------+
| vtw_core_top.sv                          |      | vtw_service.c       |
|  +-----------+   +--------------------+  | Axi  |  (thin: config,     |
|  | 65C02     |   | shadow BRAM        |  |Simple|   boot ROM copy,    |
|  | soft core |<->| ~172 KB dual-port  |<-+------+   $C074 state,     |
|  | (CE-      |   | A: core  B: ARM    |  |      |   diagnostics)     |
|  | throttled)|   +--------------------+  |      +---------------------+
|  +-----+-----+                           |
|        | route (translate_apple_addr)    |
|  +-----v-----------------------------+   |
|  | vtw_bus_engine.sv                 |   |
|  |  sync cycle FSM | posted wr queue |   |
|  |  /DMA + parked-cycle driver       |   |
|  |  (new ab_write arbiter client)    |   |
|  +-----------------------------------+   |
+------------------------------------------+
```

- **Core: soft 65C02 in fabric, supplied by the project owner.** The wrapper
  is written against a conventional synchronous 65C02 interface (clk, CE,
  addr[15:0], data in/out, we, irq_n, nmi_n, res_n, sync/rdy as available)
  so the specific core drops in. Regardless of source, the integration gate
  is a behavioral sanity suite in simulation (Klaus Dormann's 6502/65C02
  functional tests run against the BRAM shadow) before first hardware boot.
- **Shadow: dual-port BRAM, ~172 KB ≈ 40 RAMB36.** Budget measured against
  the current build: 36/140 tiles in use (25.7%), so shadow + core lands
  around ~55% — comfortable. Layout: 64 KB main, 64 KB aux, 16 KB main-LC,
  16 KB aux-LC, 12 KB ROM copy. Port A: the core, single-cycle. Port B:
  AxiSimple-mapped ARM access for boot-time ROM copy, debug peek/poke, and
  state dumps — a built-in debugger for free.
- **Memory routing: reuse `translate_apple_addr` in RTL.** The vTW's bank
  selection (main/aux/LC/ROM/bus classification) is exactly the function the
  soft-switch manager already owns. The core wrapper instantiates the same
  function against a private copy of the switch state, which the vTW tracks
  from its own accesses (it is the CPU; it sees every $C0xx access it makes
  before the bus does). One source of truth for //e banking semantics.
- **Speed: clock-enable throttling.** The core runs at fabric clock with a
  programmable CE pattern: full-rate bursts (BRAM-fed, tens of
  MHz-equivalent), M/N fractional rates for the config-menu presets, or
  CE locked to the wrapper's `sss_en` tick for an *exact* cycle-locked 1 MHz
  mode — which passes cycle-counting detection probes (A2DeskTop) that the
  real TW fails.
- **ARM's role is thin** — enable/config, orchestrating the boot ROM copy,
  mirroring $C074 state for the menu, and `:vtw status` diagnostics. CPU0's
  main loop gains no hot work.

## 4. PL bus engine specification

Timing in the wrapper's existing tap vocabulary (133 MHz fabric, PHI0 edges
from the majority filter; ~7.5 ns/tap).

**Synchronous cycle (all $C000–$CFFF, boot-time ROM copy, phase-2 RamWorks):**
1. Request from the core wrapper (or ARM port B during boot):
   `{addr[15:0], rw, wdata[7:0]}`; core CE deasserts.
2. Engine waits for the next PHI0 **fall**, then at `fall+TAP_DRIVE_ADDR`
   (~tap 8, inside early PHI1) drives address + R/W through
   `ab_write.wr_addr / wr_rw / wr_addr_rw_en`.
3. Writes: data driven from `data_phase_emit` (existing tap 25 after rise)
   until end of cycle, via `wr_data / wr_data_en`.
4. Reads: result latched from `data_clean` at `TAP_DATA_SNAP` (existing
   tap 59 after rise) — the same instant the capture path samples, so master
   and observer agree by construction.
5. Response releases the core CE. Stall cost: 0–1 µs sync + 1 µs cycle.
6. Address/R-W stay driven after the cycle (becomes the new parked state).

**Posted write-through (video windows only):**
- RTL address filter on core writes to $0400–$0BFF / $2000–$5FFF pushes
  `{addr, data}` to the posted queue (BRAM FIFO, 512 deep) in addition to the
  shadow write.
- Engine drains one posted write per idle PHI0 cycle. Idle bandwidth
  (~1M cycles/s minus I/O) vastly exceeds video write rates; queue-full
  back-pressures the core CE (mirrors the real TW's measured screen-write
  throttling, with a far deeper queue).
- Ordering rule: a synchronous request whose address falls in a video window
  flushes the posted queue first (read-after-write consistency); otherwise
  synchronous I/O may overtake posted writes, like the real card's decoupled
  engine.

**Parked-cycle driver:** whenever enabled and no cycle is active, re-drive
the last real address with R/W=1 with the same `fall+TAP_DRIVE_ADDR`
discipline. Every cycle on the bus is a clean, fully-driven cycle for the
motherboard, our own snoop path, and any third-party card.

**/DMA protocol:** assert during PHI1 following reset-release (TN#2), hold
until disabled+reset — with one exception: every Apple RES# window releases
the bus completely (MMU/IOU reset processing requires stock conditions),
and the engine re-arms through the same DMA-then-grace-then-drive sequence
at release. The arbiter's `inh_allowed` gate already blocks `assert_dma`
on non-//e machines; vTW enable additionally requires machine_mode == IIe.

**Arbitration note:** while the vTW masters the bus, the Appletini
simultaneously *serves* some of the cycles it issues (its own virtual cards'
I/O). Address drive (master) and data drive (server) use different
transceivers (`tini_addr_dir` vs `tini_data_dir`) — electrically and
logically sound; the serving path needs no changes because our cycles meet
the early-assert contract the serve path was built for.

## 5. Boot and control flow

1. PS asserts vTW mode before Apple reset-release (config/persisted); core
   held in reset. The engine waits out RES#, then asserts /DMA and the
   parked driver takes the bus.
2. ARM (via BRAM port B + the sync-cycle FIFO) copies motherboard ROM
   $D000–$FFFF into the shadow over real bus reads — works with any ROM
   revision, exactly like the real card's power-up copy.
3. Core released at the shadow reset vector; autostart proceeds; every
   $Cxxx access (slot scan, boot menu, disk boot) happens over the real bus.
4. $C074 writes are seen by the core wrapper (it issues them) and mirrored
   to a status register for the ARM/menu; semantics per §2.
5. Diagnostics: `vtw status` on the UART console — speed mode,
   executed-cycle counters, posted-queue depth/high-water, soft-switch
   state, post-reset soft-switch trail, $C1xx–$CFFF sync-cycle trail.

## 6. No slot

The accelerator needs no slot decode to function — a DMA bus master has no
slot presence; its only software-visible surface is the $C074 register in
motherboard address space, decoded by watching the bus exactly as the real
card's PALs do.

## 7. Phase 2 plan

In priority order:

1. **RamWorks at accelerator speed** — implemented (see the §2 RamWorks
   row). 8 MB from PSRAM through the vTW line cache when the Appletini
   provides aux; clean 64K semantics otherwise.
2. **Speed controls: config menu + assignable USB keymap actions —
   implemented.** No new soft switches, ever — $C074 stays the only
   decoded register (TW contract; silently claiming more I/O addresses
   risks colliding with 40 years of software). Four assignable keyboard
   actions in the Boot-settings USB keymap (global: they fire with the
   menu open or closed, unbound by default), all PS-side writes to the
   VTW control register:
   - *TW 1MHz* — 1 MHz ⇄ configured speed ($C074-style toggle).
   - *TW speed+ / TW speed−* — one step along the preset ladder
     1 → 2.6 → 3.6 → 7 → 13 → Warp.
   - *TW slug .05* — 0.05 MHz ⇄ configured speed, for single-stepping-
     grade debugging. Independent toggle, NOT on the ladder: +/− can
     never land on it (a step up from slug exits to 1 MHz). The pace
     divider is 16-bit (50 kHz = divider 2667, CTRL[31:16]). The slug key
     is **disarmed by default** and ignored until the TransWarp tab's
     "Enable 0.05 MHz slug debug key" checkbox is set — an accidental
     press must never silently near-halt the machine; disarming while
     slugged restores the configured speed immediately.
   Every speed action raises a **top-of-screen overlay** (`TW: 7 MHz`)
   so the change is visible even without the UART — anchored at the top
   so it never collides with the bottom screenshot overlay. Runtime
   overrides never persist and a configured (menu) speed change clears
   them, matching $C074 semantics. Session enable/disable stays
   BOOT-mode-only; only speed is live. The boot beeps double as audible
   speed confirmation.
3. **Own-card I/O short-circuit — SmartPort at core speed (implemented).**
   Previously every slot-7 access — the driver code in slot/expansion ROM
   *and* the DATA/CTRL/DPOP registers — was a 1 MHz bus cycle, so disk I/O
   was the one thing acceleration did not accelerate (the 512-byte
   transfer loop and its instruction fetches both ran at bus rate). Now,
   when the Appletini's SmartPort card owns slot 7 (`sp_active`, i.e. after
   the boot menu hands off), `vtw_core_top` classifies the core's accesses
   to the card's Apple surface — slot ROM $C700–$C7FF, expansion ROM
   $C800–$CFEE, and DATA/CTRL/DPOP at $CFF0–$CFF2 — using its **private**
   soft-switch state (correct on both //e INTCXROM/IOSEL and a II+ with no
   MMU), and serves them through a fabric-internal port on `smartport_card`
   (`X_SP_ISSUE`→`WAIT`→`DONE`, ~2-cycle latency) instead of a sync bus
   cycle. The card's fast port drives the **same** FIFO/exec/IRQ state as
   the bus path (unified `data_write_ev`/`ctrl_write_ev`/`pop_write_ev`
   events), so ROM and OUT-head reads stay side-effect-free and the PS
   protocol is unchanged. The gate on `sp_active` also resolves the
   shared slot-7 ownership: before hand-off the boot-menu card still sees
   $C7xx over the bus; after, SmartPort is short-circuited. The disk
   *service* latency (PS/USB) is unchanged — only the transport and driver
   execution accelerate, which is the dominant cost.
4. **Virtual Disk II reads at core speed — implementation pending hardware
   validation.** When the Appletini Disk II owns slot 6, even reads of
   `$C0E0-$C0EE` use a private `disk2_card` port. One virtual Disk II tick is
   generated for each completed vTW CPU cycle, so standard nibble rotation,
   WOZ bit cells, empty-drive settling, and deferred head steps scale with the
   chosen CPU speed. A track-stage or DDR line-cache miss holds both the CPU
   and virtual disk time until valid data is ready; it cannot return a stale
   latch or consume a missing byte. The `Disable DiskII Acceleration` setting
   bypasses this private path and sends all Disk II accesses through the
   original physical 1 MHz route for problem disks. Odd reads still use the
   motherboard's floating bus. Every CPU write uses the physical Apple bus
   at 1 MHz. While Q7 write mode is active, the whole core stays at 1 MHz
   until Q7 clears, so the existing DSK/PO/NIB/WOZ write path and its
   copy-protection timing stay
   unchanged. Motor and seek audio remain tied to real time. Covered by
   `tb_disk2_vtw_read`, `test_disk2_standard.py`, and `test_disk2_woz.py`.
5. **Per-region slowdown — implemented (TW DIP block 2).** After the core
   touches an enabled timing-sensitive region it drops to cycle-locked
   1 MHz for a configurable window (retriggered per access), so speaker
   pitch, paddle timing, and per-slot device loops stay correct at high
   core speeds. `vtw_core_top` classifies the per-cycle address (speaker
   $C030-$C03F, paddle $C064-$C067 + PTRIG $C070, per-slot device I/O
   $C0n0-$C0nF for slots 1-7) and folds a one-shot counter into `eff_mode`;
   config lives in AxiSimple register 0x6B ([8:0] region enables,
   [31:16] window). Exposed in the TransWarp tab: speaker, paddle, a
   per-slot selector, and the window size — all **default OFF**. The
   virtual Mockingboard/Phasor slot (4) is **always** slowed regardless of
   config, because its 6522-VIA cycle-counted detection otherwise fails at
   warp. Validated by `tb_vtw_slowdown` (warp baseline runs fast; speaker
   slowdown collapses the rate to ~1 MHz; a non-matching region enable
   stays warp).
6. **II/II+ acceleration — implemented, bench validation pending.**
   Acceleration only, no memory changes: the machine gate accepts a
   positive II/II+ identification, and the core's translator runs a
   sanitized view (aux switches, internal $CX ROM, and slot-3 internal
   ROM forced off; slot 3 owns $C3xx; language card tracked normally —
   the shadow carries the LC image, so a stock $C08x-protocol LC works).
   IIe-probing software can write $C00x freely without deranging the
   shadow. The boot copy runs unchanged ($C100–$CFFF captures floating
   garbage the sanitized translator never reads; $D000–$FFFF gets the
   real II+ ROM through forced LC-ROM reads). Aux-provide/RamWorks stay
   off via the existing IIe-only PS policy. Still needs first-hardware
   validation on a real II+: bus buffering under /DMA mastering and the
   machine's reset physiology have never been measured there.
7. **Config-menu polish** — slug-key arm checkbox + top-of-screen
   speed overlay done; per-region slowdown toggles and a RamWorks
   status row remain (follow items 4 and 1 respectively).

**Explicit non-goals:** IIgs support; mid-flight handback (impossible on
NMOS, same as the real TW); NMOS-cycle-timing emulation (the vTW is a
65C02 exactly as the physical TW is — for 100% NMOS cycle accuracy,
disable acceleration).

**Fallback (de-risk only):** if the chosen core misbehaves, an ARM-side
interpreted 65C02 (Appli-Card Z80 pattern) can drive the same
`vtw_bus_engine` request interface unchanged — the engine contract is
requester-agnostic by design.

## 8. Risks and their measurements

| Risk | Measurement / mitigation |
|---|---|
| BRAM budget | Measured: 36/140 tiles used today; shadow ≈ +40 → ~55%. Re-check utilization + timing after integration; drop aux-LC (−4 tiles) if squeezed |
| Core correctness/licensing | License check before import; Klaus Dormann functional tests in simulation against the BRAM shadow; undocumented-opcode policy = 65C02 NOPs |
| Timing closure with core + shadow | Core and BRAM are a self-contained island off the fabric clock; only the bus-engine FIFO crosses into tap-domain logic. Keep the island in its own hierarchy for floorplanning |
| Posted-queue coherency corners | Flush-before-video-window-I/O rule; test vapor-lock/floating-bus software |
| Our own capture/serve paths seeing master cycles | By design they must (that is how HDMI renders); early-assert discipline makes our cycles the easy case, validated by the existing suite |
| Contention with a physical accelerator | vTW enable is manual + documented: never with a real TW/FastChip installed (two DMA masters) |

## 9. Acceptance tests

The physical-TW validation suite, re-run against the vTW at full speed and
at cycle-locked 1 MHz, plus a vTW-disabled stock pass unchanged:
cold boot through boot menu → BASIC; CATALOG + DOS 3.3 SAVE/LOAD round-trip;
Ultima V logo + Phasor music; SmartPort "A:APPLETINI" boot (DPOP protocol);
Disk II DSK/PO save-load round-trip; NIB and WOZ copy-protected reads; empty
drive 2 prompt and disk swap; Disk II writes at forced 1 MHz;
monitor `0L` listing; A2DeskTop slot listing (Phasor must appear at 1 MHz
mode — a fidelity improvement over the real card); CRT liveness via the
posted write-through; Klaus Dormann suite in simulation before first
hardware boot.
