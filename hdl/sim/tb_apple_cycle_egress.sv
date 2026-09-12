`timescale 1ns / 1ps

// Focused checks for the registered ring-space guards. The guards may lag a
// consumer advance by one fabric clock, but must preserve the two reserved
// eight-byte slots at the end of the ring.
module tb_apple_cycle_egress;

    import apple_cycle_capture_pkg::*;

    logic clk = 1'b0;
    always #3.75 clk = ~clk;

    logic resetn = 1'b0;
    AppleCycleRecord cycle_capture_data = '0;
    logic cycle_capture_empty = 1'b1;
    logic cycle_capture_rd_en;
    logic capture_drop_sticky = 1'b0;
    logic capture_drop_ack;
    logic cfg_enable = 1'b0;
    logic [31:0] cfg_ring_base_addr = 32'h3F00_0000;
    logic [4:0] cfg_ring_size_log2 = 5'd12;
    logic [31:0] cfg_producer_ptr_addr = 32'h3F01_0000;
    logic [31:0] cfg_consumer_ptr = 32'd0;
    logic [31:0] stat_producer_ptr;
    logic [31:0] stat_records_written;
    logic [31:0] stat_gap_markers;
    logic [31:0] stat_bursts_issued;
    logic [31:0] stat_full_stall_cycles;
    Axi3_write_if #(.ADDR_WIDTH(32), .DATA_WIDTH(64)) axi_write();

    int failures = 0;
    int cap_cases = 0;
    int cap_clock_checks = 0;
    bit check_cap_each_clock = 1'b0;
    logic [31:0] forced_producer = 32'd0;
    logic [31:0] random_state = 32'h6E67_7265;

    // Reference is the original full-width formula, including unsigned
    // wrap for unsupported small sizes. It assumes no pointer alignment.
    function automatic logic [4:0] reference_cap(
        input logic [31:0] producer,
        input logic [31:0] consumer,
        input logic [31:0] size_bytes
    );
        logic [31:0] used, threshold, free_space;
        used = (producer - consumer) & (size_bytes - 32'd1);
        threshold = size_bytes - 32'd16;
        free_space = (used >= threshold) ? 32'd0 : threshold - used;
        return ((free_space >> 3) >= 32'd16) ? 5'd16 : free_space[7:3];
    endfunction

    function automatic logic [31:0] next_random(input logic [31:0] value);
        logic [31:0] result;
        result = value ^ (value << 13);
        result = result ^ (result >> 17);
        return result ^ (result << 5);
    endfunction

    apple_cycle_egress dut (
        .clk(clk),
        .resetn(resetn),
        .cycle_capture_data(cycle_capture_data),
        .cycle_capture_empty(cycle_capture_empty),
        .cycle_capture_rd_en(cycle_capture_rd_en),
        .capture_drop_sticky(capture_drop_sticky),
        .capture_drop_ack(capture_drop_ack),
        .cfg_enable(cfg_enable),
        .cfg_ring_base_addr(cfg_ring_base_addr),
        .cfg_ring_size_log2(cfg_ring_size_log2),
        .cfg_producer_ptr_addr(cfg_producer_ptr_addr),
        .cfg_consumer_ptr(cfg_consumer_ptr),
        .stat_producer_ptr(stat_producer_ptr),
        .stat_records_written(stat_records_written),
        .stat_gap_markers(stat_gap_markers),
        .stat_bursts_issued(stat_bursts_issued),
        .stat_full_stall_cycles(stat_full_stall_cycles),
        .axi_hp0_write(axi_write)
    );

    task automatic check_flags(input logic records_full,
                               input logic gap_full,
                               input string label_text);
        begin
            if ((dut.ring_full_for_records_q !== records_full) ||
                (dut.ring_full_for_gap_q !== gap_full)) begin
                $display("FAIL: %s records=%b gap=%b expected=%b/%b",
                         label_text, dut.ring_full_for_records_q,
                         dut.ring_full_for_gap_q, records_full, gap_full);
                failures++;
            end
        end
    endtask

    // Check the exact register edge, including the first edge after each
    // consumer/config change. A changed pipeline latency must fail here.
    always @(posedge clk) begin : cap_reference_monitor
        logic [4:0] expected_cap;
        logic expected_records_full, expected_gap_full;
        logic [31:0] expected_used;
        if (check_cap_each_clock) begin
            expected_used = (dut.producer_ptr_q - dut.consumer_ptr_q) &
                            (dut.ring_size_bytes - 32'd1);
            expected_cap = resetn ? reference_cap(dut.producer_ptr_q,
                                                  dut.consumer_ptr_q,
                                                  dut.ring_size_bytes) : 5'd0;
            expected_records_full = resetn &&
                (expected_used >= (dut.ring_size_bytes - 32'd16));
            expected_gap_full = resetn &&
                (expected_used >= (dut.ring_size_bytes - 32'd8));
            #1;
            cap_clock_checks++;
            if ((dut.free_beats_q !== expected_cap) ||
                (dut.ring_full_for_records_q !== expected_records_full) ||
                (dut.ring_full_for_gap_q !== expected_gap_full)) begin
                $display("FAIL: cap edge %0d got=%0d expected=%0d flags=%b/%b expected=%b/%b",
                         cap_clock_checks, dut.free_beats_q, expected_cap,
                         dut.ring_full_for_records_q, dut.ring_full_for_gap_q,
                         expected_records_full, expected_gap_full);
                failures++;
                if (failures >= 10)
                    $fatal(1, "Too many cap mismatches");
            end
        end
    end

    task automatic check_cap_case(input logic [4:0] size_log2,
                                  input logic [31:0] producer,
                                  input logic [31:0] consumer);
        @(negedge clk);
        cfg_ring_size_log2 = size_log2;
        forced_producer = producer;
        cfg_consumer_ptr = consumer;
        repeat (2) @(posedge clk);
        #2;
        cap_cases++;
    endtask

    initial begin
        axi_write.awready = 1'b1;
        axi_write.wready = 1'b1;
        axi_write.bresp = 2'b00;
        axi_write.bvalid = 1'b0;

        repeat (5) @(posedge clk);
        resetn = 1'b1;
        repeat (4) @(posedge clk);

        // A 4KB ring is full for records at 4080 bytes, with one gap slot.
        force dut.producer_ptr_q = 32'd4080;
        repeat (2) @(posedge clk);
        #1 check_flags(1'b1, 1'b0, "record reservation");

        // At 4088 bytes, not even the gap slot remains.
        force dut.producer_ptr_q = 32'd4088;
        repeat (2) @(posedge clk);
        #1 check_flags(1'b1, 1'b1, "gap reservation");

        // A consumer advance is sampled, then reaches the registered flags.
        cfg_consumer_ptr = 32'd8;
        repeat (2) @(posedge clk);
        #1 check_flags(1'b1, 1'b0, "consumer frees gap slot");

        cfg_consumer_ptr = 32'd16;
        repeat (2) @(posedge clk);
        #1 check_flags(1'b0, 1'b0, "consumer frees record slot");

        release dut.producer_ptr_q;
        force dut.producer_ptr_q = forced_producer;
        check_cap_each_clock = 1'b1;
        for (int size_log2 = 0; size_log2 < 32; size_log2++) begin
            // Exhaust all occupancies for small rings and sweep empty,
            // wrap, reservation and every 0..16-beat boundary for all sizes.
            // Vary the consumer low bits so aligned pointers are not assumed.
            for (int offset = 0; offset < 1024; offset++) begin
                random_state = next_random(random_state);
                check_cap_case(5'(size_log2), random_state + 32'(offset),
                               random_state);
            end
            for (int offset = 0; offset < 256; offset++) begin
                random_state = next_random(random_state);
                check_cap_case(5'(size_log2), random_state - 32'(offset),
                               random_state);
            end
            for (int sample = 0; sample < 2048; sample++) begin
                logic [31:0] producer;
                random_state = next_random(random_state);
                producer = random_state;
                random_state = next_random(random_state);
                check_cap_case(5'(size_log2), producer, random_state);
            end
        end

        // Reset must still clear the cap on the same clock edge, even with
        // forced nonzero pointers and the largest ring configuration.
        @(negedge clk);
        resetn = 1'b0;
        repeat (2) @(posedge clk);
        #2;
        @(negedge clk);
        resetn = 1'b1;
        repeat (3) @(posedge clk);
        #2;
        check_cap_each_clock = 1'b0;
        release dut.producer_ptr_q;

        $display("APPLE CYCLE EGRESS CAP EQUIVALENCE: %0d cases, %0d clock checks, 32 sizes",
                 cap_cases, cap_clock_checks);

        if (failures == 0)
            $display("APPLE CYCLE EGRESS RING FLAGS PASS");
        else
            $display("APPLE CYCLE EGRESS RING FLAGS FAIL: %0d", failures);
        $finish;
    end

endmodule
