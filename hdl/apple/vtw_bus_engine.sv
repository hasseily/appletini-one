`timescale 1ns / 1ps
//////////////////////////////////////////////////////////////////////////////////
// Virtual TransWarp bus engine.
//
// The vTW's window onto the physical Apple bus. Replicates the physical
// TransWarp's bus contract exactly -- held /DMA, sparse traffic, posted video
// write-through -- with one sanctioned electrical improvement: every cycle we
// emit is driven with ideal 6502 timing (address/R-W updated only at the
// wrapper's TAP_DRIVE_ADDR point in early PHI1, held through the whole cycle,
// bus actively parked between accesses). The late-assert/early-release
// pathology of the real card is designed out.
//
// Cycle types, chosen at each drive_en strobe while running:
//   1. Synchronous cycle -- a pending core/ARM request ($C000-$CFFF I/O,
//      boot-time ROM copy reads). The requester stalls until resp_valid.
//      A sync request whose address falls in a posted-write video window
//      first drains the posted queue (read-after-write consistency);
//      all other sync requests may overtake posted writes, matching the
//      real card's decoupled write-through engine.
//   2. Posted write -- head of the posted queue ({addr,data} video-window
//      writes pushed by vtw_core_top). One per otherwise-idle Apple cycle.
//   3. Parked cycle -- keep a read address driven between real cycles. A //e
//      replays ordinary memory and sanitizes I/O to $FFFF. A II/II+ uses
//      fixed main RAM because $FFFF may be driven by a selected Language
//      Card. The bus is never floated.
//
// /DMA discipline (Apple IIe Tech Note #2): asserted at a drive_en point
// (early PHI1) when the session starts, then one full grace cycle before we
// begin driving the bus, giving the motherboard 6502's buffers time to
// release. A II/II+ keeps /DMA held across in-session RESET; a //e temporarily
// returns to stock cycles so its MMU/IOU can reset. Session handback is
// CTRL-RESET with the vTW disabled, exactly like a real card's $C074 = 3.
//
// The engine is requester-agnostic: vtw_core_top's 65C02 wrapper and the
// ARM's boot-time ROM copy drive the same request interface. Read results
// are latched from ab_read.data at data_en -- the same instant the capture
// path samples the bus, so master and observer agree by construction.
//////////////////////////////////////////////////////////////////////////////////

package vtw_pkg;

    // Posted write-through windows: text pages ($0400-$0BFF) and the
    // graphics pages. Main (bank 0) covers HGR 1/2 ($2000-$5FFF). Aux
    // (bank 1) extends to $9FFF so the Super Hi-Res framebuffer, its
    // scan-line control bytes ($9D00) and palette ($9E00) -- all in aux
    // $2000-$9FFF -- are echoed to the bus and reach the video capture.
    // Everything else never appears on the bus.
    function automatic logic vtw_is_video_window(input logic [15:0] addr,
                                                 input logic is_aux,
                                                 input logic wide_main);
        // wide_main: paged SHR is armed (second field in MAIN
        // $2000-$9FFF), either from the core's direct aux-$9DF8 tracker or
        // the PS CARD_CTRL fallback. Off by default: main $6000+
        // is program space and posting it would flood the write queue.
        return (addr[15:10] == 6'b000001) ||           // $0400-$07FF text
               (addr[15:10] == 6'b000010) ||           // $0800-$0BFF text
               (addr[15:13] == 3'b001)    ||           // $2000-$3FFF gfx
               (addr[15:13] == 3'b010)    ||           // $4000-$5FFF gfx
               ((is_aux || wide_main) && (addr[15:13] == 3'b011)) ||  // $6000-$7FFF
               ((is_aux || wide_main) && (addr[15:13] == 3'b100));    // $8000-$9FFF
    endfunction

    // Soft switches that steer video-window writes between main and aux
    // memory. A posted write lands in whichever bank the MOTHERBOARD's
    // switch state selects when it drains, so a sync cycle touching one
    // of these must flush the queue first or queued aux writes would
    // retire into the wrong bank:
    //   $C000-$C007 writes  (80STORE / RAMRD / RAMWRT and neighbors)
    //   $C054-$C057 any     (PAGE2 / HIRES switch on read or write)
    function automatic logic vtw_is_bank_steer(input logic [15:0] addr,
                                               input logic rw);
        return ((addr[15:3] == 13'h1800) && !rw) ||
               (addr[15:2] == 14'h3015);
    endfunction

    /* Rewind a native video position by zero, one, or two Apple cycles.
     * The vTW uses one cycle of history for synthesized RDVBLBAR and two
     * cycles for CPU-visible floating-bus data. A rewind from cycle 0/1
     * crosses to the previous scanline, including the NTSC/PAL frame wrap. */
    function automatic logic [15:0] vtw_video_position_rewind(
        input logic       mode_50hz,
        input logic [8:0] line,
        input logic [6:0] cycle,
        input logic [1:0] cycles_back
    );
        logic [8:0] rewound_line;
        logic [6:0] rewound_cycle;
        logic [8:0] line_max;

        line_max = mode_50hz ? 9'd311 : 9'd261;
        rewound_line = line;
        if ({1'b0, cycle} >= {6'b0, cycles_back}) begin
            rewound_cycle = cycle - cycles_back;
        end
        else begin
            rewound_cycle = cycle + 7'd65 - cycles_back;
            rewound_line = (line == 9'd0) ? line_max : line - 9'd1;
        end

        return {rewound_line, rewound_cycle};
    endfunction

    /* Apple //e video-scanner address for one native 65-cycle bus slot.
     * This is the address-generator equation from Understanding the Apple
     * IIe (and AppleWin's independently validated VideoGetScannerAddress).
     *
     * The caller supplies the video mode that was active during PHI1. PAGE2
     * is ignored while 80STORE is set, exactly as on the //e scanner. The
     * result always names main memory: even in 80-column/double-hires modes,
     * the byte left on the floating CPU data bus is the main-bank fetch. */
    function automatic logic [15:0] vtw_scanner_address(
        input logic       mode_50hz,
        input logic [8:0] line,
        input logic [6:0] cycle,
        input logic       sw_text,
        input logic       sw_mixed,
        input logic       sw_page2,
        input logic       sw_hires,
        input logic       sw_80store
    );
        logic [6:0]  h_clock;
        logic [6:0]  h_state;
        logic [9:0]  v_state;
        logic [4:0]  sum;
        logic        hires_eff;
        logic        page2_eff;
        logic [15:0] addr;

        h_clock = cycle + 7'd40;
        if (h_clock >= 7'd65) begin
            h_clock = h_clock - 7'd65;
        end

        h_state = 7'h18 + h_clock;
        if (h_clock >= 7'd41) begin
            // Horizontal preset repeats state zero for one scanner clock.
            h_state = h_state - 7'd1;
        end

        v_state = 10'h100 + {1'b0, line};
        if (line >= 9'd256) begin
            v_state = v_state -
                      (mode_50hz ? 10'd312 : 10'd262);
        end

        hires_eff = sw_hires && !sw_text;
        // Mixed HGR switches back to text addressing in its bottom region.
        if (hires_eff && sw_mixed && v_state[7] && v_state[5]) begin
            hires_eff = 1'b0;
        end

        sum = 5'h0D +
              {2'b00, h_state[5:3]} +
              {1'b0, v_state[7], v_state[6],
                       v_state[7], v_state[6]};

        addr      = '0;
        addr[2:0] = h_state[2:0];
        addr[6:3] = sum[3:0];
        addr[9:7] = v_state[5:3];

        page2_eff = sw_page2 && !sw_80store;
        if (hires_eff) begin
            addr[12:10] = v_state[2:0];
            addr[14:13] = page2_eff ? 2'b10 : 2'b01;
        end
        else begin
            addr[11:10] = page2_eff ? 2'b10 : 2'b01;
        end

        return addr;
    endfunction

endpackage

module vtw_bus_engine #(
    parameter int POST_DEPTH_LOG2 = 9,
    // Queue-full is reported this many entries early so the core wrapper can
    // stall its CE with pushes already in flight.
    parameter int POST_FULL_MARGIN = 4
) (
    input  logic                    clk,
    input  logic                    rstn,

    /* Session enable: vTW configured on AND the //e or II/II+ machine gate
     * satisfied (apple_top qualifies). Deassert + CTRL-RESET = handback. */
    input  logic                    enable,
    /* A II/II+ needs a fixed, side-effect-free park address. In particular,
     * $FFFF is not inert while a Language Card has RAM banked in: the card
     * asserts /INH and drives data there between accelerated I/O cycles.
     * The //e retains the established replay/sanitize policy. */
    input  logic                    host_is_iiplus,

    input  globals::AppleBus_read   ab_read,
    output globals::AppleBus_write  ab_write,
    /* Arbiter-merged data-drive enable. This is sampled only for passive
     * diagnostics so a $C000 failure can identify whether the immediately
     * preceding physical cycle had an internal card driving the data bus. */
    input  logic                    data_drive_in,
    input  logic [7:0]              data_drive_value_in,
    input  logic                    dbg_clear,
    /* Freeze the rolling I/O trace when vtw_core_top sees an instruction
     * fetch from the internal $C600 self-test. Apple RESET deliberately
     * does not freeze it: the post-reset ROM path is the evidence. */
    input  logic                    dbg_trace_freeze,

    // Synchronous cycle request (core wrapper, or ARM during boot copy).
    input  logic                    sync_req_valid,
    output logic                    sync_req_ready,
    input  logic [15:0]             sync_req_addr,
    input  logic                    sync_req_rw,      // 1=read, 0=write
    input  logic [7:0]              sync_req_wdata,
    output logic                    sync_resp_valid,  // 1-clk pulse
    output logic [7:0]              sync_resp_rdata,

    // Posted write push (video-window writes, filtered by vtw_core_top).
    input  logic                    post_we,
    input  logic [15:0]             post_addr,
    input  logic [7:0]              post_wdata,
    output logic                    post_full,        // stall the core CE
    output logic [POST_DEPTH_LOG2:0] post_fill,

    // Session status + diagnostics (`:vtw status`).
    output logic                    bus_owned,        // parked driver active
    output logic [31:0]             cnt_sync_cycles,
    output logic [31:0]             cnt_posted_writes,
    output logic [POST_DEPTH_LOG2:0] post_high_water,
    output logic [31:0]             cnt_post_drops,   // pushes into a hard-full queue (must stay 0)

    /* Bench forensics: the last completed synchronous cycle. */
    output logic [15:0]             dbg_last_sync_addr,
    output logic [7:0]              dbg_last_sync_data,
    output logic                    dbg_last_sync_rw,
    /* The last eight $C100-$CFFF synchronous cycles (newest in [0]).
     * Idle keyboard polling is $C0xx-only, so this ring preserves the
     * tail of the most recent slot-ROM/C8 activity -- e.g., where a
     * dying driver call stopped -- however long ago it happened. */
    output logic [8*16-1:0]         dbg_cxxx_ring,
    /* The FIRST eight $C00x/$C01x cycles after an Apple reset, WITH their
     * latched data: {2'b0, rw, addr[4:0], data[7:0]} per entry, newest in
     * [0] (so the first post-reset access is entry [7] once full, and the
     * ring freezes there). Excludes READS of $C000/$C010 (keyboard
     * polling). Captures the reset handler's own trampoline sequence --
     * the BIT $C015 snapshot, the STA $C007, and whether the restoring
     * STA $C006 ever follows -- verbatim. Re-arms at every RES#. */
    output logic [8*16-1:0]         dbg_c0_ring,
    /* Synchronous-write loopback: high half is a saturating mismatch count,
     * then the last mismatching expected and observed bytes. */
    output logic [31:0]             dbg_sync_write_check,
    output logic [15:0]             dbg_sync_write_addr,
    /* Last accelerated $C000 read and its immediately preceding physical
     * cycle. Context packing:
     * {prev_addr[15:0], prev_kind[1:0], prev_rw, prev_data_drive,
     *  4'b0, c000_data[7:0]}. kind: 0 park, 1 posted, 2 sync read,
     * 3 sync write. Counts pack {bit7_high[15:0], total[15:0]}. */
    output logic [31:0]             dbg_c000_context,
    output logic [31:0]             dbg_c000_counts,
    /* Event-frozen physical-cycle trace. Each 32-bit entry is
     * {rw, self_drive, dma_released, addr_bad, data_bad, 3'b0,
     *  requested_addr[15:0], sampled_data[7:0]}; newest in [31:0].
     * The first II+ $C000 read with bit 7 high raises the pulse and freezes
     * this trace on the offending cycle. */
    output logic [16*32-1:0]        dbg_io_trace,
    output logic                    dbg_bad_c000_pulse,
    /* {address_mismatches[15:0], self-drive data mismatches[7:0],
     *   observed-DMA-released cycles[7:0]}. */
    output logic [31:0]             dbg_bus_faults
);

    import vtw_pkg::*;

    localparam int POST_DEPTH = 1 << POST_DEPTH_LOG2;

    // ------------------------------------------------------------------
    // Session FSM
    // ------------------------------------------------------------------
    typedef enum logic [2:0] {
        S_OFF,          // outputs released
        S_ARM,          // waiting for post-reset stock run + PHI1 /DMA point
        S_GRACE,        // /DMA held, one full cycle for the 6502 to let go
        S_RUN,          // parked driver + cycle engine active
        S_RESET_HOLD,   // II+: addr/data released, /DMA held through RESET
        S_RELEASE       // addr/data released; one guarded cycle before /DMA
    } session_t;
    session_t session_q;

    /* Stock cycles the motherboard 6502 must run after RES# rises before
     * we assert /DMA (see the S_ARM comment). */
    localparam int STOCK_RUN_CYCLES = 80;
    /* Main RAM, outside zero page, stack, display, I/O, and LC space. A
     * normal motherboard DRAM read has no side effects and releases through
     * the same path a native CPU uses before its next I/O access. */
    localparam logic [15:0] IIPLUS_PARK_ADDR = 16'h0200;
    logic [7:0] stock_run_cnt_q;

    // Per-cycle engine state (meaningful in S_RUN)
    typedef enum logic [2:0] {
        CYC_PARKED,
        CYC_POST_FETCH,   // posted queue BRAM read this clock
        CYC_POST_LOAD,    // queue data -> drive registers
        CYC_POST,         // posted write cycle on the bus
        CYC_SYNC          // synchronous cycle on the bus
    } cyc_t;
    cyc_t cyc_q;

    // ------------------------------------------------------------------
    // Posted write queue (inferred BRAM), its own always block. push/pop
    // never coincide with clear: pop happens one clock after a drive_en,
    // clear exactly at one; pushes are suppressed during the clear clock.
    // ------------------------------------------------------------------
    logic [23:0]                 post_mem [0:POST_DEPTH-1];
    logic [POST_DEPTH_LOG2-1:0]  post_wptr_q, post_rptr_q;
    logic [POST_DEPTH_LOG2:0]    post_fill_q;
    logic [POST_DEPTH_LOG2:0]    post_hw_q;
    logic [23:0]                 post_rdata_q;
    logic [31:0]                 post_drops_q;

    wire post_hard_full = (post_fill_q == (POST_DEPTH_LOG2+1)'(POST_DEPTH));
    wire post_pop   = (session_q == S_RUN) && ab_read.res && enable &&
                      (cyc_q == CYC_POST_FETCH);
    /* Apple RES# is a transaction boundary: queued posted writes are
     * discarded (the reset flow rewrites the screen; a real TW's write
     * buffer would not survive its reset either). */
    wire post_clear = (session_q == S_RUN) &&
                      ((ab_read.drive_en && !enable) || !ab_read.res);
    wire post_push  = post_we && !post_hard_full && !post_clear;

    always_ff @(posedge clk) begin
        if (!rstn) begin
            post_wptr_q  <= '0;
            post_rptr_q  <= '0;
            post_fill_q  <= '0;
            post_hw_q    <= '0;
            post_drops_q <= '0;
            post_rdata_q <= '0;
        end
        else begin
            if (post_push) begin
                post_mem[post_wptr_q] <= {post_addr, post_wdata};
                post_wptr_q           <= post_wptr_q + 1'b1;
            end
            if (post_we && (post_hard_full || post_clear)) begin
                post_drops_q <= post_drops_q + 32'd1;
            end
            if (post_pop) begin
                post_rdata_q <= post_mem[post_rptr_q];
                post_rptr_q  <= post_rptr_q + 1'b1;
            end
            if (post_clear) begin
                post_rptr_q <= post_wptr_q;
                post_fill_q <= '0;
            end
            else begin
                post_fill_q <= post_fill_q
                    + ((POST_DEPTH_LOG2+1)'(post_push ? 1 : 0))
                    - ((POST_DEPTH_LOG2+1)'(post_pop  ? 1 : 0));
            end
            if (post_fill_q > post_hw_q) begin
                post_hw_q <= post_fill_q;
            end
        end
    end

    assign post_full = (post_fill_q >=
                        (POST_DEPTH_LOG2+1)'(POST_DEPTH - POST_FULL_MARGIN));
    assign post_fill       = post_fill_q;
    assign post_high_water = post_hw_q;
    assign cnt_post_drops  = post_drops_q;

    // ------------------------------------------------------------------
    // Request latch + drive registers
    // ------------------------------------------------------------------
    logic        sync_pending_q;
    logic [15:0] sync_addr_q;
    logic        sync_rw_q;
    logic [7:0]  sync_wdata_q;
    logic        resp_valid_q;
    logic [7:0]  resp_rdata_q;

    logic [15:0] wr_addr_q;
    logic        wr_rw_q;
    logic        wr_addr_rw_en_q;
    logic [7:0]  wr_data_q;
    logic        wr_data_en_q;
    logic        assert_dma_q;

    logic [31:0] cnt_sync_q;
    logic [31:0] cnt_posted_q;
    logic [15:0] dbg_sync_addr_q;
    logic        dbg_sync_rw_q;
    logic [15:0] dbg_ring_q [0:7];
    logic [15:0] dbg_c0_ring_q [0:7];
    logic [3:0]  dbg_c0_count_q;   // freezes the ring at 8 entries
    logic [15:0] dbg_wr_mismatch_count_q;
    logic [7:0]  dbg_wr_expected_q;
    logic [7:0]  dbg_wr_observed_q;
    logic [15:0] dbg_wr_addr_q;
    logic [15:0] dbg_prev_addr_q;
    logic [1:0]  dbg_prev_kind_q;
    logic        dbg_prev_rw_q;
    logic        dbg_prev_data_drive_q;
    logic [15:0] dbg_c000_prev_addr_q;
    logic [1:0]  dbg_c000_prev_kind_q;
    logic        dbg_c000_prev_rw_q;
    logic        dbg_c000_prev_data_drive_q;
    logic [7:0]  dbg_c000_data_q;
    logic [15:0] dbg_c000_total_q;
    logic [15:0] dbg_c000_bit7_q;
    logic [31:0] dbg_io_trace_q [0:15];
    logic        dbg_io_trace_frozen_q;
    logic [15:0] dbg_addr_mismatch_q;
    logic [7:0]  dbg_self_data_mismatch_q;
    logic [7:0]  dbg_dma_released_q;

    assign dbg_last_sync_addr = dbg_sync_addr_q;
    assign dbg_last_sync_data = resp_rdata_q;
    assign dbg_last_sync_rw   = dbg_sync_rw_q;
    assign dbg_sync_write_check = {dbg_wr_mismatch_count_q,
                                   dbg_wr_expected_q,
                                   dbg_wr_observed_q};
    assign dbg_sync_write_addr  = dbg_wr_addr_q;
    assign dbg_c000_context     = {dbg_c000_prev_addr_q,
                                   dbg_c000_prev_kind_q,
                                   dbg_c000_prev_rw_q,
                                   dbg_c000_prev_data_drive_q,
                                   4'b0000,
                                   dbg_c000_data_q};
    assign dbg_c000_counts      = {dbg_c000_bit7_q, dbg_c000_total_q};
    assign dbg_bus_faults       = {dbg_addr_mismatch_q,
                                   dbg_self_data_mismatch_q,
                                   dbg_dma_released_q};
    generate
        for (genvar gi = 0; gi < 8; gi++) begin : dbg_ring_pack
            assign dbg_cxxx_ring[gi*16 +: 16] = dbg_ring_q[gi];
            assign dbg_c0_ring[gi*16 +: 16]   = dbg_c0_ring_q[gi];
        end
        for (genvar ti = 0; ti < 16; ti++) begin : dbg_io_trace_pack
            assign dbg_io_trace[ti*32 +: 32] = dbg_io_trace_q[ti];
        end
    endgenerate

    /* A video-window sync request must not overtake queued posted writes
     * (read-after-write consistency), and neither may a bank-steering
     * soft-switch access (queued aux/main writes must retire under the
     * switch regime they were issued in). */
    /* Slot-7 DEVSEL includes the linear-text command port. Drain posted
     * text-buffer writes before any register access so SHOW enters the
     * capture stream after every byte it exposes. */
    wire sync_slot7_devsel = (sync_addr_q[15:4] == 12'hC0F);
    wire sync_flush_wait = (vtw_is_video_window(sync_addr_q, 1'b1, 1'b1) ||
                            vtw_is_bank_steer(sync_addr_q, sync_rw_q) ||
                            sync_slot7_devsel) &&
                            (post_fill_q != '0);
    wire [1:0] dbg_current_kind =
        (cyc_q == CYC_POST) ? 2'd1 :
        (cyc_q == CYC_SYNC) ? (sync_rw_q ? 2'd2 : 2'd3) :
                              2'd0;
    wire dbg_sync_complete = ab_read.data_en &&
                             (cyc_q == CYC_SYNC) &&
                             ab_read.res && enable;
    wire dbg_addr_bad = (ab_read.addr != sync_addr_q);
    wire dbg_self_data_bad = data_drive_in &&
                             (ab_read.data != data_drive_value_in);
    wire dbg_bad_c000_event = dbg_sync_complete && host_is_iiplus &&
                              sync_rw_q &&
                              (sync_addr_q == 16'hC000) &&
                              ab_read.data[7];
    assign dbg_bad_c000_pulse = dbg_bad_c000_event &&
                                !dbg_io_trace_frozen_q;

    assign sync_req_ready  = (session_q == S_RUN) && enable &&
                             !sync_pending_q && ab_read.res;
    assign sync_resp_valid = resp_valid_q;
    assign sync_resp_rdata = resp_rdata_q;
    assign bus_owned       = wr_addr_rw_en_q;
    assign cnt_sync_cycles   = cnt_sync_q;
    assign cnt_posted_writes = cnt_posted_q;

    always_comb begin
        ab_write               = '0;
        ab_write.wr_addr       = wr_addr_q;
        ab_write.wr_rw         = wr_rw_q;
        ab_write.wr_addr_rw_en = wr_addr_rw_en_q;
        ab_write.wr_data       = wr_data_q;
        ab_write.wr_data_en    = wr_data_en_q;
        /* Posted writes target motherboard video RAM. The normal data
         * window stays asserted so the late bus capture sees the byte, but
         * it opens too late for the //e DRAM CAS setup limit. Arm the same
         * early PH0 DMA-write window used by the AD8088 master as well.
         * Synchronous $C0xx writes remain on the normal window: virtual and
         * physical slot devices consume them at the late data strobe. */
        ab_write.wr_dma_data_en = wr_data_en_q && (cyc_q == CYC_POST);
        ab_write.assert_dma    = assert_dma_q;
    end

    /* Rolling physical-cycle recorder. It is deliberately independent of
     * the engine control state: diagnostics cannot feed timing or behavior
     * back into the functional path. `busdbg clear` re-arms it after the
     * user's final deliberate RESET; the first interesting event then
     * preserves the lead-up even if the virtual CPU subsequently runs away. */
    always_ff @(posedge clk) begin
        if (!rstn || dbg_clear) begin
            for (int i = 0; i < 16; i++) begin
                dbg_io_trace_q[i] <= '0;
            end
            dbg_io_trace_frozen_q  <= 1'b0;
            dbg_addr_mismatch_q    <= '0;
            dbg_self_data_mismatch_q <= '0;
            dbg_dma_released_q     <= '0;
        end
        else if (!dbg_io_trace_frozen_q) begin
            if (dbg_sync_complete) begin
                for (int i = 15; i > 0; i--) begin
                    dbg_io_trace_q[i] <= dbg_io_trace_q[i-1];
                end
                dbg_io_trace_q[0] <= {
                    sync_rw_q,
                    data_drive_in,
                    ab_read.dma,
                    dbg_addr_bad,
                    dbg_self_data_bad,
                    3'b000,
                    sync_addr_q,
                    ab_read.data
                };

                if (dbg_addr_bad && dbg_addr_mismatch_q != 16'hFFFF) begin
                    dbg_addr_mismatch_q <= dbg_addr_mismatch_q + 16'd1;
                end
                if (dbg_self_data_bad &&
                    dbg_self_data_mismatch_q != 8'hFF) begin
                    dbg_self_data_mismatch_q <=
                        dbg_self_data_mismatch_q + 8'd1;
                end
                if (ab_read.dma && dbg_dma_released_q != 8'hFF) begin
                    dbg_dma_released_q <= dbg_dma_released_q + 8'd1;
                end
            end

            if (dbg_trace_freeze || dbg_bad_c000_event) begin
                dbg_io_trace_frozen_q <= 1'b1;
            end
        end
    end

    // ------------------------------------------------------------------
    // Sequential
    // ------------------------------------------------------------------
    always_ff @(posedge clk) begin
        if (!rstn) begin
            session_q       <= S_OFF;
            cyc_q           <= CYC_PARKED;
            sync_pending_q  <= 1'b0;
            sync_addr_q     <= '0;
            sync_rw_q       <= 1'b1;
            sync_wdata_q    <= '0;
            resp_valid_q    <= 1'b0;
            resp_rdata_q    <= '0;
            wr_addr_q       <= 16'hFFFF;
            wr_rw_q         <= 1'b1;
            wr_addr_rw_en_q <= 1'b0;
            wr_data_q       <= '0;
            wr_data_en_q    <= 1'b0;
            assert_dma_q    <= 1'b0;
            cnt_sync_q      <= '0;
            cnt_posted_q    <= '0;
            dbg_sync_addr_q <= '0;
            dbg_sync_rw_q   <= 1'b1;
            for (int i = 0; i < 8; i++) begin
                dbg_ring_q[i]    <= '0;
                dbg_c0_ring_q[i] <= '0;
            end
            dbg_c0_count_q  <= '0;
            stock_run_cnt_q <= '0;
            dbg_wr_mismatch_count_q <= '0;
            dbg_wr_expected_q       <= '0;
            dbg_wr_observed_q       <= '0;
            dbg_wr_addr_q           <= '0;
            dbg_prev_addr_q         <= 16'hFFFF;
            dbg_prev_kind_q         <= 2'd0;
            dbg_prev_rw_q           <= 1'b1;
            dbg_prev_data_drive_q   <= 1'b0;
            dbg_c000_prev_addr_q       <= 16'hFFFF;
            dbg_c000_prev_kind_q       <= 2'd0;
            dbg_c000_prev_rw_q         <= 1'b1;
            dbg_c000_prev_data_drive_q <= 1'b0;
            dbg_c000_data_q          <= '0;
            dbg_c000_total_q         <= '0;
            dbg_c000_bit7_q          <= '0;
        end
        else begin
            resp_valid_q <= 1'b0;

            // ---- Sync request latch ----
            if (sync_req_valid && sync_req_ready) begin
                sync_pending_q <= 1'b1;
                sync_addr_q    <= sync_req_addr;
                sync_rw_q      <= sync_req_rw;
                sync_wdata_q   <= sync_req_wdata;
            end

            // ---- Session transitions + per-cycle decisions ----
            unique case (session_q)
                S_OFF: begin
                    wr_addr_rw_en_q <= 1'b0;
                    wr_data_en_q    <= 1'b0;
                    assert_dma_q    <= 1'b0;
                    cyc_q           <= CYC_PARKED;
                    stock_run_cnt_q <= '0;
                    if (enable && ab_read.drive_en) begin
                        session_q <= S_ARM;
                    end
                end

                S_ARM: begin
                    /* Assert /DMA at a PHI1 drive point (TN#2: transitions
                     * during PHI1 only), after the motherboard has run
                     * STOCK_RUN_CYCLES real cycles with RES# high.
                     *
                     * The //e MMU's soft-switch clear is not completed by
                     * the RES#-low window alone: it requires the 6502's
                     * own post-reset bus activity (reset sequence +
                     * $FFFC/$FFFD vector fetch). Every //e RESET release,
                     * and the initial acquisition on either host, therefore
                     * gives the motherboard 6502 a stock run before the bus
                     * is taken. 80 cycles covers the vector
                     * fetch and INIT's display soft switches (snooped by
                     * the tracker, stock-desired) while ending long before
                     * the reset handler's first MMU write (the $FA77
                     * trampoline's STA $C007, ~cycle 160). Its only other
                     * traffic is zero-page/stack writes to real RAM, which
                     * is stale under the vTW anyway. The vTW core runs
                     * from shadow in parallel and stalls on its first
                     * $C0xx sync access until S_RUN -- no interleaving. */
                    if (!enable) begin
                        session_q <= S_OFF;
                    end
                    else if (!ab_read.res) begin
                        stock_run_cnt_q <= '0;
                    end
                    else if (ab_read.drive_en) begin
                        if (stock_run_cnt_q == 8'(STOCK_RUN_CYCLES)) begin
                            assert_dma_q <= 1'b1;
                            session_q    <= S_GRACE;
                        end
                        else begin
                            stock_run_cnt_q <= stock_run_cnt_q + 8'd1;
                        end
                    end
                end

                S_GRACE: begin
                    /* One full Apple cycle with /DMA held before we drive:
                     * the 6502's bus buffers are guaranteed released. */
                    if (!enable) begin
                        /* Do not deassert /DMA at an arbitrary fabric
                         * clock. The release state waits for the next
                         * early-PHI1 drive point. */
                        session_q <= S_RELEASE;
                    end
                    else if (!ab_read.res) begin
                        stock_run_cnt_q <= '0;
                        session_q <= host_is_iiplus
                                   ? S_RESET_HOLD
                                   : S_RELEASE;
                    end
                    else if (ab_read.drive_en) begin
                        begin
                            wr_addr_q       <= host_is_iiplus
                                             ? IIPLUS_PARK_ADDR
                                             : 16'hFFFF;
                            wr_addr_rw_en_q <= 1'b1;
                            wr_rw_q         <= 1'b1;
                            session_q       <= S_RUN;
                        end
                    end
                end

                S_RUN: begin
                    /* A reset/disable boundary dominates the two-stage
                     * posted-write launcher on every fabric clock. FETCH
                     * and LOAD have not reached the pins and are canceled;
                     * a POST or SYNC cycle already on the bus is allowed to
                     * finish electrically, then all drivers are removed at
                     * the next early-PHI1 drive point. */
                    if (!ab_read.res || !enable) begin
                        if (!ab_read.res) begin
                            /* The requester clears its own in-flight flag
                             * on the same reset. Never emit a stale response
                             * that could pair with a post-reset request. */
                            sync_pending_q <= 1'b0;
                        end
                        else if (sync_pending_q) begin
                            /* A deliberate disable aborts a waiter cleanly. */
                            resp_rdata_q   <= 8'hFF;
                            resp_valid_q   <= 1'b1;
                            sync_pending_q <= 1'b0;
                        end
                        if (cyc_q == CYC_POST_FETCH ||
                            cyc_q == CYC_POST_LOAD) begin
                            wr_data_en_q <= 1'b0;
                            cyc_q        <= CYC_PARKED;
                        end
                    end
                    else begin
                        // Posted-fetch pipeline (clocks after drive_en);
                        // the queue pop itself lives in the FIFO block.
                        if (cyc_q == CYC_POST_FETCH) begin
                            cyc_q <= CYC_POST_LOAD;
                        end
                        else if (cyc_q == CYC_POST_LOAD) begin
                            wr_addr_q    <= post_rdata_q[23:8];
                            wr_data_q    <= post_rdata_q[7:0];
                            wr_rw_q      <= 1'b0;
                            wr_data_en_q <= 1'b1;
                            cyc_q        <= CYC_POST;
                        end
                    end

                    // ---- Cycle completion at the data snap ----
                    if (ab_read.data_en) begin
                        /* On a II/II+, clear the logical write drive at the
                         * data snap. The wrapper independently enforces raw
                         * PHI0 as the physical direction boundary, including
                         * vTW-owned cycles, so this late cleanup cannot extend
                         * stale data into the following cycle. Keep the
                         * established //e master timing unchanged. */
                        if (host_is_iiplus &&
                            ((cyc_q == CYC_SYNC && !sync_rw_q) ||
                             (cyc_q == CYC_POST))) begin
                            wr_data_en_q <= 1'b0;
                        end

                        /* Every owned Apple cycle becomes the predecessor
                         * context for the next accelerated $C000 read. */
                        if (ab_read.res) begin
                            if (cyc_q == CYC_SYNC && sync_rw_q &&
                                sync_addr_q == 16'hC000) begin
                                dbg_c000_prev_addr_q       <= dbg_prev_addr_q;
                                dbg_c000_prev_kind_q       <= dbg_prev_kind_q;
                                dbg_c000_prev_rw_q         <= dbg_prev_rw_q;
                                dbg_c000_prev_data_drive_q <=
                                    dbg_prev_data_drive_q;
                                dbg_c000_data_q <= ab_read.data;
                                if (dbg_c000_total_q != 16'hFFFF) begin
                                    dbg_c000_total_q <= dbg_c000_total_q + 16'd1;
                                end
                                if (ab_read.data[7] &&
                                    dbg_c000_bit7_q != 16'hFFFF) begin
                                    dbg_c000_bit7_q <= dbg_c000_bit7_q + 16'd1;
                                end
                            end
                            dbg_prev_addr_q       <= wr_addr_q;
                            dbg_prev_kind_q       <= dbg_current_kind;
                            dbg_prev_rw_q         <= wr_rw_q;
                            dbg_prev_data_drive_q <= data_drive_in;
                        end

                        if (cyc_q == CYC_SYNC && ab_read.res && enable) begin
                            resp_rdata_q    <= ab_read.data;
                            resp_valid_q    <= 1'b1;
                            sync_pending_q  <= 1'b0;
                            cnt_sync_q      <= cnt_sync_q + 32'd1;
                            dbg_sync_addr_q <= sync_addr_q;
                            dbg_sync_rw_q   <= sync_rw_q;
                            if (!sync_rw_q &&
                                ab_read.data != sync_wdata_q) begin
                                if (dbg_wr_mismatch_count_q != 16'hFFFF) begin
                                    dbg_wr_mismatch_count_q <=
                                        dbg_wr_mismatch_count_q + 16'd1;
                                end
                                dbg_wr_expected_q <= sync_wdata_q;
                                dbg_wr_observed_q <= ab_read.data;
                                dbg_wr_addr_q     <= sync_addr_q;
                            end
                            if ((sync_addr_q[15:12] == 4'hC) &&
                                (sync_addr_q[15:8] != 8'hC0)) begin
                                for (int i = 7; i > 0; i--) begin
                                    dbg_ring_q[i] <= dbg_ring_q[i-1];
                                end
                                dbg_ring_q[0] <= sync_addr_q;
                            end
                            if ((sync_addr_q[15:5] == 11'b1100_0000_000) &&
                                !(sync_rw_q && sync_addr_q[3:0] == 4'h0) &&
                                (dbg_c0_count_q != 4'd8)) begin
                                for (int i = 7; i > 0; i--) begin
                                    dbg_c0_ring_q[i] <= dbg_c0_ring_q[i-1];
                                end
                                dbg_c0_ring_q[0] <= {2'b00, sync_rw_q,
                                                     sync_addr_q[4:0],
                                                     ab_read.data};
                                dbg_c0_count_q <= dbg_c0_count_q + 4'd1;
                            end
                            cyc_q <= CYC_PARKED;
                        end
                        else if (cyc_q == CYC_POST) begin
                            /* A write launched before reset recognition is
                             * the one allowed transaction at the boundary. */
                            cnt_posted_q <= cnt_posted_q + 32'd1;
                            cyc_q        <= CYC_PARKED;
                        end
                    end

                    // ---- Per-cycle decision at the drive point ----
                    if (ab_read.drive_en) begin
                        if (!enable || !ab_read.res) begin
                            /* First remove every driven signal, while /DMA
                             * remains asserted. A II+ reset enters
                             * S_RESET_HOLD and never releases ownership. A
                             * //e reset or an explicit disable enters
                             * S_RELEASE and preserves the established guarded
                             * stock-CPU handoff. */
                            wr_addr_rw_en_q <= 1'b0;
                            wr_data_en_q    <= 1'b0;
                            assert_dma_q    <= 1'b1;
                            if (cyc_q == CYC_POST) begin
                                /* data_en should always have completed it;
                                 * count it here only as the missing-strobe
                                 * safety net. FETCH/LOAD are canceled. */
                                cnt_posted_q <= cnt_posted_q + 32'd1;
                            end
                            cyc_q           <= CYC_PARKED;
                            stock_run_cnt_q <= '0;
                            session_q <= (!enable || !host_is_iiplus)
                                       ? S_RELEASE
                                       : S_RESET_HOLD;
                        end
                        else begin
                            /* Safety net: a cycle that somehow missed its
                             * data_en completes here with the last sample. */
                            if (cyc_q == CYC_SYNC) begin
                                resp_rdata_q   <= ab_read.data;
                                resp_valid_q   <= 1'b1;
                                sync_pending_q <= 1'b0;
                                cnt_sync_q     <= cnt_sync_q + 32'd1;
                            end
                            else if (cyc_q == CYC_POST ||
                                     cyc_q == CYC_POST_LOAD ||
                                     cyc_q == CYC_POST_FETCH) begin
                                cnt_posted_q <= cnt_posted_q + 32'd1;
                            end

                            if (sync_pending_q && !sync_flush_wait &&
                                cyc_q != CYC_SYNC) begin
                                /* cyc_q != CYC_SYNC: the safety net above
                                 * just completed this request; launching it
                                 * again would double-execute an I/O access. */
                                wr_addr_q    <= sync_addr_q;
                                wr_rw_q      <= sync_rw_q;
                                wr_data_q    <= sync_wdata_q;
                                wr_data_en_q <= !sync_rw_q;
                                cyc_q        <= CYC_SYNC;
                            end
                            else if (post_fill_q != '0) begin
                                wr_data_en_q <= 1'b0;
                                cyc_q        <= CYC_POST_FETCH;
                            end
                            else begin
                                /* A II/II+ always parks in ordinary main
                                 * RAM. Replaying $FFFF is unsafe when LC RAM
                                 * is selected: the slot card continues to
                                 * assert /INH and drive the data bus before
                                 * the following keyboard/peripheral cycle.
                                 *
                                 * Preserve the proven //e behavior: replay
                                 * ordinary memory, but sanitize I/O to ROM. */
                                if (host_is_iiplus) begin
                                    wr_addr_q <= IIPLUS_PARK_ADDR;
                                end
                                else if (wr_addr_q[15:12] == 4'hC) begin
                                    wr_addr_q <= 16'hFFFF;
                                end
                                wr_rw_q      <= 1'b1;
                                wr_data_en_q <= 1'b0;
                                cyc_q        <= CYC_PARKED;
                            end
                        end
                    end
                end

                S_RESET_HOLD: begin
                    /* A II/II+ has no //e MMU/IOU that needs post-reset
                     * motherboard-CPU cycles. Keep /DMA asserted across both
                     * external and Appletini-generated RESET so the marginal
                     * translated lane never opens an ownership gap. Address,
                     * R/W and data remain released while RESET is low.
                     *
                     * After RESET rises, preserve one complete Apple cycle
                     * with only /DMA driven, then resume at the inert $0200
                     * park address. The virtual core may execute shadow-only
                     * reset cycles meanwhile and stalls at its first physical
                     * request until S_RUN. */
                    wr_addr_rw_en_q <= 1'b0;
                    wr_data_en_q    <= 1'b0;
                    wr_rw_q         <= 1'b1;
                    cyc_q           <= CYC_PARKED;
                    assert_dma_q    <= 1'b1;

                    if (!enable) begin
                        session_q <= S_RELEASE;
                    end
                    else if (!ab_read.res) begin
                        stock_run_cnt_q <= '0;
                    end
                    else if (ab_read.drive_en) begin
                        if (stock_run_cnt_q == '0) begin
                            stock_run_cnt_q <= 8'd1;
                        end
                        else begin
                            wr_addr_q       <= IIPLUS_PARK_ADDR;
                            wr_addr_rw_en_q <= 1'b1;
                            stock_run_cnt_q <= '0;
                            session_q       <= S_RUN;
                        end
                    end
                end

                S_RELEASE: begin
                    /* Symmetric handback: address/R-W and data are already
                     * tri-stated. Hold /DMA for one complete Apple cycle so
                     * the motherboard never sees CPU ownership overlap,
                     * then release /DMA at the same early-PHI1 point used
                     * for takeover. */
                    wr_addr_rw_en_q <= 1'b0;
                    wr_data_en_q    <= 1'b0;
                    cyc_q           <= CYC_PARKED;
                    assert_dma_q    <= 1'b1;
                    if (ab_read.drive_en) begin
                        assert_dma_q    <= 1'b0;
                        stock_run_cnt_q <= '0;
                        session_q       <= enable ? S_ARM : S_OFF;
                    end
                end

                default: begin
                    wr_addr_rw_en_q <= 1'b0;
                    wr_data_en_q    <= 1'b0;
                    assert_dma_q    <= 1'b0;
                    cyc_q           <= CYC_PARKED;
                    session_q       <= S_OFF;
                end
            endcase

            /* The soft-switch trail restarts at every Apple reset so a
             * post-reset capture contains only post-reset accesses (the
             * $C1xx-$CFFF ring deliberately survives: its job is
             * pre-reset context). Placed last so it wins over a
             * same-clock record of a completing pre-reset cycle. */
            if (!ab_read.res) begin
                for (int i = 0; i < 8; i++) begin
                    dbg_c0_ring_q[i] <= '0;
                end
                dbg_c0_count_q <= '0;
            end
            if (dbg_clear) begin
                dbg_wr_mismatch_count_q <= '0;
                dbg_wr_expected_q       <= '0;
                dbg_wr_observed_q       <= '0;
                dbg_wr_addr_q           <= '0;
                dbg_prev_addr_q         <= 16'hFFFF;
                dbg_prev_kind_q         <= 2'd0;
                dbg_prev_rw_q           <= 1'b1;
                dbg_prev_data_drive_q   <= 1'b0;
                dbg_c000_prev_addr_q       <= 16'hFFFF;
                dbg_c000_prev_kind_q       <= 2'd0;
                dbg_c000_prev_rw_q         <= 1'b1;
                dbg_c000_prev_data_drive_q <= 1'b0;
                dbg_c000_data_q          <= '0;
                dbg_c000_total_q         <= '0;
                dbg_c000_bit7_q          <= '0;
            end
        end
    end

endmodule
