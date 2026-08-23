`timescale 1ns / 1ps

module tb_vtw_shadow_host_port;
    timeunit 1ns;
    timeprecision 1ps;

    logic clk = 1'b0;
    always #3.75 clk = ~clk;

    logic rstn = 1'b0;
    logic addr_set = 1'b0;
    logic [17:0] addr_value = 18'd0;
    logic byte_write = 1'b0;
    logic [7:0] byte_wdata = 8'd0;
    logic word_write = 1'b0;
    logic [31:0] word_wdata = 32'd0;
    logic word_read = 1'b0;
    logic [17:0] pointer;
    logic [7:0] read_data;
    logic word_ready;
    logic word_busy;
    logic [29:0] word_accept_count;
    logic [31:0] word_read_data;
    logic word_read_ready;
    logic word_read_busy;
    logic [29:0] word_read_count;
    logic sh_en;
    logic [17:0] sh_addr;
    logic sh_we;
    logic [7:0] sh_wdata;
    logic [7:0] sh_rdata = 8'd0;
    logic [7:0] mem [0:2047];
    int fails = 0;

    vtw_shadow_host_port dut (
        .clk(clk), .rstn(rstn),
        .addr_set(addr_set), .addr_value(addr_value),
        .byte_write(byte_write), .byte_wdata(byte_wdata),
        .word_write(word_write), .word_wdata(word_wdata),
        .word_read(word_read),
        .pointer(pointer), .read_data(read_data),
        .word_ready(word_ready), .word_busy(word_busy),
        .word_accept_count(word_accept_count),
        .word_read_data(word_read_data),
        .word_read_ready(word_read_ready),
        .word_read_busy(word_read_busy),
        .word_read_count(word_read_count),
        .sh_en(sh_en), .sh_addr(sh_addr), .sh_we(sh_we),
        .sh_wdata(sh_wdata), .sh_rdata(sh_rdata)
    );

    always_ff @(posedge clk) begin
        if (sh_en) begin
            if (sh_we) begin
                mem[sh_addr] <= sh_wdata;
            end
            sh_rdata <= mem[sh_addr];
        end
    end

    // The dedicated BRAM address copy must remain cycle-exact with the
    // host-visible pointer through loads, scalar traffic, and packed traffic.
    always @(negedge clk) begin
        if (rstn) begin
            check(sh_addr == pointer, "memory address copy follows pointer");
        end
    end

    task automatic check(input bit cond, input string msg);
        if (!cond) begin
            fails++;
            $display("SHADOW HOST FAIL: %s", msg);
        end
    endtask

    task automatic get_word(output logic [31:0] data);
        logic [29:0] count_before;
        wait (word_read_ready);
        count_before = word_read_count;
        @(negedge clk);
        word_read = 1'b1;
        @(negedge clk);
        word_read = 1'b0;
        wait (!word_read_busy && word_read_count == count_before + 30'd1);
        data = word_read_data;
    endtask

    task automatic set_addr(input logic [17:0] addr);
        @(negedge clk);
        addr_value = addr;
        addr_set = 1'b1;
        @(negedge clk);
        addr_set = 1'b0;
    endtask

    task automatic put_byte(input logic [7:0] data);
        @(negedge clk);
        byte_wdata = data;
        byte_write = 1'b1;
        @(negedge clk);
        byte_write = 1'b0;
        wait (!word_busy);
        repeat (4) @(posedge clk);
    endtask

    task automatic put_word(input logic [31:0] data, input int gap);
        @(negedge clk);
        word_wdata = data;
        word_write = 1'b1;
        @(negedge clk);
        word_write = 1'b0;
        repeat (gap) @(posedge clk);
    endtask

    initial begin
        for (int i = 0; i < 2048; i++) mem[i] = 8'd0;
        repeat (8) @(posedge clk);
        rstn = 1'b1;
        repeat (4) @(posedge clk);

        set_addr(18'h00100);
        put_byte(8'hA5);
        check(mem[18'h00100] == 8'hA5, "scalar byte write");
        check(pointer == 18'h00101, "scalar pointer advances");

        set_addr(18'h00200);
        for (int word_index = 0; word_index < 128; word_index++) begin
            logic [7:0] b0 = (word_index * 4 + 0) & 8'hFF;
            logic [7:0] b1 = (word_index * 4 + 1) & 8'hFF;
            logic [7:0] b2 = (word_index * 4 + 2) & 8'hFF;
            logic [7:0] b3 = (word_index * 4 + 3) & 8'hFF;
            put_word({b3, b2, b1, b0}, 6);
        end
        wait (!word_busy);
        repeat (3) @(posedge clk);
        check(word_accept_count == 30'd128, "all 128 packed words accepted");
        check(pointer == 18'h00400, "packed pointer advances 512 bytes");
        for (int i = 0; i < 512; i++) begin
            check(mem[18'h00200 + i] == (i & 8'hFF),
                  $sformatf("packed byte %0d", i));
        end


        /* Packed reads return the same little-endian words and advance the
         * shared pointer without touching memory. */
        set_addr(18'h00200);
        for (int word_index = 0; word_index < 128; word_index++) begin
            logic [31:0] data;
            get_word(data);
            check(data == {8'((word_index * 4 + 3) & 8'hFF),
                           8'((word_index * 4 + 2) & 8'hFF),
                           8'((word_index * 4 + 1) & 8'hFF),
                           8'((word_index * 4 + 0) & 8'hFF)},
                  $sformatf("packed read word %0d", word_index));
        end
        check(pointer == 18'h00400, "packed read pointer advances 512 bytes");

        /* Three back-to-back writes exceed the two-word queue. The third
         * must not alter the accepted count, and a new address must cancel
         * any residue before scalar fallback. */
        set_addr(18'h00500);
        put_word(32'h03020100, 0);
        put_word(32'h07060504, 0);
        put_word(32'h0B0A0908, 0);
        wait (!word_busy);
        check(word_accept_count == 30'd130,
              "full queue rejects rather than silently accepting");
        set_addr(18'h00500);
        put_byte(8'hCC);
        check(mem[18'h00500] == 8'hCC,
              "new address plus scalar write repairs packed fallback");

        if (fails == 0) begin
            $display("VTW SHADOW HOST PASS");
            $finish;
        end
        $fatal(1, "VTW shadow host failures: %0d", fails);
    end
endmodule
