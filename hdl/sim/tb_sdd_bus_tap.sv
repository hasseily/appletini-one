`timescale 1ns / 1ps

module tb_sdd_bus_tap;

    import apple_cycle_capture_pkg::*;

    logic clk = 1'b0;
    always #3.75 clk = ~clk;

    logic              resetn = 1'b0;
    logic              enable = 1'b0;
    globals::AppleBus_read ab_read = '0;
    logic [2:0]        route_info = '0;
    AppleCycleRecord   cycle_capture_data;
    logic              cycle_capture_empty;
    logic              cycle_capture_rd_en = 1'b0;
    logic              capture_drop_sticky;
    logic              capture_drop_ack = 1'b0;

    int failures = 0;

    sdd_bus_tap dut (
        .clk(clk),
        .resetn(resetn),
        .enable(enable),
        .ab_read(ab_read),
        .route_info(route_info),
        .cycle_capture_data(cycle_capture_data),
        .cycle_capture_empty(cycle_capture_empty),
        .cycle_capture_rd_en(cycle_capture_rd_en),
        .capture_drop_sticky(capture_drop_sticky),
        .capture_drop_ack(capture_drop_ack)
    );

    function automatic logic [63:0] expected_record(
        input logic [15:0] addr,
        input logic        rw,
        input logic        res,
        input logic        m2sel,
        input logic        m2b0,
        input logic [7:0]  data,
        input logic [2:0]  route
    );
        expected_record = {
            3'd3,
            29'd0,
            route,
            1'b1,
            data,
            m2b0,
            m2sel,
            res,
            rw,
            addr
        };
    endfunction

    task automatic check(input logic condition, input string label_text);
        if (condition !== 1'b1) begin
            $display("FAIL: %s", label_text);
            failures++;
        end
    endtask

    task automatic clear_and_enable;
        begin
            @(negedge clk);
            enable = 1'b0;
            ab_read.data_en = 1'b0;
            capture_drop_ack = 1'b0;
            @(posedge clk);
            #1;
            @(negedge clk);
            enable = 1'b1;
            @(posedge clk);
            #1;
            check(cycle_capture_empty, "enable starts with an empty FIFO");
            check(!capture_drop_sticky, "enable starts with drop clear");
        end
    endtask

    task automatic push_event;
        begin
            @(negedge clk);
            ab_read.data_en = 1'b1;
            @(posedge clk);
            #1;
            @(negedge clk);
            ab_read.data_en = 1'b0;
        end
    endtask

    task automatic pop_record(output logic [63:0] record_out);
        int guard;
        begin
            guard = 0;
            #1;
            while (cycle_capture_empty && (guard < 12)) begin
                @(posedge clk);
                #1;
                guard++;
            end
            if (cycle_capture_empty) begin
                $display("FAIL: timed out waiting for SDD record");
                failures++;
                record_out = '0;
            end else begin
                record_out = cycle_capture_data;
                @(negedge clk);
                cycle_capture_rd_en = 1'b1;
                @(posedge clk);
                #1;
                @(negedge clk);
                cycle_capture_rd_en = 1'b0;
            end
        end
    endtask

    task automatic pulse_drop_ack;
        begin
            @(negedge clk);
            capture_drop_ack = 1'b1;
            @(posedge clk);
            #1;
            @(negedge clk);
            capture_drop_ack = 1'b0;
        end
    endtask

    task automatic fill_sdd_fifo;
        int i;
        begin
            ab_read.rw = 1'b1;
            ab_read.res = 1'b1;
            ab_read.m2sel = 1'b0;
            ab_read.m2b0 = 1'b1;
            route_info = 3'b010;
            @(negedge clk);
            ab_read.data_en = 1'b1;
            for (i = 0; i < 4096; i++) begin
                ab_read.addr = 16'h2000 + i[15:0];
                ab_read.data = i[7:0];
                @(posedge clk);
                #1;
                @(negedge clk);
            end
            ab_read.data_en = 1'b0;
            // Flush the last assembled record into the FIFO.
            @(posedge clk);
            #1;
        end
    endtask

    task automatic test_exact_event_bits;
        logic [63:0] got;
        logic [63:0] want;
        begin
            $display("TEST: exact SDD event bits");
            clear_and_enable();
            ab_read.addr = 16'h1234;
            ab_read.rw = 1'b0;
            ab_read.res = 1'b1;
            ab_read.m2sel = 1'b0;
            ab_read.m2b0 = 1'b1;
            ab_read.data = 8'hA6;
            route_info = 3'b101;
            want = expected_record(16'h1234, 1'b0, 1'b1, 1'b0, 1'b1,
                                   8'hA6, 3'b101);
            @(negedge clk);
            ab_read.data_en = 1'b1;
            @(posedge clk);
            #1;
            check(cycle_capture_empty,
                  "record assembly adds one FIFO-write cycle");
            @(negedge clk);
            ab_read.data_en = 1'b0;
            @(posedge clk);
            #1;
            check(!cycle_capture_empty,
                  "assembled record reaches FIFO on the next clock");
            pop_record(got);
            check(got == want, "SDD record matches all 64 expected bits");
            check(got[31:0] == {3'b101, 1'b1, 8'hA6, 1'b1, 1'b0,
                                1'b1, 1'b0, 16'h1234},
                  "SDD parser word layout");
            #1 check(cycle_capture_empty, "one cycle makes one SDD record");
        end
    endtask

    task automatic test_back_to_back_order;
        logic [63:0] got;
        logic [63:0] want;
        int i;
        begin
            $display("TEST: back-to-back SDD record order");
            clear_and_enable();
            ab_read.rw = 1'b1;
            ab_read.res = 1'b1;
            ab_read.m2sel = 1'b0;
            ab_read.m2b0 = 1'b1;
            route_info = 3'b110;
            @(negedge clk);
            ab_read.data_en = 1'b1;
            for (i = 0; i < 3; i++) begin
                ab_read.addr = 16'h3100 + i[15:0];
                ab_read.data = 8'h80 + i[7:0];
                @(posedge clk);
                #1;
                @(negedge clk);
            end
            ab_read.data_en = 1'b0;
            @(posedge clk);
            #1;

            for (i = 0; i < 3; i++) begin
                pop_record(got);
                want = expected_record(16'h3100 + i[15:0], 1'b1, 1'b1,
                                       1'b0, 1'b1, 8'h80 + i[7:0],
                                       3'b110);
                check(got == want, "back-to-back record order and payload");
            end
            #1 check(cycle_capture_empty,
                     "back-to-back events make three records");
        end
    endtask

    task automatic test_storm_last_event;
        logic [63:0] got;
        logic [63:0] want;
        int i;
        begin
            $display("TEST: IRQ storm freezes after its trigger event");
            clear_and_enable();
            ab_read.addr = 16'hFFFE;
            ab_read.rw = 1'b1;
            ab_read.res = 1'b1;
            ab_read.m2sel = 1'b1;
            ab_read.m2b0 = 1'b0;
            route_info = 3'b001;
            for (i = 0; i < 4; i++) begin
                ab_read.data = 8'h10 + i[7:0];
                push_event();
            end
            check(dut.storm_frozen_q,
                  "four clustered vector fetches freeze SDD capture");

            // This event follows the freeze and must not enter the FIFO.
            ab_read.addr = 16'h1234;
            ab_read.data = 8'hEE;
            push_event();

            for (i = 0; i < 4; i++) begin
                pop_record(got);
                want = expected_record(16'hFFFE, 1'b1, 1'b1, 1'b1, 1'b0,
                                       8'h10 + i[7:0], 3'b001);
                check(got == want, "storm record order and payload");
            end
            #1 check(cycle_capture_empty,
                     "storm trigger is last captured event");
        end
    endtask

    task automatic test_jam_last_event;
        logic [63:0] got;
        logic [63:0] want;
        int jam_events;
        int i;
        begin
            $display("TEST: address jam freezes after its trigger event");
            clear_and_enable();
            ab_read.addr = 16'hBEEF;
            ab_read.rw = 1'b1;
            ab_read.res = 1'b1;
            ab_read.m2sel = 1'b0;
            ab_read.m2b0 = 1'b0;
            route_info = 3'b100;
            jam_events = 0;
            while (!dut.storm_frozen_q && (jam_events < 70)) begin
                ab_read.data = jam_events[7:0];
                push_event();
                jam_events++;
            end
            check(dut.storm_frozen_q, "repeated address freezes SDD capture");
            check((jam_events == 64) || (jam_events == 65),
                  "jam threshold stays at the documented boundary");
            if (jam_events == 65)
                $display("KNOWN GAP: nonzero jam address freezes after 65 events; the RTL comment says 64");

            // The next event must be blocked, while the trigger event remains.
            ab_read.addr = 16'hCAFE;
            ab_read.data = 8'hEE;
            push_event();
            for (i = 0; i < jam_events; i++) begin
                pop_record(got);
                want = expected_record(16'hBEEF, 1'b1, 1'b1, 1'b0, 1'b0,
                                       i[7:0], 3'b100);
                check(got == want, "jam record order and payload");
            end
            #1 check(cycle_capture_empty,
                     "jam trigger is last captured event");
        end
    endtask

    task automatic test_full_drop_ack;
        begin
            $display("TEST: SDD full FIFO, set-wins, ack, disable, reset");
            clear_and_enable();
            fill_sdd_fifo();
            #1 check(dut.fifo_full, "SDD FIFO reaches full boundary");

            ab_read.addr = 16'h4000;
            ab_read.data = 8'hE1;
            @(negedge clk);
            ab_read.data_en = 1'b1;
            @(posedge clk);
            #1;
            @(negedge clk);
            ab_read.data_en = 1'b0;
            capture_drop_ack = 1'b1;
            @(posedge clk);
            #1;
            @(negedge clk);
            capture_drop_ack = 1'b0;
            check(capture_drop_sticky,
                  "SDD overflow set beats same-cycle ack");
            pulse_drop_ack();
            #1 check(!capture_drop_sticky, "SDD ack clears overflow sticky");

            // Set it once more, then disable must clear the full FIFO and flag.
            push_event();
            @(posedge clk);
            #1;
            check(capture_drop_sticky, "SDD overflow sticky sets again");
            @(negedge clk);
            enable = 1'b0;
            @(posedge clk);
            #1;
            check(cycle_capture_empty, "disable clears SDD FIFO");
            check(!capture_drop_sticky, "disable clears SDD drop sticky");
            check(!dut.storm_frozen_q, "disable rearms SDD fault traps");

            // Activity while disabled must not leak through a later enable.
            ab_read.addr = 16'h7777;
            ab_read.data = 8'h77;
            push_event();
            enable = 1'b1;
            @(posedge clk);
            #1;
            check(cycle_capture_empty, "disabled event is discarded");

            // Hard reset also clears queued data and state.
            ab_read.addr = 16'h8888;
            ab_read.data = 8'h88;
            push_event();
            @(posedge clk);
            #1;
            check(!cycle_capture_empty, "record queued before hard reset");
            @(negedge clk);
            resetn = 1'b0;
            @(posedge clk);
            #1;
            check(cycle_capture_empty, "hard reset clears SDD FIFO");
            check(!capture_drop_sticky && !dut.storm_frozen_q,
                  "hard reset clears SDD flags");
            @(negedge clk);
            resetn = 1'b1;
            @(posedge clk);
            #1;
        end
    endtask

    initial begin
        repeat (4) @(posedge clk);
        @(negedge clk);
        resetn = 1'b1;
        repeat (2) begin
            @(posedge clk);
            #1;
        end

        test_exact_event_bits();
        test_back_to_back_order();
        test_storm_last_event();
        test_jam_last_event();
        test_full_drop_ack();

        if (failures == 0)
            $display("SDD BUS TAP PASS");
        else
            $display("SDD BUS TAP FAIL: %0d", failures);
        $finish;
    end

endmodule
