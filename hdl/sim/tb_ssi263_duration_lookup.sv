`timescale 1ns / 1ps

// Exercise the production lookup directly. The oracle deliberately keeps
// the original arithmetic; it does not read or regenerate the lookup table.
module tb_ssi263_duration_lookup;
    logic [7:0] duration_phoneme = 8'd0;
    logic [7:0] rate_inflection = 8'd0;
    integer checks = 0;
    integer expected;
    logic [13:0] actual;

    sc01a_digital_core dut (
        .clk(1'b0),
        .rstn(1'b0),
        .reset(1'b0),
        .audio_tick(1'b0),
        .xck_ce(1'b0),
        .start(1'b0),
        .start_phone(6'd0),
        .start_votrax(1'b0),
        .current_function(2'd0),
        .duration_phoneme(duration_phoneme),
        .inflection(8'd0),
        .rate_inflection(rate_inflection),
        .articulation(3'd0)
    );

    initial begin
        // All 64 D/R combinations, with every combination of the ignored
        // phone and inflection bits: 256 * 256 distinct input bytes.
        for (integer duration_byte = 0; duration_byte < 256; duration_byte++) begin
            for (integer rate_byte = 0; rate_byte < 256; rate_byte++) begin
                duration_phoneme = 8'(duration_byte);
                rate_inflection = 8'(rate_byte);
                #1;
                expected = (4 - (duration_byte / 64)) *
                           (16 - (rate_byte / 16)) * 256 - 1;
                actual = dut.ssi_durclk_ticks_minus_one();
                if (actual !== 14'(expected)) begin
                    $display("SSI263 DURATION LOOKUP FAIL D=%0d R=%0d got=%0d expected=%0d",
                             duration_byte / 64, rate_byte / 16, actual, expected);
                    $finish;
                end
                if (actual[7:0] !== 8'hFF || expected < 255 || expected > 16383) begin
                    $display("SSI263 DURATION LOOKUP FAIL range or constant low bits");
                    $finish;
                end
                checks++;
            end
        end
        $display("SSI263 DURATION LOOKUP PASS checks=%0d combinations=64", checks);
        $finish;
    end
endmodule
