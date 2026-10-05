`timescale 1ns / 1ps

module tb_ssi263_filter_register;
    logic clk = 1'b0;
    logic rstn = 1'b0;
    logic apple_res = 1'b1;
    logic write_strobe = 1'b0;
    logic [2:0] reg_index = 3'd0;
    logic [7:0] wdata = 8'd0;
    logic d7, irq;
    logic [6:0] ifr_set, ifr_clr;
    logic signed [15:0] audio;
    integer checks = 0;

    always #5 clk = ~clk;
    ssi263_bus_wrapper #(.SSI263_TYPE(2), .HAS_SC01(1'b1)) dut (
        .clk(clk), .rstn(rstn), .apple_res(apple_res), .card_enabled(1'b1),
        .card_mode(3'd5), .audio_tick(1'b0), .xck_ce(1'b0),
        .ssi_write_strobe(write_strobe), .ssi_reg(reg_index), .ssi_wdata(wdata),
        .ssi_d7(d7), .votrax_write_strobe(1'b0), .votrax_wdata(8'd0),
        .via_pcr(8'd0), .via_ifr_set(ifr_set), .via_ifr_clr(ifr_clr),
        .audio(audio), .direct_irq(irq), .dbg_backend_done(), .dbg_enable_ints()
    );

    task automatic require(input logic condition, input string message);
        checks = checks + 1;
        if (condition !== 1'b1) $fatal(1, "SSI263 FILTER REGISTER FAIL: %s", message);
    endtask

    task automatic write_reg(input logic [2:0] address, input logic [7:0] value);
        @(negedge clk);
        reg_index = address;
        wdata = value;
        write_strobe = 1'b1;
        @(negedge clk);
        write_strobe = 1'b0;
        repeat (3) @(negedge clk);
    endtask

    initial begin
        repeat (3) @(negedge clk);
        rstn = 1'b1;
        repeat (3) @(negedge clk);
        require(dut.filter_freq_q == 0 && audio == 0 && dut.ctrl_art_amp_q[7],
                "power reset default/mute changed");
        write_reg(0, 8'hAD);
        write_reg(1, 8'h52);
        write_reg(2, 8'hB8);
        write_reg(3, 8'h0F);
        // A real backend completion interface pulse establishes pending IRQ.
        force dut.backend_done = 1'b1;
        @(negedge clk);
        release dut.backend_done;
        @(negedge clk);
        require(d7 && irq, "fixture failed to establish pending request");
        for (integer address = 4; address < 8; address = address + 1) begin
            for (integer value = 0; value < 256; value = value + 1) begin
                @(negedge clk);
                reg_index = address;
                wdata = value;
                write_strobe = 1'b1;
                @(negedge clk);
                require(dut.filter_freq_q == value, "FF address/data decode failed");
                require(!dut.backend_start_q, "FF write restarted speech");
                require(d7 && irq && ifr_clr == 0, "FF write acknowledged IRQ");
                require(dut.duration_phoneme_q == 8'hAD &&
                        dut.inflection_q == 8'h52 && dut.rate_inflection_q == 8'hB8 &&
                        dut.ctrl_art_amp_q == 8'h0F, "FF changed another register");
                write_strobe = 1'b0;
                repeat (2) @(negedge clk);
                require(dut.formant_backend_i.filter_freq == value,
                        "stored FF did not reach the backend");
            end
        end
        write_reg(4, 8'hEA);
        apple_res = 1'b0;
        repeat (3) @(negedge clk);
        require(dut.filter_freq_q == 8'hEA && audio == 0 && dut.ctrl_art_amp_q[7],
                "AP warm reset must retain FF and mute audio");
        apple_res = 1'b1;
        repeat (3) @(negedge clk);
        rstn = 1'b0;
        repeat (3) @(negedge clk);
        require(dut.filter_freq_q == 0 && audio == 0,
                "power reset must restore FF default and silence");
        $display("SSI263 FILTER REGISTER PASS checks=%0d writes=1024", checks);
        $finish;
    end
endmodule
