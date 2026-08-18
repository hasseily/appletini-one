`timescale 1ns / 1ps

// Integration regression for the registered GP0 write-data path and the real
// Disk II register block. This bench checks the state-changing edge at the
// AxiSimple client, not just the upstream AXI handshake.
module tb_axisimple_disk2_base;

    timeunit 1ns;
    timeprecision 1ps;

    localparam logic [31:0] DISK2_WINDOW = 32'h4006_0000;
    localparam logic [31:0] DISK2_BASE_ADDR = DISK2_WINDOW + 32'h0000_0008;
    localparam logic [31:0] DISK2_BASE_RESET = 32'h0070_0000;

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
            if (gi != 6)
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

    globals::AppleBus_read ab_read;
    globals::SoftSwitchState sss;
    globals::AppleBus_write ab_write;
    logic [20:0] mc_line_addr;
    logic        mc_rw;
    logic [63:0] mc_wdata;
    logic [7:0]  mc_wstrb;
    logic        mc_valid;
    logic        vtw_req_ready;
    logic        vtw_resp_valid;
    logic [7:0]  vtw_resp_rdata;
    logic        vtw_time_ready;
    logic        vtw_write_timing_active;

    disk2_card card_i (
        .clk(clk),
        .rstn(rstn),
        .ab_read(ab_read),
        .rom_serve_en(1'b0),
        .sss(sss),
        .slot_assign(3'h6),
        .as_common(as_common),
        .as_client(as_clients[6]),
        .mc_line_addr(mc_line_addr),
        .mc_rw(mc_rw),
        .mc_wdata(mc_wdata),
        .mc_wstrb(mc_wstrb),
        .mc_valid(mc_valid),
        .mc_ready(1'b0),
        .mc_rdata(64'h0000_0000_0000_0000),
        .mc_rvalid(1'b0),
        .ab_write(ab_write),
        .vtw_active(1'b0),
        .vtw_req_valid(1'b0),
        .vtw_req_addr(4'h0),
        .vtw_req_ready(vtw_req_ready),
        .vtw_resp_valid(vtw_resp_valid),
        .vtw_resp_rdata(vtw_resp_rdata),
        .vtw_cycle_tick(1'b0),
        .vtw_native_cycle_active(1'b0),
        .vtw_time_ready(vtw_time_ready),
        .vtw_write_timing_active(vtw_write_timing_active),
        .sound_spinning(),
        .sound_qtrack(),
        .sound_event(),
        .sound_seek_start_qtrack(),
        .sound_seek_distance()
    );

    integer fails = 0;
    integer fabric_cycle = 0;
    integer card_write_count = 0;
    integer b_count = 0;
    integer card_write_cycle [0:31];
    logic [7:0] card_write_addr [0:31];
    logic [31:0] card_write_data [0:31];
    logic [3:0] card_write_strb [0:31];
    logic [11:0] b_id [0:31];

    task automatic check(input bit condition, input string message);
        if (!condition) begin
            $display("FAIL: %s", message);
            fails = fails + 1;
        end
    endtask

    always @(posedge clk) begin
        fabric_cycle = fabric_cycle + 1;
        if (rstn && as_clients[6].awvalid) begin
            if (card_write_count < 32) begin
                card_write_cycle[card_write_count] = fabric_cycle;
                card_write_addr[card_write_count] = as_common.awaddr;
                card_write_data[card_write_count] = as_common.wdata;
                card_write_strb[card_write_count] = as_common.wstrb;
            end
            card_write_count = card_write_count + 1;
        end
        if (rstn && s_bvalid && s_bready) begin
            if (b_count < 32)
                b_id[b_count] = s_bid;
            b_count = b_count + 1;
        end
    end

    task automatic send_aw(
        input logic [31:0] addr,
        input logic [11:0] id
    );
        begin
            @(negedge clk);
            s_awaddr = addr;
            s_awid = id;
            s_awlen = 4'd0;
            s_awsize = 3'd2;
            s_awburst = 2'b01;
            s_awvalid = 1'b1;
            @(posedge clk);
            while (!s_awready)
                @(posedge clk);
            @(negedge clk);
            s_awvalid = 1'b0;
        end
    endtask

    task automatic send_w(
        input logic [31:0] data,
        input logic [3:0] strb
    );
        begin
            @(negedge clk);
            s_wdata = data;
            s_wstrb = strb;
            s_wlast = 1'b1;
            s_wvalid = 1'b1;
            @(posedge clk);
            while (!s_wready)
                @(posedge clk);
            @(negedge clk);
            s_wvalid = 1'b0;
            s_wdata = 32'hDEAD_DEAD;
            s_wstrb = 4'h0;
            s_wlast = 1'b0;
        end
    endtask

    task automatic wait_for_card_writes(
        input integer target,
        input string message
    );
        integer timeout;
        begin
            timeout = 0;
            while (card_write_count < target && timeout < 100) begin
                @(posedge clk);
                timeout = timeout + 1;
            end
            check(card_write_count >= target, message);
        end
    endtask

    task automatic wait_for_b(
        input integer target,
        input string message
    );
        integer timeout;
        begin
            timeout = 0;
            while (b_count < target && timeout < 100) begin
                @(posedge clk);
                timeout = timeout + 1;
            end
            check(b_count >= target, message);
        end
    endtask

    task automatic read_disk2_base(
        input logic [11:0] id,
        output logic [31:0] value
    );
        integer timeout;
        begin
            @(negedge clk);
            s_araddr = DISK2_BASE_ADDR;
            s_arid = id;
            s_arlen = 4'd0;
            s_arsize = 3'd2;
            s_arburst = 2'b01;
            s_arvalid = 1'b1;
            @(posedge clk);
            while (!s_arready)
                @(posedge clk);
            @(negedge clk);
            s_arvalid = 1'b0;

            timeout = 0;
            while (!s_rvalid && timeout < 100) begin
                @(negedge clk);
                timeout = timeout + 1;
            end
            check(s_rvalid, "Disk II BASE read returned RVALID");
            value = s_rdata;
            check(s_rid == id && s_rresp == 2'b00 && s_rlast,
                  "Disk II BASE read returned the requested ID and OKAY");
            @(posedge clk);
        end
    endtask

    task automatic force_live_cache_and_fifo;
        begin
            force card_i.prefetch_current_line_q = 21'h12345;
            force card_i.prefetch_next_line_q = 21'h12346;
            force card_i.prefetch_valid_q = 4'hF;
            force card_i.prefetch_req_q = 1'b1;
            force card_i.prefetch_resp_pending_q = 1'b1;
            force card_i.cache_patch_pending_q = 1'b1;
            force card_i.woz_cached_valid_q = 1'b1;
            force card_i.woz_cached_ready_q = 1'b1;
            force card_i.woz_seam_arm_q = 1'b1;
            force card_i.write_fifo_head_q = 4'd3;
            force card_i.write_fifo_tail_q = 4'd8;
            force card_i.write_fifo_count_q = 5'd5;
            force card_i.disk_write_pending_q = 1'b1;
            force card_i.woz_write_pending_q = 1'b1;
            force card_i.write_req_q = 1'b1;
        end
    endtask

    task automatic release_live_cache_and_fifo;
        begin
            release card_i.prefetch_current_line_q;
            release card_i.prefetch_next_line_q;
            release card_i.prefetch_valid_q;
            release card_i.prefetch_req_q;
            release card_i.prefetch_resp_pending_q;
            release card_i.cache_patch_pending_q;
            release card_i.woz_cached_valid_q;
            release card_i.woz_cached_ready_q;
            release card_i.woz_seam_arm_q;
            release card_i.write_fifo_head_q;
            release card_i.write_fifo_tail_q;
            release card_i.write_fifo_count_q;
            release card_i.disk_write_pending_q;
            release card_i.woz_write_pending_q;
            release card_i.write_req_q;
        end
    endtask

    task automatic check_base_invalidation;
        begin
            check(card_i.prefetch_current_line_q == 21'd0 &&
                  card_i.prefetch_next_line_q == 21'd0 &&
                  card_i.prefetch_valid_q == 4'h0 &&
                  !card_i.prefetch_req_q &&
                  !card_i.prefetch_resp_pending_q,
                  "BASE apply invalidated all prefetch state");
            check(!card_i.cache_patch_pending_q &&
                  !card_i.woz_cached_valid_q &&
                  !card_i.woz_cached_ready_q &&
                  !card_i.woz_seam_arm_q,
                  "BASE apply invalidated cached byte and patch state");
            check(card_i.write_fifo_head_q == 4'd0 &&
                  card_i.write_fifo_tail_q == 4'd0 &&
                  card_i.write_fifo_count_q == 5'd0 &&
                  !card_i.disk_write_pending_q &&
                  !card_i.woz_write_pending_q &&
                  !card_i.write_req_q,
                  "BASE apply canceled the staged write queue");
        end
    endtask

    integer wbase;
    integer bbase;
    logic [31:0] read_value;

    initial begin
        ab_read = '0;
        ab_read.res = 1'b1;
        ab_read.rw = 1'b1;
        sss = '0;

        repeat (6) @(posedge clk);
        #1ps;
        check(!s_wready, "WREADY stays low during reset");
        check(card_i.psram_base_q == DISK2_BASE_RESET,
              "Disk II BASE reset value is present");
        @(negedge clk);
        rstn = 1'b1;
        s_bready = 1'b1;
        repeat (3) @(posedge clk);

        // Buffer W first. The card must not see data or change state until a
        // matching address reaches the existing AW path.
        wbase = card_write_count;
        bbase = b_count;
        send_w(32'h0012_3457, 4'hF);
        repeat (4) begin
            @(posedge clk);
            #1ps;
            check(card_i.psram_base_q == DISK2_BASE_RESET &&
                  card_write_count == wbase,
                  "buffered W cannot update Disk II before AW");
        end
        send_aw(DISK2_BASE_ADDR, 12'h101);
        check(as_clients[6].awvalid && as_common.awaddr == 8'h02 &&
              as_common.wdata == 32'h0012_3457 && as_common.wstrb == 4'hF,
              "full BASE tuple is stable before the client edge");
        check(card_i.psram_base_q == DISK2_BASE_RESET,
              "full BASE stays old until the client pulse edge");
        @(posedge clk);
        #1ps;
        check(card_i.psram_base_q == 32'h0012_3450,
              "full BASE write applies all bytes and aligns to eight bytes");
        wait_for_b(bbase + 1, "full BASE write returned B");
        check(b_id[bbase] == 12'h101,
              "full BASE write kept its response ID");
        read_disk2_base(12'h181, read_value);
        check(read_value == 32'h0012_3450,
              "B-then-read returned the full aligned BASE value");

        // Partial strobes merge byte lanes before the mandatory alignment.
        wbase = card_write_count;
        bbase = b_count;
        fork
            send_aw(DISK2_BASE_ADDR, 12'h202);
            send_w(32'hABCD_EF07, 4'b0101);
        join
        wait_for_card_writes(wbase + 1, "partial BASE reached the card");
        #1ps;
        check(card_i.psram_base_q == 32'h00CD_3400,
              "partial BASE merged selected lanes and cleared bits 2:0");
        wait_for_b(bbase + 1, "partial BASE write returned B");
        read_disk2_base(12'h282, read_value);
        check(read_value == 32'h00CD_3400,
              "B-then-read returned the partial aligned BASE value");

        // Present two complete single-beat writes on adjacent upstream edges.
        // Their client pulses must also be adjacent. Seed every state that a
        // BASE change must invalidate and hold it until the first pulse.
        wbase = card_write_count;
        bbase = b_count;
        @(negedge clk);
        force_live_cache_and_fifo();
        s_awaddr = DISK2_BASE_ADDR;
        s_awid = 12'h301;
        s_awvalid = 1'b1;
        s_wdata = 32'h0020_0003;
        s_wstrb = 4'hF;
        s_wlast = 1'b1;
        s_wvalid = 1'b1;
        @(posedge clk);
        check(s_awready && s_wready,
              "first back-to-back BASE beat was accepted");
        #1ps;
        check(as_clients[6].awvalid &&
              card_i.psram_base_q == 32'h00CD_3400,
              "first queued BASE is visible but not applied early");
        release_live_cache_and_fifo();

        @(negedge clk);
        s_awid = 12'h302;
        s_wdata = 32'h0030_0007;
        @(posedge clk);
        check(s_awready && s_wready,
              "second back-to-back BASE beat was accepted");
        #1ps;
        check(card_i.psram_base_q == 32'h0020_0000,
              "first back-to-back BASE applied on its client edge");
        check_base_invalidation();

        @(negedge clk);
        s_awvalid = 1'b0;
        s_wvalid = 1'b0;
        s_wdata = 32'hDEAD_DEAD;
        s_wstrb = 4'h0;
        s_wlast = 1'b0;
        @(posedge clk);
        #1ps;
        check(card_i.psram_base_q == 32'h0030_0000,
              "second back-to-back BASE applied in order");
        check(card_write_count == wbase + 2 &&
              card_write_cycle[wbase + 1] == card_write_cycle[wbase] + 1,
              "back-to-back BASE client pulses were one fabric clock apart");
        check(card_write_addr[wbase] == 8'h02 &&
              card_write_data[wbase] == 32'h0020_0003 &&
              card_write_strb[wbase] == 4'hF &&
              card_write_addr[wbase + 1] == 8'h02 &&
              card_write_data[wbase + 1] == 32'h0030_0007,
              "back-to-back BASE tuples stayed ordered and atomic");
        wait_for_b(bbase + 2, "back-to-back BASE writes returned two B responses");
        check(b_id[bbase] == 12'h301 && b_id[bbase + 1] == 12'h302,
              "back-to-back BASE response IDs stayed ordered");
        read_disk2_base(12'h383, read_value);
        check(read_value == 32'h0030_0000,
              "B-then-read returned the last back-to-back BASE value");

        // A W beat accepted without AW is outstanding, not a Disk II command.
        // Reset must cancel it before a later address can select the card.
        wbase = card_write_count;
        bbase = b_count;
        send_w(32'h0040_0007, 4'hF);
        check(card_i.psram_base_q == 32'h0030_0000,
              "unmatched W stayed outside the Disk II card");
        @(negedge clk);
        rstn = 1'b0;
        s_bready = 1'b0;
        repeat (4) @(posedge clk);
        #1ps;
        check(!s_wready && card_i.psram_base_q == DISK2_BASE_RESET,
              "reset canceled buffered W and reset the Disk II BASE");
        @(negedge clk);
        rstn = 1'b1;
        s_bready = 1'b1;
        repeat (2) @(posedge clk);
        send_aw(DISK2_BASE_ADDR, 12'h404);
        repeat (5) @(posedge clk);
        #1ps;
        check(card_write_count == wbase && b_count == bbase &&
              card_i.psram_base_q == DISK2_BASE_RESET,
              "post-reset AW did not consume the canceled W beat");
        send_w(32'h0050_0000, 4'hF);
        wait_for_card_writes(wbase + 1, "fresh post-reset BASE reached the card");
        wait_for_b(bbase + 1, "fresh post-reset BASE returned B");
        #1ps;
        check(card_i.psram_base_q == 32'h0050_0000,
              "fresh post-reset BASE applied normally");
        read_disk2_base(12'h484, read_value);
        check(read_value == 32'h0050_0000,
              "post-reset B-then-read returned the fresh BASE");

        repeat (5) @(posedge clk);
        if (fails != 0)
            $fatal(1, "AXISIMPLE DISK2 BASE FAILED: %0d checks", fails);
        $display("AXISIMPLE DISK2 BASE PASS");
        $finish;
    end

    initial begin
        #2ms;
        $fatal(1, "AXISIMPLE DISK2 BASE FAIL: global timeout");
    end

endmodule
