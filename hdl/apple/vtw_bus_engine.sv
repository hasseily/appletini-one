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
//   3. Parked cycle -- re-drive the last real address with R/W=1. The bus
//      is never floated. (Parked cycles replay the last address as a read;
//      the serving stack was hardware-validated against exactly this
//      behavior from the physical TW -- pop-on-read protocols were already
//      redesigned away.)
//
// /DMA discipline (Apple IIe Tech Note #2): asserted at a drive_en point
// (early PHI1) when the session starts, then one full grace cycle before we
// begin driving the bus, giving the motherboard 6502's buffers time to
// release. Held until the session ends; handback is CTRL-RESET with the vTW
// disabled, exactly like the real card's $C074 = 3.
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
        // wide_main: the PS saw SHR interlace armed (second field in MAIN
        // $2000-$9FFF) and widened the main window via CARD_CTRL so the
        // capture sees that field too. Off by default: main $6000+
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

    input  globals::AppleBus_read   ab_read,
    output globals::AppleBus_write  ab_write,

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
    output logic [8*16-1:0]         dbg_c0_ring
);

    import vtw_pkg::*;

    localparam int POST_DEPTH = 1 << POST_DEPTH_LOG2;

    // ------------------------------------------------------------------
    // Session FSM
    // ------------------------------------------------------------------
    typedef enum logic [1:0] {
        S_OFF,     // outputs released
        S_ARM,     // waiting for post-reset stock run + PHI1 /DMA point
        S_GRACE,   // /DMA held, one full cycle for the 6502 to let go
        S_RUN      // parked driver + cycle engine active
    } session_t;
    session_t session_q;

    /* Stock cycles the motherboard 6502 must run after RES# rises before
     * we assert /DMA (see the S_ARM comment for why the MMU needs them). */
    localparam int STOCK_RUN_CYCLES = 80;
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
    wire post_pop   = (session_q == S_RUN) && (cyc_q == CYC_POST_FETCH);
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

    assign dbg_last_sync_addr = dbg_sync_addr_q;
    assign dbg_last_sync_data = resp_rdata_q;
    assign dbg_last_sync_rw   = dbg_sync_rw_q;
    generate
        for (genvar gi = 0; gi < 8; gi++) begin : dbg_ring_pack
            assign dbg_cxxx_ring[gi*16 +: 16] = dbg_ring_q[gi];
            assign dbg_c0_ring[gi*16 +: 16]   = dbg_c0_ring_q[gi];
        end
    endgenerate

    /* A video-window sync request must not overtake queued posted writes
     * (read-after-write consistency), and neither may a bank-steering
     * soft-switch access (queued aux/main writes must retire under the
     * switch regime they were issued in). */
    wire sync_flush_wait = (vtw_is_video_window(sync_addr_q, 1'b1, 1'b1) ||
                            vtw_is_bank_steer(sync_addr_q, sync_rw_q)) &&
                           (post_fill_q != '0);

    assign sync_req_ready  = (session_q == S_RUN) && !sync_pending_q &&
                             ab_read.res;
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
        ab_write.assert_dma    = assert_dma_q;
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
                     * during PHI1 only) -- but only after the motherboard
                     * has run STOCK_RUN_CYCLES real cycles with RES# high.
                     *
                     * The //e MMU's soft-switch clear is not completed by
                     * the RES#-low window alone: it requires the 6502's
                     * own post-reset bus activity (reset sequence +
                     * $FFFC/$FFFD vector fetch). Every RES# release must
                     * therefore give the motherboard 6502 a stock run
                     * before the bus is taken. 80 cycles covers the vector
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
                    if (!ab_read.res) begin
                        assert_dma_q <= 1'b0;
                        session_q    <= S_ARM;
                    end
                    else if (ab_read.drive_en) begin
                        if (!enable) begin
                            assert_dma_q <= 1'b0;
                            session_q    <= S_OFF;
                        end
                        else begin
                            wr_addr_rw_en_q <= 1'b1;
                            wr_rw_q         <= 1'b1;
                            session_q       <= S_RUN;
                        end
                    end
                end

                S_RUN: begin
                    /* Apple RES# transaction boundary: drop any pending
                     * (not-yet-driven) request and never respond to it --
                     * the requester clears its in-flight tracking on the
                     * same reset, so a stale response can never be paired
                     * with a post-reset request. A cycle already on the
                     * bus completes harmlessly below (its response fires
                     * into a requester that is no longer listening). */
                    if (!ab_read.res) begin
                        sync_pending_q <= 1'b0;
                    end

                    // ---- Posted-fetch pipeline (clocks after a drive_en);
                    //      the queue pop itself lives in the FIFO block. ----
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

                    // ---- Cycle completion at the data snap ----
                    if (ab_read.data_en) begin
                        if (cyc_q == CYC_SYNC) begin
                            resp_rdata_q   <= ab_read.data;
                            resp_valid_q   <= 1'b1;
                            sync_pending_q <= 1'b0;
                            cnt_sync_q     <= cnt_sync_q + 32'd1;
                            dbg_sync_addr_q <= sync_addr_q;
                            dbg_sync_rw_q   <= sync_rw_q;
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
                            cyc_q          <= CYC_PARKED;
                        end
                        else if (cyc_q == CYC_POST) begin
                            cnt_posted_q <= cnt_posted_q + 32'd1;
                            cyc_q        <= CYC_PARKED;
                        end
                    end

                    // ---- Per-cycle decision at the drive point ----
                    if (ab_read.drive_en) begin
                        /* Safety net: a cycle that somehow missed its
                         * data_en (never expected on a //e) completes here
                         * with the last-sampled bus data. */
                        if (cyc_q == CYC_SYNC) begin
                            resp_rdata_q   <= ab_read.data;
                            resp_valid_q   <= 1'b1;
                            sync_pending_q <= 1'b0;
                            cnt_sync_q     <= cnt_sync_q + 32'd1;
                        end
                        else if (cyc_q == CYC_POST || cyc_q == CYC_POST_LOAD ||
                                 cyc_q == CYC_POST_FETCH) begin
                            cnt_posted_q <= cnt_posted_q + 32'd1;
                        end

                        if (!enable) begin
                            /* Session end: release everything at a PHI1
                             * point. CTRL-RESET returns the motherboard
                             * 6502 cold, per the contract. */
                            wr_addr_rw_en_q <= 1'b0;
                            wr_data_en_q    <= 1'b0;
                            assert_dma_q    <= 1'b0;
                            cyc_q           <= CYC_PARKED;
                            session_q       <= S_OFF;
                            // Abort a pending request so no CE hangs.
                            if (sync_pending_q) begin
                                resp_rdata_q <= 8'hFF;
                                resp_valid_q <= 1'b1;
                            end
                            sync_pending_q  <= 1'b0;
                        end
                        else if (!ab_read.res) begin
                            /* Apple RES# low: release the bus COMPLETELY --
                             * /DMA deasserted, nothing driven -- and re-arm
                             * once the reset ends (S_ARM waits for RES#
                             * high plus the stock-run window, then DMA +
                             * grace + drive as usual).
                             *
                             * The //e MMU does not process a reset while a
                             * DMA master holds the bus: its soft switches
                             * (80STORE, INTCXROM) survive, diverging from
                             * the vTW's trackers, and the firmware's
                             * state-preserving trampolines then latch the
                             * divergence permanently. The motherboard must
                             * experience reset in fully stock conditions:
                             * its own 6502 free-running in-reset read
                             * cycles, no DMA master. Post-reset the 6502
                             * free-runs behind the disabled buffers exactly
                             * as in steady-state TW operation. */
                            wr_addr_rw_en_q <= 1'b0;
                            wr_data_en_q    <= 1'b0;
                            assert_dma_q    <= 1'b0;
                            cyc_q           <= CYC_PARKED;
                            session_q       <= S_ARM;
                        end
                        else if (sync_pending_q && !sync_flush_wait &&
                                 cyc_q != CYC_SYNC) begin
                            /* cyc_q != CYC_SYNC: the safety net above just
                             * completed this request; launching it again
                             * would double-execute an I/O access. */
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
                            /* Parked: last real address, R/W=1, no data.
                             * I/O-page addresses are substituted with
                             * $FFFF: our parked cycles are CLEAN, fully
                             * decodable replays (unlike the real TW's
                             * decayed floating bus), so a parked I/O read
                             * would re-execute read side effects every
                             * idle cycle -- clearing keyboard strobes,
                             * feeding NSC pattern matchers, bumping VDP
                             * auto-increment pointers. Memory addresses
                             * park authentically. */
                            if (wr_addr_q[15:12] == 4'hC) begin
                                wr_addr_q <= 16'hFFFF;
                            end
                            wr_rw_q      <= 1'b1;
                            wr_data_en_q <= 1'b0;
                            cyc_q        <= CYC_PARKED;
                        end
                    end
                end

                default: session_q <= S_OFF;
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
        end
    end

endmodule
