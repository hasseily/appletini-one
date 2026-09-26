`timescale 1ns / 1ps

module tb_vtw_joystick_bridge;
    logic clk = 1'b0;
    always #5 clk = ~clk;
    logic resetn = 1'b0;
    logic enabled = 1'b0;
    logic ps_wr_en = 1'b0;
    logic [7:0] ps_addr = 8'd0;
    logic [31:0] ps_wdata = 32'd0;
    logic [3:0] ps_wstrb = 4'd0;
    logic [7:0] ps_read_addr = 8'hAC;
    wire [31:0] ps_rdata;
    wire joystick_active;
    wire [2:0] joystick_buttons;
    wire [31:0] joystick_paddles;
    integer checks = 0;

    vtw_joystick_bridge dut (.*);

    task automatic check_state(input logic active, input logic [2:0] buttons,
                               input logic [31:0] paddles, input string label);
        begin
            #1;
            if (joystick_active !== active || joystick_buttons !== buttons ||
                joystick_paddles !== paddles)
                $fatal(1, "%s: got active=%b buttons=%b paddles=%h", label,
                       joystick_active, joystick_buttons, joystick_paddles);
            checks = checks + 1;
        end
    endtask

    task automatic write_reg(input logic [7:0] addr, input logic [31:0] value,
                             input logic [3:0] strobes);
        begin
            @(negedge clk);
            ps_wr_en = 1'b1;
            ps_addr = addr;
            ps_wdata = value;
            ps_wstrb = strobes;
            @(negedge clk);
            ps_wr_en = 1'b0;
            ps_wstrb = 4'd0;
        end
    endtask

    task automatic check_read(input logic [7:0] addr, input logic [31:0] expected);
        begin
            ps_read_addr = addr;
            #1;
            if (ps_rdata !== expected)
                $fatal(1, "register %h expected %h got %h", addr, expected, ps_rdata);
            checks = checks + 1;
        end
    endtask

    initial begin
        repeat (3) @(negedge clk);
        check_state(0, 0, 32'h8080_8080, "reset");
        check_read(8'hAC, 32'h4A00_0000);
        resetn = 1;
        write_reg(8'hAB, 32'h1234_5678, 4'hF);
        write_reg(8'hAC, 32'hF, 4'hF);
        check_state(0, 0, 32'h8080_8080, "disabled writes ignored");
        enabled = 1;
        @(negedge clk);
        check_read(8'hAC, 32'h4A00_0100);
        check_read(8'hAB, 32'h8080_8080);

        write_reg(8'hAB, 32'hFF00_8040, 4'hF);
        check_state(0, 0, 32'h8080_8080, "staging does not connect");
        write_reg(8'hAC, 32'hB, 4'hE);
        check_state(0, 0, 32'h8080_8080, "control needs low byte");
        write_reg(8'hAC, 32'hB, 4'h1);
        check_state(1, 3'b101, 32'hFF00_8040, "atomic first commit");
        check_read(8'hAC, 32'h4A00_010B);

        write_reg(8'hAB, 32'h1234_5678, 4'b0101);
        check_read(8'hAB, 32'hFF34_8078);
        check_state(1, 3'b101, 32'hFF00_8040, "partial stage leaves live tuple");
        write_reg(8'hAC, 32'h5, 4'h0);
        check_state(1, 3'b101, 32'hFF00_8040, "zero strobe ignored");
        write_reg(8'hAC, 32'h5, 4'h1);
        check_state(1, 3'b010, 32'hFF34_8078, "axes and buttons commit together");
        write_reg(8'hAA, 32'd0, 4'hF);
        check_state(1, 3'b010, 32'hFF34_8078, "other register ignored");
        check_read(8'hAA, 32'd0);

        write_reg(8'hAC, 32'hE, 4'h1);
        check_state(0, 0, 32'h8080_8080, "disconnect clears live input");
        check_read(8'hAC, 32'h4A00_0100);
        write_reg(8'hAB, 32'h0080_FFFF, 4'hF);
        write_reg(8'hAC, 32'h1, 4'h1);
        check_state(1, 0, 32'h0080_FFFF, "centered buttons still present");

        // The boundary must mask before the next rising clock, not merely
        // after the sequential state-clearing branch has run.
        @(negedge clk);
        enabled = 0;
        check_state(0, 0, 32'h8080_8080, "immediate mode exit");
        check_read(8'hAC, 32'h4A00_0000);
        repeat (2) @(negedge clk);
        enabled = 1;
        check_state(0, 0, 32'h8080_8080, "mode reentry stays absent");
        check_read(8'hAB, 32'h8080_8080);
        write_reg(8'hAC, 32'h1, 4'h1);
        check_state(1, 0, 32'h8080_8080, "old staged axes cleared on exit");

        @(negedge clk);
        resetn = 0;
        check_state(0, 0, 32'h8080_8080, "immediate reset mask");
        repeat (2) @(negedge clk);
        resetn = 1;
        check_state(0, 0, 32'h8080_8080, "reset requires new commit");
        $display("VTW JOYSTICK BRIDGE PASS: %0d checks", checks);
        $finish;
    end

    initial begin
        #10000;
        $fatal(1, "timeout");
    end
endmodule
