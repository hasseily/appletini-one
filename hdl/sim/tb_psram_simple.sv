`timescale 1ns / 1ps
// Handshake-level testbench for psram_simple + psram_driver
// The PS DMA engine is the background client.
// No PSRAM chip model: data is garbage, but ready/rvalid protocol
// completeness (the thing that can wedge the system) is fully
// exercised. Drives a fake Apple bus cadence (addr_en/sss_en every
// 125 clocks) and pushes DMA + disk2 ops through the module.
module tb_psram_simple;

    logic clk = 0;
    logic resetn = 0;
    always #5 clk = ~clk;   // 100 MHz equivalent; ratio irrelevant

    globals::AppleBus_read  ab_read;
    globals::SoftSwitchState sss;
    globals::AppleBus_write ab_write;

    logic [20:0] dma_line_addr = '0;
    logic        dma_rw = 1'b0;
    logic [63:0] dma_wdata = '0;
    logic        dma_valid = 1'b0;
    logic        dma_ready, dma_rvalid;
    logic [63:0] dma_rdata;

    logic        psram_valid, psram_ready, psram_rvalid;
    logic [7:0]  psram_cmd;
    logic [23:0] psram_addr;
    logic [63:0] psram_wdata, psram_rdata;
    logic        psram_done;

    logic [31:0] dbg_r, dbg_w, dbg_m, dbg_drop;

    // PSRAM pins: loop b to a, float-ish.
    logic [3:0] psram_oe, psram_a_o, psram_b_o;
    logic       psram_ce_n, psram_clk_w;

    psram_simple dut (
        .clk(clk), .resetn(resetn),
        .ab_read(ab_read), .sss(sss),
        .aux_provide_en(1'b1),
        .ab_write(ab_write),
        .dma_line_addr(dma_line_addr), .dma_rw(dma_rw),
        .dma_wdata(dma_wdata), .dma_valid(dma_valid),
        .dma_ready(dma_ready), .dma_rdata(dma_rdata),
        .dma_rvalid(dma_rvalid),
        .psram_valid(psram_valid), .psram_ready(psram_ready),
        .psram_cmd(psram_cmd), .psram_addr(psram_addr),
        .psram_wdata(psram_wdata), .psram_rvalid(psram_rvalid),
        .psram_rdata(psram_rdata),
        .dbg_aux_read_count(dbg_r),
        .dbg_aux_write_count(dbg_w),
        .dbg_deadline_miss_count(dbg_m),
        .dbg_wq_drop_count(dbg_drop)
    );

    psram_driver drv (
        .clk(clk), .resetn(resetn),
        .valid(psram_valid), .ready(psram_ready),
        .cmd(psram_cmd), .addr(psram_addr), .wdata(psram_wdata),
        .rvalid(psram_rvalid), .rdata(psram_rdata), .done(psram_done),
        .dcount_wr_en(1'b0), .dcount_wr(5'd0), .dcount_edge(1'b0),
        .psram_oe(psram_oe),
        .psram_a_i(4'h0), .psram_a_o(psram_a_o),
        .psram_b_i(4'h0), .psram_b_o(psram_b_o),
        .psram_ce_n(psram_ce_n), .psram_clk(psram_clk_w)
    );

    // Fake Apple bus: real cadence -- 130 clocks per cycle, data_en late.
    // No CACHE-routed cycles (route BUS) so no aux serves interfere.
    integer buscnt = 0;
    always @(posedge clk) begin
        ab_read.addr_en <= 1'b0;
        ab_read.sss_en  <= 1'b0;
        ab_read.data_en <= 1'b0;
        if (resetn) begin
            buscnt <= buscnt + 1;
            if (buscnt % 130 == 0)   ab_read.addr_en <= 1'b1;
            if (buscnt % 130 == 1)   ab_read.sss_en  <= 1'b1;
            if (buscnt % 130 == 124) ab_read.data_en <= 1'b1;
        end
    end

    // Present one Apple bus cycle of the given flavor and wait it out.
    // The free-running cadence fires addr_en/sss_en/data_en; we hold
    // the decode/rw/data fields across the whole cycle.
    task aux_cycle(input logic is_write, input [23:0] adr,
                   input [7:0] dat);
        begin
            @(posedge clk iff (buscnt % 130 == 129));
            sss.route_kind     = globals::APPLE_ROUTE_CACHE;
            sss.addr_decode_en = 1'b1;
            sss.addr_decode    = adr;
            ab_read.rw   = !is_write;
            ab_read.data = dat;
            @(posedge clk iff (buscnt % 130 == 129));
            sss.route_kind     = globals::APPLE_ROUTE_BUS;
            sss.addr_decode_en = 1'b0;
            ab_read.rw = 1'b1;
        end
    endtask

    task idle_cycle;
        begin
            @(posedge clk iff (buscnt % 130 == 129));
            sss.route_kind     = globals::APPLE_ROUTE_BUS;
            sss.addr_decode_en = 1'b0;
            ab_read.rw = 1'b1;
            @(posedge clk iff (buscnt % 130 == 129));
        end
    endtask

    integer miss_prev = 0;
    // Trace every deadline miss with FSM context.
    always @(posedge clk) begin
        if (resetn && dut.dbg_deadline_miss_count != miss_prev) begin
            miss_prev <= dut.dbg_deadline_miss_count;
            $display("[%0t] MISS state=%0d addr=%h valid=%b ready=%b buesph=%0d",
                     $time, dut.state, dut.sss.addr_decode[15:0],
                     psram_valid, psram_ready, buscnt % 130);
        end
    end


    initial begin
        ab_read = '0;
        sss = '0;
        sss.route_kind = globals::APPLE_ROUTE_BUS;
        repeat (10) @(posedge clk);
        resetn = 1;

        // Wait out init (ENTER_QPI + TOGGLE_WRAP).
        fork
            begin : init_watch
                wait (dut.state == dut.S_IDLE);
                $display("[%0t] INIT COMPLETE", $time);
            end
            begin
                repeat (20000) @(posedge clk);
                if (dut.state != dut.S_IDLE) begin
                    $display("FAIL: stuck in init, state=%0d", dut.state);
                    $finish;
                end
            end
        join_any
        disable fork;

        // --- DMA line WRITE (staging): MC-port rw=0 = write ---
        dma_rw = 1'b0; dma_wdata = 64'hDEAD_BEEF_CAFE_F00D;
        dma_line_addr = 21'h1C0000;  // disk2 staging region
        dma_valid = 1'b1;
        fork
            begin
                wait (dma_ready);
                @(posedge clk);
                dma_valid = 1'b0;
                wait (dma_rvalid);
                $display("[%0t] DMA WRITE COMPLETE", $time);
            end
            begin
                repeat (5000) @(posedge clk);
                $display("FAIL: DMA write wedged. dut.state=%0d psram_valid=%b ready=%b",
                         dut.state, psram_valid, psram_ready);
                $finish;
            end
        join_any
        disable fork;

        // --- DMA line READ: MC-port rw=1 = read ---
        @(posedge clk);
        dma_rw = 1'b1; dma_valid = 1'b1;
        fork
            begin
                wait (dma_ready);
                @(posedge clk);
                dma_valid = 1'b0;
                wait (dma_rvalid);
                $display("[%0t] DMA READ COMPLETE", $time);
            end
            begin
                repeat (5000) @(posedge clk);
                $display("FAIL: DMA read wedged. dut.state=%0d", dut.state);
                $finish;
            end
        join_any
        disable fork;


        // --- burst: 16 back-to-back DMA writes (staging pattern) ---
        begin : burst
            integer i;
            for (i = 0; i < 16; i = i + 1) begin
                @(posedge clk);
                dma_rw = 1'b0; dma_line_addr = 21'h1C0000 + i; dma_valid = 1'b1;
                fork
                    begin
                        wait (dma_ready);
                        @(posedge clk);
                        dma_valid = 1'b0;
                        wait (dma_rvalid);
                    end
                    begin
                        repeat (5000) @(posedge clk);
                        $display("FAIL: burst write %0d wedged. dut.state=%0d",
                                 i, dut.state);
                        $finish;
                    end
                join_any
                disable fork;
            end
            $display("[%0t] BURST 16 WRITES COMPLETE", $time);
        end

        // --- MGTK worst-case pattern: two back-to-back pushes (JSR),
        // then 36 iterations of 14 serve-reads + 1 aux write. The chained
        // drain must retire every entry with zero drops.
        fork
            begin : dma_bg
                // Background PS-DMA line writes every ~40 bus cycles add
                // realistic command-driver contention.
                integer d;
                d = 0;
                forever begin
                    d = d + 1;
                    repeat (40*130) @(posedge clk);
                    dma_rw = 1'b0;
                    dma_wdata = {8{d[7:0]}};
                    dma_line_addr = 21'h1C0100 + d[20:0];
                    dma_valid = 1'b1;
                    wait (dma_ready);
                    @(posedge clk);
                    dma_valid = 1'b0;
                    wait (dma_rvalid);
                end
            end
        begin : mgtk
            integer rnd, it, s;
            for (rnd = 0; rnd < 20; rnd = rnd + 1) begin
                // interrupt/JSR burst: 3 consecutive pushes
                aux_cycle(1'b1, 24'h0101FB, 8'h12);
                aux_cycle(1'b1, 24'h0101FA, 8'h34);
                aux_cycle(1'b1, 24'h0101F9, 8'h40);
                aux_cycle(1'b1, 24'h0101F8, 8'h30);
                for (it = 0; it < 36; it = it + 1) begin
                    for (s = 0; s < 14; s = s + 1) begin
                        aux_cycle(1'b0, 24'h0140DD + s[23:0], 8'h00);
                    end
                    aux_cycle(1'b1, 24'h0100D0 + it[23:0], 8'hA5);
                end
            end
            repeat (6) idle_cycle;
            if (dut.wq_count != 0) begin
                $display("FAIL: MGTK pattern left wq_count=%0d", dut.wq_count);
                $finish;
            end
            if (dbg_drop != 0) begin
                $display("FAIL: MGTK pattern dropped %0d writes", dbg_drop);
                $finish;
            end
            if (dut.state != dut.S_IDLE) begin
                $display("FAIL: MGTK pattern wedged, state=%0d", dut.state);
                $finish;
            end
            $display("[%0t] MGTK PATTERN: drained clean, 0 drops (reads=%0d writes=%0d misses=%0d)",
                     $time, dbg_r, dbg_w, dbg_m);
            if (dbg_m != 0) begin
                $display("FAIL: %0d deadline misses under disk2 load", dbg_m);
                $finish;
            end
        end
        join_any
        disable fork;

        $display("ALL HANDSHAKES PASS");
        $finish;
    end

endmodule
