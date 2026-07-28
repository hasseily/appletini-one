`timescale 1ns / 1ps
// Unit bench for vtw_bus_engine: drives the ab_read strobe vocabulary
// directly (no pins) and checks the engine's cycle-type decisions, the
// posted/sync ordering rules, /DMA sequencing, queue backpressure, and
// session release. Pin-accurate behavior is covered by tb_vtw_system.

module tb_vtw_engine_unit;

    timeunit 1ns;
    timeprecision 1ps;

    import vtw_pkg::*;

    logic clk = 0;
    always #3.75 clk = ~clk;   // 133.333 MHz

    logic rstn = 0;
    logic enable = 0;

    globals::AppleBus_read  ab_read;
    globals::AppleBus_write ab_write;

    logic        sync_req_valid = 0;
    logic        sync_req_ready;
    logic [15:0] sync_req_addr = '0;
    logic        sync_req_rw = 1;
    logic [7:0]  sync_req_wdata = '0;
    logic        sync_resp_valid;
    logic [7:0]  sync_resp_rdata;

    logic        post_we = 0;
    logic [15:0] post_addr = '0;
    logic [7:0]  post_wdata = '0;
    logic        post_full;
    logic [9:0]  post_fill;

    logic        bus_owned;
    logic [31:0] cnt_sync_cycles;
    logic [31:0] cnt_posted_writes;
    logic [9:0]  post_high_water;
    logic [31:0] cnt_post_drops;

    vtw_bus_engine dut (
        .clk(clk),
        .rstn(rstn),
        .enable(enable),
        .ab_read(ab_read),
        .ab_write(ab_write),
        .sync_req_valid(sync_req_valid),
        .sync_req_ready(sync_req_ready),
        .sync_req_addr(sync_req_addr),
        .sync_req_rw(sync_req_rw),
        .sync_req_wdata(sync_req_wdata),
        .sync_resp_valid(sync_resp_valid),
        .sync_resp_rdata(sync_resp_rdata),
        .post_we(post_we),
        .post_addr(post_addr),
        .post_wdata(post_wdata),
        .post_full(post_full),
        .post_fill(post_fill),
        .bus_owned(bus_owned),
        .cnt_sync_cycles(cnt_sync_cycles),
        .cnt_posted_writes(cnt_posted_writes),
        .post_high_water(post_high_water),
        .cnt_post_drops(cnt_post_drops)
    );

    // ------------------------------------------------------------------
    // Strobe generator: a 130-fclk Apple cycle. Clock 0 = PHI0 fall.
    //   drive_en at clk 8 (the wrapper's fall+TAP_DRIVE_ADDR strobe)
    //   data_en  at clk 124 (rise 65 + TAP_DATA_SNAP 59)
    // ------------------------------------------------------------------
    localparam int CYC_CLKS  = 130;
    localparam int DRIVE_CLK = 8;
    localparam int DATA_CLK  = 124;

    int cyc_clk = 0;
    logic [7:0] bus_read_value = 8'hA7;   // what the "bus" returns at data_en
    logic force_res = 0;                  // drive ab_read.res low (Apple reset)
    /* Freezing the strobes between Apple cycles lets a scenario stage
     * queue pushes and a sync request atomically with respect to the
     * engine's per-cycle decision point. */
    logic strobes_on = 1;

    always @(posedge clk) begin
        ab_read <= '0;
        ab_read.res      <= !force_res;
        ab_read.data     <= bus_read_value;
        if (strobes_on) begin
            ab_read.drive_en <= (cyc_clk == DRIVE_CLK);
            ab_read.data_en  <= (cyc_clk == DATA_CLK);
            cyc_clk <= (cyc_clk == CYC_CLKS-1) ? 0 : cyc_clk + 1;
        end
    end

    // ------------------------------------------------------------------
    // Per-cycle bus record + event classifier. A parked cycle repeats the
    // previous cycle's address as a read; anything else the engine drives
    // is an "event" (a real posted write or sync cycle).
    // ------------------------------------------------------------------
    typedef struct {
        logic        owned;
        logic [15:0] addr;
        logic        rw;
        logic        dma;
        logic        data_drive;
        logic [7:0]  data;
    } cycrec_t;
    cycrec_t recs [$];
    cycrec_t events [$];

    cycrec_t cur, prev;
    initial prev = '{default: '0};

    always @(posedge clk) begin
        if (cyc_clk == 90) begin
            cur.owned = ab_write.wr_addr_rw_en;
            cur.addr  = ab_write.wr_addr;
            cur.rw    = ab_write.wr_rw;
            cur.dma   = ab_write.assert_dma;
        end
        if (cyc_clk == 120) begin
            cur.data_drive = ab_write.wr_data_en;
            cur.data       = ab_write.wr_data;
            recs.push_back(cur);
            if (cur.owned) begin
                if (!cur.rw) begin
                    // Write cycles are always events (flood uses distinct
                    // addresses, so a repeated tuple never occurs here).
                    events.push_back(cur);
                end
                else if (cur.addr == 16'hFFFF) begin
                    // Sanitized park (I/O-page addresses are not replayed;
                    // the engine parks on $FFFF instead). Never an event.
                end
                else if (!(prev.owned && prev.addr == cur.addr)) begin
                    // Read to a new address = sync cycle (parked replays
                    // repeat the previous address).
                    events.push_back(cur);
                end
            end
            prev = cur;
        end
    end

    int fails = 0;
    task automatic check(input bit cond, input string msg);
        if (!cond) begin
            fails++;
            $display("VTW ENGINE FAIL: %s (t=%0t)", msg, $time);
        end
    endtask

    task automatic check_event(input int idx, input logic rw,
                               input logic [15:0] a, input logic [7:0] d,
                               input string msg);
        if (idx >= events.size()) begin
            fails++;
            $display("VTW ENGINE FAIL: %s -- missing event %0d (t=%0t)",
                     msg, idx, $time);
        end
        else begin
            check(events[idx].rw == rw && events[idx].addr == a &&
                  (rw || (events[idx].data == d && events[idx].data_drive)),
                  msg);
        end
    endtask

    task automatic wait_cycles(input int n);
        repeat (n) @(posedge clk iff (cyc_clk == CYC_CLKS-1));
    endtask

    task automatic push_posted(input logic [15:0] a, input logic [7:0] d);
        @(posedge clk);
        post_we    <= 1'b1;
        post_addr  <= a;
        post_wdata <= d;
        @(posedge clk);
        post_we    <= 1'b0;
    endtask

    /* Ready/valid discipline: drop valid at the acceptance handshake (the
     * engine's ready reasserts on the response clock; a still-high valid
     * would double-issue -- vtw_core_top gates this with req_inflight_q). */
    task automatic sync_start(input logic [15:0] a, input logic rw,
                              input logic [7:0] wd);
        @(posedge clk);
        sync_req_valid <= 1'b1;
        sync_req_addr  <= a;
        sync_req_rw    <= rw;
        sync_req_wdata <= wd;
        @(posedge clk iff (sync_req_valid && sync_req_ready));
        sync_req_valid <= 1'b0;
    endtask

    task automatic sync_finish(output logic [7:0] d);
        @(posedge clk iff sync_resp_valid);
        d = sync_resp_rdata;
    endtask

    int ebase;

    initial begin
        logic [7:0] rd;
        logic [15:0] rewind_pos;

        // ---- 0. Scanner address equations used by accelerated floating
        //      $C05x reads. These are marker locations used by Lancaster-
        //      style vapor lock, plus mode/page and NTSC/PAL discriminators.
        check(vtw_scanner_address(
                  1'b0, 9'd253, 7'd25,
                  1'b0, 1'b0, 1'b0, 1'b1, 1'b0) == 16'h37F8,
              "scanner HGR line 253/cycle 25 -> $37F8");
        check(vtw_scanner_address(
                  1'b0, 9'd253, 7'd24,
                  1'b0, 1'b0, 1'b0, 1'b1, 1'b0) == 16'h37F7 &&
              vtw_scanner_address(
                  1'b0, 9'd253, 7'd26,
                  1'b0, 1'b0, 1'b0, 1'b1, 1'b0) == 16'h37F9,
              "scanner distinguishes adjacent horizontal cycles");
        check(vtw_scanner_address(
                  1'b0, 9'd253, 7'd25,
                  1'b1, 1'b0, 1'b0, 1'b1, 1'b0) == 16'h07F8,
              "scanner text mode selects text page 1");
        check(vtw_scanner_address(
                  1'b0, 9'd253, 7'd25,
                  1'b0, 1'b0, 1'b1, 1'b1, 1'b0) == 16'h57F8 &&
              vtw_scanner_address(
                  1'b0, 9'd253, 7'd25,
                  1'b0, 1'b0, 1'b1, 1'b1, 1'b1) == 16'h37F8,
              "scanner PAGE2 is suppressed by 80STORE");
        check(vtw_scanner_address(
                  1'b0, 9'd256, 7'd25,
                  1'b0, 1'b0, 1'b0, 1'b1, 1'b0) == 16'h2BF8 &&
              vtw_scanner_address(
                  1'b1, 9'd256, 7'd25,
                  1'b0, 1'b0, 1'b0, 1'b1, 1'b0) == 16'h20F8,
              "scanner vertical preset differs for NTSC and PAL");
        check(vtw_video_position_rewind(
                  1'b0, 9'd253, 7'd27, 2'd2) == {9'd253, 7'd25},
              "floating-bus phase rewinds two cycles on one scanline");
        check(vtw_video_position_rewind(
                  1'b0, 9'd192, 7'd1, 2'd2) == {9'd191, 7'd64},
              "two-cycle rewind crosses to the previous scanline");
        check(vtw_video_position_rewind(
                  1'b0, 9'd0, 7'd0, 2'd2) == {9'd261, 7'd63} &&
              vtw_video_position_rewind(
                  1'b1, 9'd0, 7'd0, 2'd2) == {9'd311, 7'd63},
              "two-cycle rewind wraps NTSC and PAL frames");
        check(vtw_video_position_rewind(
                  1'b0, 9'd192, 7'd0, 2'd1) == {9'd191, 7'd64} &&
              vtw_video_position_rewind(
                  1'b0, 9'd192, 7'd1, 2'd1) == {9'd192, 7'd0},
              "RDVBLBAR phase becomes visible one cycle after line 192");
        rewind_pos = vtw_video_position_rewind(
                         1'b0, 9'd253, 7'd27, 2'd2);
        check(vtw_scanner_address(
                  1'b0, rewind_pos[15:7], rewind_pos[6:0],
                  1'b0, 1'b0, 1'b0, 1'b1, 1'b0) == 16'h37F8,
              "BEAMPOS-style raw cycle 27 returns scanner cycle 25");

        repeat (10) @(posedge clk);
        rstn = 1;
        wait_cycles(2);

        // ---- 1. Session start: post-reset stock-run window, then /DMA
        //      one full cycle before ownership ----
        check(!bus_owned, "idle before enable");
        enable = 1;
        /* The engine leaves the motherboard 80 stock cycles before the
         * takeover (the //e MMU finishes its reset processing via the
         * 6502's own post-reset bus activity). */
        wait_cycles(40);
        check(!bus_owned, "no takeover inside the stock-run window");
        wait_cycles(45);
        begin
            int n = recs.size();
            bit found = 0;
            check(recs[n-1].owned && recs[n-1].dma, "parked+dma after enable");
            check(recs[n-1].rw == 1'b1, "parked cycle is a read");
            for (int i = 0; i < n; i++) begin
                if (recs[i].dma) begin
                    found = 1;
                    check(!recs[i].owned,
                          "grace cycle: dma asserted before driving");
                    break;
                end
            end
            check(found, "dma was asserted");
        end

        // ---- 2. Posted writes drain in order, then re-park ----
        wait_cycles(1);
        strobes_on = 0;
        ebase = events.size();
        push_posted(16'h0400, 8'h5A);
        push_posted(16'h0427, 8'h5B);
        push_posted(16'h2000, 8'h5C);
        strobes_on = 1;
        wait_cycles(6);
        check_event(ebase+0, 1'b0, 16'h0400, 8'h5A, "posted write 1 on bus");
        check_event(ebase+1, 1'b0, 16'h0427, 8'h5B, "posted write 2 on bus");
        check_event(ebase+2, 1'b0, 16'h2000, 8'h5C, "posted write 3 on bus");
        check(events.size() == ebase+3, "no extra cycles after drain");
        begin
            int n = recs.size();
            check(recs[n-1].rw && recs[n-1].addr == 16'h2000 &&
                  !recs[n-1].data_drive, "parked on last posted address");
        end
        check(cnt_posted_writes == 32'd3, "posted counter");

        // ---- 3. Sync read: one bus cycle, response data, sanitized park ----
        ebase = events.size();
        bus_read_value = 8'h3C;
        sync_start(16'hC000, 1'b1, 8'h00);
        sync_finish(rd);
        check(rd == 8'h3C, "sync read returns bus data");
        wait_cycles(3);
        check_event(ebase+0, 1'b1, 16'hC000, 8'h00, "sync read cycle on bus");
        check(events.size() == ebase+1, "parked replay emits no event");
        check(cnt_sync_cycles == 32'd1, "sync counter");
        begin
            int n = recs.size();
            check(recs[n-1].rw && recs[n-1].addr == 16'hFFFF,
                  "I/O park sanitized to $FFFF (no phantom replays)");
        end

        // ---- 4. Video-window sync request drains the queue first ----
        wait_cycles(1);
        strobes_on = 0;
        ebase = events.size();
        push_posted(16'h2100, 8'h61);
        push_posted(16'h2101, 8'h62);
        sync_start(16'h0400, 1'b1, 8'h00);
        strobes_on = 1;
        sync_finish(rd);
        wait_cycles(3);
        check_event(ebase+0, 1'b0, 16'h2100, 8'h61, "flush: posted 1 first");
        check_event(ebase+1, 1'b0, 16'h2101, 8'h62, "flush: posted 2 second");
        check_event(ebase+2, 1'b1, 16'h0400, 8'h00,
                    "flush: video-window sync after drain");

        // ---- 5. Non-window sync overtakes queued posted writes ----
        wait_cycles(1);
        strobes_on = 0;
        ebase = events.size();
        push_posted(16'h2200, 8'h71);
        push_posted(16'h2201, 8'h72);
        sync_start(16'hC010, 1'b1, 8'h00);
        strobes_on = 1;
        sync_finish(rd);
        wait_cycles(4);
        check_event(ebase+0, 1'b1, 16'hC010, 8'h00,
                    "overtake: sync I/O before posted drain");
        check_event(ebase+1, 1'b0, 16'h2200, 8'h71, "overtake: posted 1 after");
        check_event(ebase+2, 1'b0, 16'h2201, 8'h72, "overtake: posted 2 after");

        // ---- 5b. Apple RES# transaction boundary: a request interrupted
        //      by a reset must never pair its response with the next
        //      post-reset request (the poisoned-BIT-$C015 class). ----
        begin
            bus_read_value = 8'hAA;
            @(posedge clk);
            sync_req_valid <= 1'b1;
            sync_req_addr  <= 16'hC000;
            sync_req_rw    <= 1'b1;
            @(posedge clk iff (sync_req_valid && sync_req_ready));
            sync_req_valid <= 1'b0;
            // Reset lands while the request is latched/in flight.
            repeat (10) @(posedge clk);
            force_res = 1'b1;
            wait_cycles(3);
            /* Stock reset window: the engine must release the bus entirely
             * (no drive, no /DMA) -- the //e MMU only processes a reset
             * when no DMA master holds the bus. */
            begin
                int n = recs.size();
                check(!recs[n-1].owned && !recs[n-1].dma,
                      "bus fully released during the reset");
            end
            force_res = 1'b0;
            /* 80 stock cycles for the motherboard, then re-take. */
            wait_cycles(40);
            begin
                int n = recs.size();
                check(!recs[n-1].owned && !recs[n-1].dma,
                      "stock-run window after reset release (still hands-off)");
            end
            wait_cycles(45);
            /* Re-take after release follows the same discipline as the
             * first takeover: /DMA asserted at least one full cycle before
             * driving resumes. */
            begin
                int n = recs.size();
                bit dma_before_own = 0;
                // Most recent ownership-gain edge = the post-reset re-take.
                for (int i = n-1; i > 0; i--) begin
                    if (recs[i].owned && !recs[i-1].owned) begin
                        dma_before_own = recs[i-1].dma;
                        break;
                    end
                end
                check(recs[n-1].owned && recs[n-1].dma,
                      "bus re-taken after reset release");
                check(dma_before_own,
                      "re-take keeps dma-before-drive ordering");
            end
            // The next request must see its own data, not $AA.
            bus_read_value = 8'hBB;
            sync_start(16'hC011, 1'b1, 8'h00);
            sync_finish(rd);
            check(rd == 8'hBB,
                  $sformatf("post-reset request gets its own response (got %h)", rd));
        end

        // ---- 6. Queue backpressure: fast fill, no drops, full drain ----
        begin
            int target = 600;
            int before_posted = int'(cnt_posted_writes);
            bit saw_full = 0;
            // Concurrent watcher: the push loop below only ever samples in
            // the guaranteed-not-full slot, so observe post_full async.
            fork
                begin
                    wait (post_full === 1'b1);
                    saw_full = 1;
                end
            join_none
            for (int i = 0; i < target; i++) begin
                // Respect post_full exactly like the core wrapper does.
                @(posedge clk iff !post_full);
                post_we    <= 1'b1;
                post_addr  <= 16'h2000 + 16'(i);
                post_wdata <= 8'(i);
                @(posedge clk);
                post_we    <= 1'b0;
            end
            check(saw_full, "flood reached backpressure");
            wait_cycles(620);
            check(post_fill == '0, "queue drained");
            check(cnt_post_drops == 32'd0, "no posted drops");
            check(int'(cnt_posted_writes) - before_posted == target,
                  "every flooded write retired");
            check(post_high_water >= 10'd500, "high-water mark recorded");
        end

        // ---- 7. Session release at a PHI1 point ----
        enable = 0;
        wait_cycles(3);
        begin
            int n = recs.size();
            check(!recs[n-1].owned && !recs[n-1].dma, "released after disable");
        end
        check(!bus_owned, "bus_owned deasserts");

        if (fails == 0) $display("VTW ENGINE UNIT PASS");
        else            $display("VTW ENGINE UNIT FAILED: %0d checks", fails);
        $finish;
    end

    initial begin
        #80ms;
        $display("VTW ENGINE FAIL: timeout");
        $finish;
    end

endmodule
