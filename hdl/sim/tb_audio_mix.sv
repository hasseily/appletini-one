`timescale 1ns / 1ps

module tb_audio_mix;
    logic signed [15:0] a, b, c;
    wire signed [15:0] current_pair, current_mixed;
    int checks = 0;
    int positive_clips = 0;
    int negative_clips = 0;
    int boundary_operands [0:6] = '{-32768, -32767, -1, 0, 1, 32766, 32767};
    int triple_operands [0:10] = '{-32768, -32767, -16384, -2, -1,
                                  0, 1, 2, 16384, 32766, 32767};
    logic [31:0] random_state = 32'h519A36C7;

    audio_mix_current dut (
        .a(a), .b(b), .c(c), .pair(current_pair), .mixed(current_mixed)
    );

    // Independent wide reference: no wrapping occurs before either clamp.
    function automatic logic signed [15:0] reference_add(
        input integer lhs,
        input integer rhs
    );
        integer sum;
        begin
            sum = lhs + rhs;
            if (sum > 32767)
                reference_add = 16'sh7fff;
            else if (sum < -32768)
                reference_add = 16'sh8000;
            else
                reference_add = 16'(sum);
        end
    endfunction

    function automatic logic [31:0] next_random(input logic [31:0] state);
        logic [31:0] value;
        begin
            value = state ^ (state << 13);
            value = value ^ (value >> 17);
            next_random = value ^ (value << 5);
        end
    endfunction

    task automatic check_mix(input integer aa, input integer bb, input integer cc);
        logic signed [15:0] expected_pair;
        logic signed [15:0] expected_mixed;
        begin
            a = 16'(aa);
            b = 16'(bb);
            c = 16'(cc);
            expected_pair = reference_add(aa, bb);
            expected_mixed = reference_add(expected_pair, cc);
            #1;
            if (current_pair !== expected_pair || current_mixed !== expected_mixed)
                $fatal(1, "mix(%0d,%0d,%0d): pair=%0d expected=%0d; mixed=%0d expected=%0d",
                       aa, bb, cc, current_pair, expected_pair,
                       current_mixed, expected_mixed);
            if (aa + bb > 32767) positive_clips++;
            if (aa + bb < -32768) negative_clips++;
            checks++;
        end
    endtask

    initial begin
        // Every 16-bit input, sign boundary, and saturation threshold ±1.
        for (int value = -32768; value <= 32767; value++) begin
            foreach (boundary_operands[index])
                check_mix(value, boundary_operands[index], 0);
            for (int delta = -1; delta <= 1; delta++) begin
                automatic int lower = -32768 - value + delta;
                automatic int upper = 32767 - value + delta;
                if (lower >= -32768 && lower <= 32767)
                    check_mix(value, lower, 0);
                if (upper >= -32768 && upper <= 32767)
                    check_mix(value, upper, 0);
            end
        end

        // Saturation is not associative: cancellation after the first clip
        // must not silently become a single wide, three-input sum.
        foreach (triple_operands[i])
            foreach (triple_operands[j])
                foreach (triple_operands[k])
                    check_mix(triple_operands[i], triple_operands[j], triple_operands[k]);
        check_mix(32767, 32767, -32768);
        if (current_mixed !== -16'sd1)
            $fatal(1, "positive clip cancellation lost the two-stage order");
        check_mix(-32768, -32768, 32767);
        if (current_mixed !== -16'sd1)
            $fatal(1, "negative clip cancellation lost the two-stage order");

        for (int index = 0; index < 65536; index++) begin
            automatic logic signed [15:0] ra, rb, rc;
            random_state = next_random(random_state); ra = random_state[15:0];
            random_state = next_random(random_state); rb = random_state[15:0];
            random_state = next_random(random_state); rc = random_state[15:0];
            check_mix(ra, rb, rc);
        end
        if (positive_clips == 0 || negative_clips == 0)
            $fatal(1, "test did not exercise both saturation directions");
        $display("AUDIO MIX PASS: %0d exact pair/triple cases; actual RTL and wide reference agree", checks);
        $finish;
    end
endmodule
