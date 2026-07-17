`timescale 1ns / 1ps

// DS1216E-style no-slot clock.
//
// The clock is hidden underneath the Appletini card ROM. Before the DS1216
// 64-bit access pattern matches, this module only observes this card's slot
// ROM page and owned C800 expansion-ROM window; it never drives the Apple bus.
// The ROM socket protocol uses A2 as the DS1216E write/read selector. A2 low
// shifts address bit A0 into the access-pattern matcher or clock registers;
// A2 high resets the matcher or shifts clock data out. After a match, the next
// 64 A2-low/high cycles write/read the eight clock registers, LSB first.

module no_slot_clock (
    input  logic                    clk,
    input  logic                    rstn,
    input  logic                    enabled,
    input  logic [7:0]              slot_mask,
    input  logic [63:0]             time_bcd,
    input  globals::AppleBus_read   ab_read,
    input  globals::SoftSwitchState sss,
    output logic [63:0]             write_time_bcd,
    output logic                    write_time_strobe,
    output globals::AppleBus_write  ab_write
);

    localparam logic [63:0] ACCESS_PATTERN = 64'h5CA3_3AC5_5CA3_3AC5;

    typedef enum logic [1:0] {
        NSC_IDLE,
        NSC_MATCH,
        NSC_TRANSFER
    } nsc_state_t;

    nsc_state_t state_q;
    logic [5:0] bit_count_q;
    logic [63:0] pattern_q;
    logic [63:0] shift_q;
    logic [63:0] write_shift_q;

    globals::AppleBus_write ab_write_d;
    globals::AppleBus_write ab_write_q;

    wire slot_rom_hit =
        enabled &&
        sss.slot_access &&
        (ab_read.addr[15:12] == 4'hC) &&
        (ab_read.addr[11] == 1'b0) &&
        (ab_read.addr[10:8] != 3'd0) &&
        slot_mask[ab_read.addr[10:8]];

    wire c8_rom_hit =
        enabled &&
        (sss.c8_select != 3'd0) &&
        slot_mask[sss.c8_select] &&
        (ab_read.addr[15:12] == 4'hC) &&
        (ab_read.addr[11] == 1'b1) &&
        (ab_read.addr[10:0] != 11'h7FF);

    wire rom_hit         = slot_rom_hit || c8_rom_hit;
    wire rom_cycle       = ab_read.serve_en && rom_hit;
    wire rom_output_read = rom_cycle && ab_read.rw && ab_read.addr[2];
    wire rom_input_cycle = rom_cycle && !ab_read.addr[2];
    wire rom_reset_cycle = rom_cycle && ab_read.addr[2];

    assign ab_write = ab_write_q;

    always_comb begin
        ab_write_d                 = ab_write_q;
        ab_write_d.assert_inh      = 1'b0;
        ab_write_d.assert_res      = 1'b0;
        ab_write_d.assert_irq      = 1'b0;
        ab_write_d.assert_rdy      = 1'b0;
        ab_write_d.assert_nmi      = 1'b0;
        ab_write_d.assert_dma      = 1'b0;
        ab_write_d.wr_addr         = 16'h0000;
        ab_write_d.wr_rw           = 1'b0;
        ab_write_d.wr_addr_rw_en   = 1'b0;

        if (rom_output_read && state_q == NSC_TRANSFER) begin
            ab_write_d.wr_data    = {7'h00, shift_q[0]};
            ab_write_d.wr_data_en = 1'b1;
        end else if (ab_read.sss_en || ab_read.data_en) begin
            ab_write_d.wr_data    = 8'h00;
            ab_write_d.wr_data_en = 1'b0;
        end
    end

    always_ff @(posedge clk) begin
        if (!rstn) begin
            state_q      <= NSC_MATCH;
            bit_count_q  <= 6'd0;
            pattern_q    <= ACCESS_PATTERN;
            shift_q      <= 64'd0;
            write_shift_q <= 64'd0;
            write_time_bcd <= 64'd0;
            write_time_strobe <= 1'b0;
            ab_write_q   <= '0;
        end else begin
            ab_write_q <= ab_write_d;
            write_time_strobe <= 1'b0;

            if (!enabled || slot_mask == 8'h00 || !ab_read.res) begin
                state_q       <= NSC_MATCH;
                bit_count_q   <= 6'd0;
                pattern_q     <= ACCESS_PATTERN;
                shift_q       <= 64'd0;
                write_shift_q <= 64'd0;
            end else if (rom_output_read) begin
                if (state_q == NSC_TRANSFER) begin
                    shift_q <= {1'b0, shift_q[63:1]};
                    if (bit_count_q == 6'd63) begin
                        state_q       <= NSC_MATCH;
                        bit_count_q   <= 6'd0;
                        pattern_q     <= ACCESS_PATTERN;
                        write_shift_q <= 64'd0;
                    end else begin
                        bit_count_q <= bit_count_q + 6'd1;
                    end
                end else begin
                    state_q       <= NSC_MATCH;
                    bit_count_q   <= 6'd0;
                    pattern_q     <= ACCESS_PATTERN;
                    write_shift_q <= 64'd0;
                end
            end else if (rom_input_cycle) begin
                if (state_q == NSC_MATCH) begin
                    if (ab_read.addr[0] == pattern_q[0]) begin
                        if (bit_count_q == 6'd63) begin
                            state_q       <= NSC_TRANSFER;
                            bit_count_q   <= 6'd0;
                            pattern_q     <= ACCESS_PATTERN;
                            shift_q       <= time_bcd;
                            write_shift_q <= 64'd0;
                        end else begin
                            state_q       <= NSC_MATCH;
                            bit_count_q   <= bit_count_q + 6'd1;
                            pattern_q     <= {1'b0, pattern_q[63:1]};
                        end
                    end else begin
                        state_q       <= NSC_IDLE;
                        bit_count_q   <= 6'd0;
                        pattern_q     <= ACCESS_PATTERN;
                        write_shift_q <= 64'd0;
                    end
                end else if (state_q == NSC_TRANSFER) begin
                    if (bit_count_q == 6'd63) begin
                        write_time_bcd <= {ab_read.addr[0], write_shift_q[62:0]};
                        write_time_strobe <= 1'b1;
                        state_q       <= NSC_MATCH;
                        bit_count_q   <= 6'd0;
                        write_shift_q <= 64'd0;
                    end else begin
                        write_shift_q[bit_count_q] <= ab_read.addr[0];
                        bit_count_q <= bit_count_q + 6'd1;
                    end
                end
            end else if (rom_reset_cycle) begin
                state_q       <= NSC_MATCH;
                bit_count_q   <= 6'd0;
                pattern_q     <= ACCESS_PATTERN;
                write_shift_q <= 64'd0;
            end else if (ab_read.serve_en && rom_hit) begin
                state_q       <= NSC_IDLE;
                bit_count_q   <= 6'd0;
                pattern_q     <= ACCESS_PATTERN;
                write_shift_q <= 64'd0;
            end
        end
    end

endmodule
