`timescale 1ns / 1ps

// Focused regression for the registered write-data skid at the hard-PS AXI
// boundary. The downstream AxiSimple clients have no ready input, so the
// bench observes each decoded write at the same edge as client awvalid.
module tb_axisimple_wrapper_wskid;

    timeunit 1ns;
    timeprecision 1ps;

    logic clk = 1'b0;
    always #3.75 clk = ~clk;

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
    wire [7:0] client_awvalid;

    genvar gi;
    generate
        for (gi = 0; gi < 8; gi = gi + 1) begin : CLIENT_WIRES
            assign client_awvalid[gi] = as_clients[gi].awvalid;
            assign as_clients[gi].rdata = 32'hA5000000 | gi;
        end
    endgenerate

    axisimple_wrapper dut (
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

    integer fails = 0;
    integer write_count = 0;
    integer response_count = 0;
    integer write_client [0:63];
    logic [7:0] write_addr [0:63];
    logic [31:0] write_data [0:63];
    logic [3:0] write_strb [0:63];
    logic [11:0] response_id [0:63];
    logic [1:0] response_code [0:63];
    integer monitor_i;
    integer monitor_hits;

    task automatic check(input bit condition, input string message);
        if (!condition) begin
            $display("FAIL: %s", message);
            fails = fails + 1;
        end
    endtask

    always @(posedge clk) begin
        if (rstn) begin
            monitor_hits = 0;
            for (monitor_i = 0; monitor_i < 8; monitor_i = monitor_i + 1) begin
                if (client_awvalid[monitor_i]) begin
                    monitor_hits = monitor_hits + 1;
                    if (write_count < 64) begin
                        write_client[write_count] = monitor_i;
                        write_addr[write_count] = as_common.awaddr;
                        write_data[write_count] = as_common.wdata;
                        write_strb[write_count] = as_common.wstrb;
                    end
                    write_count = write_count + 1;
                end
            end
            if (monitor_hits > 1) begin
                $display("FAIL: more than one AxiSimple client selected");
                fails = fails + 1;
            end

            if (s_bvalid && s_bready) begin
                if (response_count < 64) begin
                    response_id[response_count] = s_bid;
                    response_code[response_count] = s_bresp;
                end
                response_count = response_count + 1;
            end
        end
    end

    task automatic send_aw(
        input logic [31:0] addr,
        input logic [3:0] len,
        input logic [11:0] id
    );
        begin
            @(negedge clk);
            s_awaddr = addr;
            s_awlen = len;
            s_awid = id;
            s_awsize = 3'd2;
            s_awburst = 2'b01;
            s_awvalid = 1'b1;
            @(posedge clk);
            while (!s_awready) @(posedge clk);
            @(negedge clk);
            s_awvalid = 1'b0;
        end
    endtask

    task automatic send_w(
        input logic [31:0] data,
        input logic [3:0] strb,
        input logic last
    );
        begin
            @(negedge clk);
            s_wdata = data;
            s_wstrb = strb;
            s_wlast = last;
            s_wvalid = 1'b1;
            @(posedge clk);
            while (!s_wready) @(posedge clk);
            @(negedge clk);
            s_wvalid = 1'b0;
            // Poison the live input after every handshake. A skid that fails
            // to retain any payload bit will corrupt a later client write.
            s_wdata = 32'hDEADDEAD;
            s_wstrb = ~strb;
            s_wlast = !last;
        end
    endtask

    task automatic wait_for_writes(
        input integer target,
        input string message
    );
        integer cycles;
        begin
            cycles = 0;
            while ((write_count < target) && (cycles < 200)) begin
                @(posedge clk);
                cycles = cycles + 1;
            end
            check(write_count >= target, message);
        end
    endtask

    task automatic wait_for_responses(
        input integer target,
        input string message
    );
        integer cycles;
        begin
            cycles = 0;
            while ((response_count < target) && (cycles < 200)) begin
                @(posedge clk);
                cycles = cycles + 1;
            end
            check(response_count >= target, message);
        end
    endtask

    task automatic check_write(
        input integer index,
        input integer client,
        input logic [7:0] addr,
        input logic [31:0] data,
        input logic [3:0] strb,
        input string message
    );
        begin
            check(write_client[index] == client &&
                  write_addr[index] == addr &&
                  write_data[index] == data &&
                  write_strb[index] == strb,
                  $sformatf("%s: client=%0d addr=%02x data=%08x strb=%x",
                            message, write_client[index], write_addr[index],
                            write_data[index], write_strb[index]));
        end
    endtask

    task automatic check_response(
        input integer index,
        input logic [11:0] id,
        input string message
    );
        begin
            check(response_id[index] == id && response_code[index] == 2'b00,
                  $sformatf("%s: id=%03x resp=%x", message,
                            response_id[index], response_code[index]));
        end
    endtask

    integer wbase;
    integer bbase;
    integer burst_i;
    logic [11:0] held_bid;

    initial begin
        // Initial reset and the reset-side ready gate.
        repeat (5) @(posedge clk);
        #1ps;
        check(!s_wready, "WREADY stays low while reset is asserted");
        @(negedge clk);
        rstn = 1'b1;
        s_bready = 1'b1;
        repeat (3) @(posedge clk);

        // Reset must discard a buffered W beat. An AW sent after reset may
        // only pair with the fresh beat supplied below.
        wbase = write_count;
        bbase = response_count;
        send_w(32'hBAD0F00D, 4'h5, 1'b1);
        @(negedge clk);
        rstn = 1'b0;
        s_bready = 1'b0;
        repeat (4) @(posedge clk);
        #1ps;
        check(!s_wready, "reset closes the wrapper W input");
        check(write_count == wbase && response_count == bbase,
              "reset flushes a W beat held without AW");
        @(negedge clk);
        rstn = 1'b1;
        s_bready = 1'b1;
        repeat (2) @(posedge clk);
        send_aw(32'h40000010, 4'd0, 12'h101);
        repeat (5) @(posedge clk);
        check(write_count == wbase,
              "post-reset AW does not consume stale W payload");
        send_w(32'h01020304, 4'hF, 1'b1);
        wait_for_writes(wbase + 1, "fresh post-reset write reaches client");
        wait_for_responses(bbase + 1, "fresh post-reset write returns B");
        check_write(wbase, 0, 8'h04, 32'h01020304, 4'hF,
                    "reset-flush write");
        check_response(bbase, 12'h101, "reset-flush response");

        // Reset must also discard an address accepted without W. A fresh W
        // held after reset must wait for a new AW and use that new AW's ID.
        wbase = write_count;
        bbase = response_count;
        send_aw(32'h40020050, 4'd0, 12'hA01);
        repeat (4) @(posedge clk);
        check(write_count == wbase,
              "AW-only request remains pending without W");
        @(negedge clk);
        rstn = 1'b0;
        s_bready = 1'b0;
        repeat (4) @(posedge clk);
        check(write_count == wbase && response_count == bbase,
              "reset cancels an AW-only request");
        @(negedge clk);
        rstn = 1'b1;
        s_bready = 1'b1;
        repeat (2) @(posedge clk);
        send_w(32'hA0A10001, 4'h6, 1'b1);
        repeat (5) @(posedge clk);
        check(write_count == wbase,
              "post-reset W does not consume stale AW");
        send_aw(32'h40050054, 4'd0, 12'hA02);
        wait_for_writes(wbase + 1, "fresh AW completes post-reset W");
        wait_for_responses(bbase + 1, "fresh AW returns post-reset B");
        check_write(wbase, 5, 8'h15, 32'hA0A10001, 4'h6,
                    "AW-only reset cancellation write");
        check_response(bbase, 12'hA02,
                       "AW-only reset cancellation response");

        // Capture AW and W on one edge, then assert reset before the edge on
        // which the registered W output could reach a client. Neither half
        // of the canceled transaction may survive reset.
        wbase = write_count;
        bbase = response_count;
        @(negedge clk);
        s_awaddr = 32'h40030058;
        s_awlen = 4'd0;
        s_awid = 12'hB01;
        s_awvalid = 1'b1;
        s_wdata = 32'hB0B10001;
        s_wstrb = 4'h9;
        s_wlast = 1'b1;
        s_wvalid = 1'b1;
        #1ps;
        check(s_awready && s_wready,
              "AW and W are both accepted before reset cancellation");
        @(posedge clk);
        @(negedge clk);
        s_awvalid = 1'b0;
        s_wvalid = 1'b0;
        s_wdata = 32'hDEADDEAD;
        s_wstrb = 4'h6;
        s_wlast = 1'b0;
        rstn = 1'b0;
        s_bready = 1'b0;
        repeat (4) @(posedge clk);
        check(write_count == wbase && response_count == bbase,
              "reset cancels jointly buffered AW and W");
        @(negedge clk);
        rstn = 1'b1;
        s_bready = 1'b1;
        repeat (5) @(posedge clk);
        check(write_count == wbase && response_count == bbase,
              "jointly buffered AW and W do not reappear after reset");
        fork
            send_aw(32'h4006005C, 4'd0, 12'hB02);
            send_w(32'hB0B20002, 4'h9, 1'b1);
        join
        wait_for_writes(wbase + 1,
                        "fresh transaction follows joint reset cancellation");
        wait_for_responses(bbase + 1,
                           "fresh transaction returns B after joint reset");
        check_write(wbase, 6, 8'h17, 32'hB0B20002, 4'h9,
                    "joint reset cancellation write");
        check_response(bbase, 12'hB02,
                       "joint reset cancellation response");

        // W-before-AW must retain all 37 payload bits while the interconnect
        // waits for an address.
        wbase = write_count;
        bbase = response_count;
        send_w(32'h11112222, 4'h3, 1'b1);
        repeat (4) @(posedge clk);
        check(write_count == wbase, "W-before-AW remains buffered");
        send_aw(32'h40010024, 4'd0, 12'h111);
        wait_for_writes(wbase + 1, "W-before-AW reaches client");
        wait_for_responses(bbase + 1, "W-before-AW returns B");
        check_write(wbase, 1, 8'h09, 32'h11112222, 4'h3,
                    "W-before-AW write");
        check_response(bbase, 12'h111, "W-before-AW response");

        // AW-before-W exercises the opposite independent-channel order.
        wbase = write_count;
        bbase = response_count;
        send_aw(32'h4002002C, 4'd0, 12'h222);
        repeat (4) @(posedge clk);
        check(write_count == wbase, "AW-before-W waits for data");
        send_w(32'h33334444, 4'hC, 1'b1);
        wait_for_writes(wbase + 1, "AW-before-W reaches client");
        wait_for_responses(bbase + 1, "AW-before-W returns B");
        check_write(wbase, 2, 8'h0B, 32'h33334444, 4'hC,
                    "AW-before-W write");
        check_response(bbase, 12'h222, "AW-before-W response");

        // Both upstream channels may handshake on the same edge.
        wbase = write_count;
        bbase = response_count;
        fork
            send_aw(32'h40030030, 4'd0, 12'h333);
            send_w(32'h55556666, 4'hA, 1'b1);
        join
        wait_for_writes(wbase + 1, "simultaneous AW/W reaches client");
        wait_for_responses(bbase + 1, "simultaneous AW/W returns B");
        check_write(wbase, 3, 8'h0C, 32'h55556666, 4'hA,
                    "simultaneous write");
        check_response(bbase, 12'h333, "simultaneous response");

        // With no AW, the registered output holds one beat and the skid
        // register holds a second. The wrapper must then apply backpressure.
        wbase = write_count;
        bbase = response_count;
        send_w(32'h70000001, 4'h1, 1'b1);
        send_w(32'h70000002, 4'h2, 1'b1);
        #1ps;
        check(!s_wready, "two buffered W beats deassert WREADY");
        repeat (3) begin
            @(posedge clk);
            #1ps;
            check(!s_wready, "WREADY stays low while both W slots are full");
        end
        send_aw(32'h40000040, 4'd0, 12'h401);
        send_aw(32'h40070044, 4'd0, 12'h402);
        wait_for_writes(wbase + 2, "two buffered W beats drain in order");
        wait_for_responses(bbase + 2, "two buffered W beats return B responses");
        check_write(wbase, 0, 8'h10, 32'h70000001, 4'h1,
                    "first buffered write");
        check_write(wbase + 1, 7, 8'h11, 32'h70000002, 4'h2,
                    "second buffered write");
        check_response(bbase, 12'h401, "first buffered response");
        check_response(bbase + 1, 12'h402, "second buffered response");

        // The AXI3 port's maximum 16-beat incrementing burst checks every
        // AWLEN bit, distinct data/strobes, WLAST, and address updates.
        wbase = write_count;
        bbase = response_count;
        send_aw(32'h40040020, 4'd15, 12'h444);
        for (burst_i = 0; burst_i < 16; burst_i = burst_i + 1) begin
            send_w(32'hB4000000 + burst_i,
                   burst_i[3:0], burst_i == 15);
            if (burst_i < 15) begin
                check(response_count == bbase,
                      $sformatf("burst beat %0d does not assert WLAST",
                                burst_i));
            end
        end
        wait_for_writes(wbase + 16, "16-beat burst reaches client");
        wait_for_responses(bbase + 1, "16-beat burst returns one B");
        for (burst_i = 0; burst_i < 16; burst_i = burst_i + 1) begin
            check_write(wbase + burst_i, 4, 8'h08 + burst_i,
                        32'hB4000000 + burst_i, burst_i[3:0],
                        $sformatf("burst beat %0d", burst_i));
        end
        check(response_count == bbase + 1,
              "beat-15 WLAST yields exactly one response for the 16-beat burst");
        check_response(bbase, 12'h444, "16-beat burst response");

        // Queue writes to different decode windows while BREADY is low, then
        // verify that the response FIFO preserves the upstream ID order.
        wbase = write_count;
        bbase = response_count;
        @(negedge clk);
        s_bready = 1'b0;
        fork
            send_aw(32'h40050014, 4'd0, 12'h501);
            send_w(32'h50000001, 4'hF, 1'b1);
        join
        fork
            send_aw(32'h40010018, 4'd0, 12'h502);
            send_w(32'h50000002, 4'h7, 1'b1);
        join
        fork
            send_aw(32'h4006001C, 4'd0, 12'h503);
            send_w(32'h50000003, 4'hE, 1'b1);
        join
        wait_for_writes(wbase + 3, "different client-window writes complete");
        check_write(wbase, 5, 8'h05, 32'h50000001, 4'hF,
                    "client-window write 0");
        check_write(wbase + 1, 1, 8'h06, 32'h50000002, 4'h7,
                    "client-window write 1");
        check_write(wbase + 2, 6, 8'h07, 32'h50000003, 4'hE,
                    "client-window write 2");
        while (!s_bvalid) @(posedge clk);
        #1ps;
        held_bid = s_bid;
        check(held_bid == 12'h501 && s_bresp == 2'b00,
              "first queued B response has the first transaction ID");
        repeat (3) begin
            @(posedge clk);
            #1ps;
            check(s_bvalid && s_bid == held_bid,
                  "B response stays stable under backpressure");
        end
        @(negedge clk);
        s_bready = 1'b1;
        wait_for_responses(bbase + 3, "queued B responses drain");
        check_response(bbase, 12'h501, "queued response 0");
        check_response(bbase + 1, 12'h502, "queued response 1");
        check_response(bbase + 2, 12'h503, "queued response 2");

        repeat (5) @(posedge clk);
        if (fails == 0) begin
            $display("AXISIMPLE WRAPPER WSKID PASS");
            $finish;
        end
        $fatal(1, "AXISIMPLE WRAPPER WSKID FAILED: %0d checks", fails);
    end

    initial begin
        #2ms;
        $fatal(1, "AXISIMPLE WRAPPER WSKID FAIL: global timeout");
    end

endmodule
