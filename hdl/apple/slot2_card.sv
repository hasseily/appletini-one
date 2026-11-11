`timescale 1ns / 1ps

// One slot-2 bus client: Off, AppleMouse, 4Play, SNES MAX, or Phasor.
// $AE/$AF stage two 12-bit held-button sets each. A low-byte $AD write
// commits both words, presence [7:4], and selection [2:0] together.
// Reset selects Mouse for compatibility with firmware predating $AD.
module slot2_card (
    input  logic                     clk,
    input  logic                     resetn,
    input  logic                     card_resetn,
    input  logic                     enabled,
    input  globals::AppleBus_read     ab_read,
    input  globals::AxiSimple_common  as_common,
    input  logic                     ps_wr_en,
    output logic [31:0]               ps_rdata,
    output logic                     mouse_selected,
    output logic                     phasor_selected,
    output logic                     phasor_location2,
    output globals::AppleBus_read    mouse_ab_read,
    input  globals::AppleBus_write   mouse_ab_write,
    input  globals::AppleBus_write   phasor_ab_write,
    output globals::AppleBus_write   ab_write
);
    localparam logic [7:0] REG_CONTROL = 8'hAD;
    localparam logic [7:0] REG_STATE_LO = 8'hAE;
    localparam logic [7:0] REG_STATE_HI = 8'hAF;
    localparam logic [2:0] MODE_MOUSE = 3'd1;
    localparam logic [2:0] MODE_PHASOR = 3'd4;

    logic [31:0] staged_lo_q;
    logic [31:0] staged_hi_q;
    logic [47:0] buttons_q;
    logic [3:0] present_q;
    logic [2:0] mode_q;
    globals::AppleBus_write gamepad_ab_write;
    wire card_active = resetn && card_resetn && enabled;
    assign mouse_selected = card_active && (mode_q == MODE_MOUSE);
    assign phasor_location2 = mode_q == MODE_PHASOR;
    assign phasor_selected = card_active && phasor_location2;

    always_comb begin
        ps_rdata = 32'd0;
        case (as_common.araddr)
            REG_CONTROL: ps_rdata = {16'h5332, 6'd0, 2'b10, present_q, 1'b0, mode_q};
            REG_STATE_LO: ps_rdata = staged_lo_q;
            REG_STATE_HI: ps_rdata = staged_hi_q;
            default: ps_rdata = 32'd0;
        endcase
        mouse_ab_read = ab_read;
        if (!mouse_selected) begin
            mouse_ab_read.addr_en = 1'b0;
            mouse_ab_read.data_en = 1'b0;
            mouse_ab_read.sss_en = 1'b0;
            mouse_ab_read.serve_en = 1'b0;
        end
        // Mask every bus enable and assertion before both arbiters.
        ab_write = '0;
        if (card_active) begin
            case (mode_q)
                MODE_MOUSE: ab_write = mouse_ab_write;
                3'd2, 3'd3: ab_write = gamepad_ab_write;
                MODE_PHASOR: ab_write = phasor_ab_write;
                default: ab_write = '0;
            endcase
        end
        // Inactive payload is ignored by both arbiters. Keep reset/enable
        // qualification off these bits; it remains on every control above.
        case (mode_q)
            MODE_MOUSE:  ab_write.wr_data = mouse_ab_write.wr_data;
            MODE_PHASOR: ab_write.wr_data = phasor_ab_write.wr_data;
            default:     ab_write.wr_data = gamepad_ab_write.wr_data;
        endcase
    end

    always_ff @(posedge clk) begin
        if (!resetn) begin
            staged_lo_q <= 32'd0;
            staged_hi_q <= 32'd0;
            buttons_q <= 48'd0;
            present_q <= 4'd0;
            mode_q <= MODE_MOUSE;
        end else if (ps_wr_en) begin
            case (as_common.awaddr)
                REG_STATE_LO: staged_lo_q <= globals::apply_wstrb(
                    staged_lo_q, as_common.wdata, as_common.wstrb) & 32'h0FFF_0FFF;
                REG_STATE_HI: staged_hi_q <= globals::apply_wstrb(
                    staged_hi_q, as_common.wdata, as_common.wstrb) & 32'h0FFF_0FFF;
                REG_CONTROL: if (as_common.wstrb[0]) begin
                    buttons_q <= {staged_hi_q[27:16], staged_hi_q[11:0],
                                  staged_lo_q[27:16], staged_lo_q[11:0]};
                    present_q <= as_common.wdata[7:4];
                    mode_q <= as_common.wdata[2:0] <= MODE_PHASOR ?
                              as_common.wdata[2:0] : 3'd0;
                end
                default: begin end
            endcase
        end
    end

    slot2_gamepad_card gamepad_card_i (
        .clk(clk),
        .resetn(resetn && card_resetn),
        .enabled(enabled),
        .mode(mode_q[1:0]),
        .buttons(buttons_q),
        .present(present_q),
        .ab_read(ab_read),
        .ab_write(gamepad_ab_write)
    );
endmodule
