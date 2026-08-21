`timescale 1ns / 1ps

// Built-in Enhanced Apple //e I/O for the isolated ONE//e virtual bus.
//
// softswitch_ab_read must feed the existing soft_switch_manager. This module
// owns only motherboard I/O state which is absent from SoftSwitchState. It
// relies on that manager for MMU/video state. On a //e, $C05E/$C05F always
// reach both the DHIRES video latch and AN3. The IOUDIS gate described in
// later Apple manuals belongs to the //c and must not suppress either //e
// side effect.
module onee_motherboard_io #(
    parameter integer PADDLE_BASE_CYCLES  = 4,
    parameter integer PADDLE_SCALE_CYCLES = 11
) (
    input  logic                       clk,
    input  logic                       resetn,
    input  logic                       enabled,
    input  globals::AppleBus_read      ab_read,
    input  globals::SoftSwitchState    sss,
    output globals::AppleBus_read      softswitch_ab_read,
    output globals::AppleBus_write     ab_write,

    input  logic [7:0]                 floating_bus_data,
    input  logic                       video_vblank,

    // PS supplies already translated 7-bit Apple key codes. Shift, Control,
    // and Caps Lock alter that translation but have no direct register on the
    // base Enhanced //e. The live modifier output is for status/debug.
    input  logic                       keyboard_event_valid,
    output wire                        keyboard_event_ready,
    input  logic [6:0]                 keyboard_event_code,
    input  logic                       keyboard_any_down,
    input  logic [2:0]                 keyboard_modifiers_in,
    output wire [2:0]                  keyboard_modifiers_state,
    output wire [7:0]                  keyboard_latch,
    output logic                       keyboard_strobe,

    // pushbuttons[0] = Open Apple, [1] = Closed/Solid Apple, [2] = PB2.
    input  logic [2:0]                 pushbuttons,
    input  logic                       cassette_in,
    // Bytes [7:0] through [31:24] are PDL0 through PDL3. Values are
    // snapshotted on every $C070-$C07F access.
    input  logic [31:0]                paddle_values,

    output logic                       cassette_out,
    output logic                       speaker,
    output logic                       utility_strobe_pulse,
    output logic [3:0]                 annunciators,
    // The //e has no working IOUDIS latch. Keep this compatibility/debug
    // output low; $C07E/$C07F remain paddle-trigger aliases.
    output wire                        ioudis,
    output wire [3:0]                  paddle_active,
    output logic                       paddle_trigger_pulse
);

    wire bus_active = resetn && enabled && ab_read.res &&
                      ab_read.cycle_valid;
    wire is_c0xx = (ab_read.addr[15:8] == 8'hC0);
    wire serve_c0xx = bus_active && ab_read.serve_en && is_c0xx;
    // Writes anywhere in $C010-$C01F clear the keyboard strobe. Reads clear
    // it only at $C010; $C011-$C01F are non-destructive status reads.
    wire keyboard_clear_access = serve_c0xx &&
                                 ((!ab_read.rw &&
                                   (ab_read.addr[7:4] == 4'h1)) ||
                                  (ab_read.rw &&
                                   (ab_read.addr[7:0] == 8'h10)));

    logic [6:0] keyboard_code_q;

    assign keyboard_latch = {keyboard_strobe, keyboard_code_q};
    assign keyboard_modifiers_state = keyboard_modifiers_in;
    assign keyboard_event_ready = resetn && enabled && ab_read.res &&
                                  (!keyboard_strobe || keyboard_clear_access);

    // A //e applies both effects of $C05E/$C05F: the shared manager tracks
    // DHIRES and this module tracks AN3. Do not apply the //c IOUDIS gate.
    always_comb begin
        softswitch_ab_read = ab_read;
    end

    assign ioudis = 1'b0;

    function automatic logic [15:0] paddle_reload(
        input logic [7:0] value
    );
        integer reload_value;
        begin
            reload_value = PADDLE_BASE_CYCLES +
                           (value * PADDLE_SCALE_CYCLES);
            if (reload_value > 16'hFFFF)
                paddle_reload = 16'hFFFF;
            else if (reload_value < 1)
                paddle_reload = 16'h0001;
            else
                paddle_reload = reload_value[15:0];
        end
    endfunction

    logic [15:0] paddle_count_q [0:3];

    assign paddle_active = {
        paddle_count_q[3] != 16'd0,
        paddle_count_q[2] != 16'd0,
        paddle_count_q[1] != 16'd0,
        paddle_count_q[0] != 16'd0
    };

    logic       read_claim;
    logic [7:0] read_data;
    logic       peripheral_status_bit;
    wire [7:0]  c01x_status_byte;

    apple_c01x_status_decode c01x_status_decode_i (
        .selector   (ab_read.addr[3:0]),
        .sss        (sss),
        .vbl_bar    (~video_vblank),
        .low_bits   (keyboard_code_q),
        .status_byte(c01x_status_byte)
    );

    // Keyboard and soft-switch status reads keep the last key code in bits
    // 6:0, as the Enhanced //e IOU does. Cassette, button, and paddle reads
    // preserve floating-bus bits 6:0. All other reads remain unclaimed so
    // apple_virtual_bus supplies its full floating/scanner byte.
    always_comb begin
        read_claim = 1'b0;
        read_data  = floating_bus_data;
        peripheral_status_bit = 1'b0;

        if (bus_active && ab_read.rw && is_c0xx) begin
            unique casez (ab_read.addr[7:0])
                8'h0?: begin
                    read_claim = 1'b1;
                    read_data  = keyboard_latch;
                end
                8'h10: begin
                    read_claim = 1'b1;
                    read_data  = {keyboard_any_down, keyboard_code_q};
                end
                // Enhanced //e $C07E/$C07F both return the floating bus.
                // The documented RDIOUDIS/RDDHIRES bits exist on the //c,
                // not on real //e hardware.
                default: ;
            endcase

            if ((ab_read.addr[7:4] == 4'h1) &&
                (ab_read.addr[3:0] != 4'h0)) begin
                read_claim = 1'b1;
                read_data  = c01x_status_byte;
            end

            // The motherboard C06X selector ignores A3, so C068-C06F mirror
            // the cassette, button, and paddle reads at C060-C067.
            if (ab_read.addr[7:4] == 4'h6) begin
                unique case (ab_read.addr[2:0])
                    3'h0: peripheral_status_bit = cassette_in;
                    3'h1: peripheral_status_bit = pushbuttons[0];
                    3'h2: peripheral_status_bit = pushbuttons[1];
                    3'h3: peripheral_status_bit = pushbuttons[2];
                    3'h4: peripheral_status_bit = paddle_active[0];
                    3'h5: peripheral_status_bit = paddle_active[1];
                    3'h6: peripheral_status_bit = paddle_active[2];
                    3'h7: peripheral_status_bit = paddle_active[3];
                endcase
                read_claim = 1'b1;
                read_data  = {
                    peripheral_status_bit, floating_bus_data[6:0]
                };
            end
        end
    end

    globals::AppleBus_write ab_write_q;
    globals::AppleBus_write ab_write_d;

    // The registered response may span serve_en through data_en. Gate the
    // public bus combinationally so disabling ONE//e kills any response at
    // once, without waiting for the next fabric clock.
    always_comb begin
        if (enabled)
            ab_write = ab_write_q;
        else
            ab_write = '0;
    end

    // Hold a read response from serve_en through data_en, matching the shared
    // AppleBus contract used by the existing slot-card responders.
    always_comb begin
        ab_write_d = ab_write_q;
        ab_write_d.wr_dma_data_en = 1'b0;
        ab_write_d.wr_addr        = 16'h0000;
        ab_write_d.wr_rw          = 1'b0;
        ab_write_d.wr_addr_rw_en  = 1'b0;
        ab_write_d.assert_inh     = 1'b0;
        ab_write_d.assert_res     = 1'b0;
        ab_write_d.assert_irq     = 1'b0;
        ab_write_d.assert_rdy     = 1'b0;
        ab_write_d.assert_nmi     = 1'b0;
        ab_write_d.assert_dma     = 1'b0;

        if (ab_read.serve_en) begin
            ab_write_d.wr_data    = read_data;
            ab_write_d.wr_data_en = read_claim;
        end else if (ab_read.data_en) begin
            ab_write_d.wr_data    = 8'h00;
            ab_write_d.wr_data_en = 1'b0;
        end
    end

    always_ff @(posedge clk) begin
        if (!resetn || !enabled || !ab_read.res) begin
            keyboard_code_q     <= 7'h00;
            keyboard_strobe     <= 1'b0;
            cassette_out        <= 1'b0;
            speaker             <= 1'b0;
            utility_strobe_pulse <= 1'b0;
            annunciators        <= 4'h0;
            paddle_count_q[0]   <= 16'd0;
            paddle_count_q[1]   <= 16'd0;
            paddle_count_q[2]   <= 16'd0;
            paddle_count_q[3]   <= 16'd0;
            paddle_trigger_pulse <= 1'b0;
            ab_write_q          <= '0;
        end else begin
            utility_strobe_pulse <= 1'b0;
            paddle_trigger_pulse <= 1'b0;
            ab_write_q <= ab_write_d;

            // One data_en marks one native Apple cycle, including idle bus
            // cycles. Paddle timing therefore remains independent of CPU warp.
            if (ab_read.data_en && ab_read.cycle_valid) begin
                if (paddle_count_q[0] != 16'd0)
                    paddle_count_q[0] <= paddle_count_q[0] - 16'd1;
                if (paddle_count_q[1] != 16'd0)
                    paddle_count_q[1] <= paddle_count_q[1] - 16'd1;
                if (paddle_count_q[2] != 16'd0)
                    paddle_count_q[2] <= paddle_count_q[2] - 16'd1;
                if (paddle_count_q[3] != 16'd0)
                    paddle_count_q[3] <= paddle_count_q[3] - 16'd1;
            end

            if (keyboard_clear_access)
                keyboard_strobe <= 1'b0;

            // A newly accepted key wins over a simultaneous clear.
            if (keyboard_event_valid && keyboard_event_ready) begin
                keyboard_code_q <= keyboard_event_code;
                keyboard_strobe <= 1'b1;
            end

            if (serve_c0xx) begin
                // The legacy decoders mirror cassette and speaker toggles
                // through their full 16-byte ranges.
                if (ab_read.addr[7:4] == 4'h2)
                    cassette_out <= ~cassette_out;
                if (ab_read.addr[7:4] == 4'h3)
                    speaker <= ~speaker;

                // The motherboard C04X decoder mirrors the utility strobe
                // through $C040-$C04F.
                if (ab_read.addr[7:4] == 4'h4)
                    utility_strobe_pulse <= 1'b1;

                // On a //e, $C058-$C05F always select AN0 through AN3.
                // $C05E/$C05F also reach the DHIRES latch through the
                // shared soft-switch manager.
                if (ab_read.addr[7:3] == 5'b01011) begin
                    annunciators[ab_read.addr[2:1]] <= ab_read.addr[0];
                end

                // Every $C070-$C07F access triggers all four timers. The
                // //c-only IOUDIS write gate is absent on an Enhanced //e.
                if (ab_read.addr[7:4] == 4'h7) begin
                    paddle_count_q[0] <= paddle_reload(paddle_values[7:0]);
                    paddle_count_q[1] <= paddle_reload(paddle_values[15:8]);
                    paddle_count_q[2] <= paddle_reload(paddle_values[23:16]);
                    paddle_count_q[3] <= paddle_reload(paddle_values[31:24]);
                    paddle_trigger_pulse <= 1'b1;
                end
            end
        end
    end

endmodule
