`timescale 1ns / 1ps

// PS-to-motherboard input bridge for the built-in ONE//e.
//
// The bridge owns CARD_CTRL offsets $5C-$5F. ps_wr_en is a one-clock write
// pulse; ps_addr and ps_read_addr are byte offsets in the CARD_CTRL block.
//
// $5C KEY_FIFO
//   Write: bits 6:0 enqueue one translated Apple key code. Every write is
//          one enqueue request. A write to a full FIFO is dropped and sets
//          the sticky overflow flag unless a key is accepted that cycle.
//   Read:  bits 6:0 head code, bit 7 head valid, bits 11:8 entry count,
//          bit 12 full, bit 13 empty, bit 14 overflow sticky.
// $5D LIVE_INPUTS
//   Write/read: bit 0 any key down, bit 1 Open Apple, bit 2 Closed Apple,
//               bits 5:3 raw USB joystick PB0-PB2.
//   Open and Closed Apple also assert effective pushbuttons 0 and 1.
// $5E PADDLES
//   Write/read: bytes 0 through 3 hold PDL0 through PDL3. $80 is neutral.
// $5F CONTROL_STATUS
//   Write: bit 0 requests a warm reset and holds it until warm_reset_ack,
//          bit 1 clears FIFO overflow, bit 2 flushes the key FIFO, and
//          bit 3 releases all live inputs and restores neutral paddles.
//   Read:  bit 0 warm-reset request, bit 1 FIFO overflow, bit 2 FIFO empty,
//          bit 3 FIFO full, bits 7:4 FIFO count, bit 8 reset acknowledge,
//          bit 9 enabled, and bits 31:24 signature $E1.
//
// enabled is the safety boundary. Deasserting it masks every output at once.
// The next clock clears queued and live input state. No PS write, FIFO pop, or
// reset acknowledge can change state while the bridge is disabled.
module onee_input_bridge (
    input  logic                       clk,
    input  logic                       resetn,
    input  logic                       enabled,

    input  logic                       ps_wr_en,
    input  logic [7:0]                 ps_addr,
    input  logic [31:0]                ps_wdata,
    input  logic [7:0]                 ps_read_addr,
    output logic [31:0]                ps_rdata,

    output logic                       keyboard_event_valid,
    input  logic                       keyboard_event_ready,
    output logic [6:0]                 keyboard_event_code,
    output logic                       keyboard_any_down,
    output logic [2:0]                 keyboard_modifiers,
    output logic [2:0]                 pushbuttons,
    output logic [31:0]                paddle_values,

    output logic                       warm_reset_request,
    input  logic                       warm_reset_ack
);

    localparam logic [7:0] REG_KEY_FIFO       = 8'h5C;
    localparam logic [7:0] REG_LIVE_INPUTS    = 8'h5D;
    localparam logic [7:0] REG_PADDLES        = 8'h5E;
    localparam logic [7:0] REG_CONTROL_STATUS = 8'h5F;

    localparam logic [31:0] NEUTRAL_PADDLES = 32'h8080_8080;

    logic [6:0] key_fifo_q [0:7];
    logic [2:0] key_rd_q;
    logic [2:0] key_wr_q;
    logic [3:0] key_count_q;
    logic       key_overflow_q;

    logic       keyboard_any_down_q;
    logic       open_apple_q;
    logic       closed_apple_q;
    logic [2:0] joystick_buttons_q;
    logic [31:0] paddle_values_q;
    logic       warm_reset_request_q;

    wire key_empty = (key_count_q == 4'd0);
    wire key_full = (key_count_q == 4'd8);
    wire key_pop = resetn && enabled && !key_empty &&
                   keyboard_event_ready;
    wire key_push_request = resetn && enabled && ps_wr_en &&
                            (ps_addr == REG_KEY_FIFO);
    wire key_push = key_push_request && (!key_full || key_pop);
    wire key_drop = key_push_request && key_full && !key_pop;
    wire control_write = resetn && enabled && ps_wr_en &&
                         (ps_addr == REG_CONTROL_STATUS);

    // All consumer-facing signals use an immediate enable mask. This prevents
    // a queued key or reset request from escaping during the clock in which
    // the ONE//e safety guard drops enabled.
    always_comb begin
        keyboard_event_valid = 1'b0;
        keyboard_event_code = 7'h00;
        keyboard_any_down = 1'b0;
        keyboard_modifiers = 3'b000;
        pushbuttons = 3'b000;
        paddle_values = 32'h0000_0000;
        warm_reset_request = 1'b0;

        if (resetn && enabled) begin
            keyboard_event_valid = !key_empty;
            keyboard_event_code = key_empty ? 7'h00 :
                                  key_fifo_q[key_rd_q];
            keyboard_any_down = keyboard_any_down_q;
            keyboard_modifiers = {1'b0, closed_apple_q, open_apple_q};
            pushbuttons = joystick_buttons_q |
                          {1'b0, closed_apple_q, open_apple_q};
            paddle_values = paddle_values_q;
            warm_reset_request = warm_reset_request_q;
        end
    end

    // Reads also vanish at the safety boundary. The top-level register mux
    // can OR or select this value without exposing stale standalone state.
    always_comb begin
        ps_rdata = 32'h0000_0000;
        if (resetn && enabled) begin
            case (ps_read_addr)
                REG_KEY_FIFO: begin
                    ps_rdata = {
                        17'h00000,
                        key_overflow_q,
                        key_empty,
                        key_full,
                        key_count_q,
                        !key_empty,
                        key_empty ? 7'h00 : key_fifo_q[key_rd_q]
                    };
                end
                REG_LIVE_INPUTS: begin
                    ps_rdata = {
                        26'h0000000,
                        joystick_buttons_q,
                        closed_apple_q,
                        open_apple_q,
                        keyboard_any_down_q
                    };
                end
                REG_PADDLES: begin
                    ps_rdata = paddle_values_q;
                end
                REG_CONTROL_STATUS: begin
                    ps_rdata = {
                        8'hE1,
                        14'h0000,
                        1'b1,
                        warm_reset_ack,
                        key_count_q,
                        key_full,
                        key_empty,
                        key_overflow_q,
                        warm_reset_request_q
                    };
                end
                default: ps_rdata = 32'h0000_0000;
            endcase
        end
    end

    integer key_index;
    always_ff @(posedge clk) begin
        if (!resetn) begin
            key_rd_q <= 3'd0;
            key_wr_q <= 3'd0;
            key_count_q <= 4'd0;
            key_overflow_q <= 1'b0;
            keyboard_any_down_q <= 1'b0;
            open_apple_q <= 1'b0;
            closed_apple_q <= 1'b0;
            joystick_buttons_q <= 3'b000;
            paddle_values_q <= NEUTRAL_PADDLES;
            warm_reset_request_q <= 1'b0;
            for (key_index = 0; key_index < 8;
                 key_index = key_index + 1) begin
                key_fifo_q[key_index] <= 7'h00;
            end
        end else if (!enabled) begin
            key_rd_q <= 3'd0;
            key_wr_q <= 3'd0;
            key_count_q <= 4'd0;
            key_overflow_q <= 1'b0;
            keyboard_any_down_q <= 1'b0;
            open_apple_q <= 1'b0;
            closed_apple_q <= 1'b0;
            joystick_buttons_q <= 3'b000;
            paddle_values_q <= NEUTRAL_PADDLES;
            warm_reset_request_q <= 1'b0;
            for (key_index = 0; key_index < 8;
                 key_index = key_index + 1) begin
                key_fifo_q[key_index] <= 7'h00;
            end
        end else begin
            // An acknowledge clears an old request. A new request in the
            // same cycle wins so it remains visible for at least one clock.
            if (warm_reset_ack)
                warm_reset_request_q <= 1'b0;
            if (control_write && ps_wdata[0])
                warm_reset_request_q <= 1'b1;

            if (ps_wr_en && (ps_addr == REG_LIVE_INPUTS)) begin
                keyboard_any_down_q <= ps_wdata[0];
                open_apple_q <= ps_wdata[1];
                closed_apple_q <= ps_wdata[2];
                joystick_buttons_q <= ps_wdata[5:3];
            end

            if (ps_wr_en && (ps_addr == REG_PADDLES))
                paddle_values_q <= ps_wdata;

            if (control_write && ps_wdata[3]) begin
                keyboard_any_down_q <= 1'b0;
                open_apple_q <= 1'b0;
                closed_apple_q <= 1'b0;
                joystick_buttons_q <= 3'b000;
                paddle_values_q <= NEUTRAL_PADDLES;
            end

            if (control_write && ps_wdata[1])
                key_overflow_q <= 1'b0;
            if (key_drop)
                key_overflow_q <= 1'b1;

            if (control_write && ps_wdata[2]) begin
                key_rd_q <= 3'd0;
                key_wr_q <= 3'd0;
                key_count_q <= 4'd0;
            end else begin
                if (key_push) begin
                    key_fifo_q[key_wr_q] <= ps_wdata[6:0];
                    key_wr_q <= key_wr_q + 3'd1;
                end
                if (key_pop)
                    key_rd_q <= key_rd_q + 3'd1;

                case ({key_push, key_pop})
                    2'b10: key_count_q <= key_count_q + 4'd1;
                    2'b01: key_count_q <= key_count_q - 4'd1;
                    default: key_count_q <= key_count_q;
                endcase
            end
        end
    end

endmodule
