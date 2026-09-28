`timescale 1ns / 1ps
// Use the generated AMD Clocking Wizard and UNISIM MMCM, not the AXI mock.
module tb_video_clock_wizard;
    import video_pkg::*;
    logic clk = 0, reference_clk = 0, resetn = 0;
    always #3.75 clk = ~clk;
    always #3.333333 reference_clk = ~reference_clk;
    logic request = 0;
    logic [3:0] requested_mode = VIDEO_DEFAULT_MODE;
    wire [3:0] active_mode;
    wire [31:0] committed_base;
    wire commit, busy, error, reader_pause, video_hold, clock_resetn;
    wire pixel_clk, locked, locked_sync;
    wire [10:0] awaddr;
    wire [31:0] wdata;
    wire awvalid, awready, wvalid, wready, bvalid, bready;
    wire [1:0] bresp;
    logic frame_ack_enabled = 1, frame_latched = 0;
    always @(posedge clk) frame_latched <= !video_hold && frame_ack_enabled;
    cdc_bit_sync locked_cdc (
        .clk(clk), .resetn(resetn), .din(locked), .dout(locked_sync)
    );
    video_mode_control #(.TIMEOUT_CYCLES(200000)) controller (
        .clk(clk), .resetn(resetn), .request(request),
        .requested_mode(requested_mode), .requested_base(32'h3E400000),
        .reader_quiescent(1'b1), .frame_latched(frame_latched),
        .clock_locked(locked_sync), .active_mode(active_mode),
        .committed_base(committed_base), .commit(commit), .busy(busy), .error(error),
        .reader_pause(reader_pause), .video_hold(video_hold), .clock_resetn(clock_resetn),
        .clock_awaddr(awaddr), .clock_awvalid(awvalid), .clock_awready(awready),
        .clock_wdata(wdata), .clock_wvalid(wvalid), .clock_wready(wready),
        .clock_bresp(bresp), .clock_bvalid(bvalid), .clock_bready(bready)
    );
    zynq_ps_bd_clk_wiz_0_0 wizard (
        .clk_in1(reference_clk), .clk_out1(pixel_clk), .locked(locked),
        .s_axi_aclk(clk), .s_axi_aresetn(clock_resetn),
        .s_axi_awaddr(awaddr), .s_axi_awvalid(awvalid), .s_axi_awready(awready),
        .s_axi_wdata(wdata), .s_axi_wstrb(4'hF), .s_axi_wvalid(wvalid),
        .s_axi_wready(wready), .s_axi_bresp(bresp), .s_axi_bvalid(bvalid),
        .s_axi_bready(bready), .s_axi_araddr(11'b0), .s_axi_arvalid(1'b0),
        .s_axi_arready(), .s_axi_rdata(), .s_axi_rresp(), .s_axi_rvalid(),
        .s_axi_rready(1'b1)
    );
    real frequencies[VIDEO_MODE_COUNT] = '{65.0, 67.5, 108.0, 119.0, 148.5, 85.5};
    realtime start_time, measured_period;
    task automatic measure(input int mode);
        @(posedge pixel_clk); start_time = $realtime;
        repeat (120) @(posedge pixel_clk);
        measured_period = ($realtime - start_time) / 120.0;
        if (measured_period < 1000.0 / frequencies[mode] - 0.02 ||
            measured_period > 1000.0 / frequencies[mode] + 0.02)
            $fatal(1, "actual wizard mode %0d period %f ns, expected %f", mode,
                measured_period, 1000.0 / frequencies[mode]);
        $display("CLOCK mode=%0d target=%f MHz measured=%f MHz", mode,
            frequencies[mode], 1000.0 / measured_period);
    endtask
    task automatic start_mode(input logic [3:0] id);
        @(negedge clk); requested_mode = id; request = 1;
        @(negedge clk); request = 0;
    endtask
    initial begin
        repeat (30) @(negedge clk);
        resetn = 1;
        wait (locked_sync);
        measure(VIDEO_DEFAULT_MODE);
        for (int mode = 0; mode < VIDEO_MODE_COUNT; mode++) begin
            start_mode(4'(mode));
            wait (commit);
            if (active_mode != mode || error) $fatal(1, "wizard preset apply failed");
            measure(mode);
            wait (!busy);
        end
        // Force a late scanout-start failure after programming 65 MHz.
        // Recovery must really restore 148.5 MHz, not only its mode ID.
        frame_ack_enabled = 0;
        start_mode(0);
        wait (commit);
        measure(0);
        wait (!clock_resetn);
        wait (commit);
        if (active_mode != VIDEO_DEFAULT_MODE || !error)
            $fatal(1, "wizard late failure did not recover default mode");
        measure(VIDEO_DEFAULT_MODE);
        frame_ack_enabled = 1;
        wait (!busy);
        $display("VIDEO CLOCK WIZARD PASS");
        $finish;
    end
    initial begin
        #10000000;
        $fatal(1, "actual Clocking Wizard test timeout");
    end
endmodule
