`timescale 1ns / 1ps

module tb_vtw_video_bank_sync;
    logic clk = 1'b0;
    always #3.75 clk = ~clk;
    logic rstn = 1'b0, clear = 1'b0, start = 1'b0;
    logic [1:0] bank_pending = 2'b00;
    logic bus_idle = 1'b0;
    logic sw_ramwrt = 1'b0, sw_page2 = 1'b0, sw_80store = 1'b0;
    logic busy, flush_valid, flush_bank, flush_drained;
    logic sync_req_valid, sync_req_rw, sync_req_ready;
    logic [15:0] sync_req_addr;
    logic [7:0] sync_req_wdata;
    logic sync_resp_valid = 1'b0;
    logic restore_ramwrt, restore_page2;
    vtw_video_bank_sync dut (.*);

    logic allow_engine = 1'b1, allow_flush = 1'b1;
    logic response_pending = 1'b0;
    logic [15:0] response_addr;
    logic physical_ramwrt = 1'b0, physical_page2 = 1'b0;
    logic initial_ramwrt = 1'b0, initial_page2 = 1'b0;
    logic initial_80store = 1'b0;
    logic [1:0] expected_banks = 2'b00, flushed_banks = 2'b00;
    int cycles = 0, response_delay = 0, flush_cycles = 0;
    int request_count = 0, case_count = 0;
    logic [15:0] request_log [0:15];
    logic request_stalled = 1'b0;
    logic [15:0] stalled_addr;

    assign sync_req_ready = allow_engine && !response_pending &&
                            (cycles % 5 >= 2);
    assign flush_drained = allow_flush && flush_valid &&
                           (flush_cycles >= (flush_bank ? 9 : 6));

    // Independent motherboard model. Switches take effect only when their
    // delayed responses are emitted; a drain must wait for those responses.
    always @(posedge clk) begin
        if (!rstn || clear) begin
            cycles <= 0;
            response_pending <= 1'b0;
            sync_resp_valid <= 1'b0;
            physical_ramwrt <= initial_ramwrt;
            physical_page2 <= initial_page2;
            request_count <= 0;
            flushed_banks <= '0;
            flush_cycles <= 0;
            request_stalled <= 1'b0;
        end else begin
            cycles <= cycles + 1;
            sync_resp_valid <= 1'b0;
            if (request_stalled && (!sync_req_valid || sync_req_addr != stalled_addr))
                $fatal(1, "request changed under backpressure");
            request_stalled <= sync_req_valid && !sync_req_ready;
            stalled_addr <= sync_req_addr;
            if (sync_req_valid && sync_req_ready) begin
                if (!busy || response_pending || sync_req_rw || sync_req_wdata != 0)
                    $fatal(1, "invalid maintenance request handshake");
                if (!(sync_req_addr inside {16'hC004, 16'hC005, 16'hC054, 16'hC055}))
                    $fatal(1, "unexpected synthetic I/O %04x", sync_req_addr);
                if ((sync_req_addr == 16'hC054 || sync_req_addr == 16'hC055) &&
                    !initial_80store)
                    $fatal(1, "PAGE2 changed while it selects the displayed page");
                request_log[request_count] <= sync_req_addr;
                request_count <= request_count + 1;
                response_pending <= 1'b1;
                response_addr <= sync_req_addr;
                response_delay <= 3 + request_count % 4;
            end
            if (response_pending) begin
                if (!busy)
                    $fatal(1, "ownership released before synthetic response");
                if (response_delay == 0) begin
                    response_pending <= 1'b0;
                    sync_resp_valid <= 1'b1;
                    if (response_addr[7:4] == 4'h0)
                        physical_ramwrt <= response_addr[0];
                    else
                        physical_page2 <= response_addr[0];
                end else begin
                    response_delay <= response_delay - 1;
                end
            end
            if (flush_valid) begin
                flush_cycles <= flush_cycles + 1;
                if (!busy || response_pending || physical_ramwrt != flush_bank ||
                    (initial_80store && physical_page2 != flush_bank))
                    $fatal(1, "drain started before the correct bank was selected");
                if (!expected_banks[flush_bank])
                    $fatal(1, "clean bank was drained");
                if (flush_drained) begin
                    if (flushed_banks[flush_bank])
                        $fatal(1, "bank drained more than once");
                    flushed_banks[flush_bank] <= 1'b1;
                end
            end else begin
                flush_cycles <= 0;
            end
        end
    end

    task automatic prepare(input logic store80, input logic page2,
                           input logic ramwrt, input logic [1:0] banks);
        @(negedge clk);
        clear = 1'b1;
        start = 1'b0;
        bus_idle = 1'b0;
        sw_80store = store80;
        sw_page2 = page2;
        sw_ramwrt = ramwrt;
        bank_pending = banks;
        initial_80store = store80;
        initial_page2 = page2;
        initial_ramwrt = ramwrt;
        expected_banks = banks;
        allow_engine = 1'b1;
        allow_flush = 1'b1;
        repeat (2) @(posedge clk);
        @(negedge clk);
        clear = 1'b0;
        start = 1'b1;
    endtask

    task automatic launch;
        // start may wait arbitrarily for both the posted and normal bus
        // paths to drain. No snapshot or synthetic access is allowed early.
        repeat (7) begin
            @(posedge clk); #1;
            if (busy || sync_req_valid || flush_valid)
                $fatal(1, "started while the caller's bus was occupied");
        end
        @(negedge clk); bus_idle = 1'b1;
        @(posedge clk); #1;
        if (!busy) $fatal(1, "start was not accepted on an idle bus");
        if (restore_ramwrt != initial_ramwrt || restore_page2 != initial_page2)
            $fatal(1, "restore state snapshot is wrong");
        @(negedge clk);
        start = 1'b0;
        bus_idle = 1'b0;
        // The controller must retain its transaction snapshot.
        bank_pending = ~expected_banks;
        sw_80store = ~initial_80store;
        sw_page2 = ~initial_page2;
        sw_ramwrt = ~initial_ramwrt;
    endtask

    task automatic check_complete;
        logic [15:0] expected_requests [0:15];
        logic ramwrt, page2;
        int expected_count;
        int guard;
        guard = 0;
        while (busy && guard < 400) begin
            @(posedge clk); #1;
            guard++;
        end
        if (busy) $fatal(1, "bank drain did not complete");
        if (flushed_banks != expected_banks || response_pending ||
            physical_ramwrt != initial_ramwrt || physical_page2 != initial_page2)
            $fatal(1, "drain lost a bank or failed to restore the motherboard");
        // Required transitions are MAIN, AUX (when dirty), then the saved
        // mapping. This also rejects redundant switch writes for clean banks.
        ramwrt = initial_ramwrt;
        page2 = initial_page2;
        expected_count = 0;
        for (int bank = 0; bank < 2; bank++) begin
            if (expected_banks[bank]) begin
                if (ramwrt != 1'(bank)) begin
                    expected_requests[expected_count++] = bank ? 16'hC005 : 16'hC004;
                    ramwrt = 1'(bank);
                end
                if (initial_80store && page2 != 1'(bank)) begin
                    expected_requests[expected_count++] = bank ? 16'hC055 : 16'hC054;
                    page2 = 1'(bank);
                end
            end
        end
        if (ramwrt != initial_ramwrt)
            expected_requests[expected_count++] = initial_ramwrt ? 16'hC005 : 16'hC004;
        if (page2 != initial_page2)
            expected_requests[expected_count++] = initial_page2 ? 16'hC055 : 16'hC054;
        if (request_count != expected_count)
            $fatal(1, "wrong request count: got %0d expected %0d", request_count, expected_count);
        for (int i = 0; i < expected_count; i++) begin
            if (request_log[i] != expected_requests[i])
                $fatal(1, "wrong switch sequence at %0d", i);
        end
        repeat (4) begin
            @(posedge clk); #1;
            if (busy || sync_req_valid || flush_valid)
                $fatal(1, "completed drain restarted without start");
        end
        case_count++;
    endtask

    task automatic cancel;
        @(negedge clk); clear = 1'b1; start = 1'b0;
        #1;
        if (sync_req_valid || flush_valid)
            $fatal(1, "reset did not suppress bus requests immediately");
        @(posedge clk); #1;
        if (busy) $fatal(1, "reset did not discard the maintenance transaction");
        @(negedge clk); clear = 1'b0;
        repeat (8) begin
            @(posedge clk); #1;
            if (busy || sync_req_valid || flush_valid)
                $fatal(1, "reset transaction leaked after release");
        end
        case_count++;
    endtask

    initial begin
        repeat (3) @(posedge clk);
        @(negedge clk); rstn = 1'b1;
        for (int mode = 0; mode < 8; mode++) begin
            for (int banks = 0; banks < 4; banks++) begin
                prepare(mode[2], mode[1], mode[0], 2'(banks));
                launch();
                check_complete();
            end
        end

        prepare(1'b1, 1'b1, 1'b1, 2'b01);
        allow_engine = 1'b0;
        launch();
        wait (sync_req_valid);
        repeat (5) @(posedge clk);
        cancel();

        prepare(1'b1, 1'b1, 1'b1, 2'b01);
        launch();
        wait (response_pending);
        cancel();

        prepare(1'b1, 1'b1, 1'b1, 2'b01);
        allow_flush = 1'b0;
        launch();
        wait (flush_valid);
        repeat (5) @(posedge clk);
        cancel();

        prepare(1'b1, 1'b1, 1'b1, 2'b01);
        launch();
        wait (sync_req_valid && sync_req_addr == 16'hC005);
        wait (response_pending);
        cancel();
        $display("VTW VIDEO BANK SYNC PASS: %0d cases", case_count);
        $finish;
    end

    initial begin
        #1000000;
        $fatal(1, "video bank synchronization timeout");
    end
endmodule
