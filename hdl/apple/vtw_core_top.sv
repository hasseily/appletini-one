`timescale 1ns / 1ps
//////////////////////////////////////////////////////////////////////////////////
// Virtual TransWarp core wrapper.
//
// Wraps the fabric W65C02 core with everything that makes it an Apple //e
// CPU running at configurable speed under the physical TransWarp's bus
// contract:
//
//   - vtw_shadow: 144 KB BRAM the core executes from (main/aux/ROM).
//   - A private soft_switch_manager instance fed one synthetic serve_en
//     pulse per core cycle: the vTW tracks //e banking state from its own
//     accesses (it is the CPU -- it sees every $C0xx access before the bus
//     does). Same module, same semantics as the motherboard-facing tracker.
//   - globals::translate_apple_addr against that private state routes every
//     core cycle: CACHE/ROM -> shadow, BUS ($C000-$CFFF) -> a real bus cycle
//     through vtw_bus_engine (core stalled, ~1-2 us, the real card's cost
//     profile), video-window writes -> shadow AND the posted write queue.
//   - Speed control: full-rate bursts (one core cycle per 4 fabric clocks),
//     a divided mode for menu presets, and a cycle-locked 1 MHz mode paced
//     on ab_read.data_en so consecutive core cycles land exactly one Apple
//     cycle apart -- cycle-counting detection probes (A2DeskTop) see a
//     stock machine, which the real TW fails.
//   - $C074 (0=fast, 1=1 MHz, 3=off-until-reset): decoded from the core's
//     own writes; the write still goes to the bus like the real card's.
//     A persisted compatibility override may ignore every write.
//
// Interrupts arrive through the physical pins: virtual cards assert IRQ on
// the real line via the arbiter, the wrapper samples it back, and the core
// sees it here -- one loop through the edge connector, exactly like the
// motherboard 6502 it replaces. Vector fetches route through the shadow
// with correct LC banking (strictly better than the real TW's reboot stub,
// same observable contract for a booted OS).
//
// ARM interface: shadow port B peek/poke, and a synchronous bus-cycle
// request port (active while the core is held) for the boot-time ROM copy
// over real bus reads -- works with any ROM revision, exactly like the
// real card's power-up copy.
//////////////////////////////////////////////////////////////////////////////////

module vtw_core_top (
    input  logic                    clk,
    input  logic                    rstn,

    /* Session enable: vTW configured on AND the //e or II/II+ machine gate
     * satisfied. Must be asserted before Apple reset release. */
    input  logic                    enable,
    /* Session-latched physical host type. It selects only the electrically
     * safe parked-bus address; the accelerated personality remains a //e. */
    input  logic                    host_is_iiplus,
    /* ARM releases the core after the boot ROM copy. While low the core
     * is held in reset and the ARM owns the sync-cycle port. */
    input  logic                    core_run,
    /* Pull the Apple RES# line low (open-collector, like CTRL-RESET).
     * The takeover sequence asserts this for ~100 ms so the whole machine
     * -- cards, MMU, PS reset detection -- restarts coherently before
     * the vTW core boots. A //e gets the guarded stock-CPU reset window; a
     * II/II+ keeps /DMA continuously asserted and resumes from shadow. */
    input  logic                    assert_apple_res,

    // Speed configuration (config menu / persisted profile). 16-bit
    // divider: the 0.05 MHz slug mode needs ~2667 fabric clks/cycle.
    input  logic [1:0]              speed_mode,   // 0=full, 1=divided, 2=1MHz-locked
    input  logic [15:0]             pace_divider, // divided: min fabric clks/cycle
    /* Ignore every software write to the TransWarp speed register. Raising
     * this live also clears a value that software latched earlier, so the
     * selected menu/USB speed takes effect at once. The physical write still
     * reaches the Apple bus; only its vTW control effect is suppressed. */
    input  logic                    ignore_c074,

    /* Per-region slowdown (physical TransWarp DIP block 2). After the core
     * touches an enabled timing-sensitive region, it drops to cycle-locked
     * 1 MHz for slow_duration Apple cycles (retriggered on each access).
     * slow_region_en: [6:0] slots 1..7, [7] floating-bus/video timing
     * ($C019 and $C030-$C05F), [8] paddle, [9] reserved.
     * slow_duration = 0 disables the feature. */
    input  logic [9:0]              slow_region_en,
    input  logic [15:0]             slow_duration,

    /* Virtual Disk II read shortcut. Even-address reads in $C0E0-$C0EF
     * may run against the private card port. Odd reads, every CPU write,
     * and the whole Q7 write-mode interval retain the proven physical
     * 1 MHz path. The card may hold virtual time while CPU0 stages a track
     * or the DDR line cache catches up. */
    input  logic                    d2_active,
    output logic                    d2_req_valid,
    output logic [3:0]              d2_req_addr,
    input  logic                    d2_req_ready,
    input  logic                    d2_resp_valid,
    input  logic [7:0]              d2_resp_rdata,
    output logic                    d2_cycle_tick,
    output logic                    d2_native_cycle_active,
    input  logic                    d2_time_ready,
    input  logic                    d2_write_timing_active,

    /* RamWorks 8 MB expansion (card-control register 0x62, i.e. the same
     * config bit that arms the motherboard-side PSRAM aux serving). When
     * set, the private translator emits banks 2..128 and the vTW serves
     * them from PSRAM at core speed through the line port below. When
     * clear, $C071/$C073 writes are ignored (stock 64K-aux //e semantics:
     * RamWorks probes see aliasing and size the machine correctly). */
    input  logic                    ramworks_en,

    /* Vertical-blank level from the video timing generator (same fabric
     * clock as this core -- no CDC). Feeds the synthesized $C019 RDVBLBAR
     * status read so accelerated code sees real frame timing regardless of
     * the host machine: an accelerated session is always an Enhanced //e,
     * and a II+ motherboard has no //e IOU to answer $C019. */
    input  logic                    video_vbl,

    /* Native scanner position. Floating-bus reads cannot sample the video
     * byte from the expansion-slot pins: the motherboard scanner fetch is
     * on its private memory bus during PHI1. Instead, the vTW reproduces
     * the //e scanner address and reads that byte from its main shadow. */
    input  logic                    video_mode_50hz,
    input  logic [8:0]              video_line,
    input  logic [6:0]              video_cycle,

    /* PS fallback for the main $6000-$9FFF posted-write window. The core
     * also tracks aux $9DF8 itself so an accelerated second-field load does
     * not race the physical capture and CPU1 service path. */
    input  logic                    post_main_wide,

    /* An armed linear-text buffer is another write-through window. The
     * physical capture path must see these writes even when the accelerated
     * core keeps ordinary program RAM only in its private shadow. */
    input  logic                    overlay_capture_armed,
    input  logic                    overlay_capture_bank_aux,
    input  logic [15:0]             overlay_capture_base,
    input  logic [15:0]             overlay_capture_limit,

    input  globals::AppleBus_read   ab_read,
    output globals::AppleBus_write  ab_write,

    /* Arbiter-merged virtual-card IRQ assert (pre-pin). The core must not
     * depend on the physical IRQ pin round-trip for interrupts our own
     * cards generate: the open-drain pad -> slot net -> sampler loop has
     * proven electrically unreliable on the II/II+ host. The pin sample
     * term is kept so real slot cards still interrupt the core. */
    input  logic                    irq_assert_in,
    input  logic                    data_drive_in,
    input  logic [7:0]              data_drive_value_in,
    input  logic                    dbg_clear,

    /* Serve $C061-$C063 Apple-key reads internally as $00 (not pressed).
     * An accelerated session presents as an Enhanced //e, so software
     * legitimately polls the Apple keys -- but on a controller-less II/II+
     * host those addresses are the game-connector pushbuttons and FLOAT
     * HIGH ("pressed"). Set by the PS for II/II+ hosts (vtw ctrl bit 5);
     * clear it to restore real reads when a controller is attached. */
    input  logic                    iiplus_buttons_zero,

    /* PSRAM line port (psram_simple vtw client): 8-byte line reads and
     * writes for RamWorks banks. One op per background admission window,
     * so a miss costs up to ~one Apple bus cycle; the single-line cache
     * with write-allocate makes sequential traffic ~8x denser. */
    output logic                    rw_req_valid,
    output logic                    rw_req_rw,      // 1 = read line, 0 = write line
    output logic [23:0]             rw_req_addr,
    output logic [63:0]             rw_req_wline,
    input  logic                    rw_req_ready,
    input  logic                    rw_resp_valid,
    input  logic [63:0]             rw_resp_rline,

    /* SmartPort short-circuit. When the Appletini's own SmartPort card
     * owns slot 7, the core's accesses to its Apple surface (slot ROM,
     * expansion ROM, and the DATA/CTRL/DPOP registers) are served
     * fabric-internally instead of as 1 MHz bus cycles -- disk I/O then
     * runs at core speed. sp_active gates the whole path; the classifier
     * uses the core's private soft-switch state, so it stays correct on
     * a //e (INTCXROM/IOSEL) and a II+ (no MMU). During a vTW cold boot
     * targeting Disk II, sp_boot_suppress makes slot 7 read as an empty
     * slot until the boot scan reaches slot 6. */
    input  logic                    sp_active,   // SmartPort owns slot 7
    input  logic                    sp_boot_suppress,
    output logic                    sp_req_valid,
    output logic [2:0]              sp_req_target, // SP_TGT_* (see vtw_pkg use)
    output logic [10:0]             sp_req_addr,   // ROM offset
    output logic                    sp_req_rw,     // 1 = read, 0 = write
    output logic [7:0]              sp_req_wdata,
    input  logic                    sp_req_ready,
    input  logic                    sp_resp_valid,
    input  logic [7:0]              sp_resp_rdata,
    /* Exact private state for the SmartPort command snapshot. Bit 21 is
     * the effective wide-main video flag; bits 20:0 match SP_REG_SSS. */
    output logic [21:0]             sp_sss_snapshot,

    // ARM shadow access (port B), AxiSimple-mapped in apple_top.
    input  logic                    sh_en,
    input  logic [17:0]             sh_addr,
    input  logic                    sh_we,
    input  logic [7:0]              sh_wdata,
    output logic [7:0]              sh_rdata,

    // ARM synchronous bus access (boot ROM copy; honored only when the
    // core is held).
    input  logic                    arm_req_valid,
    input  logic [15:0]             arm_req_addr,
    input  logic                    arm_req_rw,
    input  logic [7:0]              arm_req_wdata,
    output logic                    arm_req_busy,
    output logic                    arm_resp_valid,  // 1-clk pulse
    output logic [7:0]              arm_resp_rdata,

    /* CPU0 posted-write injection. SmartPort uses this only while the core
     * waits for its command response: shadow RAM receives the block through
     * port B, while video-window bytes enter the same ordered queue used by
     * normal core writes. arm_post_ready is a true per-write handshake. */
    input  logic                    arm_post_we,
    input  logic [15:0]             arm_post_addr,
    input  logic [7:0]              arm_post_wdata,
    output logic                    arm_post_ready,

    /* CPU0 flush+hold for SmartPort direct copies into RamWorks. A
     * flush request freezes the core by gating core_en; once the
     * one-access-lookahead xstate FSM has drained clear of the RamWorks
     * cache states and the PSRAM channel is idle, the dirty line is
     * written back, the cache invalidated, and done pulsed. The core
     * stays frozen until arm_rw_hold_release, so the PS-DMA block write
     * cannot race any core access -- including an IRQ handler's stack
     * pushes into a RamWorks bank. Session end or Apple RES#
     * auto-releases: a wedged CPU0 must never leave the Apple frozen. */
    input  logic                    arm_rw_flush_req,
    input  logic                    arm_rw_hold_release,
    output logic                    arm_rw_flush_done,
    output logic                    arm_rw_hold_state,

    // Status / diagnostics (`:vtw status`).
    output logic [1:0]              c074_state,
    output logic                    bus_owned,
    output logic                    video_phase_1mhz,
    output logic [15:0]             dbg_core_pc,
    output logic [31:0]             cnt_core_cycles,
    output logic [31:0]             cnt_bus_cycles,
    output logic [31:0]             cnt_posted_writes,
    output logic [9:0]              post_fill,
    output logic [9:0]              post_high_water,
    output logic [31:0]             cnt_post_drops,
    output logic [31:0]             cnt_invalid_routes,

    /* Bench forensics: private //e switch state and the last completed
     * bus cycle -- `vtw status` prints both. dbg_vsss packing:
     * {intcxrom, slotc3rom, intc8rom, lc_read, lc_write, lc_bank2,
     *  altzp, ramrd, ramwrt, 80store, text}. */
    output logic [10:0]             dbg_vsss,
    output logic [15:0]             dbg_last_sync_addr,
    output logic [7:0]              dbg_last_sync_data,
    output logic                    dbg_last_sync_rw,
    /* Saturating count of IRQ assertions the core observed (falling
     * edges of the filtered line). Distinguishes "sampling fix worked,
     * no interrupts" from "a card is genuinely pulling IRQ". */
    output logic [6:0]              dbg_irq_edges,
    /* Last eight $C1xx-$CFFF bus cycles, newest in [15:0]. */
    output logic [8*16-1:0]         dbg_cxxx_ring,
    /* Last eight $C00x/$C01x soft-switch cycles with data (see engine). */
    output logic [8*16-1:0]         dbg_c0_ring,
    output logic [31:0]             dbg_sync_write_check,
    output logic [15:0]             dbg_sync_write_addr,
    output logic [31:0]             dbg_c000_context,
    output logic [31:0]             dbg_c000_counts,
    /* Event-frozen forensic traces, re-armed by `busdbg clear`.
     * PC entries are instruction-fetch addresses; I/O entries are packed
     * by vtw_bus_engine. TRACE_STATUS: [0] frozen, [2:1] reason
     * (2=$C000 bit7, 3=internal $C600). RESET source is recorded
     * independently and the rings continue through RESET so they retain
     * the post-reset ROM path. */
    output logic [16*16-1:0]        dbg_pc_trace,
    output logic [16*32-1:0]        dbg_io_trace,
    output logic [31:0]             dbg_trace_status,
    output logic [31:0]             dbg_bus_faults
);

    import globals::*;
    import vtw_shadow_pkg::*;
    import vtw_pkg::*;

    // ------------------------------------------------------------------
    // Core
    // ------------------------------------------------------------------
    logic        core_en;
    logic [15:0] core_addr;
    logic [7:0]  core_data_out;
    logic        core_sync;
    /* Registered CPU operand. Every X_*_DONE-entry edge resolves that
     * cycle's response -- shadow BRAM, bus engine, RamWorks cache byte,
     * SmartPort card, synthesized $C01x status, or the $FF dead-route
     * filler -- into this one flop, so the core reads a stable register
     * instead of an xstate-selected mux stacked ahead of its ALU. That
     * keeps the mux and its high-fanout select off the single-cycle
     * execute path (the design's tightest). */
    logic [7:0]  core_data_in_q;
    logic        core_rwb;

    /* Motherboard RES# resets the core through the shadow's reset vector;
     * /DMA stays held (vtw_bus_engine). CTRL-RESET with the vTW disabled
     * is the cold handback path, handled at the session level. The session
     * global reset asserts asynchronously and releases synchronously. The
     * remaining session gates are already in this clock domain, so register
     * their conjunction before it drives the core's async-clear pins. */
    wire  core_reset_domain_n;
    logic core_res_n;

    reset_sync core_reset_sync_i (
        .clk(clk),
        .arst_n(rstn),
        .srst_n(core_reset_domain_n)
    );

    always_ff @(posedge clk) begin
        if (!core_reset_domain_n) begin
            core_res_n <= 1'b0;
        end
        else begin
            core_res_n <= enable && core_run && ab_read.res;
        end
    end

    /* Interrupt merge: our own virtual cards' asserts reach the core
     * directly; the sampled physical line keeps real slot cards working.
     * ab_read.irq is the filtered LINE level (1 = released). */
    wire core_irq_n = ab_read.irq & ~irq_assert_in;

    w65c02_core #(.DEBUG_STATE_LOAD(1'b0)) core_i (
        .clk(clk),
        .reset_n(core_res_n),
        .enable(core_en),
        .ready(1'b1),
        .irq_n(core_irq_n),
        .nmi_n(ab_read.nmi),
        .so_n(1'b1),
        .data_in(core_data_in_q),
        .addr(core_addr),
        .data_out(core_data_out),
        .rwb(core_rwb),
        .sync(core_sync),
        .vpb_n(),
        .mlb_n(),
        .waiting(),
        .stopped(),
        .instruction_done(),
        .debug_load(1'b0),
        .debug_pc_in('0),
        .debug_s_in('0),
        .debug_a_in('0),
        .debug_x_in('0),
        .debug_y_in('0),
        .debug_p_in('0),
        .debug_pc(dbg_core_pc),
        .debug_s(),
        .debug_a(),
        .debug_x(),
        .debug_y(),
        .debug_p()
    );

    // ------------------------------------------------------------------
    // Private //e switch state, tracked from the core's own accesses.
    // X_CAPTURE saves the cycle and its pre-access bank state. X_ROUTE then
    // gives the private manager one synthetic serve_en/data_en pulse from
    // that saved tuple. The manager ignores non-$Cxxx addresses, so pulsing
    // every cycle preserves stock //e behavior, including shadow-served C3
    // claims, CFFF releases, and language-card double-access rules. Keeping
    // this apply pulse off the live core state also removes the CPU-state to
    // RamWorks-bank enable path.
    // ------------------------------------------------------------------
    globals::SoftSwitchState vsss;
    logic                    ssm_pulse;
    logic                    ssm_apply_pulse;
    globals::AppleBus_read   core_ab;
    logic [15:0]             cycle_addr_q;
    logic [7:0]              cycle_wdata_q;
    logic                    cycle_rw_q;
    TranslateState           cycle_translate_state_q;

    always_comb begin
        core_ab             = '0;
        core_ab.res         = ab_read.res && enable;
        core_ab.addr        = cycle_addr_q;
        core_ab.rw          = cycle_rw_q;
        core_ab.data        = cycle_wdata_q;
        core_ab.cycle_valid = 1'b1;
        core_ab.serve_en    = ssm_apply_pulse;
        core_ab.data_en     = ssm_apply_pulse;
    end

    soft_switch_manager vtw_ssm (
        .clk(clk),
        .rstn(rstn),
        .ramworks_en(ramworks_en),
        .ab_read(core_ab),
        .sss(vsss)
    );

    assign dbg_vsss = {vsss.sw_intcxrom, vsss.sw_slotc3rom,
                       vsss.c8_internal_rom, vsss.sw_lcram_read,
                       vsss.sw_lcram_write, vsss.sw_lcram_bank2,
                       vsss.sw_altzp, vsss.sw_ramrd, vsss.sw_ramwrt,
                       vsss.sw_80store, vsss.sw_text};

    logic [6:0] irq_edge_cnt_q;
    logic       irq_prev_q;
    always_ff @(posedge clk) begin
        if (!rstn) begin
            irq_edge_cnt_q <= '0;
            irq_prev_q     <= 1'b1;
        end
        else begin
            irq_prev_q <= ab_read.irq;
            if (irq_prev_q && !ab_read.irq && irq_edge_cnt_q != 7'h7F) begin
                irq_edge_cnt_q <= irq_edge_cnt_q + 7'd1;
            end
        end
    end
    assign dbg_irq_edges = irq_edge_cnt_q;

    // ------------------------------------------------------------------
    // Routing: one translate per core cycle, against a snapshot of the
    // private state.  The raw core cycle is registered before translation;
    // this breaks the core-address -> banking -> BRAM control timing path.
    // ------------------------------------------------------------------
    logic [31:0]       xl_decoded;
    apple_route_kind_e xl_route;
    logic              xl_shadow_valid;
    logic [17:0]       xl_shadow_phys;

    logic              cycle_video_text_q;
    logic              cycle_video_mixed_q;
    logic              cycle_video_page2_q;
    logic              cycle_video_hires_q;
    logic              cycle_video_80store_q;

    always_comb begin
        translate_apple_addr(cycle_translate_state_q,
                             cycle_addr_q, cycle_rw_q,
                             xl_decoded, xl_route);
        vtw_shadow_map(xl_route, xl_decoded, xl_shadow_valid, xl_shadow_phys);
    end

    wire xl_is_bus   = (xl_route == APPLE_ROUTE_BUS);
    wire xl_is_write = !cycle_rw_q;
    wire xl_is_aux   = (xl_decoded[23:16] == 8'd1);
    /* Paged SHR control lives in aux $9DF8. Track it at the private shadow
     * write, before the posted copy reaches the physical bus. Waiting for
     * CPU1 to see that posted byte can lose the start of a fast main-field
     * load, including its mode magic at $9DFC. */
    logic shr_post_main_wide_q;
    wire post_main_wide_eff = post_main_wide | shr_post_main_wide_q;
    assign sp_sss_snapshot = {
        post_main_wide_eff,
        vsss.sw_ramworks_bank,
        vsss.sw_lcram_write,
        vsss.sw_lcram_read,
        vsss.sw_lcram_bank2,
        vsss.sw_dhires,
        vsss.sw_80col,
        vsss.sw_altcharset,
        vsss.sw_hires,
        vsss.sw_page2,
        vsss.sw_mixed,
        vsss.sw_text,
        vsss.sw_altzp,
        vsss.sw_ramwrt,
        vsss.sw_ramrd,
        vsss.sw_80store
    };
    /* Posted write-through: video-window writes to main (bank 0) AND base
     * aux (bank 1). The bus cycle carries only the Apple address; the
     * motherboard's own 80STORE/PAGE2/RAMWRT state -- which mirrors the
     * core's, since it tracks the same $C0xx cycles -- steers it into the
     * right bank, and the engine's bank-steer flush rule keeps queued
     * writes ordered against switch changes. Without the aux half, the
     * capture shadow's aux banks never update and 80-col/DHGR render
     * empty. The aux window extends to $9FFF (vs $5FFF for main) so the
     * Super Hi-Res framebuffer, SCB ($9D00) and palette ($9E00) -- all in
     * aux $2000-$9FFF -- reach the capture/renderer too. */
    wire xl_is_overlay_post =
        overlay_capture_armed && !xl_is_bus && xl_shadow_valid &&
        xl_is_write && (xl_decoded[23:17] == 7'd0) &&
        (xl_decoded[16] == overlay_capture_bank_aux) &&
        (xl_decoded[15:0] >= overlay_capture_base) &&
        (xl_decoded[15:0] < overlay_capture_limit);
    wire xl_is_posted = !xl_is_bus && xl_shadow_valid && xl_is_write &&
                        (xl_decoded[23:16] <= 8'd1) &&
                        (vtw_is_video_window(cycle_addr_q, xl_is_aux,
                                             post_main_wide_eff) ||
                         xl_is_overlay_post);

    /* Synthesized //e status reads ($C011-$C01F). On a real //e the IOU
     * answers these over the bus; an accelerated II+ has no IOU, so the
     * core serves them internally from its own tracked switch state (and
     * even on a //e this saves a bus round-trip). These addresses have no
     * read side effects. $C000 (keyboard data) and $C010 (strobe clear)
     * keep their real-bus semantics and are excluded. */
    wire xl_c01x_rd = xl_is_bus && cycle_rw_q &&
                      (cycle_addr_q[15:4] == 12'hC01) &&
                      (cycle_addr_q[3:0] != 4'h0);

    /* Floating II/II+ pushbuttons: serve $C061-$C063 reads internally as
     * "not pressed" when the PS flags a II/II+ host (see the
     * iiplus_buttons_zero port comment). These reads have no side
     * effects, so skipping the bus cycle is faithful. $C060 (cassette
     * in) keeps real-bus semantics. */
    wire xl_btn_rd = xl_is_bus && cycle_rw_q && iiplus_buttons_zero &&
                     (cycle_addr_q[15:2] == 14'h3018) &&
                     (cycle_addr_q[1:0] != 2'b00);

    /* $C019 RDVBLBAR bit 7 uses the //e (AppleWin-ported renderer)
     * convention: 1 during active display, 0 during vertical blanking.
     * Hardware exposes the transition one native cycle behind the raw
     * scanner counter. At scanline cycle zero, classify the rewound line;
     * elsewhere the raw VBL level is already stable. */
    wire [15:0] status_video_pos =
        vtw_video_position_rewind(video_mode_50hz, video_line,
                                  video_cycle, 2'd1);
    wire status_video_vbl = (video_cycle == 7'd0)
                          ? (status_video_pos[15:7] >= 9'd192)
                          : video_vbl;
    logic [7:0] status_byte;
    always_comb begin
        unique case (cycle_addr_q[3:0])
            4'h1:    status_byte = {vsss.sw_lcram_bank2, 7'b0}; // $C011 RDLCBNK2
            4'h2:    status_byte = {vsss.sw_lcram_read,  7'b0}; // $C012 RDLCRAM
            4'h3:    status_byte = {vsss.sw_ramrd,       7'b0}; // $C013 RDRAMRD
            4'h4:    status_byte = {vsss.sw_ramwrt,      7'b0}; // $C014 RDRAMWRT
            4'h5:    status_byte = {vsss.sw_intcxrom,    7'b0}; // $C015 RDCXROM
            4'h6:    status_byte = {vsss.sw_altzp,       7'b0}; // $C016 RDALTZP
            4'h7:    status_byte = {vsss.sw_slotc3rom,   7'b0}; // $C017 RDC3ROM
            4'h8:    status_byte = {vsss.sw_80store,     7'b0}; // $C018 RD80STORE
            4'h9:    status_byte = {~status_video_vbl,   7'b0}; // $C019 RDVBLBAR
            4'hA:    status_byte = {vsss.sw_text,        7'b0}; // $C01A RDTEXT
            4'hB:    status_byte = {vsss.sw_mixed,       7'b0}; // $C01B RDMIXED
            4'hC:    status_byte = {vsss.sw_page2,       7'b0}; // $C01C RDPAGE2
            4'hD:    status_byte = {vsss.sw_hires,       7'b0}; // $C01D RDHIRES
            4'hE:    status_byte = {vsss.sw_altcharset,  7'b0}; // $C01E RDALTCHAR
            4'hF:    status_byte = {vsss.sw_80col,       7'b0}; // $C01F RD80COL
            default: status_byte = 8'h00;
        endcase
    end
    /* RamWorks banks 2..128: CACHE-routed decodes with no shadow backing
     * exist only when the private translator's RamWorks banking is armed.
     * Served from PSRAM through the line cache below. */
    wire xl_is_ramworks = (xl_route == APPLE_ROUTE_CACHE) &&
                          !xl_shadow_valid && ramworks_en;

    // ------------------------------------------------------------------
    // SmartPort short-circuit classifier. The Appletini's SmartPort card
    // is slot 7. When it owns the slot (sp_active, i.e. post boot-handoff)
    // its Apple surface is served fabric-internally instead of over the
    // 1 MHz bus. Classification uses the core's own switch snapshot so it
    // is correct on a //e (INTCXROM/IOSEL) and a II+ (no MMU, both zero).
    // The bus route already sends these addresses to APPLE_ROUTE_BUS, so
    // sp_hit is intercepted there.
    // ------------------------------------------------------------------
    localparam logic [2:0] SP_SLOT = 3'd7;
    localparam logic [2:0] SP_TGT_SLOT_ROM = 3'd0;
    localparam logic [2:0] SP_TGT_C8_ROM   = 3'd1;
    localparam logic [2:0] SP_TGT_DATA     = 3'd2;
    localparam logic [2:0] SP_TGT_CTRL     = 3'd3;
    localparam logic [2:0] SP_TGT_DPOP     = 3'd4;

    /* IOSEL (C8 ownership) for slot 7, snapshot pre-update at X_CAPTURE
     * with the same access; the slot-ROM read that sets it uses the
     * address decode, not this bit. */
    logic cycle_sp_iosel7_q;

    wire sp_intcxrom = cycle_translate_state_q.sw_intcxrom;
    wire sp_is_cxxx  = (cycle_addr_q[15:12] == 4'hC);

    wire sp_slot_rom = sp_active && sp_is_cxxx && !sp_intcxrom &&
                       (cycle_addr_q[11] == 1'b0) &&
                       (cycle_addr_q[10:8] == SP_SLOT);
    wire sp_boot_suppress_hit = sp_boot_suppress && sp_is_cxxx &&
                                !sp_intcxrom &&
                                (cycle_addr_q[11] == 1'b0) &&
                                (cycle_addr_q[10:8] == SP_SLOT);
    wire sp_c8_win   = sp_active && sp_is_cxxx && !sp_intcxrom &&
                       cycle_sp_iosel7_q &&
                       (cycle_addr_q[11] == 1'b1) &&
                       (cycle_addr_q[10:0] != 11'h7FF);
    wire sp_data_reg = sp_c8_win && (cycle_addr_q[10:0] == 11'h7F0);
    wire sp_ctrl_reg = sp_c8_win && (cycle_addr_q[10:0] == 11'h7F1);
    wire sp_pop_reg  = sp_c8_win && (cycle_addr_q[10:0] == 11'h7F2);
    wire sp_hit      = sp_slot_rom || sp_c8_win;

    assign sp_req_target = sp_slot_rom ? SP_TGT_SLOT_ROM :
                           sp_data_reg ? SP_TGT_DATA     :
                           sp_ctrl_reg ? SP_TGT_CTRL     :
                           sp_pop_reg  ? SP_TGT_DPOP     :
                                         SP_TGT_C8_ROM;
    assign sp_req_addr   = cycle_addr_q[10:0];
    assign sp_req_rw     = cycle_rw_q;
    assign sp_req_wdata  = cycle_wdata_q;

    // ------------------------------------------------------------------
    // Disk II read shortcut. The direct path is deliberately narrow:
    // only reads of even $C0E0-$C0EE switches may bypass the Apple bus.
    // Odd reads need the motherboard floating-bus value, CPU writes keep
    // their native data phase, and Q7 write mode keeps all timing at 1 MHz.
    // ------------------------------------------------------------------
    wire d2_io_hit = d2_active &&
                     (cycle_addr_q[15:8] == 8'hC0) &&
                     (cycle_addr_q[7:4] == 4'hE);
    wire d2_fast_hit = d2_io_hit && cycle_rw_q &&
                       !cycle_addr_q[0] && !d2_write_timing_active;
    assign d2_req_addr = cycle_addr_q[3:0];

    // ------------------------------------------------------------------
    // Bus engine + request mux (core when running, ARM during boot copy)
    // ------------------------------------------------------------------
    logic        eng_req_valid;
    logic        eng_req_ready;
    logic [15:0] eng_req_addr;
    logic        eng_req_rw;
    logic [7:0]  eng_req_wdata;
    logic        eng_resp_valid;
    logic [7:0]  eng_resp_rdata;
    logic        eng_post_we;
    logic        eng_post_full;
    logic [15:0] eng_post_addr;
    logic [7:0]  eng_post_wdata;
    logic        eng_bad_c000_pulse;
    logic [15:0] dbg_pc_trace_q [0:15];
    logic        dbg_trace_frozen_q;
    logic [1:0]  dbg_trace_reason_q;
    wire dbg_trace_selftest_event =
        core_en && core_sync && (core_addr == 16'hC600) &&
        vsss.sw_intcxrom;

    logic        fsm_req_valid;
    logic        arm_owns_bus;
    logic        arm_pending_q;
    logic [15:0] arm_addr_q;
    logic        arm_rw_q;
    logic [7:0]  arm_wdata_q;
    logic [7:0]  arm_rdata_q;
    logic        arm_resp_valid_q;

    /* Both requesters hold their valid until the response arrives, but the
     * engine's ready reasserts on the response clock itself -- without this
     * tracker the still-high valid would re-latch and double-execute the
     * I/O access. Gate valid off from handshake until response. */
    logic        req_inflight_q;
    wire         eng_req_valid_raw;

    assign arm_owns_bus      = !core_run;
    assign eng_req_valid_raw = arm_owns_bus ? arm_pending_q : fsm_req_valid;
    assign eng_req_valid     = eng_req_valid_raw && !req_inflight_q;
    assign eng_req_addr      = arm_owns_bus ? arm_addr_q    : cycle_addr_q;
    assign eng_req_rw        = arm_owns_bus ? arm_rw_q      : cycle_rw_q;
    assign eng_req_wdata     = arm_owns_bus ? arm_wdata_q   : cycle_wdata_q;

    globals::AppleBus_write eng_ab_write;
    always_comb begin
        ab_write            = eng_ab_write;
        ab_write.assert_res = assert_apple_res;
    end

    vtw_bus_engine engine_i (
        .clk(clk),
        .rstn(rstn),
        .enable(enable),
        .host_is_iiplus(host_is_iiplus),
        .ab_read(ab_read),
        .ab_write(eng_ab_write),
        .data_drive_in(data_drive_in),
        .data_drive_value_in(data_drive_value_in),
        .dbg_clear(dbg_clear),
        .dbg_trace_freeze(dbg_trace_selftest_event),
        .sync_req_valid(eng_req_valid),
        .sync_req_ready(eng_req_ready),
        .sync_req_addr(eng_req_addr),
        .sync_req_rw(eng_req_rw),
        .sync_req_wdata(eng_req_wdata),
        .sync_resp_valid(eng_resp_valid),
        .sync_resp_rdata(eng_resp_rdata),
        .post_we(eng_post_we),
        .post_addr(eng_post_addr),
        .post_wdata(eng_post_wdata),
        .post_full(eng_post_full),
        .post_fill(post_fill),
        .bus_owned(bus_owned),
        .cnt_sync_cycles(cnt_bus_cycles),
        .cnt_posted_writes(cnt_posted_writes),
        .post_high_water(post_high_water),
        .cnt_post_drops(cnt_post_drops),
        .dbg_last_sync_addr(dbg_last_sync_addr),
        .dbg_last_sync_data(dbg_last_sync_data),
        .dbg_last_sync_rw(dbg_last_sync_rw),
        .dbg_cxxx_ring(dbg_cxxx_ring),
        .dbg_c0_ring(dbg_c0_ring),
        .dbg_sync_write_check(dbg_sync_write_check),
        .dbg_sync_write_addr(dbg_sync_write_addr),
        .dbg_c000_context(dbg_c000_context),
        .dbg_c000_counts(dbg_c000_counts),
        .dbg_io_trace(dbg_io_trace),
        .dbg_bad_c000_pulse(eng_bad_c000_pulse),
        .dbg_bus_faults(dbg_bus_faults)
    );

    generate
        for (genvar pi = 0; pi < 16; pi++) begin : dbg_pc_trace_pack
            assign dbg_pc_trace[pi*16 +: 16] = dbg_pc_trace_q[pi];
        end
    endgenerate
    assign dbg_trace_status = {29'b0, dbg_trace_reason_q,
                               dbg_trace_frozen_q};

    /* Instruction history is passive and event-frozen. It deliberately
     * continues across Apple RESET so a later $C600 trigger preserves the
     * reset-handler path. A phantom keyboard strobe wins over the later
     * consequence of entering self-test. */
    always_ff @(posedge clk) begin
        if (!rstn) begin
            for (int i = 0; i < 16; i++) begin
                dbg_pc_trace_q[i] <= '0;
            end
            dbg_trace_frozen_q <= 1'b0;
            dbg_trace_reason_q <= 2'd0;
        end
        else if (dbg_clear) begin
            for (int i = 0; i < 16; i++) begin
                dbg_pc_trace_q[i] <= '0;
            end
            dbg_trace_frozen_q <= 1'b0;
            dbg_trace_reason_q <= 2'd0;
        end
        else begin
            if (!dbg_trace_frozen_q) begin
                if (core_en && core_sync) begin
                    for (int i = 15; i > 0; i--) begin
                        dbg_pc_trace_q[i] <= dbg_pc_trace_q[i-1];
                    end
                    dbg_pc_trace_q[0] <= core_addr;
                end

                if (eng_bad_c000_pulse) begin
                    dbg_trace_frozen_q <= 1'b1;
                    dbg_trace_reason_q <= 2'd2;
                end
                else if (dbg_trace_selftest_event) begin
                    dbg_trace_frozen_q <= 1'b1;
                    dbg_trace_reason_q <= 2'd3;
                end
            end
        end
    end

    always_ff @(posedge clk) begin
        if (!rstn) begin
            req_inflight_q <= 1'b0;
        end
        else begin
            if (eng_req_valid && eng_req_ready) begin
                req_inflight_q <= 1'b1;
            end
            if (eng_resp_valid) begin
                req_inflight_q <= 1'b0;
            end
            /* Apple RES# transaction boundary (matches the engine dropping
             * its pending request on reset): without this, a request
             * interrupted by a bouncing reset release could leave the
             * tracker set -- or worse, its late response could pair with
             * the first post-reset request and hand the //e firmware
             * another cycle's data (a poisoned BIT $C015 there latches
             * INTCXROM on machine-wide via the trampoline's faithful
             * state preservation). */
            if (!ab_read.res) begin
                req_inflight_q <= 1'b0;
            end
        end
    end

    // ARM request latch (single outstanding, resp mirrors engine's).
    always_ff @(posedge clk) begin
        if (!rstn) begin
            arm_pending_q    <= 1'b0;
            arm_addr_q       <= '0;
            arm_rw_q         <= 1'b1;
            arm_wdata_q      <= '0;
            arm_rdata_q      <= '0;
            arm_resp_valid_q <= 1'b0;
        end
        else begin
            arm_resp_valid_q <= 1'b0;
            if (arm_req_valid && arm_owns_bus && !arm_pending_q) begin
                arm_pending_q <= 1'b1;
                arm_addr_q    <= arm_req_addr;
                arm_rw_q      <= arm_req_rw;
                arm_wdata_q   <= arm_req_wdata;
            end
            if (arm_owns_bus && arm_pending_q && eng_resp_valid) begin
                arm_pending_q    <= 1'b0;
                arm_rdata_q      <= eng_resp_rdata;
                arm_resp_valid_q <= 1'b1;
            end
        end
    end
    assign arm_req_busy   = arm_pending_q;
    assign arm_resp_valid = arm_resp_valid_q;
    assign arm_resp_rdata = arm_rdata_q;

    // ------------------------------------------------------------------
    // Shadow
    // ------------------------------------------------------------------
    logic       shadow_a_en;
    logic       shadow_a_we;
    logic [17:0] shadow_a_addr;
    logic [7:0] shadow_a_rdata;

    vtw_shadow shadow_i (
        .clk(clk),
        .a_en(shadow_a_en),
        .a_addr(shadow_a_addr),
        .a_we(shadow_a_we),
        .a_wdata(cycle_wdata_q),
        .a_rdata(shadow_a_rdata),
        .b_en(sh_en),
        .b_addr(sh_addr),
        .b_we(sh_we),
        .b_wdata(sh_wdata),
        .b_rdata(sh_rdata)
    );

    // ------------------------------------------------------------------
    // Core cycle FSM. X_CAPTURE snapshots the core outputs and banking
    // state. X_ROUTE performs translation and issues the BRAM access from
    // those registers. X_MEM_CAPTURE adds a register between the BRAM and
    // the core ALU. These two explicit cuts keep both sides of the inferred
    // BRAM inside the 133.333 MHz fabric-clock budget.
    // ------------------------------------------------------------------
    typedef enum logic [3:0] {
        X_CAPTURE,
        X_ROUTE,      // translate registered cycle + issue shadow access
        X_MEM_CAPTURE,// capture synchronous BRAM output
        X_MEM_DONE,   // registered read data ready / waiting for pace
        X_POST_STALL, // posted queue full: push pending
        X_BUS,        // sync bus cycle in flight
        X_BUS_DONE,   // response latched, completing edge pending
        X_RW_LOOKUP,  // RamWorks: line-cache hit test / patch
        X_RW_FLUSH,   // RamWorks: dirty line write-back in flight
        X_RW_FILL,    // RamWorks: line read in flight
        X_RW_DONE,    // RamWorks: access complete, waiting for pace
        X_SP_ISSUE,   // SmartPort short-circuit: request the card
        X_SP_WAIT,    // SmartPort: response in flight
        X_SP_DONE,    // SmartPort: response latched, waiting for pace
        X_STATUS_DONE,// synthesized $C01x status read: serve the status byte
        X_DEAD        // unmapped route (must not happen): serve $FF
    } xstate_t;
    xstate_t xstate_q;

    // Private-card response selection. SmartPort needs a single-outstanding
    // guard because its card port can remain busy; Disk II accepts at most
    // one request because the common state leaves ISSUE on the handshake.
    logic       private_d2_q;
    logic       sp_inflight_q;
    assign sp_req_valid = (xstate_q == X_SP_ISSUE) && !private_d2_q &&
                          !sp_inflight_q && core_run && ab_read.res;
    assign d2_req_valid = (xstate_q == X_SP_ISSUE) && private_d2_q &&
                          core_run && ab_read.res;

    /* RamWorks single-line cache (8 bytes), write-allocate: every access
     * lands in a cached line, so a flush is always a full-line write and
     * sequential traffic costs one PSRAM op per 8 bytes. No other agent
     * touches banks 2..128 during a session (posted writes are banks 0/1
     * only and parked cycles are reads). SmartPort freezes the core, then
     * flushes and invalidates this line before its PS-DMA block writes and
     * keeps it frozen until the DMA completes, so the cache cannot go stale.
     * A dirty line
     * is flushed by the sequencer below whenever the core drops into
     * reset -- session end, ARM re-hold, or Apple RES# -- so RamWorks
     * contents survive warm resets and handback; the cache invalidates
     * once clean when the session ends (the motherboard CPU may write
     * aux banks through the serve path between sessions). */
    logic        rwc_valid_q;
    logic        rwc_dirty_q;
    logic [20:0] rwc_line_q;              // decoded[23:3]
    logic [63:0] rwc_data_q;
    logic        rw_inflight_q;           // psram op accepted, resp pending
    logic        rw_req_valid_q;
    logic        rw_req_rw_q;
    logic [23:0] rw_req_addr_q;
    logic        rw_flush_pending_q;
    logic        rw_flush_active_q;
    logic        rw_release_pending_q;
    logic        rw_hold_q;

    wire [20:0] xl_line = xl_decoded[23:3];
    wire [2:0]  xl_lane = xl_decoded[2:0];
    wire        rwc_hit = rwc_valid_q && (rwc_line_q == xl_line);

    assign rw_req_valid = rw_req_valid_q && !rw_inflight_q;
    assign rw_req_rw    = rw_req_rw_q;
    assign rw_req_addr  = rw_req_addr_q;
    assign rw_req_wline = rwc_data_q;

    localparam logic [1:0] SPEED_FULL    = 2'd0;
    localparam logic [1:0] SPEED_DIVIDED = 2'd1;
    localparam logic [1:0] SPEED_1MHZ    = 2'd2;

    logic [1:0]  c074_q;
    logic [15:0] pace_cnt_q;
    logic        pace_tick_pending_q;
    /* Exact-1 MHz $C019 reads span one physical Apple cycle. Resample at
     * data_en and hold the registered operand for the core's completing
     * edge. status_video_vbl retains the physical //e's one-cycle lag. */
    logic        status_vbl_data_phase_q;
    logic        status_vbl_sampled_q;
    logic [31:0] cnt_core_q;
    logic [31:0] cnt_invalid_q;

    /* Per-region slowdown one-shot: nonzero => this and the following
     * cycles pace at 1 MHz. Region classification is combinational off the
     * per-cycle address; the counter is (re)loaded and decremented in the
     * main always_ff beside the $C074 latch. */
    logic [15:0] slow_cnt_q;
    wire         slow_active = (slow_cnt_q != 16'd0);

    wire         sd_is_c0xx  = (cycle_addr_q[15:8] == 8'hC0);
    // Paddle timing set: PADDL0-3 reads $C064-$C067, and PTRIG $C070. The
    // //e soft switches at $C071-$C07F (RamWorks bank, vTW $C074, ...) are
    // deliberately excluded.
    wire         sd_paddle   = sd_is_c0xx &&
                              (((cycle_addr_q[7:2] == 6'b011001)) ||   // $C064-$C067
                               (cycle_addr_q[7:0] == 8'h70));          // $C070
    // Floating-bus/video timing region. $C030-$C03F toggles the speaker,
    // $C040-$C04F is undriven motherboard I/O, and $C050-$C05F controls
    // video/annunciators; reads from all three blocks return the scanner
    // byte. Keep RDVBLBAR $C019 in the same timing group because raster code
    // often polls it and then cycle-counts against the beam.
    wire         sd_floating_io = sd_is_c0xx &&
                                  (((cycle_addr_q[7:4] >= 4'h3) &&
                                    (cycle_addr_q[7:4] <= 4'h5)) ||
                                   (cycle_addr_q[7:0] == 8'h19));
    // Per-slot device I/O $C0n0-$C0nF for slots 1..7 ($C090-$C0FF).
    wire         sd_slot_io  = sd_is_c0xx && cycle_addr_q[7] &&
                              (cycle_addr_q[6:4] != 3'd0);
    wire [2:0]   sd_slot_num = cycle_addr_q[6:4];   // 1..7

    // Per-slot I/O-SELECT space $Cn00-$CnFF for slots 1..7 ($C100-$C7FF).
    // The Mockingboard/Phasor 6522 VIAs live here (VIA0 $Cs00-$Cs0F, VIA1
    // $Cs80-$Cs8F), NOT in the $C0n0 device-select page. A2Desktop's
    // detect_mockingboard/detect_phasor cycle-count two 6522 timer reads at
    // $Cs04/$Cs14 eight cycles apart; those reads must themselves re-lock the
    // core to 1 MHz, or the VIA (a real 1 MHz counter) yields the wrong delta
    // and detection fails at any core speed above 1 MHz.
    wire         sd_iosel      = (cycle_addr_q[15:12] == 4'hC) &&
                                !cycle_addr_q[11] &&
                                (cycle_addr_q[10:8] != 3'd0);   // $C100-$C7FF
    wire [2:0]   sd_iosel_slot = cycle_addr_q[10:8];            // 1..7

    /* Disk II accesses that do not use the private read port remain an
     * unconditional 1 MHz region. The card also holds the whole core at
     * native speed while Q7 write mode is active. This stays independent
     * of the user slowdown mask/window. */
    wire sd_disk2 = (sd_slot_io && (sd_slot_num == 3'd6)) ||
                    (sd_iosel  && (sd_iosel_slot == 3'd6));
    wire sd_disk2_native = sd_disk2 && !d2_fast_hit;

    wire sd_hit = (sd_floating_io && slow_region_en[7]) ||
                  (sd_paddle      && slow_region_en[8]) ||
                  (sd_slot_io && slow_region_en[sd_slot_num  - 3'd1]) ||
                  (sd_iosel   && slow_region_en[sd_iosel_slot - 3'd1]);

    /* $C074 = 1 or 3 forces stock speed; the divided/full presets apply
     * only in state 0. ignore_c074 keeps that state at zero. Per-region
     * slowdown, physical Disk II accesses, and Disk II Q7 write mode also
     * force 1 MHz. */
    wire [1:0] eff_mode =
        (c074_q != 2'd0 || slow_active || sd_disk2_native ||
         d2_write_timing_active) ?
                          SPEED_1MHZ : speed_mode;

    wire pace_ok =
        (eff_mode == SPEED_FULL)    ? 1'b1 :
        (eff_mode == SPEED_DIVIDED) ? (pace_cnt_q >= pace_divider) :
                                      pace_tick_pending_q;

    /* Suppress bus side effects while the core is held or in reset: a
     * held core's cycles are served $FF instead of reaching the bus. */
    wire core_active = enable && core_run && ab_read.res;

    assign ssm_pulse       = core_active && (xstate_q == X_CAPTURE);
    assign ssm_apply_pulse = core_active && (xstate_q == X_ROUTE);
    /* Reads from the fully floating motherboard-I/O region $C030-$C05F
     * execute their physical bus cycle for any speaker/video/annunciator
     * side effect, but the return byte is the main-memory byte fetched by
     * the scanner during that cycle's PHI1. The native line/cycle equation
     * is registered at serve_en, then port A is issued at data_en while the
     * core is stalled in X_BUS. This keeps the address arithmetic off the
     * BRAM setup path and leaves a full fabric clock for the synchronous
     * result before eng_resp_valid is consumed.
     *
     * The mode snapshot is pre-access: the scanner fetch precedes the
     * $C05x soft-switch transition in the same Apple cycle. */
    wire full_floating_read =
        cycle_rw_q &&
        (cycle_addr_q[15:8] == 8'hC0) &&
        (cycle_addr_q[7:4] >= 4'h3) &&
        (cycle_addr_q[7:4] <= 4'h5);
    /* The byte a CPU sees during PHI0-high is the scanner fetch from two
     * native slots earlier. Rewind the complete position so cycle 0/1 also
     * select the correct previous scanline and NTSC/PAL frame. */
    wire [15:0] floating_scan_pos =
        vtw_video_position_rewind(video_mode_50hz, video_line,
                                  video_cycle, 2'd2);
    wire [15:0] floating_scan_addr =
        vtw_scanner_address(video_mode_50hz,
                            floating_scan_pos[15:7],
                            floating_scan_pos[6:0],
                            cycle_video_text_q, cycle_video_mixed_q,
                            cycle_video_page2_q, cycle_video_hires_q,
                            cycle_video_80store_q);
    logic [15:0] floating_scan_addr_q;
    wire floating_scan_addr_latch =
        core_active && (xstate_q == X_BUS) && full_floating_read &&
        ab_read.serve_en && ab_read.rw &&
        (ab_read.addr == cycle_addr_q);
    wire floating_scan_issue =
        core_active && (xstate_q == X_BUS) && full_floating_read &&
        ab_read.data_en && ab_read.rw &&
        (ab_read.addr == cycle_addr_q);
    wire core_shadow_issue =
        (xstate_q == X_ROUTE) && xl_shadow_valid && core_res_n;

    /* No shadow traffic while the core is held: port B (ARM) owns the
     * memory, and a held core's outputs must not scribble on it. */
    assign shadow_a_en   = core_shadow_issue || floating_scan_issue;
    assign shadow_a_addr = floating_scan_issue
                         ? {2'b00, floating_scan_addr_q}
                         : xl_shadow_phys;
    assign shadow_a_we   = core_shadow_issue && xl_is_write;

    always_ff @(posedge clk) begin
        if (!rstn) begin
            floating_scan_addr_q <= '0;
        end
        else if (floating_scan_addr_latch) begin
            floating_scan_addr_q <= floating_scan_addr;
        end
    end

    assign fsm_req_valid = (xstate_q == X_BUS) && core_run && ab_read.res;
    wire core_post_req = (core_active && (xstate_q == X_ROUTE) &&
                          xl_is_posted) || (xstate_q == X_POST_STALL);
    wire core_post_push = core_post_req && !eng_post_full;
    /* SmartPort holds the core outside X_ROUTE until READY, so contention is
     * not expected. Keeping it in the handshake still makes a stray CPU0
     * request fail closed instead of replacing a core write. */
    assign arm_post_ready = core_active && !eng_post_full && !core_post_req;
    assign eng_post_we    = core_post_push || (arm_post_we && arm_post_ready);
    assign eng_post_addr  = (arm_post_we && arm_post_ready)
                          ? arm_post_addr : cycle_addr_q;
    assign eng_post_wdata = (arm_post_we && arm_post_ready)
                          ? arm_post_wdata : cycle_wdata_q;

    /* Completing edge: enable seen high by the core at the clock edge. */
    wire complete_mem    = (xstate_q == X_MEM_DONE)    && pace_ok;
    wire complete_bus    = (xstate_q == X_BUS_DONE)    && pace_ok;
    wire complete_rw     = (xstate_q == X_RW_DONE)     && pace_ok;
    wire complete_sp     = (xstate_q == X_SP_DONE)     && pace_ok;
    wire complete_status = (xstate_q == X_STATUS_DONE) && pace_ok &&
                           (!status_vbl_data_phase_q ||
                            status_vbl_sampled_q);
    wire complete_dead   = (xstate_q == X_DEAD)        && pace_ok;
    assign core_en = core_res_n && !rw_hold_q &&
                     d2_time_ready &&
                     (complete_mem || complete_bus ||
                      complete_rw || complete_sp ||
                      complete_status || complete_dead);
    assign arm_rw_hold_state = rw_hold_q;

    /* One Disk II time tick per virtual 65C02 cycle. Stage the accepted
     * normal-cycle predicate for one fabric clock so the card's readiness
     * feedback does not feed its own rotation enables on the same edge.
     * disk2_card adds the direct-access tick when its registered private
     * request executes. Physical Disk II cycles use ab_read.sss_en inside
     * disk2_card and are suppressed here. */
    wire d2_req_fire = d2_req_valid && d2_req_ready;
    wire d2_cycle_tick_accept =
        core_en && d2_active && !private_d2_q && !sd_disk2_native;
    logic d2_cycle_tick_q;
    assign d2_cycle_tick = d2_cycle_tick_q;
    assign d2_native_cycle_active =
        core_active && (xstate_q == X_BUS) && sd_disk2_native;

    /* The xstate FSM runs one access ahead of a frozen core: these are
     * the only states from which it can still reach the RamWorks cache.
     * From every other state the in-flight access drains to a *_DONE
     * state with no RamWorks traffic and parks there against the gated
     * core_en, so the CPU0 flush below may safely take the cache. */
    wire rw_flush_unsafe =
        core_res_n &&
        ((xstate_q == X_CAPTURE) || (xstate_q == X_ROUTE) ||
         (xstate_q == X_RW_LOOKUP) || (xstate_q == X_RW_FLUSH) ||
         (xstate_q == X_RW_FILL));

    assign c074_state      = c074_q;
    assign cnt_core_cycles = cnt_core_q;
    assign cnt_invalid_routes = cnt_invalid_q;

    always_ff @(posedge clk) begin
        if (!rstn) begin
            video_phase_1mhz    <= 1'b0;
            xstate_q            <= X_CAPTURE;
            c074_q              <= 2'd0;
            pace_cnt_q          <= '0;
            pace_tick_pending_q <= 1'b0;
            status_vbl_data_phase_q <= 1'b0;
            status_vbl_sampled_q    <= 1'b0;
            core_data_in_q      <= 8'hFF;
            cycle_addr_q        <= '0;
            cycle_wdata_q       <= '0;
            cycle_rw_q          <= 1'b1;
            cycle_translate_state_q <= '0;
            cycle_video_text_q   <= 1'b0;
            cycle_video_mixed_q  <= 1'b0;
            cycle_video_page2_q  <= 1'b0;
            cycle_video_hires_q  <= 1'b0;
            cycle_video_80store_q <= 1'b0;
            shr_post_main_wide_q <= 1'b0;
            cnt_core_q          <= '0;
            cnt_invalid_q       <= '0;
            rwc_valid_q         <= 1'b0;
            rwc_dirty_q         <= 1'b0;
            rwc_line_q          <= '0;
            rwc_data_q          <= '0;
            rw_inflight_q       <= 1'b0;
            rw_req_valid_q      <= 1'b0;
            rw_req_rw_q         <= 1'b1;
            rw_req_addr_q       <= '0;
            rw_flush_pending_q  <= 1'b0;
            rw_flush_active_q   <= 1'b0;
            rw_release_pending_q <= 1'b0;
            rw_hold_q           <= 1'b0;
            arm_rw_flush_done   <= 1'b0;
            cycle_sp_iosel7_q   <= 1'b0;
            private_d2_q        <= 1'b0;
            d2_cycle_tick_q     <= 1'b0;
            sp_inflight_q       <= 1'b0;
            slow_cnt_q          <= 16'd0;
        end
        else begin
            arm_rw_flush_done <= 1'b0;
            if (!core_active)
                d2_cycle_tick_q <= 1'b0;
            else
                d2_cycle_tick_q <= d2_cycle_tick_accept;

            if (arm_rw_flush_req) begin
                rw_flush_pending_q <= 1'b1;
                rw_release_pending_q <= 1'b0;
                rw_hold_q          <= 1'b1;
            end
            if (arm_rw_hold_release) begin
                if (rw_flush_active_q) begin
                    /* The line write has reached the PSRAM channel and
                     * cannot be cancelled. Keep the core frozen until its
                     * response arrives, then release below. */
                    rw_release_pending_q <= 1'b1;
                end else begin
                    rw_hold_q            <= 1'b0;
                    rw_release_pending_q <= 1'b0;
                    /* A release while the flush is still pending is CPU0's
                     * timeout path. Cancel it and acknowledge completion so
                     * apple_top cannot leave STATUS.busy stuck forever. */
                    if (rw_flush_pending_q || arm_rw_flush_req) begin
                        rw_flush_pending_q <= 1'b0;
                        arm_rw_flush_done  <= 1'b1;
                    end
                end
            end

            /* Status-only output sampled by CPU1. Registering it keeps the
             * effective-speed decode out of the AXI read-data path; its
             * one-fabric-clock reporting latency is immaterial to the
             * renderer's frame-level mode gate. */
            video_phase_1mhz <=
                core_active && bus_owned && (eff_mode == SPEED_1MHZ);

            // ---- PSRAM line-port handshake ----
            if (rw_req_valid && rw_req_ready) begin
                rw_req_valid_q <= 1'b0;
                rw_inflight_q  <= 1'b1;
            end
            if (rw_resp_valid) begin
                rw_inflight_q <= 1'b0;
            end

            // ---- SmartPort short-circuit handshake ----
            if (sp_req_valid && sp_req_ready) begin
                sp_inflight_q <= 1'b1;
            end
            if (sp_resp_valid) begin
                sp_inflight_q <= 1'b0;
            end
            if (!ab_read.res) begin
                sp_inflight_q <= 1'b0;
            end

            /* Dirty-line write-back whenever the core is held (session
             * end, ARM re-hold, Apple RES#): RamWorks contents must
             * survive resets and handback. The FSM idles in X_CAPTURE
             * while the core is held, so this never competes with an
             * in-flight access; a response to a write op during the held
             * window (whether launched here or an interrupted X_RW_FLUSH)
             * clears the dirty bit. The cache invalidates once clean when
             * the session is disabled -- between sessions the motherboard
             * CPU can write aux banks through the serve path. */
            if (!core_res_n && rwc_dirty_q &&
                !rw_flush_pending_q && !rw_flush_active_q &&
                !rw_inflight_q && !rw_req_valid_q) begin
                rw_req_valid_q <= 1'b1;
                rw_req_rw_q    <= 1'b0;
                rw_req_addr_q  <= {rwc_line_q, 3'b000};
            end
            if (!core_res_n && rw_resp_valid && !rw_req_rw_q) begin
                rwc_dirty_q <= 1'b0;
            end
            if (!enable && !rwc_dirty_q) begin
                rwc_valid_q <= 1'b0;
            end

            /* SmartPort direct copies into RamWorks use the PS DMA client,
             * which sits outside this cache. The flush request has already
             * frozen the core (rw_hold_q gates core_en); wait for the
             * lookahead access to drain clear of the cache states, then
             * write back a dirty line and invalidate. done implies both
             * "cache clean" and "core frozen" -- CPU0 runs the DMA, then
             * releases the hold. */
            if (rw_flush_pending_q && !arm_rw_hold_release &&
                !rw_flush_unsafe &&
                !rw_inflight_q && !rw_req_valid_q) begin
                rw_flush_pending_q <= 1'b0;
                if (rwc_dirty_q) begin
                    rw_req_valid_q    <= 1'b1;
                    rw_req_rw_q       <= 1'b0;
                    rw_req_addr_q     <= {rwc_line_q, 3'b000};
                    rw_flush_active_q <= 1'b1;
                end else begin
                    rwc_valid_q       <= 1'b0;
                    rw_flush_active_q <= 1'b0;
                    arm_rw_flush_done <= 1'b1;
                end
            end
            if (rw_flush_active_q && rw_resp_valid && !rw_req_rw_q) begin
                rwc_dirty_q         <= 1'b0;
                rwc_valid_q         <= 1'b0;
                rw_flush_active_q   <= 1'b0;
                arm_rw_flush_done   <= 1'b1;
                if (rw_release_pending_q || arm_rw_hold_release) begin
                    rw_hold_q            <= 1'b0;
                    rw_release_pending_q <= 1'b0;
                end
            end
            /* Auto-release: session teardown or Apple RES# must never
             * leave the core frozen by a wedged CPU0. A cancelled flush
             * still pulses done -- with the held status low -- so the PS
             * side sees closure and falls back. Placed last so it
             * overrides a same-cycle request or completion. */
            if (!enable || !ab_read.res) begin
                rw_hold_q <= 1'b0;
                if (rw_flush_pending_q || rw_flush_active_q ||
                    arm_rw_flush_req) begin
                    arm_rw_flush_done <= 1'b1;
                end
                rw_flush_pending_q <= 1'b0;
                rw_flush_active_q  <= 1'b0;
                rw_release_pending_q <= 1'b0;
            end
            // Pace bookkeeping: the divider counts fabric clocks, the
            // 1 MHz lock counts Apple data snaps. Both clear on the
            // completing edge, so every mode meters cycle STARTS.
            if (pace_cnt_q != 16'hFFFF) begin
                pace_cnt_q <= pace_cnt_q + 16'd1;
            end
            if (ab_read.data_en) begin
                pace_tick_pending_q <= 1'b1;
            end
            if (core_en) begin
                pace_cnt_q          <= '0;
                pace_tick_pending_q <= 1'b0;
                cnt_core_q          <= cnt_core_q + 32'd1;
            end

            // $C074: decoded from the core's own write cycles. A write of
            // 3 latches until the next Apple reset, like the real card's
            // off-until-reset. The override discards all values and clears
            // any state that software latched before it was enabled.
            if (!ab_read.res || ignore_c074) begin
                c074_q <= 2'd0;
            end
            else if (ssm_pulse && !core_rwb && core_addr == 16'hC074 &&
                     c074_q != 2'd3) begin
                c074_q <= core_data_out[1:0];
            end

            /* The write is already translated here, so xl_is_aux names the
             * actual target bank after RAMWRT/80STORE/PAGE2 routing. Values
             * 1 and 2 both use a second field in main $2000-$9FFF. */
            if (!core_res_n) begin
                shr_post_main_wide_q <= 1'b0;
            end
            else if (xstate_q == X_ROUTE && xl_shadow_valid &&
                     xl_is_write && xl_is_aux &&
                     cycle_addr_q == 16'h9DF8) begin
                shr_post_main_wide_q <=
                    (cycle_wdata_q == 8'd1 || cycle_wdata_q == 8'd2);
            end

            // Per-region slowdown one-shot: (re)arm to slow_duration when a
            // completing cycle hits an enabled region, else count down. The
            // counter meters Apple cycles: while it is nonzero the core is
            // 1 MHz-locked, so one core_en fires per Apple cycle.
            if (core_en && sd_hit) begin
                slow_cnt_q <= slow_duration;
            end
            else if (core_en && slow_cnt_q != 16'd0) begin
                slow_cnt_q <= slow_cnt_q - 16'd1;
            end
            if (!ab_read.res) begin
                slow_cnt_q <= 16'd0;
            end

            unique case (xstate_q)
                X_CAPTURE: begin
                    if (!core_res_n) begin
                        // Core held/reset: idle here (cheap wait state).
                        xstate_q <= X_CAPTURE;
                    end
                    else begin
                        cycle_addr_q            <= core_addr;
                        cycle_wdata_q           <= core_data_out;
                        cycle_rw_q              <= core_rwb;
                        // Accelerated sessions are always an Enhanced //e,
                        // even on a II/II+ host: the fixed //e ROM drives the
                        // //e switches and the private tracker follows them,
                        // so the full MMU model always applies.
                        cycle_translate_state_q <= translate_state_from_sss(vsss);
                        cycle_video_text_q       <= vsss.sw_text;
                        cycle_video_mixed_q      <= vsss.sw_mixed;
                        cycle_video_page2_q      <= vsss.sw_page2;
                        cycle_video_hires_q      <= vsss.sw_hires;
                        cycle_video_80store_q    <= vsss.sw_80store;
                        // Snapshot slot-7 IOSEL pre-update, for the
                        // SmartPort C8-window classifier.
                        cycle_sp_iosel7_q       <= vsss.io_select[SP_SLOT];
                        xstate_q                 <= X_ROUTE;
                    end
                end

                X_ROUTE: begin
                    if (xl_is_bus) begin
                        // core_res_n implies the session is active, so the
                        // bus engine is running and will take the request.
                        // Private Disk II reads and SmartPort accesses are
                        // short-circuited to their cards. The //e status
                        // reads ($C011-$C01F) use tracked switch state.
                        if (sp_boot_suppress_hit) begin
                            // The accelerated ROM is performing a second
                            // cold slot scan after takeover. Disk II was the
                            // selected boot target, so make slot 7
                            // deterministically absent for this probe rather
                            // than leaking whatever is on the physical bus.
                            core_data_in_q <= 8'hFF;
                            xstate_q       <= X_DEAD;
                        end
                        else if (d2_fast_hit) begin
                            private_d2_q <= 1'b1;
                            xstate_q     <= X_SP_ISSUE;
                        end
                        else if (sp_hit) begin
                            private_d2_q <= 1'b0;
                            xstate_q <= X_SP_ISSUE;
                        end
                        else if (xl_c01x_rd) begin
                            core_data_in_q <= status_byte;
                            status_vbl_data_phase_q <=
                                (cycle_addr_q[3:0] == 4'h9) &&
                                (eff_mode == SPEED_1MHZ);
                            status_vbl_sampled_q <= 1'b0;
                            xstate_q       <= X_STATUS_DONE;
                        end
                        else if (xl_btn_rd) begin
                            // II/II+ host: Apple keys read "not pressed"
                            // instead of the floating game-connector level.
                            core_data_in_q <= 8'h00;
                            xstate_q       <= X_DEAD;
                        end
                        else begin
                            xstate_q <= X_BUS;
                        end
                    end
                    else if (xl_is_ramworks) begin
                        xstate_q <= X_RW_LOOKUP;
                    end
                    else if (!xl_shadow_valid) begin
                        /* Non-bus route with no shadow backing and RamWorks
                         * disarmed: unreachable. Serve $FF and count it. */
                        cnt_invalid_q  <= cnt_invalid_q + 32'd1;
                        core_data_in_q <= 8'hFF;
                        xstate_q       <= X_DEAD;
                    end
                    else if (xl_is_posted && eng_post_full) begin
                        xstate_q <= X_POST_STALL;
                    end
                    else begin
                        xstate_q <= X_MEM_CAPTURE;
                    end
                end

                X_RW_LOOKUP: begin
                    if (rwc_hit) begin
                        if (xl_is_write) begin
                            rwc_data_q[8*xl_lane +: 8] <= cycle_wdata_q;
                            rwc_dirty_q                <= 1'b1;
                        end
                        core_data_in_q <= rwc_data_q[8*xl_lane +: 8];
                        xstate_q <= X_RW_DONE;
                    end
                    else if (!rw_inflight_q && !rw_req_valid_q) begin
                        /* Miss: write back a dirty line first (its slot is
                         * about to be reused), then fill. */
                        if (rwc_dirty_q) begin
                            rw_req_valid_q <= 1'b1;
                            rw_req_rw_q    <= 1'b0;
                            rw_req_addr_q  <= {rwc_line_q, 3'b000};
                            xstate_q       <= X_RW_FLUSH;
                        end
                        else begin
                            rw_req_valid_q <= 1'b1;
                            rw_req_rw_q    <= 1'b1;
                            rw_req_addr_q  <= {xl_line, 3'b000};
                            xstate_q       <= X_RW_FILL;
                        end
                    end
                end

                X_RW_FLUSH: begin
                    if (rw_resp_valid) begin
                        rwc_dirty_q    <= 1'b0;
                        rw_req_valid_q <= 1'b1;
                        rw_req_rw_q    <= 1'b1;
                        rw_req_addr_q  <= {xl_line, 3'b000};
                        xstate_q       <= X_RW_FILL;
                    end
                end

                X_RW_FILL: begin
                    if (rw_resp_valid) begin
                        rwc_valid_q <= 1'b1;
                        rwc_line_q  <= xl_line;
                        if (xl_is_write) begin
                            rwc_data_q <= rw_resp_rline;
                            rwc_data_q[8*xl_lane +: 8] <= cycle_wdata_q;
                            rwc_dirty_q <= 1'b1;
                        end
                        else begin
                            rwc_data_q <= rw_resp_rline;
                        end
                        core_data_in_q <= rw_resp_rline[8*xl_lane +: 8];
                        xstate_q <= X_RW_DONE;
                    end
                end

                X_RW_DONE: begin
                    if (core_en) begin
                        xstate_q <= X_CAPTURE;
                    end
                end

                X_POST_STALL: begin
                    /* Shadow write already committed at the X_ROUTE edge;
                     * the queue push retries until accepted (mirrors the
                     * real TW's stall-on-buffer-full). */
                    if (!eng_post_full) begin
                        xstate_q <= X_MEM_CAPTURE;
                    end
                end

                X_MEM_CAPTURE: begin
                    core_data_in_q <= shadow_a_rdata;
                    xstate_q       <= X_MEM_DONE;
                end

                X_MEM_DONE: begin
                    if (core_en) begin
                        xstate_q <= X_CAPTURE;
                    end
                end

                X_BUS: begin
                    if (eng_resp_valid) begin
                        /* Fully floating $C030-$C05F reads return the
                         * same-cycle scanner byte fetched from main shadow.
                         * The physical bus cycle still performed any side
                         * effect. The BRAM read was issued at data_en one
                         * fabric clock before this response is observed. */
                        if (full_floating_read) begin
                            core_data_in_q <= shadow_a_rdata;
                        end else begin
                            core_data_in_q <= eng_resp_rdata;
                        end
                        xstate_q       <= X_BUS_DONE;
                    end
                end

                X_BUS_DONE: begin
                    if (core_en) begin
                        xstate_q <= X_CAPTURE;
                    end
                end

                X_SP_ISSUE: begin
                    // The selected private-card request asserts
                    // combinationally and advances on its handshake.
                    if ((private_d2_q && d2_req_fire) ||
                        (!private_d2_q && sp_req_valid && sp_req_ready)) begin
                        xstate_q <= X_SP_WAIT;
                    end
                end

                X_SP_WAIT: begin
                    if (private_d2_q && d2_resp_valid) begin
                        core_data_in_q <= d2_resp_rdata;
                        xstate_q       <= X_SP_DONE;
                    end
                    else if (!private_d2_q && sp_resp_valid) begin
                        core_data_in_q <= sp_resp_rdata;
                        xstate_q       <= X_SP_DONE;
                    end
                end

                X_SP_DONE: begin
                    if (core_en) begin
                        private_d2_q <= 1'b0;
                        xstate_q <= X_CAPTURE;
                    end
                end

                X_STATUS_DONE: begin
                    if (core_en) begin
                        status_vbl_data_phase_q <= 1'b0;
                        status_vbl_sampled_q    <= 1'b0;
                        xstate_q <= X_CAPTURE;
                    end
                end

                X_DEAD: begin
                    if (core_en) begin
                        xstate_q <= X_CAPTURE;
                    end
                end

                default: xstate_q <= X_CAPTURE;
            endcase

            /* The scanner coordinates have advanced at sss_en and remain
             * stable here at the native read-data phase. pace_ok cannot
             * complete this exact-1 MHz status cycle until the following
             * fabric edge, so core_data_in_q has a full registered setup
             * interval before the W65C02 consumes it. */
            if ((xstate_q == X_STATUS_DONE) &&
                status_vbl_data_phase_q && ab_read.data_en) begin
                core_data_in_q       <= {~status_video_vbl, 7'b0};
                status_vbl_sampled_q <= 1'b1;
            end

            /* Core dropped into reset mid-cycle (session end, ARM re-hold,
             * or Apple RES#): abandon the in-flight state. A latched bus
             * request completes in the engine and clears req_inflight_q on
             * its own; the stale response is simply ignored. */
            if (!core_res_n) begin
                xstate_q                   <= X_CAPTURE;
                private_d2_q              <= 1'b0;
                status_vbl_data_phase_q   <= 1'b0;
                status_vbl_sampled_q      <= 1'b0;
            end
        end
    end

endmodule
