`timescale 1ns / 1ps

module tb_disk2_ddr_bridge_shared;
    logic clk = 0;
    always #4 clk = !clk;
    logic rstn = 0;
    logic disk_valid = 0;
    logic pct_valid = 0;
    wire disk_ready, disk_done, pct_ready, pct_done, pct_error;
    wire [63:0] disk_data, pct_data;
    Axi3_read_if #(.ADDR_WIDTH(32), .DATA_WIDTH(64)) rd();
    Axi3_write_if #(.ADDR_WIDTH(32), .DATA_WIDTH(64)) wr();

    disk2_ddr_bridge dut (
        .clk(clk), .rstn(rstn),
        .mc_line_addr(21'h2), .mc_rw(1'b1), .mc_wdata(64'd0), .mc_wstrb(8'd0),
        .mc_valid(disk_valid), .mc_ready(disk_ready),
        .mc_rdata(disk_data), .mc_rvalid(disk_done),
        .pct_valid(pct_valid), .pct_write(1'b1), .pct_addr(20'h23458),
        .pct_wdata(64'h8899aabbccddeeff), .pct_wstrb(8'h08),
        .pct_ready(pct_ready), .pct_done(pct_done), .pct_rdata(pct_data),
        .pct_error(pct_error), .axi_read(rd), .axi_write(wr)
    );

    initial begin
        rd.arready = 1;
        rd.rvalid = 0;
        rd.rlast = 1;
        rd.rresp = 0;
        rd.rdata = 64'h123456789abcdef0;
        wr.awready = 0;
        wr.wready = 0;
        wr.bvalid = 0;
        wr.bresp = 2'b10;

        repeat (3) @(negedge clk);
        rstn = 1;
        disk_valid = 1;
        pct_valid = 1;
        @(negedge clk);
        assert (disk_ready && !pct_ready && rd.arvalid && rd.araddr == 32'h3d800010)
            else $fatal(1, "Disk II did not get the first request");
        disk_valid = 0;
        wait (rd.rready);
        @(negedge clk);
        assert (!pct_ready && !wr.awvalid) else $fatal(1, "DDR read lost ownership");
        rd.rvalid = 1;
        @(negedge clk);
        assert (disk_done && disk_data == rd.rdata && !pct_done)
            else $fatal(1, "Disk II did not get its reply");
        rd.rvalid = 0;
        @(negedge clk);
        assert (pct_ready && wr.awvalid && wr.awaddr == 32'h3f923458)
            else $fatal(1, "Transporter write address is incorrect");
        pct_valid = 0;
        disk_valid = 1;
        wr.awready = 1;
        @(negedge clk);
        assert (wr.wvalid && wr.wstrb == 8'h08 && wr.wlast &&
                wr.wdata == 64'h8899aabbccddeeff && !disk_ready)
            else $fatal(1, "Transporter write data or arbitration is incorrect");
        wr.wready = 1;
        wait (wr.bready);
        @(negedge clk);
        wr.bvalid = 1;
        @(negedge clk);
        assert (pct_done && pct_error && !disk_done)
            else $fatal(1, "Transporter did not get its write error");
        wr.bvalid = 0;
        @(negedge clk);
        assert (disk_ready && !pct_done) else $fatal(1, "Disk II did not resume");
        $display("DISK II AND TRANSPORTER DDR PASS");
        $finish;
    end

    initial begin
        #10000;
        $fatal(1, "DDR bridge timeout");
    end
endmodule
