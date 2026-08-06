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

        if (failures == 0)
            $display("APPLE CYCLE EGRESS RING FLAGS PASS");
        else
            $display("APPLE CYCLE EGRESS RING FLAGS FAIL: %0d", failures);
        $finish;
    end

endmodule
