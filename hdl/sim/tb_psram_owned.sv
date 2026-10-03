`timescale 1ns / 1ps

// Real command PHY and vTW ownership FSM. The pin data is zero; this bench
// checks scheduling, completion, queued-write data, and the first native
// read after handback. It does not model PSRAM signal integrity.
module tb_psram_owned;
    logic clk = 1'b0;
    always #3.75 clk = ~clk;
    logic resetn = 1'b0;
    logic enable = 1'b0;
    logic apple_res = 1'b1;
    logic aux = 1'b0;
    logic bus_write = 1'b0;
    integer phase = 0;
    integer clocks = 0;
    always @(posedge clk) begin
        phase <= phase == 129 ? 0 : phase + 1;
        clocks <= clocks + 1;
    end

    globals::AppleBus_read ab_read;
    globals::AppleBus_write engine_write, service_write;
    globals::SoftSwitchState sss;
    logic owned;
    always_comb begin
        ab_read = '0;
        ab_read.res = apple_res;
        ab_read.dma = !engine_write.assert_dma;
        ab_read.cycle_valid = 1'b1;
        ab_read.drive_en = phase == 8;
        ab_read.addr_en = phase == 25;
        ab_read.sss_en = phase == 26;
        ab_read.data_en = phase == 124;
        ab_read.rw = !bus_write;
        ab_read.rw_early = !bus_write;
        ab_read.addr = 16'h4003;
        ab_read.addr_early = 16'h4003;
        ab_read.data = 8'hA5;
        sss = '0;
        sss.route_kind = aux ? globals::APPLE_ROUTE_CACHE : globals::APPLE_ROUTE_BUS;
        sss.addr_decode_en = aux;
        sss.addr_decode = 24'h034003;
    end

    vtw_bus_engine #(.POST_DEPTH_LOG2(3)) engine (
        .clk(clk), .rstn(resetn), .enable(enable), .host_is_iiplus(1'b0),
        .ab_read(ab_read), .ab_write(engine_write),
        .data_drive_in(1'b0), .data_drive_value_in(8'd0),
        .dbg_clear(1'b0), .dbg_trace_freeze(1'b0),
        .sync_req_valid(1'b0), .sync_req_addr(16'd0),
        .sync_req_rw(1'b1), .sync_req_wdata(8'd0),
        .post_we(1'b0), .post_addr(16'd0), .post_wdata(8'd0),
        .bus_owned(owned)
    );

    logic dma_valid = 1'b0, dma_rw = 1'b1;
    logic dma_ready, dma_rvalid;
    logic vtw_valid = 1'b0, vtw_rw = 1'b1;
    logic vtw_ready, vtw_rvalid;
    logic psram_valid, psram_ready, psram_rvalid;
    logic [7:0] psram_cmd;
    logic [23:0] psram_addr;
    logic [63:0] psram_wdata, psram_rdata;
    logic [31:0] misses, drops, reads;
    psram_simple dut (
        .clk(clk), .resetn(resetn), .ab_read(ab_read), .sss(sss),
        .aux_provide_en(1'b1), .vtw_bus_owned(owned), .ab_write(service_write),
        .dma_line_addr(21'h010000), .dma_rw(dma_rw),
        .dma_wdata(64'hAABBCCDD_11223344), .dma_valid(dma_valid),
        .dma_ready(dma_ready), .dma_rvalid(dma_rvalid),
        .vtw_valid(vtw_valid), .vtw_rw(vtw_rw), .vtw_addr(24'h090000),
        .vtw_wline(64'h12345678_ABCDEF01), .vtw_ready(vtw_ready),
        .vtw_rvalid(vtw_rvalid),
        .psram_valid(psram_valid), .psram_ready(psram_ready),
        .psram_cmd(psram_cmd), .psram_addr(psram_addr),
        .psram_wdata(psram_wdata), .psram_rvalid(psram_rvalid),
        .psram_rdata(psram_rdata), .dbg_aux_read_count(reads),
        .dbg_deadline_miss_count(misses), .dbg_wq_drop_count(drops)
    );
    psram_driver driver (
        .clk(clk), .resetn(resetn), .valid(psram_valid), .ready(psram_ready),
        .cmd(psram_cmd), .addr(psram_addr), .wdata(psram_wdata),
        .rvalid(psram_rvalid), .rdata(psram_rdata),
        .dcount_wr_en(1'b0), .dcount_wr(5'd0), .dcount_edge(1'b0),
        .psram_a_i(4'd0), .psram_b_i(4'd0)
    );

    integer dma_accepts = 0, dma_completes = 0;
    integer vtw_accepts = 0, vtw_completes = 0;
    integer native_reads = 0, rmw_writes = 0;
    integer last_native_admit = -1;
    integer drain_started = -1, max_drain_clocks = 0;
    logic owned_prev = 1'b0;
    always @(posedge clk) begin
        owned_prev <= resetn && owned;
        if (!resetn) drain_started = -1;
        if (resetn) begin
            if (owned_prev && !owned) drain_started = clocks;
            if (drain_started >= 0 && !dut.owned_drain_q) begin
                if (clocks - drain_started > max_drain_clocks)
                    max_drain_clocks = clocks - drain_started;
                if (clocks - drain_started >= 125)
                    $fatal(1, "FAIL: owned operation exceeded the release guard");
                drain_started = -1;
            end
            if (dma_ready) dma_accepts = dma_accepts + 1;
            if (dma_rvalid) dma_completes = dma_completes + 1;
            if (vtw_ready) vtw_accepts = vtw_accepts + 1;
            if (vtw_rvalid) vtw_completes = vtw_completes + 1;
            if (misses != 0 || drops != 0)
                $fatal(1, "FAIL: deadline misses=%0d dropped writes=%0d", misses, drops);
            if (aux && !bus_write && ab_read.sss_en && !owned &&
                !engine_write.assert_dma) begin
                native_reads = native_reads + 1;
                if (!dut.serve_read_start)
                    $fatal(1, "FAIL: first/native aux read blocked after release");
            end
            if (!owned && (dma_ready || vtw_ready)) begin
                if (last_native_admit == clocks / 130)
                    $fatal(1, "FAIL: multiple native admissions in one bus cycle");
                last_native_admit = clocks / 130;
            end
            if (psram_valid && psram_ready && psram_cmd == 8'h02 &&
                psram_addr == 24'h034000) begin
                if (psram_wdata != 64'h00000000_A5000000)
                    $fatal(1, "FAIL: committed aux byte was lost in RMW: %h", psram_wdata);
                rmw_writes = rmw_writes + 1;
            end
        end
    end

    task tick;
        @(posedge clk);
        #1;
    endtask

    task acquire;
        @(negedge clk);
        enable = 1'b1;
        apple_res = 1'b1;
        aux = 1'b0;
        wait (owned);
        tick();
    endtask

    task idle_driver;
        while (!(dut.state == dut.S_IDLE && !psram_valid && psram_ready)) tick();
    endtask

    // A requester holds its tuple until ready, then waits for exactly one reply.
    task dma_line;
        @(negedge clk);
        dma_valid = 1'b1;
        wait (dma_ready);
        @(negedge clk);
        dma_valid = 1'b0;
        wait (dma_rvalid);
        tick();
    endtask

    integer native_time, owned_time, started, before_reads, before_writes;
    integer accepted_before, completed_before;
    initial begin
        // glbl holds primitive GSR for the first 100 ns.
        repeat (24) tick();
        @(negedge clk);
        resetn = 1'b1;
        idle_driver();
        started = clocks;
        repeat (32) dma_line();
        native_time = clocks - started;
        acquire();
        aux = 1'b1;  // every parked cycle appears to be an aux read
        started = clocks;
        repeat (32) dma_line();
        owned_time = clocks - started;
        if (owned_time * 3 >= native_time)
            $fatal(1, "FAIL: owned admission too slow native=%0d owned=%0d", native_time, owned_time);
        $display("PASS owned throughput: 32 reads native=%0d clocks owned=%0d clocks", native_time, owned_time);

        // Capture a real posted aux write, then offer both clients. Its RMW
        // must finish before the stalled vTW client; vTW must beat PS DMA.
        @(negedge clk iff (phase == 24));
        bus_write = 1'b1;
        @(negedge clk iff (phase == 125));
        bus_write = 1'b0;
        before_writes = rmw_writes;
        vtw_valid = 1'b1;
        dma_valid = 1'b1;
        wait (vtw_ready);
        if (dma_ready || rmw_writes != before_writes + 1)
            $fatal(1, "FAIL: committed write / vTW priority changed");
        @(negedge clk);
        vtw_valid = 1'b0;
        wait (dma_ready);
        @(negedge clk);
        dma_valid = 1'b0;
        wait (dma_rvalid);
        idle_driver();
        $display("PASS queued-write and stalled-vTW priority");

        // Place the accepted read/write at every offset around the drive
        // point where ownership is lost. The real engine retains /DMA for
        // a whole cycle. Repeat for explicit disable and Apple RESET.
        for (integer reset_case = 0; reset_case < 2; reset_case++) begin
            for (integer offset = 0; offset < 40; offset++) begin
                if (!owned) acquire();
                aux = 1'b1;
                dma_rw = offset[0];
                @(negedge clk iff (phase == ((138 - offset) % 130)));
                accepted_before = dma_accepts;
                completed_before = dma_completes;
                dma_valid = 1'b1;
                wait (dma_ready);
                @(negedge clk);
                dma_valid = 1'b0;
                if (reset_case == 0) enable = 1'b0;
                else apple_res = 1'b0;
                wait (!owned);
                // While only the guard owns /DMA, an aux-looking parked
                // address must never cause a deadline miss in the tail.
                wait (!engine_write.assert_dma);
                tick();
                if (dut.owned_drain_q || dma_accepts != accepted_before + 1 ||
                    dma_completes != completed_before + 1)
                    $fatal(1, "FAIL: operation not drained at handback reset=%0d offset=%0d", reset_case, offset);
                @(negedge clk);
                apple_res = 1'b1;
                before_reads = native_reads;
                repeat (130) tick();
                if (native_reads == before_reads)
                    $fatal(1, "FAIL: no native aux read following handback");
                @(negedge clk);
                aux = 1'b0;
                enable = 1'b0;
                apple_res = 1'b1;
                idle_driver();
            end
        end
        $display("PASS 80 disable/Apple-reset handbacks with first native reads");

        // A captured bus write can leave both RMW legs to drain at handback.
        // Retire the byte exactly once before the real engine releases /DMA.
        for (integer reset_case = 0; reset_case < 2; reset_case++) begin
            acquire();
            aux = 1'b1;
            before_writes = rmw_writes;
            @(negedge clk iff (phase == 24));
            bus_write = 1'b1;
            @(negedge clk iff (phase == 125));
            bus_write = 1'b0;
            if (reset_case == 0) enable = 1'b0;
            else apple_res = 1'b0;
            wait (!owned);
            if (dut.state != dut.S_RMW_READ && dut.state != dut.S_RMW_WRITE_ISSUE &&
                dut.state != dut.S_RMW_WRITE_WAIT)
                $fatal(1, "FAIL: RMW handback test missed its in-flight operation");
            wait (!engine_write.assert_dma);
            tick();
            if (rmw_writes != before_writes + 1 || dut.wq_count != 0 || dut.owned_drain_q)
                $fatal(1, "FAIL: committed RMW did not drain at handback");
            @(negedge clk);
            enable = 1'b0;
            apple_res = 1'b1;
            before_reads = native_reads;
            repeat (130) tick();
            if (native_reads == before_reads)
                $fatal(1, "FAIL: no native read after RMW handback");
            aux = 1'b0;
            idle_driver();
        end
        $display("PASS RMW handbacks; maximum owned drain=%0d clocks", max_drain_clocks);

        // Fabric reset clears both FSMs instead of draining. Reset during
        // each command phase, then require clean initialization and a new
        // command with no stale completion from the old request.
        for (integer offset = 0; offset < 36; offset++) begin
            acquire();
            @(negedge clk);
            dma_valid = 1'b1;
            wait (dma_ready);
            @(negedge clk);
            dma_valid = 1'b0;
            repeat (offset) tick();
            @(negedge clk);
            resetn = 1'b0;
            enable = 1'b0;
            repeat (4) tick();
            if (dut.owned_drain_q || psram_valid || dma_rvalid || vtw_rvalid)
                $fatal(1, "FAIL: fabric reset left a request or ownership tail");
            @(negedge clk);
            resetn = 1'b1;
            idle_driver();
            dma_line();
        end
        $display("PASS 36 fabric-reset phases and clean restart");
        $display("PSRAM OWNED PASS");
        $finish;
    end

    initial begin
        repeat (3000000) tick();
        $fatal(1, "FAIL: owned PSRAM bench timeout");
    end
endmodule
