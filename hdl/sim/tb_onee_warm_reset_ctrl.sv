`timescale 1ns / 1ps

module tb_onee_warm_reset_ctrl;
    logic clk = 1'b0;
    logic resetn = 1'b0;
    logic enabled = 1'b0;
    logic request = 1'b0;
    logic native_cycle_tick = 1'b0;
    logic virtual_res_n;
    logic acknowledge;
    logic active;

    always #5 clk = ~clk;

    onee_warm_reset_ctrl #(
        .MIN_NATIVE_CYCLES(4)
    ) dut (
        .clk              (clk),
        .resetn           (resetn),
        .enabled          (enabled),
        .request          (request),
        .native_cycle_tick(native_cycle_tick),
        .virtual_res_n    (virtual_res_n),
        .acknowledge      (acknowledge),
        .active           (active)
    );

    task automatic check_outputs(
        input logic expected_res_n,
        input logic expected_ack,
        input logic expected_active,
        input string label
    );
        begin
            #1;
            if ((virtual_res_n !== expected_res_n) ||
                (acknowledge !== expected_ack) ||
                (active !== expected_active)) begin
                $error("%s: res_n=%b ack=%b active=%b", label,
                       virtual_res_n, acknowledge, active);
                $fatal(1);
            end
        end
    endtask

    task automatic pulse_native_cycle;
        begin
            native_cycle_tick = 1'b1;
            @(posedge clk);
            #1;
            native_cycle_tick = 1'b0;
        end
    endtask

    initial begin
        check_outputs(1'b1, 1'b0, 1'b0, "global reset");
        repeat (2) @(posedge clk);
        resetn = 1'b1;
        enabled = 1'b1;
        @(posedge clk);
        check_outputs(1'b1, 1'b0, 1'b0, "idle");

        request = 1'b1;
        @(posedge clk);
        check_outputs(1'b0, 1'b0, 1'b1, "request starts reset");

        repeat (20) @(posedge clk);
        check_outputs(1'b0, 1'b0, 1'b1, "fabric clocks do not count");

        pulse_native_cycle();
        pulse_native_cycle();
        pulse_native_cycle();
        check_outputs(1'b0, 1'b0, 1'b1, "minimum not reached");
        pulse_native_cycle();
        check_outputs(1'b0, 1'b1, 1'b1, "four native cycles acknowledged");

        // Model the bridge clearing its held request after acknowledge.
        @(negedge clk);
        request = 1'b0;
        @(posedge clk);
        check_outputs(1'b1, 1'b0, 1'b0, "reset released after request clears");

        // Disabling ONE//e masks and clears an in-flight virtual reset.
        @(negedge clk);
        request = 1'b1;
        @(posedge clk);
        check_outputs(1'b0, 1'b0, 1'b1, "second request starts");
        #2 enabled = 1'b0;
        check_outputs(1'b1, 1'b0, 1'b0, "disable masks immediately");
        @(posedge clk);
        request = 1'b0;
        enabled = 1'b1;
        @(posedge clk);
        check_outputs(1'b1, 1'b0, 1'b0, "disable cleared state");

        $display("PASS: ONE//e warm reset controller");
        $finish;
    end

endmodule
