`timescale 1ns / 1ps

// Compare stock CPU pin writes with vTW synchronous bus-master writes.
// Both paths use the production wrapper, soft switches, Phasor and SSI cores.
// The vTW sync port is driven directly: this isolates pin timing and register
// capture from the CPU instruction stream and software's speed detection.
module tb_vtw_ssi263_bus;
    `define PRIMARY card_i.ssi263_primary_i.bus_wrapper_i
    `define SECONDARY card_i.ssi263_secondary_i.bus_wrapper_i

    logic clk = 0, rstn = 0, phi0 = 0, q3 = 0;
    logic enable = 0, stock_active = 0;
    logic [15:0] stock_addr = 16'hFFFF;
    logic [7:0] stock_data = 0;
    logic stock_rw = 1;
    wire [15:0] apple_addr;
    wire [7:0] apple_data;
    wire apple_rw, apple_inh, apple_res, apple_irq, apple_rdy, apple_dma, apple_nmi;
    globals::AppleBus_read bus;
    globals::AppleBus_write merged_write, vtw_write, card_write;
    globals::SoftSwitchState sss;
    logic req_valid = 0, req_ready, req_rw = 1, resp_valid, bus_owned;
    logic [15:0] req_addr = 0;
    logic [7:0] req_data = 0;
    logic [31:0] write_check, bus_faults;
    integer checks = 0, transfers = 0, primary_writes = 0, secondary_writes = 0;
    integer primary_native = 0, secondary_native = 0;
    integer q3_div = 0, audio_div = 0;
    logic audio_tick = 0;
    logic expect_active = 0;
    logic [15:0] expect_addr;
    logic [7:0] expect_data;
    logic expect_rw;

    always #3.75 clk = ~clk;
    always #490 phi0 = ~phi0;
    always @(negedge clk) begin
        q3_div = (q3_div + 1) % 65;
        q3 = q3_div >= 32;
        audio_div = (audio_div + 1) % 2778;
        audio_tick = (audio_div == 0);
    end

    assign (weak0, weak1) apple_addr = 16'hFFFF;
    assign (weak0, weak1) apple_data = 8'hFF;
    assign (weak0, weak1) apple_rw = 1'b1;
    assign (weak0, weak1) apple_inh = 1'b1;
    assign (weak0, weak1) apple_res = 1'b1;
    assign (weak0, weak1) apple_irq = 1'b1;
    assign (weak0, weak1) apple_rdy = 1'b1;
    assign (weak0, weak1) apple_dma = 1'b1;
    assign (weak0, weak1) apple_nmi = 1'b1;
    assign apple_addr = !merged_write.assert_dma ? stock_addr : 16'hZZZZ;
    assign apple_rw = !merged_write.assert_dma ? stock_rw : 1'bZ;
    assign apple_data = !merged_write.assert_dma && stock_active && !stock_rw && phi0
                      ? stock_data : 8'hZZ;

    apple_bus_wrapper wrapper_i (
        .clk(clk), .rstn(rstn), .physical_bus_isolate(1'b0),
        .inh_allowed(1'b1), .physical_slave_select_ok(1'b1),
        .physical_irq_allowed(1'b1), .gs_m2_qualify(1'b0),
        .m2sel_active_high(1'b0), .host_is_iiplus(1'b0),
        .iiplus_dma_refresh_active(1'b0), .dbg_clear(1'b0),
        .apple_data_pin(apple_data), .apple_addr_pin(apple_addr),
        .apple_rw_pin(apple_rw), .apple_phi0_pin(phi0),
        .apple_m2sel_pin(1'b0), .apple_m2b0_pin(1'b0), .apple_devsel_n_pin(1'b1),
        .apple_inh_pin(apple_inh), .apple_res_pin(apple_res),
        .apple_irq_pin(apple_irq), .apple_rdy_pin(apple_rdy),
        .apple_dma_pin(apple_dma), .apple_nmi_pin(apple_nmi), .tini_5v_pin(1'b0),
        .ab_read(bus), .ab_write(merged_write)
    );
    apple_bus_write_arbiter #(.NUM_CLIENTS(2)) arbiter_i (
        .inh_allowed(1'b1), .client_writes({vtw_write, card_write}),
        .ab_write(merged_write)
    );
    soft_switch_manager soft_switch_i (
        .clk(clk), .rstn(rstn), .ramworks_en(1'b0), .ab_read(bus), .sss(sss)
    );
    vtw_bus_engine engine_i (
        .clk(clk), .rstn(rstn), .enable(enable), .host_is_iiplus(1'b0),
        .ab_read(bus), .ab_write(vtw_write),
        .data_drive_in(merged_write.wr_data_en), .data_drive_value_in(merged_write.wr_data),
        .dbg_clear(1'b0), .dbg_trace_freeze(1'b0),
        .sync_req_valid(req_valid), .sync_req_ready(req_ready),
        .sync_req_addr(req_addr), .sync_req_rw(req_rw), .sync_req_wdata(req_data),
        .sync_resp_valid(resp_valid), .post_we(1'b0), .post_addr(16'd0), .post_wdata(8'd0),
        .bus_owned(bus_owned), .dbg_sync_write_check(write_check), .dbg_bus_faults(bus_faults)
    );
    mockingboard card_i (
        .clk(clk), .rstn(rstn), .apple_q3_raw(q3), .ab_read(bus), .sss(sss),
        .slot_assign(3'd4), .pan(48'h888888888888), .ssi_pan(8'hF0),
        .audio_control(32'h10000000), .audio_sample_tick(audio_tick), .ab_write(card_write)
    );

    task automatic require(input logic condition, input string message);
        checks = checks + 1;
        if (condition !== 1'b1)
            $fatal(1, "VTW SSI263 BUS FAIL: %s time=%0t vtw=%0d addr=%h data=%h",
                   message, $time, enable, expect_addr, expect_data);
    endtask

    // Check the exact events seen by each physical socket and by its delayed
    // native engine, so a duplicate or misplaced write cannot pass merely
    // because the final FF register happens to contain the correct byte.
    always @(posedge clk) begin
        if (rstn) begin
            if (card_i.ssi_primary_write || card_i.ssi_secondary_write) begin
                require(expect_active && !expect_rw, "unexpected SSI write");
                require(bus.addr === expect_addr && bus.data === expect_data,
                        "captured SSI address/data differs from requested write");
            end
            if (card_i.ssi_primary_write) primary_writes = primary_writes + 1;
            if (card_i.ssi_secondary_write) secondary_writes = secondary_writes + 1;
            if (`PRIMARY.native_event_q.write_strobe) begin
                primary_native = primary_native + 1;
                require(expect_active && `PRIMARY.native_event_q.write_reg === expect_addr[2:0] &&
                        `PRIMARY.native_event_q.write_data === expect_data,
                        "primary native event changed register/data");
            end
            if (`SECONDARY.native_event_q.write_strobe) begin
                secondary_native = secondary_native + 1;
                require(expect_active && `SECONDARY.native_event_q.write_reg === expect_addr[2:0] &&
                        `SECONDARY.native_event_q.write_data === expect_data,
                        "secondary native event changed register/data");
            end
        end
    end

    task automatic access_bus(input logic [15:0] address, input logic [7:0] value,
                              input logic reading);
        integer p_before, s_before, pn_before, sn_before, p_expected, s_expected;
        p_before = primary_writes;
        s_before = secondary_writes;
        pn_before = primary_native;
        sn_before = secondary_native;
        p_expected = !reading && address[15:8] == 8'hC4 && address[6];
        s_expected = !reading && address[15:8] == 8'hC4 && address[5];
        expect_active = 1;
        expect_addr = address;
        expect_data = value;
        expect_rw = reading;
        if (enable) begin
            @(negedge clk);
            req_addr = address;
            req_data = value;
            req_rw = reading;
            req_valid = 1;
            do @(posedge clk); while (!req_ready);
            @(negedge clk);
            req_valid = 0;
            do @(negedge clk); while (!resp_valid);
            // Leave time for native_event_q and the engine's idle park.
            @(negedge phi0);
            repeat (20) @(negedge clk);
        end else begin
            @(negedge phi0);
            stock_addr = address;
            stock_data = value;
            stock_rw = reading;
            stock_active = 1;
            do @(negedge clk); while (!bus.data_en);
            @(negedge phi0);
            stock_addr = 16'hFFFF;
            stock_rw = 1;
            stock_active = 0;
            repeat (20) @(negedge clk);
        end
        require(primary_writes - p_before == p_expected &&
                secondary_writes - s_before == s_expected,
                "SSI socket did not accept each requested write exactly once");
        require(primary_native - pn_before == p_expected &&
                secondary_native - sn_before == s_expected,
                "SSI native engine did not accept each write exactly once");
        expect_active = 0;
        transfers = transfers + 1;
    endtask

    task automatic check_filters(input logic [7:0] primary_ff, input logic [7:0] secondary_ff);
        require(`PRIMARY.filter_freq_q === primary_ff && `SECONDARY.filter_freq_q === secondary_ff,
                "socket FF latch mismatch");
        require(`PRIMARY.native_engine_i.controller_i.debug_filter_frequency === primary_ff &&
                `SECONDARY.native_engine_i.controller_i.debug_filter_frequency === secondary_ff,
                "native controller FF latch mismatch");
    endtask

    task automatic demo_sequence;
        access_bus(16'hC443, 8'h80, 0);
        access_bus(16'hC440, 8'hC0, 0);
        access_bus(16'hC441, 8'h40, 0);
        access_bus(16'hC442, 8'hA8, 0);
        access_bus(16'hC443, 8'h5A, 0);
        access_bus(16'hC444, 8'hE8, 0);
        require(`PRIMARY.duration_phoneme_q === 8'hC0 && `PRIMARY.inflection_q === 8'h40 &&
                `PRIMARY.rate_inflection_q === 8'hA8 && `PRIMARY.ctrl_art_amp_q === 8'h5A,
                "Phasor demo initialization changed speech parameters");
        require(`PRIMARY.native_engine_i.controller_i.debug_filter_frequency === 8'hE8,
                "Phasor demo default FF did not reach native controller");
    endtask

    initial begin
        repeat (12) @(negedge clk);
        rstn = 1;
        repeat (5) @(negedge phi0);
        for (integer accelerated = 0; accelerated < 2; accelerated = accelerated + 1) begin
            if (accelerated) begin
                @(negedge clk);
                enable = 1;
                wait (bus_owned && req_ready);
            end
            for (integer native_mode = 0; native_mode < 2; native_mode = native_mode + 1) begin
                access_bus(native_mode ? 16'hC0CD : 16'hC0C8, 0, 1);
                require(card_i.phasor_mode_q == (native_mode ? 5 : 0), "card mode selection failed");
                demo_sequence();
                // Every FF byte and alias, on each socket and both together.
                for (integer alias_reg = 4; alias_reg < 8; alias_reg = alias_reg + 1) begin
                    for (integer value = 0; value < 256; value = value + 1) begin
                        access_bus(16'hC440 + 16'(alias_reg), 8'(value), 0);
                        access_bus(16'hC420 + 16'(alias_reg), 8'(255 - value), 0);
                        check_filters(8'(value), 8'(255 - value));
                        access_bus(16'hC460 + 16'(alias_reg), 8'(value), 0);
                        check_filters(8'(value), 8'(value));
                    end
                end
                $display("VTW SSI263 BUS MATRIX vtw=%0d mode=%0d transfers=%0d",
                         accelerated, native_mode ? 5 : 0, transfers);
            end
        end
        repeat (3) @(negedge phi0);
        require(write_check == 0 && bus_faults == 0, "vTW physical loopback diagnostics found a mismatch");
        $display("VTW SSI263 BUS PASS checks=%0d transfers=%0d primary=%0d secondary=%0d",
                 checks, transfers, primary_writes, secondary_writes);
        $finish;
    end
    initial begin
        #100000000;
        $fatal(1, "VTW SSI263 BUS FAIL: timeout");
    end
endmodule
