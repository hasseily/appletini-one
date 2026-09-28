`timescale 1ns / 1ps

// Slot-2 gamepad interfaces. Inputs and the sampled Apple bus share clk.
// Button order: B, Y, Select, Start, Up, Down, Left, Right, A, X, L, R.
// 4Play Rev B exposes B/A/Y as triggers 1/2/3. SNES MAX uses players 1/2.
module slot2_gamepad_card (
    input  logic                    clk,
    input  logic                    resetn,
    input  logic                    enabled,
    input  logic [1:0]              mode,
    input  logic [47:0]             buttons,
    input  logic [3:0]              present,
    input  globals::AppleBus_read   ab_read,
    output globals::AppleBus_write ab_write
);
    localparam logic [1:0] MODE_4PLAY = 2'd2;
    localparam logic [1:0] MODE_SNES = 2'd3;

    logic [1:0] mode_q;
    logic [4:0] serial_bit_q;
    logic [23:0] serial_buttons_q;
    logic [1:0] serial_present_q;
    globals::AppleBus_write response_q;
    wire active = resetn && enabled && ab_read.res &&
                  ((mode == MODE_4PLAY) || (mode == MODE_SNES));
    wire io_hit = ab_read.addr[15:4] == 12'hC0A;
    wire [1:0] player = ab_read.addr[1:0];
    wire [11:0] player_buttons = buttons[player * 12 +: 12];
    logic [7:0] fourplay_data;
    logic [1:0] serial_data;

    always_comb begin
        // Bit 5 identifies the card even with no controller connected.
        fourplay_data = 8'h20;
        if (present[player]) begin
            fourplay_data = {player_buttons[0], player_buttons[8], 1'b1,
                             player_buttons[1], player_buttons[7],
                             player_buttons[6], player_buttons[5],
                             player_buttons[4]};
        end
        serial_data = 2'b11;
        for (int pad = 0; pad < 2; pad = pad + 1) begin
            if (serial_present_q[pad]) begin
                if (serial_bit_q < 5'd12)
                    serial_data[pad] = ~serial_buttons_q[pad * 12 + serial_bit_q];
                else if (serial_bit_q == 5'd16)
                    serial_data[pad] = 1'b0;
            end
        end
        ab_write = '0;
        if (active && (mode == mode_q))
            ab_write = response_q;
        // The arbiter ignores payload unless its data enable is asserted.
        ab_write.wr_data = response_q.wr_data;
    end

    always_ff @(posedge clk) begin
        mode_q <= mode;
        if (!active || (mode != mode_q)) begin
            serial_bit_q <= 5'd16;
            serial_buttons_q <= 24'd0;
            serial_present_q <= 2'd0;
            response_q <= '0;
        end else begin
            // Reads hold their response through the Apple data phase.
            if (ab_read.serve_en) begin
                response_q <= '0;
                if (ab_read.rw && io_hit) begin
                    if (mode == MODE_4PLAY) begin
                        response_q.wr_data <= fourplay_data;
                        response_q.wr_data_en <= 1'b1;
                    end else begin
                        response_q.wr_data <= {serial_data[0], serial_data[1], 6'd0};
                        response_q.wr_data_en <= 1'b1;
                    end
                end
            end else if (ab_read.data_en) begin
                response_q <= '0;
            end
            if ((mode == MODE_SNES) && io_hit &&
                ab_read.data_en && !ab_read.rw) begin
                // The real SNES MAX decodes only A0, not A1..A3.
                if (!ab_read.addr[0]) begin
                    serial_buttons_q <= buttons[23:0];
                    serial_present_q <= present[1:0];
                    serial_bit_q <= 5'd0;
                end else if (serial_bit_q < 5'd16) begin
                    serial_bit_q <= serial_bit_q + 5'd1;
                end
            end
        end
    end
endmodule
