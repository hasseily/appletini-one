`timescale 1ns / 1ps

module tb_linear_text_overlay;
    logic clk = 1'b0;
    logic rstn = 1'b0;
    always #3.75 clk = ~clk;

    globals::AppleBus_read ab_read;
    globals::SoftSwitchState sss;
    globals::AppleBus_write ab_write;
    logic overlay_io_read_valid;
    logic [7:0] overlay_io_read_data;
    logic capture_drop = 1'b0;
    logic ps_wr_en = 1'b0;
    logic [7:0] ps_addr = 8'd0;
    logic [31:0] ps_wdata = 32'd0;
    logic [7:0] ps_read_addr = 8'd0;
    logic [31:0] ps_rdata;
    logic devsel_enabled;
    logic capture_armed;
    logic capture_bank_aux;
    logic [15:0] capture_base;
    logic [15:0] capture_limit;
    logic canvas_shr_active = 1'b0;

    linear_text_overlay_card dut (
        .clk(clk), .rstn(rstn), .ab_read(ab_read), .sss(sss),
        .slot_assign(3'd7), .capture_drop_sticky(capture_drop),
        .canvas_shr_active(canvas_shr_active),
        .ps_wr_en(ps_wr_en), .ps_addr(ps_addr), .ps_wdata(ps_wdata),
        .ps_read_addr(ps_read_addr), .ps_rdata(ps_rdata),
        .io_read_valid(overlay_io_read_valid),
        .io_read_data(overlay_io_read_data),
        .devsel_enabled(devsel_enabled),
        .capture_armed(capture_armed),
        .capture_bank_aux(capture_bank_aux),
        .capture_base(capture_base), .capture_limit(capture_limit)
    );

    always_ff @(posedge clk) begin
        if (!rstn) begin
            ab_write <= '0;
        end else begin
            if (overlay_io_read_valid) begin
                ab_write.wr_data <= overlay_io_read_data;
                ab_write.wr_data_en <= 1'b1;
            end else if (ab_read.data_en) begin
                ab_write.wr_data <= 8'h00;
                ab_write.wr_data_en <= 1'b0;
            end
        end
    end

    logic [7:0] rd;
    task automatic bus_read(input logic [15:0] addr);
        @(negedge clk);
        ab_read.addr = addr;
        ab_read.rw = 1'b1;
        ab_read.serve_en = 1'b1;
        @(negedge clk);
        ab_read.serve_en = 1'b0;
        @(negedge clk);
        rd = ab_write.wr_data;
        if (!ab_write.wr_data_en)
            $fatal(1, "no overlay response at %04x", addr);
        ab_read.data_en = 1'b1;
        @(negedge clk);
        ab_read.data_en = 1'b0;
    endtask

    task automatic bus_write(input logic [15:0] addr,
                             input logic [7:0] data);
        @(negedge clk);
        ab_read.addr = addr;
        ab_read.rw = 1'b0;
        ab_read.serve_en = 1'b1;
        @(negedge clk);
        ab_read.serve_en = 1'b0;
        ab_read.data = data;
        ab_read.data_en = 1'b1;
        @(negedge clk);
        ab_read.data_en = 1'b0;
        ab_read.rw = 1'b1;
    endtask

    task automatic write_indexed(input logic [7:0] index,
                                 input logic [7:0] data);
        bus_write(16'hC0F0, index);
        bus_write(16'hC0F1, data);
    endtask

    task automatic read_indexed(input logic [7:0] index);
        bus_write(16'hC0F0, index);
        bus_read(16'hC0F1);
    endtask

    task automatic ps_write(input logic [7:0] addr,
                            input logic [31:0] data);
        @(negedge clk);
        ps_addr = addr;
        ps_wdata = data;
        ps_wr_en = 1'b1;
        @(negedge clk);
        ps_wr_en = 1'b0;
    endtask

    task automatic expect_status(input logic [7:0] mask,
                                 input logic [7:0] value,
                                 input string what);
        bus_read(16'hC0F4);
        if ((rd & mask) !== value)
            $fatal(1, "%s: status %02x mask %02x expected %02x",
                   what, rd, mask, value);
    endtask

    initial begin
        ab_read = '0;
        sss = '0;
        ab_read.res = 1'b1;
        ab_read.rw = 1'b1;
        ab_read.cycle_valid = 1'b1;
        repeat (4) @(posedge clk);
        rstn = 1'b1;
        repeat (3) @(posedge clk);

        bus_read(16'hC0F8); if (rd !== "L") $fatal(1, "bad LINTXT L");
        bus_read(16'hC0FD); if (rd !== "T") $fatal(1, "bad LINTXT T");
        bus_read(16'hC0FE); if (rd !== 8'h4C) $fatal(1, "bad magic");
        bus_read(16'hC0FF); if (rd !== 8'h10) $fatal(1, "bad version");
        expect_status(8'hFF, 8'h00, "reset");

        read_indexed(8'h10); if (rd !== 8'h60) $fatal(1, "legacy width lo");
        read_indexed(8'h11); if (rd !== 8'h04) $fatal(1, "legacy width hi");
        read_indexed(8'h12); if (rd !== 8'h00) $fatal(1, "legacy height lo");
        read_indexed(8'h13); if (rd !== 8'h03) $fatal(1, "legacy height hi");
        canvas_shr_active = 1'b1;
        read_indexed(8'h10); if (rd !== 8'h00) $fatal(1, "SHR width lo");
        read_indexed(8'h11); if (rd !== 8'h05) $fatal(1, "SHR width hi");
        read_indexed(8'h12); if (rd !== 8'h20) $fatal(1, "SHR height lo");
        read_indexed(8'h13); if (rd !== 8'h03) $fatal(1, "SHR height hi");
        canvas_shr_active = 1'b0;

        // Default base is invalid.
        bus_write(16'hC0F3, 8'h01);
        expect_status(8'h20, 8'h20, "invalid ARM");

        write_indexed(8'h00, 8'h00);
        write_indexed(8'h01, 8'h60);
        write_indexed(8'h02, 8'h0C);
        write_indexed(8'h03, 8'd80);
        write_indexed(8'h04, 8'd24);
        write_indexed(8'h07, 8'd64);
        write_indexed(8'h09, 8'h22);
        bus_write(16'hC0F3, 8'h01);
        expect_status(8'hC2, 8'h80, "valid ARM busy");
        ps_read_addr = 8'h20;
        repeat (10) @(posedge clk);
        if ((ps_rdata & 32'h00000001) == 32'h00000000)
            $fatal(1, "ARM request missing");
        ps_write(8'h28, 32'h00000003);
        expect_status(8'hE2, 8'h02, "ARM ack");
        if (!capture_armed || capture_bank_aux ||
            capture_base !== 16'h6000 || capture_limit !== 16'h6F00)
            $fatal(1, "capture window wrong: %04x-%04x", capture_base,
                   capture_limit);

        bus_write(16'hC0F3, 8'h02);
        expect_status(8'h13, 8'h12, "SHOW pending");
        bus_write(16'hC0F3, 8'h01);
        expect_status(8'h93, 8'h12, "ARM ignored while SHOW pending");
        ps_write(8'h28, 32'h00000004);
        ps_write(8'h28, 32'h00000008);
        expect_status(8'h13, 8'h03, "SHOW complete");

        // ROWS is limited to 127. A failed ARM keeps the shown and armed
        // buffer intact.
        write_indexed(8'h04, 8'd128);
        bus_write(16'hC0F3, 8'h01);
        expect_status(8'hA3, 8'h23, "invalid row count");
        if (!capture_armed || capture_base !== 16'h6000 ||
            capture_limit !== 16'h6F00)
            $fatal(1, "static ARM failure changed capture window");
        write_indexed(8'h04, 8'd24);

        // This range passes the first value checks, then fails the end
        // address check after the serial multiply.
        write_indexed(8'h00, 8'hF0);
        write_indexed(8'h01, 8'hBF);
        bus_write(16'hC0F3, 8'h01);
        expect_status(8'h83, 8'h83, "range ARM busy");
        repeat (10) @(posedge clk);
        expect_status(8'hA3, 8'h23, "invalid range");
        if (!capture_armed || capture_base !== 16'h6000 ||
            capture_limit !== 16'h6F00)
            $fatal(1, "range ARM failure changed capture window");
        write_indexed(8'h00, 8'h00);
        write_indexed(8'h01, 8'h60);
        bus_write(16'hC0F3, 8'h01);
        expect_status(8'hC3, 8'h83, "replacement ARM busy");
        repeat (10) @(posedge clk);
        ps_write(8'h28, 32'h00000001);
        expect_status(8'hE3, 8'h03, "replacement ARM ack");

        bus_write(16'hC0F3, 8'h03);
        ps_write(8'h28, 32'h00000008);
        expect_status(8'h03, 8'h02, "HIDE keeps armed");

        bus_write(16'hC0F3, 8'h02);
        ps_write(8'h28, 32'h00000004);
        ps_write(8'h28, 32'h00000008);
        capture_drop = 1'b1;
        repeat (2) @(posedge clk);
        expect_status(8'hD3, 8'h51, "drop makes stale and pending");
        ps_write(8'h28, 32'h00000008);
        capture_drop = 1'b0;
        expect_status(8'hC3, 8'h40, "stale hide complete");

        bus_write(16'hC0F3, 8'h01);
        expect_status(8'hC2, 8'hC0, "re-ARM validating");
        repeat (10) @(posedge clk);
        ps_write(8'h28, 32'h00000001);
        expect_status(8'hC2, 8'h02, "re-ARM ack");

        $display("LINEAR TEXT OVERLAY PASS");
        $finish;
    end
endmodule
