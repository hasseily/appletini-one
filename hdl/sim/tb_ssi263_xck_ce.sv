`timescale 1ns / 1ps

module tb_ssi263_xck_ce;

    logic clk = 1'b0;
    logic rstn = 1'b0;
    logic xck_ce;
    logic default_xck_ce;

    int cycle_count = 0;
    int tick_count = 0;
    int last_tick_cycle = 0;
    int short_intervals = 0;
    int long_intervals = 0;
    int default_tick_count = 0;
    int default_last_tick_cycle = 0;
    int default_short_intervals = 0;
    int default_long_intervals = 0;

    always #5 clk = ~clk;

    // A small exact ratio makes an exhaustive spacing and count check quick.
    ssi263_xck_ce #(
        .FABRIC_HZ(10),
        .XCK_NUMERATOR_HZ(3),
        .XCK_DENOMINATOR(1)
    ) dut (
        .clk(clk),
        .rstn(rstn),
        .xck_ce(xck_ce)
    );

    // Check the actual Phasor Q3 default as well as the small exact ratio.
    ssi263_xck_ce default_dut (
        .clk(clk),
        .rstn(rstn),
        .xck_ce(default_xck_ce)
    );

    always @(posedge clk) begin
        if (rstn) begin
            cycle_count = cycle_count + 1;
            if (xck_ce) begin
                tick_count = tick_count + 1;
                if (last_tick_cycle != 0) begin
                    case (cycle_count - last_tick_cycle)
                        3: short_intervals = short_intervals + 1;
                        4: long_intervals = long_intervals + 1;
                        default: $fatal(1, "bad XCK interval: %0d",
                                        cycle_count - last_tick_cycle);
                    endcase
                end
                last_tick_cycle = cycle_count;
            end
            if (default_xck_ce) begin
                default_tick_count = default_tick_count + 1;
                if (default_last_tick_cycle != 0) begin
                    case (cycle_count - default_last_tick_cycle)
                        65: default_short_intervals =
                                default_short_intervals + 1;
                        66: default_long_intervals =
                                default_long_intervals + 1;
                        default: $fatal(1, "bad default XCK interval: %0d",
                                        cycle_count - default_last_tick_cycle);
                    endcase
                end
                default_last_tick_cycle = cycle_count;
            end
        end
    end

    initial begin
        repeat (3) @(posedge clk);
        rstn = 1'b1;

        repeat (1000) @(posedge clk);
        #1;

        if (tick_count != 300) begin
            $fatal(1, "tick count was %0d, expected 300", tick_count);
        end
        if (short_intervals == 0 || long_intervals == 0) begin
            $fatal(1, "clock enable did not use both 3- and 4-cycle spacing");
        end

        repeat (99000) @(posedge clk);
        #1;

        // floor(100000 * 14318180 / (133333344 * 7)) = 1534.
        if (default_tick_count != 1534) begin
            $fatal(1, "default tick count was %0d, expected 1534",
                    default_tick_count);
        end
        if (default_short_intervals == 0 || default_long_intervals == 0) begin
            $fatal(1, "default clock did not use both 65- and 66-cycle spacing");
        end

        $display("SSI263 XCK CE PASS exact_ticks=%0d default_ticks=%0d",
                 tick_count, default_tick_count);
        $finish;
    end

endmodule
