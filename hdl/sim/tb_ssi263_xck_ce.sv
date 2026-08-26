`timescale 1ns / 1ps

module tb_ssi263_xck_ce;

    logic clk = 1'b0;
    logic rstn = 1'b0;
    logic q3_raw = 1'b0;
    logic xck_ce;

    integer raw_rises = 0;
    integer enable_pulses = 0;
    integer failures = 0;
    logic xck_ce_q = 1'b0;

    always #5 clk = ~clk;

    ssi263_xck_ce dut (
        .clk(clk),
        .rstn(rstn),
        .q3_raw(q3_raw),
        .xck_ce(xck_ce)
    );

    always @(posedge q3_raw) begin
        if (rstn)
            raw_rises = raw_rises + 1;
    end

    always @(posedge clk) begin
        if (!rstn) begin
            xck_ce_q = 1'b0;
        end else begin
            if (xck_ce) begin
                enable_pulses = enable_pulses + 1;
                if (xck_ce_q) begin
                    failures = failures + 1;
                    $display("SSI263 XCK FAIL: enable lasted over one clock");
                end
                if (!dut.q3_sync2_q || dut.q3_sync2_d_q) begin
                    failures = failures + 1;
                    $display("SSI263 XCK FAIL: enable was not a synchronized rise");
                end
            end
            xck_ce_q = xck_ce;
        end
    end

    task automatic drive_q3_pulse(
        input time high_time,
        input time low_time
    );
        begin
            q3_raw = 1'b1;
            #(high_time);
            q3_raw = 1'b0;
            #(low_time);
        end
    endtask

    initial begin
        repeat (4) @(posedge clk);
        @(negedge clk);
        rstn = 1'b1;

        #3;
        drive_q3_pulse(37ns, 43ns);
        drive_q3_pulse(41ns, 36ns);
        drive_q3_pulse(33ns, 52ns);
        drive_q3_pulse(48ns, 31ns);
        drive_q3_pulse(35ns, 47ns);
        drive_q3_pulse(54ns, 38ns);
        repeat (6) @(posedge clk);
        #1;

        if (raw_rises != 6 || enable_pulses != raw_rises) begin
            failures = failures + 1;
            $display("SSI263 XCK FAIL: raw=%0d enables=%0d expected=6",
                     raw_rises, enable_pulses);
        end

        q3_raw = 1'b1;
        #200ns;
        q3_raw = 1'b0;
        #80ns;
        if (raw_rises != 7 || enable_pulses != raw_rises) begin
            failures = failures + 1;
            $display("SSI263 XCK FAIL: held level retriggered raw=%0d enables=%0d",
                     raw_rises, enable_pulses);
        end

        @(negedge clk);
        rstn = 1'b0;
        q3_raw = 1'b0;
        repeat (3) @(posedge clk);
        #1;
        if (xck_ce !== 1'b0 || dut.q3_sync1_q !== 1'b0 ||
            dut.q3_sync2_q !== 1'b0 || dut.q3_sync2_d_q !== 1'b0) begin
            failures = failures + 1;
            $display("SSI263 XCK FAIL: reset did not clear synchronizer");
        end

        if (failures == 0) begin
            $display("SSI263 XCK CE PASS raw_rises=%0d enables=%0d",
                     raw_rises, enable_pulses);
        end else begin
            $fatal(1, "SSI263 XCK CE FAIL count=%0d", failures);
        end
        $finish;
    end

endmodule
