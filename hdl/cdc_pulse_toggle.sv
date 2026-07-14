`timescale 1ns / 1ps

// Pulse CDC helper using toggle synchronization.
// Converts a one-cycle pulse in src_clk into a one-cycle pulse in dst_clk.
// Suitable for sparse control/event pulses crossing unrelated clock domains.

module cdc_pulse_toggle (
    input  wire src_clk,
    input  wire src_resetn,
    input  wire src_pulse,
    input  wire dst_clk,
    input  wire dst_resetn,
    output wire dst_pulse
);

    xpm_cdc_pulse #(
        .DEST_SYNC_FF(4),   // Sync stages
        .INIT_SYNC_FF(0),
        .REG_OUTPUT(0),     // 0=combinational, 1=registered
        .RST_USED(0),       // 0=no reset, 1=implement reset
        .SIM_ASSERT_CHK(1)  // Enable simulation checks
    )
    xpm_cdc_pulse_inst (
        .dest_pulse(dst_pulse),
        .dest_clk(dst_clk),
        .dest_rst(!dst_resetn), // Tie to 0 if RST_USED=0
        .src_clk(src_clk),
        .src_pulse(src_pulse),
        .src_rst(!src_resetn)   // Tie to 0 if RST_USED=0
    );
    // reg src_toggle = 1'b0;
    // (* ASYNC_REG = "TRUE" *) reg [2:0] dst_sync = 3'b000;

    // always @(posedge src_clk or negedge src_resetn) begin
    //     if (!src_resetn)
    //         src_toggle <= 1'b0;
    //     else if (src_pulse)
    //         src_toggle <= ~src_toggle;
    // end

    // always @(posedge dst_clk or negedge dst_resetn) begin
    //     if (!dst_resetn)
    //         dst_sync <= 3'b000;
    //     else
    //         dst_sync <= {dst_sync[1:0], src_toggle};
    // end

    // assign dst_pulse = dst_sync[2] ^ dst_sync[1];

endmodule
