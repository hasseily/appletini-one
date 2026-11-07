`timescale 1ns / 1ps

module tb_pc_transporter_card;
    localparam int READ_WAIT = 12;
    localparam int RAM_LINES = 98304;

    logic clk = 1'b0;
    logic rstn = 1'b0;
    logic card_enabled = 1'b1;
    always #5 clk = ~clk;

    globals::AppleBus_read ab_read;
    globals::AppleBus_write ab_write;
    globals::SoftSwitchState sss;
    globals::AxiSimple_common as_common;
    AxiSimple_if axi();

    logic mem_req_valid, mem_req_ready, mem_req_write;
    logic [19:0] mem_req_addr;
    logic [63:0] mem_req_wdata, mem_rsp_rdata;
    logic [7:0] mem_req_wstrb;
    logic mem_rsp_valid, mem_rsp_error;

    logic [63:0] backing [0:RAM_LINES-1];
    logic backend_busy = 1'b0;
    logic backend_blocked = 1'b0;
    logic inject_error = 1'b0;
    int backend_latency = 2;
    int remaining;
    int read_count = 0;
    int write_count = 0;
    int alias_reads_before;
    int alias_writes_before;
    logic [19:0] accepted_addr;
    logic [63:0] accepted_data;
    logic [7:0] accepted_strobes;
    logic accepted_write, accepted_error;

    pc_transporter_card #(.READ_WAIT_CYCLES(READ_WAIT)) dut (
        .clk(clk), .rstn(rstn), .ab_read(ab_read), .sss(sss),
        .card_enabled(card_enabled), .slot_assign(3'd5),
        .as_common(as_common), .as_client(axi), .mem_req_valid(mem_req_valid),
        .mem_req_ready(mem_req_ready), .mem_req_write(mem_req_write),
        .mem_req_addr(mem_req_addr), .mem_req_wdata(mem_req_wdata),
        .mem_req_wstrb(mem_req_wstrb), .mem_rsp_valid(mem_rsp_valid),
        .mem_rsp_rdata(mem_rsp_rdata), .mem_rsp_error(mem_rsp_error),
        .ab_write(ab_write)
    );

    soft_switch_manager switches (
        .clk(clk), .rstn(rstn), .ramworks_en(1'b0), .ab_read(ab_read), .sss(sss)
    );

    assign mem_req_ready = !backend_busy && !backend_blocked;

    always @(posedge clk) begin
        mem_rsp_valid <= 1'b0;
        if (mem_req_valid && mem_req_ready) begin
            if ((mem_req_addr[2:0] != 3'd0) || (mem_req_addr >= 20'hC0000)) begin
                $fatal(1, "Backend saw an invalid line address %05x", mem_req_addr);
            end
            if (mem_req_addr[19:10] == 10'h2FC) begin
                $fatal(1, "Backend saw a port RAM line address %05x", mem_req_addr);
            end
            backend_busy     <= 1'b1;
            remaining        <= backend_latency;
            accepted_addr    <= mem_req_addr;
            accepted_data    <= mem_req_wdata;
            accepted_strobes <= mem_req_wstrb;
            accepted_write   <= mem_req_write;
            accepted_error   <= inject_error;
            if (mem_req_write) write_count <= write_count + 1;
            else               read_count  <= read_count + 1;
        end
        if (backend_busy) begin
            if (remaining == 1) begin
                backend_busy  <= 1'b0;
                mem_rsp_valid <= 1'b1;
                mem_rsp_error <= accepted_error;
                mem_rsp_rdata <= backing[accepted_addr[19:3]];
                if (accepted_write && !accepted_error) begin
                    for (int i = 0; i < 8; i++) begin
                        if (accepted_strobes[i]) begin
                            backing[accepted_addr[19:3]][8*i +: 8] <= accepted_data[8*i +: 8];
                        end
                    end
                end
            end else begin
                remaining <= remaining - 1;
            end
        end
    end

    always @(negedge clk) begin
        if (ab_write.assert_dma || ab_write.assert_inh || ab_write.assert_rdy ||
            ab_write.assert_res || ab_write.assert_nmi || ab_write.wr_addr_rw_en ||
            ab_write.wr_dma_data_en) begin
            $fatal(1, "Slot slave drove an unrelated Apple bus signal");
        end
    end

    task automatic start_cycle(input logic [15:0] address, input logic is_read,
                               input logic qualified);
        @(negedge clk);
        ab_read.addr_en = 1'b1;
        ab_read.cycle_valid = qualified;
        @(negedge clk);
        ab_read.addr_en = 1'b0;
        ab_read.addr = address;
        ab_read.rw = is_read;
        ab_read.serve_en = 1'b1;
        @(negedge clk);
        ab_read.serve_en = 1'b0;
    endtask

    task automatic start_early_read(input logic [15:0] address, input logic dma_n);
        @(negedge clk);
        ab_read.cycle_valid = 1'b1;
        ab_read.addr_en = 1'b1;
        ab_read.addr_early = address;
        ab_read.rw_early = 1'b1;
        ab_read.dma = dma_n;
        @(negedge clk);
        ab_read.addr_en = 1'b0;
    endtask

    task automatic confirm_early_read(input logic [15:0] address);
        ab_read.addr = address;
        ab_read.rw = 1'b1;
        ab_read.serve_en = 1'b1;
        @(negedge clk);
        ab_read.serve_en = 1'b0;
        ab_read.dma = 1'b0;
    endtask

    task automatic finish_cycle(input logic [7:0] value);
        ab_read.data = value;
        ab_read.data_en = 1'b1;
        @(negedge clk);
        ab_read.data_en = 1'b0;
        #1;
        if (ab_write.wr_data_en) $fatal(1, "Read data remained driven after data_en");
    endtask

    task automatic apple_write(input logic [15:0] address, input logic [7:0] value);
        start_cycle(address, 1'b0, 1'b1);
        if (ab_write.wr_data_en) $fatal(1, "Card drove during an Apple write");
        finish_cycle(value);
    endtask

    task automatic expect_read(input logic [15:0] address, input logic [7:0] expected,
                               input logic [7:0] mask = 8'hFF);
        logic seen;
        seen = 1'b0;
        start_cycle(address, 1'b1, 1'b1);
        for (int i = 0; i <= READ_WAIT; i++) begin
            if (ab_write.wr_data_en) begin
                seen = 1'b1;
                if ((ab_write.wr_data & mask) !== (expected & mask)) begin
                    $fatal(1, "Apple read %04x got %02x expected %02x mask %02x",
                           address, ab_write.wr_data, expected, mask);
                end
            end else if (seen) begin
                $fatal(1, "Apple read %04x lost its data drive", address);
            end
            @(negedge clk);
        end
        if (!seen) $fatal(1, "Apple read %04x had no timely response", address);
        finish_cycle('0);
    endtask

    task automatic expect_quiet(input int cycles);
        for (int i = 0; i < cycles; i++) begin
            if (ab_write.wr_data_en) $fatal(1, "Unexpected drive at %04x", ab_read.addr);
            @(negedge clk);
        end
    endtask

    task automatic axi_write(input logic [7:0] index, input logic [31:0] value);
        @(negedge clk);
        as_common.awaddr = index;
        as_common.wdata = value;
        as_common.wstrb = 4'hF;
        axi.awvalid = 1'b1;
        @(negedge clk);
        axi.awvalid = 1'b0;
    endtask

    task automatic simultaneous_port_writes(input logic [15:0] apple_address,
                                            input logic [7:0] apple_value,
                                            input logic [7:0] ps_value,
                                            input logic same_byte,
                                            input logic check_deferred_seed);
        start_cycle(apple_address, 1'b0, 1'b1);
        ab_read.data = apple_value;
        ab_read.data_en = 1'b1;
        as_common.araddr = 8'h01;
        as_common.awaddr = 8'h04;
        as_common.wdata = {24'd0, ps_value};
        as_common.wstrb = 4'hF;
        axi.awvalid = 1'b1;
        #1;
        if (dut.ps_port_write !== !same_byte) begin
            $fatal(1, "PS collision decision does not match the decoded byte");
        end
        if (check_deferred_seed && (dut.seed_need_q !== 2'b11 || dut.seed_issue)) begin
            $fatal(1, "A discarded PS write did not reserve the seed port");
        end
        @(negedge clk);
        ab_read.data_en = 1'b0;
        axi.awvalid = 1'b0;
    endtask

    task automatic simultaneous_ctrl_writes(input logic [15:0] apple_address,
                                            input logic [7:0] apple_value,
                                            input logic [7:0] ps_value);
        start_cycle(apple_address, 1'b0, 1'b1);
        ab_read.data = apple_value;
        ab_read.data_en = 1'b1;
        as_common.awaddr = 8'h04;
        as_common.wdata = {24'd0, ps_value};
        as_common.wstrb = 4'hF;
        axi.awvalid = 1'b1;
        @(negedge clk);
        ab_read.data_en = 1'b0;
        axi.awvalid = 1'b0;
    endtask

    task automatic expect_axi(input logic [7:0] index, input logic [31:0] expected,
                              input logic [31:0] mask = '1);
        @(negedge clk);
        as_common.araddr = index;
        repeat (2) @(negedge clk);
        if ((axi.rdata & mask) !== (expected & mask)) begin
            $fatal(1, "AXI index %02x got %08x expected %08x mask %08x",
                   index, axi.rdata, expected, mask);
        end
    endtask

    task automatic pointer(input logic [23:0] value);
        apple_write(16'hCF00, value[7:0]);
        apple_write(16'hCF01, value[15:8]);
        apple_write(16'hCF02, value[23:16]);
    endtask

    task automatic calibration_count(input logic [15:0] count);
        int terminal_clocks;
        terminal_clocks = int'((64'(count) * 133000000) / 1193182);
        apple_write(16'hCF2A, count[7:0]);
        apple_write(16'hCF2A, count[15:8]);
        apple_write(16'hCF31, 8'h01);
        expect_read(16'hCF31, 8'h00, 8'h20);
        repeat (terminal_clocks - 160) @(negedge clk);
        expect_read(16'hCF31, 8'h00, 8'h20);
        repeat (325) @(negedge clk);
        expect_read(16'hCF31, 8'h20, 8'h20);
        apple_write(16'hCF30, 8'h01);
    endtask

    initial begin
        ab_read = '0;
        ab_read.res = 1'b1;
        as_common = '0;
        axi.awvalid = 1'b0;
        mem_rsp_valid = 1'b0;
        mem_rsp_error = 1'b0;
        backing[20'h00000 >> 3] = 64'h8877665544332211;
        backing[20'h001F8 >> 3] = 64'hA800000000000000;
        backing[20'h0FFF8 >> 3] = 64'hB700000000000000;
        backing[20'h10000 >> 3] = 64'h00000000000000C6;
        backing[20'hB0010 >> 3] = '0;
        backing[20'hB8010 >> 3] = '0;
        backing[20'hBB020 >> 3] = '0;
        backing[20'hBF020 >> 3] = 64'h0000000000990000;
        backing[20'hBF400 >> 3] = 64'h00000000000000D3;
        backing[20'hBFFF8 >> 3] = 64'hD500000000000000;

        repeat (4) @(negedge clk);
        rstn = 1'b1;
        expect_axi(8'h00, 32'h50435435);
        expect_axi(8'h01, 32'h00000047);
        expect_read(16'hC0D0, 8'hFF);
        start_cycle(16'hC822, 1'b1, 1'b1);
        expect_quiet(3);
        finish_cycle('0);

        expect_read(16'hC500, 8'hFF);
        apple_write(16'hC822, 8'hA4);
        expect_read(16'hC822, 8'hA4);
        axi_write(8'h03, 32'h00000022);
        expect_axi(8'h04, 32'h000000A4);
        axi_write(8'h04, 32'h0000004A);
        expect_read(16'hC822, 8'h4A);
        apple_write(16'hCF2C, 8'h0E);
        expect_read(16'hCBD0, 8'h0E);
        axi_write(8'h03, 32'h000003D8);
        axi_write(8'h04, 32'h00000029);
        expect_read(16'hCF2E, 8'h29);
        apple_write(16'hCF2D, 8'h23);
        apple_write(16'hCBD4, 8'h0F);
        apple_write(16'hCBD5, 8'h45);
        apple_write(16'hCF2F, 8'h17);
        expect_axi(8'h0B, 32'h00041729);
        expect_axi(8'h0D, 32'h00004523);

        start_cycle(16'hCFFF, 1'b1, 1'b1);
        expect_quiet(4);
        finish_cycle('0);
        start_cycle(16'hCF30, 1'b1, 1'b1);
        expect_quiet(4);
        finish_cycle('0);
        expect_read(16'hC500, 8'hFF);
        start_cycle(16'hC600, 1'b1, 1'b1);
        expect_quiet(4);
        finish_cycle('0);
        start_cycle(16'hCF30, 1'b1, 1'b1);
        expect_quiet(4);
        finish_cycle('0);
        apple_write(16'hC007, 8'h00);
        start_cycle(16'hC500, 1'b1, 1'b1);
        expect_quiet(4);
        finish_cycle('0);
        start_cycle(16'hC822, 1'b1, 1'b1);
        expect_quiet(4);
        finish_cycle('0);
        apple_write(16'hC006, 8'h00);
        start_cycle(16'hC300, 1'b1, 1'b1);
        expect_quiet(4);
        finish_cycle('0);
        expect_read(16'hC500, 8'hFF);
        start_cycle(16'hC822, 1'b1, 1'b1);
        expect_quiet(4);
        finish_cycle('0);
        start_cycle(16'hCFFF, 1'b1, 1'b1);
        expect_quiet(4);
        finish_cycle('0);
        expect_read(16'hC500, 8'hFF);
        start_cycle(16'hCF00, 1'b0, 1'b0);
        finish_cycle(8'hFE);
        expect_read(16'hCF00, 8'h00);

        pointer(24'h00FFFF);
        expect_read(16'hCF04, 8'hB7);
        expect_read(16'hCF00, 8'hFF);
        expect_read(16'hCF03, 8'hB7);
        expect_read(16'hCF00, 8'h00);
        expect_read(16'hCF01, 8'h00);
        expect_read(16'hCF02, 8'h01);
        expect_read(16'hCF04, 8'hC6);
        pointer(24'h0BFFFF);
        expect_read(16'hCF03, 8'hD5);
        expect_read(16'hCF04, 8'hFF);
        expect_read(16'hCF02, 8'h0C);
        pointer(24'hFFFFFF);
        expect_read(16'hCF03, 8'hFF);
        expect_read(16'hCF02, 8'h00);

        apple_write(16'hCF05, 8'h00);
        apple_write(16'hCF06, 8'h00);
        expect_read(16'hCDFF, 8'hA8);
        apple_write(16'hCF05, 8'h01);
        expect_read(16'hCC00, 8'hC6);
        apple_write(16'hCF0E, 8'h00);
        apple_write(16'hCF0F, 8'hFF);
        expect_read(16'hCEFF, 8'hB7);
        apple_write(16'hCF0E, 8'h0B);
        expect_read(16'hCEFF, 8'hD5);
        apple_write(16'hCF05, 8'h0B);
        apple_write(16'hCF06, 8'hFF);
        expect_read(16'hCCFF, 8'hD5);
        expect_read(16'hCD00, 8'hFF);

        pointer(24'h000001);
        backend_blocked = 1'b1;
        apple_write(16'hCF04, 8'h5C);
        apple_write(16'hCF04, 8'hC5);
        expect_read(16'hCF04, 8'hC5);
        expect_axi(8'h01, 32'h0, 32'h40);
        backend_blocked = 1'b0;
        repeat (25) @(negedge clk);
        expect_axi(8'h01, 32'h40, 32'h40);
        if (backing[0][15:0] !== 16'hC511 || write_count != 2) begin
            $fatal(1, "Queued writes did not drain in order or touched the wrong lane");
        end
        expect_read(16'hCF04, 8'hC5);

        // This backend cannot answer from serve_en alone. A matching early
        // address gets the line before the physical response window opens.
        backend_latency = READ_WAIT + 5;
        start_early_read(16'hCF04, 1'b1);
        expect_quiet(READ_WAIT + 9);
        confirm_early_read(16'hCF04);
        if (!ab_write.wr_data_en || ab_write.wr_data != 8'hC5) begin
            $fatal(1, "Matching early read was not available at serve_en");
        end
        finish_cycle('0);

        // The two registers map the same RAM byte. The card must compare the
        // Apple address as well. The early CF03 sample must not increment it.
        backend_latency = 2;
        start_early_read(16'hCF03, 1'b1);
        expect_quiet(10);
        backing[0][15:8] = 8'hE6;
        confirm_early_read(16'hCF04);
        if (ab_write.wr_data_en) $fatal(1, "Mismatched early address drove the bus");
        repeat (8) @(negedge clk);
        if (!ab_write.wr_data_en || ab_write.wr_data != 8'hE6) begin
            $fatal(1, "Mismatched early read did not fetch the authoritative byte");
        end
        finish_cycle('0);
        expect_read(16'hCF00, 8'h01);

        // A native-looking early address must not start a request during DMA.
        start_early_read(16'hCF04, 1'b0);
        repeat (8) begin
            if (mem_req_valid) $fatal(1, "DMA cycle started an early read");
            @(negedge clk);
        end
        confirm_early_read(16'hCF04);
        repeat (8) @(negedge clk);
        if (!ab_write.wr_data_en || ab_write.wr_data != 8'hE6) begin
            $fatal(1, "DMA cycle did not use the authoritative read");
        end
        finish_cycle('0);

        // A pointer write to the B0000 region also writes its CGA alias.
        // The page window writes the selected RAM byte only.
        pointer(24'h0B0013);
        apple_write(16'hCF04, 8'hA9);
        repeat (22) @(negedge clk);
        if (backing[20'hB0010 >> 3][31:24] !== 8'hA9 ||
            backing[20'hB8010 >> 3][31:24] !== 8'hA9) begin
            $fatal(1, "Pointer write did not reach both CGA mappings");
        end
        apple_write(16'hCF0E, 8'h0B);
        apple_write(16'hCF0F, 8'h00);
        apple_write(16'hCE13, 8'hB8);
        repeat (12) @(negedge clk);
        if (backing[20'hB0010 >> 3][31:24] !== 8'hB8 ||
            backing[20'hB8010 >> 3][31:24] !== 8'hA9) begin
            $fatal(1, "A page-window write changed the CGA alias");
        end

        // The port window, pointer, page windows and PS use one byte store.
        alias_reads_before = read_count;
        alias_writes_before = write_count;
        pointer(24'h0BF022);
        start_early_read(16'hCF04, 1'b1);
        expect_quiet(10);
        confirm_early_read(16'hCF04);
        // Port RAM replies start three clocks after serve.
        repeat (2) begin
            if (ab_write.wr_data_en) $fatal(1, "Port RAM read drove before its data was ready");
            @(negedge clk);
        end
        if (!ab_write.wr_data_en || ab_write.wr_data != 8'h4A) begin
            $fatal(1, "Port RAM pointer read did not use the local byte");
        end
        finish_cycle('0);
        apple_write(16'hCF05, 8'h0B);
        apple_write(16'hCF06, 8'hF0);
        apple_write(16'hCF0E, 8'h0B);
        apple_write(16'hCF0F, 8'hF0);
        expect_read(16'hCC22, 8'h4A);
        expect_read(16'hCE22, 8'h4A);
        apple_write(16'hCC22, 8'h19);
        expect_read(16'hC822, 8'h19);
        apple_write(16'hCE22, 8'h28);
        expect_read(16'hCF04, 8'h28);
        axi_write(8'h03, 32'h00000022);
        expect_axi(8'h04, 32'h00000028);
        axi_write(8'h04, 32'h00000037);
        expect_read(16'hCC22, 8'h37);
        if (read_count != alias_reads_before || write_count != alias_writes_before) begin
            $fatal(1, "Local port RAM changed DDR counts from %0d/%0d to %0d/%0d, last %05x",
                   alias_reads_before, alias_writes_before, read_count, write_count, accepted_addr);
        end

        apple_write(16'hCF03, 8'h6C);
        expect_read(16'hCF00, 8'h23);
        expect_read(16'hC822, 8'h6C);
        expect_axi(8'h01, 32'h40, 32'h40);
        if (write_count != alias_writes_before + 1 ||
            backing[20'hBB020 >> 3][23:16] !== 8'h6C ||
            backing[20'hBF020 >> 3][23:16] !== 8'h99) begin
            $fatal(1, "Port RAM pointer write did not update only its DDR mirror");
        end

        backend_blocked = 1'b1;
        pointer(24'h000001);
        for (int i = 0; i < 4; i++) begin
            apple_write(16'hCF04, 8'(i));
        end
        pointer(24'h0BF022);
        apple_write(16'hCF04, 8'hFF);
        expect_read(16'hC822, 8'h6C);
        expect_axi(8'h06, 32'd1);
        backend_blocked = 1'b0;
        repeat (36) @(negedge clk);
        if (backing[20'hBB020 >> 3][23:16] !== 8'h6C) begin
            $fatal(1, "Rejected port RAM write changed its DDR mirror");
        end
        axi_write(8'h08, 32'd1);

        apple_write(16'hCF0F, 8'hF3);
        apple_write(16'hCED6, 8'h0C);
        apple_write(16'hCED7, 8'h12);
        expect_axi(8'h0C, 32'h00120000, 32'h00FF0000);
        apple_write(16'hCBFF, 8'h2E);
        pointer(24'h0BF3FF);
        expect_read(16'hCF03, 8'h2E);
        expect_read(16'hCF04, 8'hD3);
        pointer(24'h000001);

        backend_latency = READ_WAIT + 8;
        start_cycle(16'hCF04, 1'b1, 1'b1);
        expect_quiet(READ_WAIT + 2);
        finish_cycle('0);
        start_cycle(16'hC822, 1'b1, 1'b1);
        repeat (2) @(negedge clk);
        repeat (12) begin
            if (!ab_write.wr_data_en || ab_write.wr_data != 8'h6C) begin
                $fatal(1, "Late RAM response corrupted a local port read");
            end
            @(negedge clk);
        end
        finish_cycle('0);
        expect_axi(8'h05, 32'd1);

        backend_latency = 4;
        start_cycle(16'hCF04, 1'b1, 1'b1);
        repeat (2) @(negedge clk);
        card_enabled = 1'b0;
        expect_quiet(10);
        finish_cycle('0);
        card_enabled = 1'b1;
        expect_axi(8'h05, 32'd1);
        pointer(24'h000001);
        backend_latency = 2;
        inject_error = 1'b1;
        start_cycle(16'hCF04, 1'b1, 1'b1);
        expect_quiet(READ_WAIT + 2);
        finish_cycle('0);
        inject_error = 1'b0;
        expect_axi(8'h07, 32'd1);

        backend_blocked = 1'b1;
        for (int i = 0; i < 5; i++) begin
            apple_write(16'hCF04, 8'(i));
        end
        expect_read(16'hCF04, 8'h03);
        expect_axi(8'h06, 32'd1);
        backend_blocked = 1'b0;
        repeat (36) @(negedge clk);
        expect_axi(8'h01, 32'h40, 32'h40);

        apple_write(16'hCF2B, 8'hB0);
        calibration_count(16'h04A9);
        calibration_count(16'h006A);

        apple_write(16'hCF30, 8'h30);
        expect_axi(8'h01, 32'h01, 32'h07);
        axi_write(8'h02, 32'h800F0060);
        expect_read(16'hCF30, 8'h80);
        expect_read(16'hCF32, 8'h80);
        expect_read(16'hCF34, 8'h60);
        expect_read(16'hCF35, 8'h00);
        if (ab_write.assert_irq) $fatal(1, "A masked pending request asserted IRQ");
        apple_write(16'hCF31, 8'h80);
        if (!ab_write.assert_irq) $fatal(1, "Unmask did not expose the pending IRQ");
        apple_write(16'hCF30, 8'h80);
        if (ab_write.assert_irq) $fatal(1, "Mask did not release IRQ");
        apple_write(16'hCF31, 8'h80);
        if (!ab_write.assert_irq) $fatal(1, "Unmask lost the request");
        pointer(24'h0BF060);
        apple_write(16'hCF03, 8'h18);
        apple_write(16'hC861, 8'h27);
        apple_write(16'hCF32, 8'h80);
        if (ab_write.assert_irq) $fatal(1, "ACK did not release IRQ");
        apple_write(16'hC860, 8'hFF);
        expect_axi(8'h0A, 32'h00002718);
        expect_axi(8'h01, 32'h00000100, 32'h0000FF20);

        // Keep both seed bytes pending until the PS and Apple write together.
        // CF04 and the PS port index name the same BF RAM byte. The Apple
        // byte wins, and the other word byte still comes from the seed read.
        apple_write(16'hC862, 8'h10);
        apple_write(16'hC863, 8'h5E);
        pointer(24'h0BF062);
        axi_write(8'h03, 32'h00000062);
        as_common.araddr = 8'h04;
        axi_write(8'h02, 32'h800F0062);
        simultaneous_port_writes(16'hCF04, 8'hA8, 8'h13, 1'b1, 1'b1);
        expect_read(16'hC862, 8'hA8);
        expect_read(16'hC863, 8'h5E);
        apple_write(16'hCF32, 8'h80);
        expect_axi(8'h0A, 32'h00005EA8);
        expect_axi(8'h01, 32'h00000200, 32'h0000FF20);

        // Different targets can write on the same clock.
        axi_write(8'h03, 32'h00000065);
        simultaneous_port_writes(16'hC864, 8'hC9, 8'hD4, 1'b0, 1'b0);
        expect_read(16'hC864, 8'hC9);
        expect_read(16'hC865, 8'hD4);

        axi_write(8'h09, 32'h00011E81);
        if (!ab_write.assert_irq) $fatal(1, "A key did not request service");
        expect_read(16'hCF30, 8'h80);
        expect_read(16'hCF36, 8'h81);
        expect_read(16'hCF32, 8'h40);
        expect_read(16'hCF33, 8'h1E);
        expect_read(16'hCF32, 8'h00);
        if (ab_write.assert_irq) $fatal(1, "Scan code read did not release IRQ");

        apple_write(16'hCF2B, 8'h10);
        apple_write(16'hCF28, 8'h02);
        repeat (300) @(negedge clk);
        if (!ab_write.assert_irq) $fatal(1, "Timer did not request service");
        expect_read(16'hCF30, 8'h80);
        expect_read(16'hCF32, 8'h20);
        apple_write(16'hCF32, 8'h20);
        expect_read(16'hCF32, 8'h00);
        if (ab_write.assert_irq) $fatal(1, "Timer service did not release IRQ");

        // Generic control bytes read zero after reset. Apple and PS writes
        // share them. An Apple write wins a same-clock write to its byte, and
        // a later PS write wins over an earlier Apple write.
        expect_read(16'hCF10, 8'h00);
        expect_read(16'hCF3F, 8'h00);
        apple_write(16'hCF10, 8'h5A);
        expect_read(16'hCF10, 8'h5A);
        axi_write(8'h03, 32'h00000410);
        expect_axi(8'h04, 32'h0000005A);
        axi_write(8'h04, 32'h000000C3);
        expect_read(16'hCF10, 8'hC3);
        axi_write(8'h03, 32'h00000430);
        axi_write(8'h04, 32'h0000003F);
        expect_read(16'hCF30, 8'h3F, 8'h3F);
        axi_write(8'h04, 32'h00000000);
        expect_read(16'hCF30, 8'h00, 8'h3F);
        axi_write(8'h03, 32'h00000411);
        simultaneous_ctrl_writes(16'hCF11, 8'h77, 8'h88);
        expect_read(16'hCF11, 8'h77);
        expect_axi(8'h04, 32'h00000077);
        axi_write(8'h03, 32'h00000412);
        simultaneous_ctrl_writes(16'hCF13, 8'h31, 8'h42);
        expect_read(16'hCF13, 8'h31);
        expect_read(16'hCF12, 8'h42);
        axi_write(8'h03, 32'h00000414);
        start_cycle(16'hCF14, 1'b0, 1'b1);
        ab_read.data = 8'h11;
        ab_read.data_en = 1'b1;
        @(negedge clk);
        ab_read.data_en = 1'b0;
        as_common.awaddr = 8'h04;
        as_common.wdata = 32'h00000022;
        as_common.wstrb = 4'hF;
        axi.awvalid = 1'b1;
        @(negedge clk);
        axi.awvalid = 1'b0;
        expect_read(16'hCF14, 8'h22);
        expect_axi(8'h04, 32'h00000022);

        // Queued writes through the page windows forward to every window that
        // maps the same RAM byte, including the high CC page.
        backend_blocked = 1'b1;
        apple_write(16'hCF05, 8'h01);
        apple_write(16'hCF06, 8'h23);
        apple_write(16'hCD45, 8'h9B);
        apple_write(16'hCC12, 8'h6D);
        expect_read(16'hCD45, 8'h9B);
        expect_read(16'hCC12, 8'h6D);
        apple_write(16'hCF0E, 8'h01);
        apple_write(16'hCF0F, 8'h24);
        expect_read(16'hCE45, 8'h9B);
        pointer(24'h012312);
        expect_read(16'hCF04, 8'h6D);
        expect_axi(8'h01, 32'h0, 32'h40);
        backend_blocked = 1'b0;
        repeat (25) @(negedge clk);
        expect_axi(8'h01, 32'h40, 32'h40);
        if (backing[20'h12440 >> 3][47:40] !== 8'h9B ||
            backing[20'h12310 >> 3][23:16] !== 8'h6D) begin
            $fatal(1, "Page-window writes did not drain to the expected bytes");
        end
        expect_read(16'hCD45, 8'h9B);

        apple_write(16'hCF31, 8'h40);
        expect_axi(8'h01, 32'h08, 32'h08);
        expect_read(16'hCF30, 8'h40);
        axi_write(8'h08, 32'h00000004);
        expect_read(16'hCF30, 8'h40);
        axi_write(8'h08, 32'h00000104);
        expect_read(16'hCF30, 8'h00);
        apple_write(16'hCF31, 8'h40);
        axi_write(8'h08, 32'h00000104);
        expect_read(16'hCF30, 8'h40);
        apple_write(16'hCF30, 8'h40);
        expect_axi(8'h01, 32'h00, 32'h08);

        apple_write(16'hCF31, 8'h10);
        expect_axi(8'h01, 32'h00010002, 32'h00FF0002);
        axi_write(8'h02, 32'h80050060);
        expect_axi(8'h01, 32'h00, 32'h20);
        apple_write(16'hCF30, 8'h10);
        axi_write(8'h02, 32'h80150060);
        expect_axi(8'h01, 32'h20, 32'h20);
        card_enabled = 1'b0;
        expect_quiet(4);
        if (ab_write.assert_irq) $fatal(1, "Disabled slot held IRQ");
        expect_axi(8'h01, 32'h06, 32'h27);

        // A fabric reset clears the control bytes again.
        card_enabled = 1'b1;
        rstn = 1'b0;
        repeat (2) @(negedge clk);
        rstn = 1'b1;
        expect_read(16'hC500, 8'hFF);
        expect_read(16'hCF10, 8'h00);
        expect_read(16'hCF11, 8'h00);

        if (read_count < 10) $fatal(1, "The synthetic backend was not exercised");
        $display("PC TRANSPORTER CARD PASS");
        $finish;
    end
endmodule
