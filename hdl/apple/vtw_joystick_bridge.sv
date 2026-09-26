`timescale 1ns / 1ps

// USB joystick state for physical-host vTW. Both ports use the fabric clock.
// CARD_CTRL $AB stages PDL0..3, one byte each; byte strobes apply normally.
// A byte-zero write to $AC commits the staged axes and PB0..2 [3:1] together.
// Bit 0 says a USB joystick is present; clearing it restores physical input.
// $AC reads signature $4A in [31:24], enabled in bit 8, and live state [3:0].
// The caller enables this bridge only for physical-host vTW. Disable masks
// outputs at once and clears state on the next clock; PS cannot arm it then.
module vtw_joystick_bridge (
    input  logic        clk,
    input  logic        resetn,
    input  logic        enabled,
    input  logic        ps_wr_en,
    input  logic [7:0]  ps_addr,
    input  logic [31:0] ps_wdata,
    input  logic [3:0]  ps_wstrb,
    input  logic [7:0]  ps_read_addr,
    output logic [31:0] ps_rdata,
    output wire         joystick_active,
    output wire [2:0]   joystick_buttons,
    output wire [31:0]  joystick_paddles
);
    localparam logic [7:0] REG_PADDLES = 8'hAB;
    localparam logic [7:0] REG_CONTROL = 8'hAC;
    localparam logic [31:0] NEUTRAL_PADDLES = 32'h8080_8080;

    logic [31:0] staged_paddles_q;
    logic [31:0] paddles_q;
    logic [2:0] buttons_q;
    logic present_q;
    wire bridge_enabled = resetn && enabled;

    assign joystick_active = bridge_enabled && present_q;
    assign joystick_buttons = joystick_active ? buttons_q : 3'b000;
    assign joystick_paddles = joystick_active ? paddles_q : NEUTRAL_PADDLES;

    always_comb begin
        ps_rdata = 32'd0;
        case (ps_read_addr)
            REG_PADDLES:
                ps_rdata = bridge_enabled ? staged_paddles_q : NEUTRAL_PADDLES;
            REG_CONTROL:
                ps_rdata = {8'h4A, 15'd0, bridge_enabled, 4'd0,
                            joystick_buttons, joystick_active};
            default: ps_rdata = 32'd0;
        endcase
    end

    always_ff @(posedge clk) begin
        if (!resetn || !enabled) begin
            staged_paddles_q <= NEUTRAL_PADDLES;
            paddles_q <= NEUTRAL_PADDLES;
            buttons_q <= 3'b000;
            present_q <= 1'b0;
        end else if (ps_wr_en) begin
            if (ps_addr == REG_PADDLES) begin
                for (int lane = 0; lane < 4; lane = lane + 1) begin
                    if (ps_wstrb[lane])
                        staged_paddles_q[lane*8 +: 8] <= ps_wdata[lane*8 +: 8];
                end
            end
            if ((ps_addr == REG_CONTROL) && ps_wstrb[0]) begin
                present_q <= ps_wdata[0];
                buttons_q <= ps_wdata[0] ? ps_wdata[3:1] : 3'b000;
                paddles_q <= ps_wdata[0] ? staged_paddles_q : NEUTRAL_PADDLES;
            end
        end
    end
endmodule
