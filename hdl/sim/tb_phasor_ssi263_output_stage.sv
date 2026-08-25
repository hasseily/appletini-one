`timescale 1ns / 1ps

module tb_phasor_ssi263_output_stage;

    logic clk = 1'b0;
    logic rstn = 1'b0;
    logic card_enabled = 1'b1;
    logic signed [15:0] line_audio = 16'sd0;
    logic signed [15:0] card_audio;
    logic clipped;
    integer checks = 0;
    integer failures = 0;

    always #5 clk = ~clk;

    phasor_ssi263_output_stage dut (
        .clk(clk),
        .rstn(rstn),
        .card_enabled(card_enabled),
        .line_audio(line_audio),
        .card_audio(card_audio),
        .clipped(clipped)
    );

    task automatic check(input logic condition, input string message);
        begin
            checks = checks + 1;
            if (condition !== 1'b1) begin
                failures = failures + 1;
                $display("PHASOR SSI263 OUTPUT FAIL: %s", message);
            end
        end
    endtask

    task automatic check_sample(
        input logic signed [15:0] input_sample,
        input logic signed [15:0] expected_sample,
        input logic expected_clipped
    );
        begin
            @(negedge clk);
            line_audio = input_sample;
            @(posedge clk);
            #1;
            check(card_audio == expected_sample,
                  $sformatf("input %0d produced %0d, expected %0d",
                            input_sample, card_audio, expected_sample));
            check(clipped == expected_clipped,
                  $sformatf("input %0d clipped=%0d, expected %0d",
                            input_sample, clipped, expected_clipped));
        end
    endtask

    initial begin
        repeat (3) @(posedge clk);
        #1;
        check(card_audio == 16'sd0 && !clipped,
              "reset did not clear the registered card stage");
        rstn = 1'b1;

        check_sample(16'sd0,     16'sd0,      1'b0);
        check_sample(16'sd1,     16'sd32,     1'b0);
        check_sample(-16'sd1,   -16'sd32,     1'b0);
        check_sample(16'sd1023,  16'sd32736,  1'b0);
        check_sample(16'sd1024,  16'sd32767,  1'b1);
        check_sample(-16'sd1024, -16'sd32768, 1'b0);
        check_sample(-16'sd1025, -16'sd32768, 1'b1);

        @(negedge clk);
        card_enabled = 1'b0;
        line_audio = 16'sd17;
        @(posedge clk);
        #1;
        check(card_audio == 16'sd0 && !clipped,
              "card disable did not mask the stage at its boundary");
        card_enabled = 1'b1;
        #1;
        check(card_audio == 16'sd544 && !clipped,
              "card re-enable did not expose the preserved stage result");

        if (failures == 0)
            $display("PHASOR SSI263 OUTPUT PASS checks=%0d", checks);
        else
            $display("PHASOR SSI263 OUTPUT FAILED checks=%0d failures=%0d",
                     checks, failures);
        $finish;
    end

endmodule
