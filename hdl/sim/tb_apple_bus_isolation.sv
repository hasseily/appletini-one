`timescale 1ns / 1ps

module tb_apple_bus_isolation;

    timeunit 1ns;
    timeprecision 1ps;

    logic clk = 1'b0;
    always #3.75 clk = ~clk;

    logic rstn = 1'b0;
    logic physical_bus_isolate = 1'b0;
    logic inh_allowed = 1'b1;
    logic physical_slave_select_ok = 1'b1;
    logic physical_irq_allowed = 1'b1;
    logic gs_m2_qualify = 1'b0;
    logic m2sel_active_high = 1'b0;
    logic host_is_iiplus = 1'b0;

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
        .physical_slave_select_ok(physical_slave_select_ok),
        .physical_irq_allowed(physical_irq_allowed),
        .gs_m2_qualify(gs_m2_qualify),
        .m2sel_active_high(m2sel_active_high),
        .host_is_iiplus(host_is_iiplus),
        .iiplus_dma_refresh_active(1'b0),
        .apple_data_pin(apple_data_pin),
        .apple_addr_pin(apple_addr_pin),
        .apple_rw_pin(apple_rw_pin),
        .apple_phi0_pin(apple_phi0_pin),
        .apple_m2sel_pin(apple_m2sel_pin),
        .apple_m2b0_pin(apple_m2b0_pin),
        .apple_devsel_n_pin(1'b1),
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

    logic [6:0] gate_inputs;
    logic gate_expected;
    localparam logic [63:0] ORIGINAL_DATA_ENABLE_INIT =
        64'hFFFF_FFFF_8088_8080;

    initial begin
        repeat (5) @(posedge clk);
        rstn = 1'b1;
        repeat (3) @(posedge clk);

        // Exercise the actual placed LUT for every original six-input tuple
        // and both isolation states. Masking both drive inputs must equal
        // the original truth table followed by an immediate isolation gate.
        force dut.bus_emit_state = gate_inputs[0];
        force dut.physical_data_en_safe = gate_inputs[1];
        force dut.physical_addr_rw_en_q = gate_inputs[4];
        force dut.data_override_safe = gate_inputs[5];
        force dut.physical_data_q = 8'hA5;
        force dut.iiplus_read_hold_active = 1'b0;
        for (int combination = 0; combination < 128; combination++) begin
            gate_inputs = combination[6:0];
            apple_phi0_pin = gate_inputs[2];
            host_is_iiplus = gate_inputs[3];
            physical_bus_isolate = gate_inputs[6];
            gate_expected = !gate_inputs[6] &&
                            ORIGINAL_DATA_ENABLE_INIT[gate_inputs[5:0]];
            #1;
            check(tini_data_dir_pin === gate_expected,
                  $sformatf("isolation LUT equivalence failed for %07b",
                            gate_inputs));
            check(apple_data_pin === (gate_expected ? 8'hA5 : 8'hzz),
                  "data drive must match the complete isolation truth table");
        end
        release dut.bus_emit_state;
        release dut.physical_data_en_safe;
        release dut.physical_addr_rw_en_q;
        release dut.data_override_safe;
        release dut.iiplus_read_hold_active;
        host_is_iiplus = 1'b0;
        physical_bus_isolate = 1'b0;
        $display("APPLE DATA ISOLATION LUT EQUIVALENCE PASS: 128 combinations");

        ab_write.wr_addr       = 16'h1234;
        ab_write.wr_rw         = 1'b0;
        ab_write.wr_addr_rw_en = 1'b1;
        ab_write.assert_irq    = 1'b1;
        ab_write.assert_dma    = 1'b1;
        force dut.apple_inh_assert = 1'b1;
        force dut.data_override_safe = 1'b1;
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
        check(tini_oe_pin == 1'b0,
              "isolation must retain the input-only clock monitor");
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
        check(tini_oe_pin == 1'b0 && tini_addr_dir_pin == 1'b0 &&
              tini_data_dir_pin == 1'b0,
              "input-only monitor direction escaped physical isolation");
        check(apple_irq_pin === 1'b1 && apple_dma_pin === 1'b1 &&
              apple_inh_pin === 1'b1,
              "control request escaped physical isolation");

        // UNKNOWN/IIgs policy must atomically release every bus-master pin,
        // even while the client tuple and registered data remain asserted.
        release dut.data_override_safe;
        physical_bus_isolate = 1'b0;
        inh_allowed = 1'b1;
        apple_phi0_pin = 1'b1;
        force dut.bus_emit_state = 1'b1;
        force dut.physical_data_en_q = 1'b1;
        force dut.physical_data_q = 8'h5A;
        force dut.ab_read_r.cycle_valid = 1'b1;
        force dut.ab_read_r.rw = 1'b1;
        @(posedge clk);
        #1;
        inh_allowed = 1'b0;
        #1;
        check(tini_addr_dir_pin == 1'b0, "unsafe mode clears address direction");
        check(tini_data_dir_pin == 1'b0, "unsafe mode clears master data direction");
        check(apple_addr_pin === 16'hzzzz, "unsafe mode releases address");
        check(apple_rw_pin === 1'bz, "unsafe mode releases R/W");
        check(apple_data_pin === 8'hzz, "unsafe mode releases master data");
        check(apple_dma_pin === 1'b1, "unsafe mode releases DMA");
        check(apple_inh_pin === 1'b1, "unsafe mode releases INH");
        check(apple_rdy_pin === 1'b1 && apple_nmi_pin === 1'b1,
              "RDY and NMI remain high-Z");

        // Each ownership field must tag the staged byte. Dropping a request
        // at the arbiter cannot erase the byte already held in the wrapper;
        // the live permission must revoke it before the next fabric edge.
        for (int owner = 0; owner < 4; owner++) begin
            ab_write.assert_inh = (owner == 0);
            ab_write.assert_dma = (owner == 1);
            ab_write.wr_addr_rw_en = (owner == 2);
            ab_write.wr_dma_data_en = (owner == 3);
            inh_allowed = 1'b1;
            @(posedge clk);
            #1;
            check(tini_data_dir_pin == 1'b1,
                  "allowed ownership tuple must stage a data response");
            ab_write.assert_inh = 1'b0;
            ab_write.assert_dma = 1'b0;
            ab_write.wr_addr_rw_en = 1'b0;
            ab_write.wr_dma_data_en = 1'b0;
            inh_allowed = 1'b0;
            #1;
            check(tini_data_dir_pin == 1'b0 && apple_data_pin === 8'hzz,
                  "live policy must revoke every staged ownership byte");
        end

        // A selected slave read uses no ownership field and remains legal.
        ab_write.wr_addr_rw_en = 1'b0;
        ab_write.assert_dma = 1'b0;
        @(posedge clk);
        #1;
        check(tini_data_dir_pin == 1'b1 && apple_data_pin === 8'h5A,
              "qualified slave read remains enabled");
        physical_slave_select_ok = 1'b0;
        @(posedge clk);
        #1;
        check(tini_data_dir_pin == 1'b0 && apple_data_pin === 8'hzz,
              "final slot select gate must release slave data");
        physical_slave_select_ok = 1'b1;
        @(posedge clk);
        #1;
        check(tini_data_dir_pin == 1'b1 && apple_data_pin === 8'h5A,
              "selected slave read restores data after the staged gate");

        physical_irq_allowed = 1'b0;
        #1;
        check(apple_irq_pin === 1'b1,
              "final GS IRQ policy must release the shared line");
        physical_irq_allowed = 1'b1;
        force dut.ab_read_r.rw = 1'b0;
        @(posedge clk);
        #1;
        check(tini_data_dir_pin == 1'b0 && apple_data_pin === 8'hzz,
              "host write cannot expose a stale slave response");
        force dut.ab_read_r.rw = 1'b1;
        force dut.ab_read_r.cycle_valid = 1'b0;
        @(posedge clk);
        #1;
        check(tini_data_dir_pin == 1'b0 && apple_data_pin === 8'hzz,
              "invalid GS cycle cannot drive slave data");

        // GS mode fixes /M2SEL active low; the writable legacy polarity bit
        // cannot reverse this safety rule.
        gs_m2_qualify = 1'b1;
        m2sel_active_high = 1'b1;
        apple_m2sel_pin = 1'b1;
        force dut.m2sel_clean = 1'b1;
        #1;
        check(!dut.m2sel_asserted && !dut.cycle_valid_now,
              "GS M2SEL high is invalid despite polarity request");
        force dut.m2sel_clean = 1'b0;
        #1;
        check(dut.m2sel_asserted && dut.cycle_valid_now,
              "GS M2SEL low is valid");

        release dut.apple_inh_assert;
        release dut.bus_emit_state;
        release dut.physical_data_en_q;
        release dut.physical_data_q;
        release dut.ab_read_r.cycle_valid;
        release dut.ab_read_r.rw;
        release dut.m2sel_clean;
        $display("APPLE BUS ISOLATION PASS");
        $finish;
    end

    initial begin
        #10000;
        $fatal(1, "APPLE BUS ISOLATION TIMEOUT");
    end

endmodule
