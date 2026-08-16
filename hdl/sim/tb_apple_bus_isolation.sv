`timescale 1ns / 1ps

module tb_apple_bus_isolation;

    timeunit 1ns;
    timeprecision 1ps;

    logic clk = 1'b0;
    always #3.75 clk = ~clk;

    logic rstn = 1'b0;
    logic physical_bus_isolate = 1'b0;
    logic inh_allowed = 1'b1;

    tri [7:0]  apple_data_pin;
    tri [15:0] apple_addr_pin;
    tri        apple_rw_pin;
    logic      apple_phi0_pin = 1'b0;
    logic      apple_m2sel_pin = 1'b0;
    logic      apple_m2b0_pin = 1'b0;
    tri        apple_inh_pin;
    tri        apple_res_pin;
    tri        apple_irq_pin;
    tri        apple_rdy_pin;
    tri        apple_dma_pin;
    tri        apple_nmi_pin;

    pullup (apple_inh_pin);
    pullup (apple_res_pin);
    pullup (apple_irq_pin);
    pullup (apple_rdy_pin);
    pullup (apple_dma_pin);
    pullup (apple_nmi_pin);

    logic tini_oe_pin;
    logic tini_addr_dir_pin;
    logic tini_data_dir_pin;
    globals::AppleBus_read ab_read;
    globals::AppleBus_write ab_write = '0;

    apple_bus_wrapper dut (
        .clk(clk),
        .rstn(rstn),
        .physical_bus_isolate(physical_bus_isolate),
        .res_filtered_out(),
        .dbg_lost_cycle_count(),
        .dbg_bus_quality(),
        .dbg_tap_mismatch(),
        .dbg_strobe_anom(),
        .dbg_tap_last(),
        .dbg_ghost_write(),
        .dbg_clear(1'b0),
        .inh_allowed(inh_allowed),
        .gs_m2_qualify(1'b0),
        .m2sel_active_high(1'b0),
        .host_is_iiplus(1'b0),
        .iiplus_dma_refresh_active(1'b0),
        .apple_data_pin(apple_data_pin),
        .apple_addr_pin(apple_addr_pin),
        .apple_rw_pin(apple_rw_pin),
        .apple_phi0_pin(apple_phi0_pin),
        .apple_m2sel_pin(apple_m2sel_pin),
        .apple_m2b0_pin(apple_m2b0_pin),
        .apple_inh_pin(apple_inh_pin),
        .apple_res_pin(apple_res_pin),
        .apple_irq_pin(apple_irq_pin),
        .apple_rdy_pin(apple_rdy_pin),
        .apple_dma_pin(apple_dma_pin),
        .apple_nmi_pin(apple_nmi_pin),
        .tini_oe_pin(tini_oe_pin),
        .tini_5v_pin(1'b0),
        .tini_addr_dir_pin(tini_addr_dir_pin),
        .tini_data_dir_pin(tini_data_dir_pin),
        .ab_read(ab_read),
        .ab_write(ab_write)
    );

    task automatic check(input logic condition, input string message);
        if (condition !== 1'b1)
            $fatal(1, "%s", message);
    endtask

    initial begin
        repeat (5) @(posedge clk);
        rstn = 1'b1;
        repeat (3) @(posedge clk);

        ab_write.wr_addr       = 16'h1234;
        ab_write.wr_rw         = 1'b0;
        ab_write.wr_addr_rw_en = 1'b1;
        ab_write.assert_irq    = 1'b1;
        ab_write.assert_dma    = 1'b1;
        force dut.apple_inh_assert = 1'b1;
        force dut.apple_data_enable_unisolated = 1'b1;
        force dut.physical_data_q = 8'hA5;
        #1;

        check(tini_oe_pin == 1'b0, "host mode must enable main transceiver");
        check(tini_addr_dir_pin == 1'b1, "host address direction");
        check(tini_data_dir_pin == 1'b1, "host data direction");
        check(apple_addr_pin === 16'h1234, "host address drive");
        check(apple_rw_pin === 1'b0, "host R/W drive");
        check(apple_data_pin === 8'hA5, "host data drive");
        check(apple_irq_pin === 1'b0, "host IRQ drive");
        check(apple_dma_pin === 1'b0, "host DMA drive");
        check(apple_inh_pin === 1'b0, "host INH drive");

        // Isolation must take effect without a fabric or Apple clock edge.
        physical_bus_isolate = 1'b1;
        #1;
        check(tini_oe_pin == 1'b1, "isolation must disable main transceiver");
        check(tini_addr_dir_pin == 1'b0, "isolation must clear address direction");
        check(tini_data_dir_pin == 1'b0, "isolation must clear data direction");
        check(apple_addr_pin === 16'hzzzz, "isolation must release address");
        check(apple_rw_pin === 1'bz, "isolation must release R/W");
        check(apple_data_pin === 8'hzz, "isolation must release data");
        check(apple_irq_pin === 1'b1, "isolation must release IRQ");
        check(apple_dma_pin === 1'b1, "isolation must release DMA");
        check(apple_inh_pin === 1'b1, "isolation must release INH");

        // Requests may remain asserted internally; the physical kill must
        // continue to win until isolation is removed.
        repeat (3) @(posedge clk);
        check(tini_oe_pin == 1'b1 && tini_addr_dir_pin == 1'b0 &&
              tini_data_dir_pin == 1'b0,
              "registered bus state escaped physical isolation");
        check(apple_irq_pin === 1'b1 && apple_dma_pin === 1'b1 &&
              apple_inh_pin === 1'b1,
              "control request escaped physical isolation");

        release dut.apple_inh_assert;
        release dut.apple_data_enable_unisolated;
        release dut.physical_data_q;
        $display("APPLE BUS ISOLATION PASS");
        $finish;
    end

    initial begin
        #10000;
        $fatal(1, "APPLE BUS ISOLATION TIMEOUT");
    end

endmodule
