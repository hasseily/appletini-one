`timescale 1ns / 1ps

// Checks the transitioned-inflection glide against the closed-form rate:
// one count of I[10:3] per 128*(8-slope)*(16-RATE) effective XCK. The bench
// measures XCK between observed pitch changes; it does not copy the RTL
// dividers.
module tb_ssi263_native_pitch;
    logic clk = 0, rstn = 0, write_strobe = 0, xck_ce = 0;
    logic [2:0] write_reg = 0;
    logic [7:0] write_data = 0;
    logic [11:0] for_tick, active;
    logic [1:0] func;
    int checks = 0;
    longint ticks = 0;

    ssi263_native_pitch dut (
        .clk(clk), .rstn(rstn), .warm_reset(1'b0), .mode_latch(1'b0),
        .write_strobe(write_strobe), .write_reg(write_reg), .write_data(write_data),
        .xck_ce(xck_ce), .inflection_for_tick(for_tick),
        .active_inflection(active), .current_function(func)
    );
    always #5 clk = !clk;

    task automatic check(input logic ok, input string what);
        checks++;
        if (!ok) begin
            $display("FAIL: %s at tick %0d", what, ticks);
            $fatal(1);
        end
    endtask
    task automatic reset();
        rstn = 0; write_strobe = 0; xck_ce = 0;
        repeat (2) @(posedge clk);
        #1 rstn = 1; ticks = 0;
    endtask
    task automatic wr(input int r, input int value);
        write_strobe = 1; write_reg = r[2:0]; write_data = value[7:0]; xck_ce = 0;
        @(posedge clk); #1 write_strobe = 0;
    endtask
    task automatic mode(input int f);
        wr(3, 8'hdc); wr(0, f << 6); wr(3, 8'h5c);
    endtask
    task automatic tick();
        xck_ce = 1; @(posedge clk); #1 xck_ce = 0; ticks++;
    endtask
    // Run until I[10:3] changes; return the XCK count used, or -1.
    task automatic until_change(input longint limit, output longint used);
        logic [7:0] start_count;
        start_count = active[10:3];
        used = -1;
        for (longint n = 1; n <= limit; n++) begin
            tick();
            if (active[10:3] != start_count) begin used = n; break; end
        end
    endtask

    initial begin
        longint used, interval;
        logic [7:0] was;
        for (int rate = 0; rate < 16; rate++)
            for (int slope = 0; slope < 8; slope++)
                for (int up = 0; up < 2; up++) begin
                    automatic int from = up ? 4 : 27;
                    automatic int to = up ? 27 : 4;
                    interval = 128 * (8 - slope) * (16 - rate);
                    reset();
                    wr(1, (from << 3) | slope);
                    wr(2, (rate << 4) | 4'h5);
                    mode(3);
                    check(func == 3, "function 3 latched");
                    check(active == {1'b0, from[4:0], 3'b000, 3'b101}, "first phone adopts its target");
                    // Let both dividers reload from RATE and the slope bits.
                    repeat (128 * 16 * 8 * 2) tick();
                    check(active[10:3] == {from[4:0], 3'b000}, "holds at target");
                    wr(1, (to << 3) | slope);
                    was = active[10:3];
                    until_change(interval, used);
                    check(used > 0, "first count arrives within one interval");
                    repeat (3) begin
                        check(active[10:3] == (up ? was + 8'd1 : was - 8'd1), "one count toward target");
                        was = active[10:3];
                        until_change(interval + 1, used);
                        check(used == interval, "count interval is 128*(8-slope)*(16-RATE) XCK");
                    end
                    // I11 and I[2:0] act at once, without XCK.
                    was = active[10:3];
                    wr(2, (rate << 4) | 4'hA);
                    check(active == {1'b1, was, 3'b010}, "I11 and I[2:0] immediate");
                    // No XCK, no movement.
                    repeat (50) @(posedge clk);
                    check(active[10:3] == was, "holds without XCK");
                end

        // Arrival and hold at the fastest setting: 16 counts of I[10:3].
        reset();
        wr(1, (3 << 3) | 7); wr(2, 8'hF0); mode(3);
        repeat (128 * 16 * 8 * 2) tick();
        wr(1, (5 << 3) | 7);
        repeat (128 * 16) tick();
        check(active[10:3] == 8'h28, "reaches target after 16 counts");
        repeat (128 * 40) tick();
        check(active[10:3] == 8'h28, "stays at target");
        // A later phone keeps the running count.
        wr(1, (9 << 3) | 7); wr(0, 8'hC1);
        check(active[10:3] == 8'h28, "phone write does not reseed");

        // Other functions use the register word directly.
        reset();
        wr(1, 8'hA6); wr(2, 8'h8B); mode(1);
        check(func == 1 && active == 12'hD33 && for_tick == 12'hD33, "immediate inflection");
        repeat (128 * 16 * 8 * 3) tick();
        check(active == 12'hD33, "immediate inflection holds");

        $display("SSI263 NATIVE PITCH PASS: %0d checks", checks);
        $finish;
    end
endmodule
