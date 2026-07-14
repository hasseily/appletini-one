`timescale 1ns / 1ps

module tb_uthernet2_card;
    logic clk = 1'b0;
    logic rstn = 1'b0;
    always #5 clk = ~clk;

    globals::AppleBus_read ab_read;
    globals::SoftSwitchState sss;
    globals::AppleBus_write ab_write;
    logic [7:0] eth_d_o;
    logic eth_d_oe;
    logic [1:0] eth_a;
    logic eth_rd_n;
    logic eth_wr_n;
    logic eth_cs_n;
    logic eth_rst_n;
    logic host_req = 1'b0;
    logic host_write = 1'b0;
    logic [15:0] host_addr = 16'h0080;
    logic [7:0] host_wdata = 8'h00;
    logic host_ready;
    logic host_done;
    logic host_error;
    logic [7:0] host_rdata;

    uthernet2_card #(
        .CLK_HZ(1_000_000),
        .RESET_HOLD_US(1),
        .RESET_READY_WAIT_US(1),
        .READ_STROBE_CYCLES(2),
        .WRITE_STROBE_CYCLES(2)
    ) dut (
        .clk(clk),
        .rstn(rstn),
        .ab_read(ab_read),
        .sss(sss),
        .slot_assign(3'h1),
        .ab_write(ab_write),
        .eth_d_i(8'hA5),
        .eth_d_o(eth_d_o),
        .eth_d_oe(eth_d_oe),
        .eth_a(eth_a),
        .eth_rd_n(eth_rd_n),
        .eth_wr_n(eth_wr_n),
        .eth_cs_n(eth_cs_n),
        .eth_rst_n(eth_rst_n),
        .eth_int_n(1'b1),
        .host_req(host_req),
        .host_write(host_write),
        .host_addr(host_addr),
        .host_wdata(host_wdata),
        .host_ready(host_ready),
        .host_done(host_done),
        .host_error(host_error),
        .host_rdata(host_rdata)
    );

    task automatic wait_ready;
        for (int i = 0; i < 300; ++i) begin
            @(posedge clk);
            if (host_ready) return;
        end
        $fatal(1, "host_ready timeout");
    endtask

    task automatic wait_host_done(input logic expected_error);
        for (int i = 0; i < 500; ++i) begin
            @(posedge clk);
            if (host_done) begin
                if (host_error != expected_error) begin
                    $fatal(1, "host_error=%0b expected=%0b", host_error,
                           expected_error);
                end
                return;
            end
        end
        $fatal(1, "host_done timeout");
    endtask

    task automatic pulse_host_read;
        @(negedge clk);
        host_req = 1'b1;
        host_write = 1'b0;
        @(negedge clk);
        host_req = 1'b0;
    endtask

    task automatic pulse_apple_read(input logic [1:0] reg_sel);
        @(negedge clk);
        ab_read.addr = 16'hC090 | reg_sel;
        ab_read.rw = 1'b1;
        ab_read.addr_en = 1'b1;
        @(negedge clk);
        ab_read.addr_en = 1'b0;
    endtask

    task automatic start_apple_write(input logic [1:0] reg_sel);
        @(negedge clk);
        ab_read.addr = 16'hC090 | reg_sel;
        ab_read.rw = 1'b0;
        ab_read.addr_en = 1'b1;
        @(negedge clk);
        ab_read.addr_en = 1'b0;
    endtask

    task automatic finish_apple_cycle(input logic [7:0] data);
        @(negedge clk);
        ab_read.data = data;
        ab_read.data_en = 1'b1;
        @(negedge clk);
        ab_read.data_en = 1'b0;
    endtask

    initial begin
        ab_read = '0;
        sss = '0;
        ab_read.res = 1'b1;
        ab_read.cycle_valid = 1'b1;

        repeat (3) @(posedge clk);
        rstn = 1'b1;
        wait_ready();

        // A simultaneous Apple read wins; the one-cycle host request is queued.
        @(negedge clk);
        ab_read.addr = 16'hC090;
        ab_read.rw = 1'b1;
        ab_read.addr_en = 1'b1;
        host_req = 1'b1;
        @(negedge clk);
        ab_read.addr_en = 1'b0;
        host_req = 1'b0;
        finish_apple_cycle(8'h00);
        wait_host_done(1'b0);
        if (host_rdata != 8'hA5) $fatal(1, "queued host read data mismatch");
        wait_ready();

        // Point the Apple data port at a physical register.
        start_apple_write(2'b01);
        finish_apple_cycle(8'h00);
        wait_ready();
        start_apple_write(2'b10);
        finish_apple_cycle(8'h10);
        wait_ready();

        // A physical Apple read arriving during a host command aborts the host
        // and still completes its address reload inside one Apple-cycle budget.
        pulse_host_read();
        wait (!eth_cs_n);
        pulse_apple_read(2'b11);
        wait_host_done(1'b1);
        for (int i = 0;
             i < 110 && !(ab_write.wr_data_en && ab_write.wr_data == 8'hA5);
             ++i) @(posedge clk);
        if (!ab_write.wr_data_en) $fatal(1, "colliding Apple read was lost");
        if (ab_write.wr_data != 8'hA5) $fatal(1, "colliding Apple read data mismatch");
        finish_apple_cycle(8'h00);
        wait_ready();

        // A write address reserves the bus until data_en, so host work cannot
        // start in the middle of the Apple cycle.
        @(negedge clk);
        ab_read.addr = 16'hC090;
        ab_read.rw = 1'b0;
        ab_read.addr_en = 1'b1;
        host_req = 1'b1;
        @(negedge clk);
        ab_read.addr_en = 1'b0;
        host_req = 1'b0;
        repeat (8) begin
            @(posedge clk);
            if (!eth_cs_n || host_done) $fatal(1, "host started inside Apple write");
        end
        finish_apple_cycle(8'h02);
        wait_host_done(1'b0);
        wait_ready();

        // A write reservation also cancels an already-running host command.
        pulse_host_read();
        wait (!eth_cs_n);
        start_apple_write(2'b00);
        wait_host_done(1'b1);
        if (host_ready) $fatal(1, "host became ready before Apple write data");
        finish_apple_cycle(8'h00);
        wait_ready();

        $display("UTHERNET2 ARBITRATION PASS");
        $finish;
    end
endmodule
