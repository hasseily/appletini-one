`timescale 1ns / 1ps

// Completion must survive idle address changes and reads to other GP0 clients.
// Use the real wrapper: its shared read address advances even without a read.
module tb_ps_dma_completion;
    timeunit 1ns;
    timeprecision 1ps;
    logic clk = 1'b0;
    always #3.75ns clk = ~clk;

    logic        rstn = 1'b0;
    logic        s_awvalid = 1'b0;
    wire         s_awready;
    logic [11:0] s_awid = 12'd0;
    logic [31:0] s_awaddr = 32'd0;
    logic [3:0]  s_awlen = 4'd0;
    logic [2:0]  s_awsize = 3'd2;
    logic [1:0]  s_awburst = 2'b01;
    logic        s_awlock = 1'b0;
    logic [3:0]  s_awcache = 4'd0;
    logic [2:0]  s_awprot = 3'd0;
    logic [3:0]  s_awqos = 4'd0;

    logic        s_wvalid = 1'b0;
    wire         s_wready;
    logic [31:0] s_wdata = 32'd0;
    logic [3:0]  s_wstrb = 4'd0;
    logic        s_wlast = 1'b0;

    wire         s_bvalid;
    logic        s_bready = 1'b0;
    wire [11:0]  s_bid;
    wire [1:0]   s_bresp;

    logic        s_arvalid = 1'b0;
    wire         s_arready;
    logic [11:0] s_arid = 12'd0;
    logic [31:0] s_araddr = 32'd0;
    logic [3:0]  s_arlen = 4'd0;
    logic [2:0]  s_arsize = 3'd2;
    logic [1:0]  s_arburst = 2'b01;
    logic        s_arlock = 1'b0;
    logic [3:0]  s_arcache = 4'd0;
    logic [2:0]  s_arprot = 3'd0;
    logic [3:0]  s_arqos = 4'd0;
    wire         s_rvalid;
    logic        s_rready = 1'b1;
    wire [11:0]  s_rid;
    wire [31:0]  s_rdata;
    wire         s_rlast;
    wire [1:0]   s_rresp;

    globals::AxiSimple_common as_common;
    AxiSimple_if as_clients [7:0] ();

    genvar gi;
    generate
        for (gi = 0; gi < 8; gi = gi + 1) begin : DUMMY_CLIENTS
            if (gi != 3)
                assign as_clients[gi].rdata = 32'h0000_0000;
        end
    endgenerate

    axisimple_wrapper wrapper_i (
        .S_AXI_ACLK(clk),
        .S_AXI_ARESETN(rstn),
        .S_AXI_AWVALID(s_awvalid),
        .S_AXI_AWREADY(s_awready),
        .S_AXI_AWID(s_awid),
        .S_AXI_AWADDR(s_awaddr),
        .S_AXI_AWLEN(s_awlen),
        .S_AXI_AWSIZE(s_awsize),
        .S_AXI_AWBURST(s_awburst),
        .S_AXI_AWLOCK(s_awlock),
        .S_AXI_AWCACHE(s_awcache),
        .S_AXI_AWPROT(s_awprot),
        .S_AXI_AWQOS(s_awqos),
        .S_AXI_WVALID(s_wvalid),
        .S_AXI_WREADY(s_wready),
        .S_AXI_WDATA(s_wdata),
        .S_AXI_WSTRB(s_wstrb),
        .S_AXI_WLAST(s_wlast),
        .S_AXI_BVALID(s_bvalid),
        .S_AXI_BREADY(s_bready),
        .S_AXI_BID(s_bid),
        .S_AXI_BRESP(s_bresp),
        .S_AXI_ARVALID(s_arvalid),
        .S_AXI_ARREADY(s_arready),
        .S_AXI_ARID(s_arid),
        .S_AXI_ARADDR(s_araddr),
        .S_AXI_ARLEN(s_arlen),
        .S_AXI_ARSIZE(s_arsize),
        .S_AXI_ARBURST(s_arburst),
        .S_AXI_ARLOCK(s_arlock),
        .S_AXI_ARCACHE(s_arcache),
        .S_AXI_ARPROT(s_arprot),
        .S_AXI_ARQOS(s_arqos),
        .S_AXI_RVALID(s_rvalid),
        .S_AXI_RREADY(s_rready),
        .S_AXI_RID(s_rid),
        .S_AXI_RDATA(s_rdata),
        .S_AXI_RLAST(s_rlast),
        .S_AXI_RRESP(s_rresp),
        .as_common(as_common),
        .as_clients(as_clients)
    );


    logic dma_req_ready = 1'b0;
    logic dma_req_done = 1'b0;
    logic dma_req_abort_done = 1'b0;
    logic [23:0] dma_req_mc_addr;
    logic [31:0] dma_req_ddr_addr;
    logic [15:0] dma_req_length;
    logic dma_req_rw, dma_req_valid, dma_req_abort;
    int fails = 0;
    int idle_status_addresses = 0;
    logic [31:0] status;

    ps_dma_command dut (
        .clk(clk), .rstn(rstn), .as_common(as_common), .as_client(as_clients[3]),
        .dma_req_mc_addr(dma_req_mc_addr), .dma_req_ddr_addr(dma_req_ddr_addr),
        .dma_req_length(dma_req_length), .dma_req_rw(dma_req_rw),
        .dma_req_valid(dma_req_valid), .dma_req_abort(dma_req_abort),
        .dma_req_ready(dma_req_ready), .dma_req_done(dma_req_done),
        .dma_req_abort_done(dma_req_abort_done)
    );

    always @(posedge clk)
        if (rstn && !s_arvalid && !s_rvalid && as_common.araddr == 8'h03)
            idle_status_addresses++;

    task automatic check(input bit condition, input string message);
        if (!condition) begin
            $display("FAIL: %s", message);
            fails++;
        end
    endtask

    task automatic reg_write(input logic [31:0] address, input logic [31:0] data);
        @(negedge clk);
        s_awaddr = address;
        s_awvalid = 1'b1;
        s_wdata = data;
        s_wstrb = 4'hF;
        s_wlast = 1'b1;
        s_wvalid = 1'b1;
        fork
            begin
                @(posedge clk);
                while (!s_awready) @(posedge clk);
                @(negedge clk);
                s_awvalid = 1'b0;
            end
            begin
                @(posedge clk);
                while (!s_wready) @(posedge clk);
                @(negedge clk);
                s_wvalid = 1'b0;
            end
        join
        while (!s_bvalid) @(negedge clk);
        check(s_bresp == 2'b00, "register write returns OKAY");
        @(posedge clk);
        @(negedge clk);
    endtask

    task automatic reg_read(input logic [31:0] address, output logic [31:0] data);
        @(negedge clk);
        s_araddr = address;
        s_arvalid = 1'b1;
        @(posedge clk);
        while (!s_arready) @(posedge clk);
        @(negedge clk);
        s_arvalid = 1'b0;
        while (!s_rvalid) @(negedge clk);
        data = s_rdata;
        check(s_rresp == 2'b00 && s_rlast, "register read returns OKAY");
        @(posedge clk);
        @(negedge clk);
    endtask

    task automatic accept_request;
        @(negedge clk);
        dma_req_ready = 1'b1;
        @(negedge clk);
        dma_req_ready = 1'b0;
    endtask

    initial begin
        s_bready = 1'b1;
        repeat (8) @(posedge clk);
        @(negedge clk);
        rstn = 1'b1;
        repeat (4) @(posedge clk);
        // Seed the wrapper's idle address stride with a real 32-bit read.
        reg_read(32'h40000024, status);
        reg_write(32'h40030000, 32'h007B0200);
        reg_write(32'h40030004, 32'h00102000);
        reg_write(32'h40030008, 32'h800001F8);
        check(dma_req_valid && dma_req_rw && dma_req_length == 504 &&
              dma_req_mc_addr == 24'h7B0200 && dma_req_ddr_addr == 32'h00102000,
              "Doom's first 504-byte DMA request reaches the engine intact");
        accept_request();
        @(negedge clk);
        dma_req_done = 1'b1;
        @(negedge clk);
        dma_req_done = 1'b0;
        // Guarded firmware polls and interrupts can leave several microseconds
        // between DMA completion and the next owner STATUS read.
        repeat (1024) @(posedge clk);
        check(idle_status_addresses > 0, "idle wrapper visits STATUS without a read");
        reg_read(32'h4003000C, status);
        check(status == 1, $sformatf("DONE survives delayed polling: %08x", status));
        reg_read(32'h4002000C, status);
        reg_read(32'h4003000C, status);
        check(status == 1, "DONE survives another peripheral's offset 0x0C read");
        reg_read(32'h4003000C, status);
        check(status == 1, "DONE remains set after the owner's STATUS read");

        reg_write(32'h40030008, 32'h800001F8);
        reg_read(32'h4003000C, status);
        check(status == 2, "new command clears old completion and sets BUSY");
        accept_request();
        reg_write(32'h40030010, 1);
        check(dma_req_abort, "abort stays requested until engine acknowledgement");
        @(negedge clk);
        dma_req_abort_done = 1'b1;
        @(negedge clk);
        dma_req_abort_done = 1'b0;
        repeat (1024) @(posedge clk);
        reg_read(32'h4000000C, status);
        reg_read(32'h4003000C, status);
        check(status == 4, "ABORTED survives idle cycles and unrelated reads");
        reg_read(32'h4003000C, status);
        check(status == 4, "ABORTED remains set after the owner's STATUS read");
        reg_write(32'h40030008, 32'h800001F8);
        reg_read(32'h4003000C, status);
        check(status == 2, "new command clears old abort result");
        @(negedge clk);
        rstn = 1'b0;
        repeat (4) @(posedge clk);
        @(negedge clk);
        rstn = 1'b1;
        reg_read(32'h4003000C, status);
        check(status == 0, "reset clears all command status");
        if (fails == 0) $display("PS DMA COMPLETION PASS");
        else $display("PS DMA COMPLETION FAILED: %0d checks", fails);
        $finish;
    end

    initial begin
        #1ms;
        $display("PS DMA COMPLETION FAIL: global timeout");
        $finish;
    end
endmodule
