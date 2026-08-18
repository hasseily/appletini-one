`timescale 1ns / 1ps

// ONE//e video-standard request and session latch.
//
// Software may change desired_50hz at any time. A running virtual machine
// keeps active_50hz fixed until its private RESET line asserts, so the bus
// and scanner never change cadence partway through a frame. While ONE//e is
// stopped, the active value follows the saved request and is ready before the
// next session starts.
module onee_video_standard_ctrl (
    input  logic clk,
    input  logic resetn,
    input  logic enabled,
    input  logic virtual_res_n,
    input  logic write_valid,
    input  logic write_50hz,
    output logic desired_50hz,
    output logic active_50hz
);

    always_ff @(posedge clk) begin
        if (!resetn) begin
            desired_50hz <= 1'b0;
            active_50hz  <= 1'b0;
        end else begin
            if (write_valid)
                desired_50hz <= write_50hz;

            if (!enabled || !virtual_res_n)
                active_50hz <= write_valid ? write_50hz : desired_50hz;
        end
    end

endmodule
