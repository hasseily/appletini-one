`timescale 1ns / 1ps

module tb_vtw_shadow_banks;
    timeunit 1ns;
    timeprecision 1ps;

    logic clk = 1'b0;
    always #3.75 clk = ~clk;

    logic        a_en = 1'b0;
    logic [17:0] a_addr = '0;
    logic        a_we = 1'b0;
    logic [7:0]  a_wdata = '0;
    logic [7:0]  a_rdata;
    logic        b_en = 1'b0;
    logic [17:0] b_addr = '0;
    logic        b_we = 1'b0;
    logic [7:0]  b_wdata = '0;
    logic [7:0]  b_rdata;
    int fails = 0;

    vtw_shadow dut (
        .clk(clk),
        .a_en(a_en), .a_addr(a_addr), .a_we(a_we),
        .a_wdata(a_wdata), .a_rdata(a_rdata),
        .b_en(b_en), .b_addr(b_addr), .b_we(b_we),
        .b_wdata(b_wdata), .b_rdata(b_rdata)
    );

    task automatic check(input bit cond, input string message);
        if (!cond) begin
            fails++;
            $display("VTW SHADOW BANKS FAIL: %s", message);
        end
    endtask

    task automatic b_write(input logic [17:0] addr,
                           input logic [7:0] data);
        @(negedge clk);
        b_en = 1'b1;
        b_we = 1'b1;
        b_addr = addr;
        b_wdata = data;
        @(posedge clk);
        #1;
        @(negedge clk);
        b_en = 1'b0;
        b_we = 1'b0;
    endtask

    task automatic a_read_check(input logic [17:0] addr,
                                input logic [7:0] expected,
                                input string message);
        @(negedge clk);
        a_en = 1'b1;
        a_we = 1'b0;
        a_addr = addr;
        @(posedge clk);
        #1;
        check(a_rdata == expected, message);
        @(negedge clk);
        a_en = 1'b0;
    endtask

    task automatic b_read_check(input logic [17:0] addr,
                                input logic [7:0] expected,
                                input string message);
        @(negedge clk);
        b_en = 1'b1;
        b_we = 1'b0;
        b_addr = addr;
        @(posedge clk);
        #1;
        check(b_rdata == expected, message);
        @(negedge clk);
        b_en = 1'b0;
    endtask

    task automatic write_b_read_a(input logic [17:0] addr,
                                  input logic [7:0] data,
                                  input string message);
        b_write(addr, data);
        a_read_check(addr, data, message);
    endtask

    initial begin
        repeat (2) @(posedge clk);

        write_b_read_a(18'h07FFF, 8'h17, "main low-bank last byte");
        write_b_read_a(18'h08000, 8'h28, "main high-bank first byte");
        write_b_read_a(18'h0FFFF, 8'h3F, "main high-bank last byte");
        write_b_read_a(18'h17FFF, 8'h47, "aux low-bank last byte");
        write_b_read_a(18'h18000, 8'h58, "aux high-bank first byte");
        write_b_read_a(18'h1FFFF, 8'h6F, "aux high-bank last byte");

        // Preserve the inferred BRAM read-first contract during a write.
        b_write(18'h08000, 8'hA5);
        @(negedge clk);
        b_en = 1'b1;
        b_we = 1'b1;
        b_addr = 18'h08000;
        b_wdata = 8'h5A;
        @(posedge clk);
        #1;
        check(b_rdata == 8'hA5, "high-bank write must return old data");
        @(negedge clk);
        b_en = 1'b0;
        b_we = 1'b0;
        b_read_check(18'h08000, 8'h5A,
                     "high-bank write must commit new data");

        // Port A must remain unable to alter the ROM region.
        b_write(18'h20000, 8'h9A);
        @(negedge clk);
        a_en = 1'b1;
        a_we = 1'b1;
        a_addr = 18'h20000;
        a_wdata = 8'h55;
        @(posedge clk);
        #1;
        check(a_rdata == 8'h9A, "core ROM write must read old ROM data");
        @(negedge clk);
        a_en = 1'b0;
        a_we = 1'b0;
        b_read_check(18'h20000, 8'h9A,
                     "core port must not change ROM data");

        if (fails == 0)
            $display("VTW SHADOW BANKS PASS");
        else
            $fatal(1, "VTW SHADOW BANKS FAIL count=%0d", fails);
        $finish;
    end

    initial begin
        #20us;
        $fatal(1, "VTW SHADOW BANKS FAIL: timeout");
    end
endmodule
