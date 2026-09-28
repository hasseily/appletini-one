`timescale 1ns / 1ps
module tb_fb_reader_modes;
    import video_pkg::*;
    logic clk = 0, pixel_clk = 0;
    always #5 clk = ~clk;
    always #4 pixel_clk = ~pixel_clk;
    logic resetn = 0, pause_request = 0, vblank = 0;
    logic [31:0] base = 32'h3E000000;
    logic [17:0] bursts = 32400;
    wire quiescent, latched;
    wire [31:0] live_base;
    wire [2:0] state;
    Axi3_read_if #(.ADDR_WIDTH(32), .DATA_WIDTH(64)) axi();
    fb_reader dut (
        .clk(clk), .resetn(resetn), .axi_read_if(axi),
        .base_addr_in(base), .frame_bursts_in(bursts),
        .pause_request(pause_request), .quiescent(quiescent),
        .last_latched_addr(live_base), .vblank_latched_pulse(latched),
        .vblank_start(vblank), .pixel_clk(pixel_clk), .pixel_rd_en(1'b0),
        .pixel_rgb565(), .axi_read_err(), .dbg_state(state),
        .dbg_burst_count(), .dbg_axi_err_count(), .dbg_underrun_count()
    );
    int pending = 0, accepted = 0;
    logic send_responses = 0;
    logic [31:0] last_address;
    assign axi.arready = 1;
    assign axi.rvalid = send_responses && pending != 0;
    assign axi.rlast = axi.rvalid;
    assign axi.rdata = 0;
    assign axi.rresp = 0;
    always @(posedge clk) if (resetn) begin
        pending <= pending + int'(axi.arvalid && axi.arready) - int'(axi.rvalid);
        if (axi.arvalid && axi.arready) begin
            accepted++;
            last_address <= axi.araddr;
        end
        if (quiescent && (axi.arvalid || pending != 0))
            $fatal(1, "mode pause acknowledged before AXI drain");
    end
    task automatic pulse_vblank;
        @(negedge clk); vblank = 1;
        @(negedge clk); vblank = 0;
    endtask
    int before_frame;
    initial begin
        repeat (4) @(negedge clk);
        resetn = 1;
        pulse_vblank();
        wait (pending == 8);
        @(negedge clk); pause_request = 1;
        repeat (12) @(negedge clk);
        if (quiescent || state != 3) $fatal(1, "pause skipped outstanding reads");
        send_responses = 1;
        wait (quiescent);
        repeat (12) @(negedge clk);
        if (!dut.fifo_reset_axi || live_base != 32'h3E000000)
            $fatal(1, "pause must hold FIFO reset and preserve old base");
        before_frame = accepted;
        base = 32'h3E400000;
        bursts = video_frame_bursts(3);
        pause_request = 0;
        repeat (4) @(negedge clk);
        pulse_vblank();
        wait (latched);
        wait (state == 2);
        wait (state == 0);
        @(negedge clk);
        if (accepted - before_frame != 27563 || pending != 0 ||
            last_address != base + 27562 * 128)
            $fatal(1, "1680x1050 rounded read extent is wrong");
        if (live_base != base) $fatal(1, "new base was not committed");
        $display("FB READER MODES PASS");
        $finish;
    end
    initial begin
        #3000000;
        $fatal(1, "reader mode test timeout");
    end
endmodule
