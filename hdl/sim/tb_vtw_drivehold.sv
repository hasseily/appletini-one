`timescale 1ns / 1ps

// II+ read-hold / raw-PHI0 write-release regression.
//
// A vTW-engine-style driver may keep wr_data_en asserted past the physical
// PHI0 fall. Writes must always release at raw PHI0. II+ card-read responses,
// however, need the saved-byte margin that the unbuffered motherboard expects.
// This bench drives the wrapper's ab_write directly and checks both paths,
// plus the unchanged //e native and vTW-owned behavior.
module tb_vtw_drivehold;
    logic clk = 0;
    always #3.75 clk = ~clk;          // 133.333 MHz
    logic phi0 = 0;
    always #490 phi0 = ~phi0;         // ~1.02 MHz
    logic rstn = 0;

    wire [7:0]  apple_data_pin;
    wire [15:0] apple_addr_pin;
    wire        apple_rw_pin;
    wire        apple_inh_pin, apple_res_pin, apple_irq_pin;
    wire        apple_rdy_pin, apple_dma_pin, apple_nmi_pin;
    assign (weak0, weak1) apple_data_pin = 8'hFF;
    assign (weak0, weak1) apple_addr_pin = 16'hFFFF;
    assign (weak0, weak1) apple_rw_pin   = 1'b1;
    assign (weak0, weak1) apple_inh_pin  = 1'b1;
    assign (weak0, weak1) apple_res_pin  = 1'b1;
    assign (weak0, weak1) apple_irq_pin  = 1'b1;
    assign (weak0, weak1) apple_rdy_pin  = 1'b1;
    assign (weak0, weak1) apple_dma_pin  = 1'b1;
    assign (weak0, weak1) apple_nmi_pin  = 1'b1;

    logic host_is_iiplus = 1'b1;
    logic tini_oe_pin, tini_addr_dir_pin, tini_data_dir_pin;
    globals::AppleBus_read  ab_read;
    globals::AppleBus_write ab_write;

    apple_bus_wrapper wrapper_i (
        .clk(clk), .rstn(rstn),
        .res_filtered_out(), .dbg_lost_cycle_count(),
        .dbg_bus_quality(), .dbg_tap_mismatch(),
        .dbg_strobe_anom(), .dbg_tap_last(),
        .dbg_clear(1'b0),
        .inh_allowed(1'b1), .gs_m2_qualify(1'b0), .m2sel_active_high(1'b0),
        .host_is_iiplus(host_is_iiplus),
        .iiplus_dma_refresh_active(1'b0),
        .apple_data_pin(apple_data_pin), .apple_addr_pin(apple_addr_pin),
        .apple_rw_pin(apple_rw_pin), .apple_phi0_pin(phi0),
        .apple_m2sel_pin(1'b0), .apple_m2b0_pin(1'b0),
        .apple_inh_pin(apple_inh_pin), .apple_res_pin(apple_res_pin),
        .apple_irq_pin(apple_irq_pin), .apple_rdy_pin(apple_rdy_pin),
        .apple_dma_pin(apple_dma_pin), .apple_nmi_pin(apple_nmi_pin),
        .tini_oe_pin(tini_oe_pin), .tini_5v_pin(1'b0),
        .tini_addr_dir_pin(tini_addr_dir_pin), .tini_data_dir_pin(tini_data_dir_pin),
        .ab_read(ab_read), .ab_write(ab_write)
    );

    int errors = 0;
    task automatic check(input bit cond, input string msg);
        if (!cond) begin
            errors++;
            $display("FAIL: %s", msg);
        end
    endtask

    initial begin
        // Engine-style master: address/RW owned continuously; write data
        // enable raised at the cycle's drive point and held until the NEXT
        // drive point (past the fall), exactly like vtw_bus_engine.
        ab_write = '0;
        repeat (8) @(posedge clk);
        rstn = 1;

        // Let the PHI0 filter lock over a few idle cycles.
        repeat (4) @(negedge phi0);

        ab_write.wr_addr_rw_en = 1'b1;
        ab_write.wr_addr = 16'h0400;
        ab_write.wr_rw = 1'b0;             // write cycle
        ab_write.wr_data = 8'hA5;
        ab_write.wr_data_en = 1'b1;        // held past the fall, engine-style

        // 1. Mid-PHI0-high of the write cycle: byte driven.
        @(posedge phi0);
        #300;
        check(apple_data_pin === 8'hA5,
              $sformatf("write byte not driven mid-PHI0 (pin=%02X)",
                        apple_data_pin));

        // 2. The XDC bounds raw PHI0 -> data direction to 8 ns. RTL
        //    simulation has no pad delay, but allow 12 ns here to express
        //    the intended physical release budget.
        @(negedge phi0);
        #12;
        check(apple_data_pin === 8'hFF,
              $sformatf("stale byte driven after PHI0 fall (pin=%02X)",
                        apple_data_pin));
        check(tini_data_dir_pin === 1'b0,
              "external transceiver still outward after PHI0 fall");

        // The engine may not clean up its logical enable until later PHI1.
        // The raw pin boundary above must remain authoritative meanwhile.
        #88;
        ab_write.wr_data_en = 1'b0;
        ab_write.wr_rw = 1'b1;
        ab_write.wr_addr = 16'hFFFF;

        // 3. 300 ns after the fall: bus remains released...
        #200;
        check(apple_data_pin === 8'hFF,
              $sformatf("stale byte still driven 300ns after fall (pin=%02X)",
                        apple_data_pin));

        // ...and stay released through the NEXT cycle's PHI0-high (the
        //    window where a real $C000 keyboard read would be corrupted).
        @(posedge phi0);
        #400;
        check(apple_data_pin === 8'hFF,
              $sformatf("stale byte driven into next PHI0-high (pin=%02X)",
                        apple_data_pin));
        check(tini_data_dir_pin === 1'b0,
              "external transceiver still outward in next cycle");

        // Repeat once to prove it is per-cycle, not a one-off.
        @(negedge phi0);
        ab_write.wr_addr = 16'h0401;
        ab_write.wr_rw = 1'b0;
        ab_write.wr_data = 8'h5A;
        ab_write.wr_data_en = 1'b1;
        @(posedge phi0);
        #300;
        check(apple_data_pin === 8'h5A,
              $sformatf("second cycle: byte not driven mid-PHI0 (pin=%02X)",
                        apple_data_pin));
        @(negedge phi0);
        #12;
        check(apple_data_pin === 8'hFF,
              $sformatf("second cycle: stale byte after fall (pin=%02X)",
                        apple_data_pin));
        check(tini_data_dir_pin === 1'b0,
              "second cycle: transceiver outward after PHI0 fall");
        #88;
        ab_write.wr_data_en = 1'b0;
        ab_write.wr_rw = 1'b1;
        #200;
        check(apple_data_pin === 8'hFF,
              $sformatf("second cycle: stale byte after fall (pin=%02X)",
                        apple_data_pin));

        // 4. A native II+ card read has no address/R-W owner. Save its
        // response, then emulate the card dropping wr_data_en before the
        // physical fall and changing its live mux output. The original byte
        // must survive briefly past raw PHI0, then release at filtered fall.
        ab_write.wr_addr_rw_en = 1'b0;
        ab_write.wr_rw = 1'b1;
        ab_write.wr_data = 8'hC3;
        ab_write.wr_data_en = 1'b1;
        @(posedge phi0);
        #300;
        check(apple_data_pin === 8'hC3,
              $sformatf("II+ native read not driven (pin=%02X)",
                        apple_data_pin));
        ab_write.wr_data_en = 1'b0;
        ab_write.wr_data = 8'h3C;
        @(negedge phi0);
        #12;
        check(apple_data_pin === 8'hC3,
              $sformatf("II+ native saved byte missing after raw fall (pin=%02X)",
                        apple_data_pin));
        check(tini_data_dir_pin === 1'b1,
              "II+ native read direction released before saved-byte hold ended");
        #68;
        check(apple_data_pin === 8'hFF,
              $sformatf("II+ native saved byte held past filtered fall (pin=%02X)",
                        apple_data_pin));
        check(tini_data_dir_pin === 1'b0,
              "II+ native read direction held past filtered fall");

        // 5. A vTW-owned read has address/R-W ownership but R/W=1. It gets
        // the same saved-byte read margin.
        ab_write.wr_addr_rw_en = 1'b1;
        ab_write.wr_addr = 16'hC0EC;
        ab_write.wr_rw = 1'b1;
        ab_write.wr_data = 8'h96;
        ab_write.wr_data_en = 1'b1;
        @(posedge phi0);
        #300;
        check(apple_data_pin === 8'h96,
              $sformatf("II+ owned read not driven (pin=%02X)",
                        apple_data_pin));
        ab_write.wr_data_en = 1'b0;
        ab_write.wr_data = 8'h69;
        @(negedge phi0);
        #12;
        check(apple_data_pin === 8'h96,
              $sformatf("II+ owned saved byte missing after raw fall (pin=%02X)",
                        apple_data_pin));
        check(tini_data_dir_pin === 1'b1,
              "II+ owned read direction released before saved-byte hold ended");
        #68;
        check(apple_data_pin === 8'hFF,
              $sformatf("II+ owned saved byte held past filtered fall (pin=%02X)",
                        apple_data_pin));
        check(tini_data_dir_pin === 1'b0,
              "II+ owned read direction held past filtered fall");

        // The low-PHI0 arm guard must keep the cleared hold from returning
        // even though address ownership remains active.
        #200;
        check(apple_data_pin === 8'hFF,
              $sformatf("II+ read hold re-armed during PHI1 (pin=%02X)",
                        apple_data_pin));

        // 6. A native //e read keeps the established raw-PHI0 release. Keep
        // wr_data_en high past the fall to prove no II+ hold can arm.
        host_is_iiplus = 1'b0;
        ab_write.wr_addr_rw_en = 1'b0;
        ab_write.wr_rw = 1'b1;
        ab_write.wr_data = 8'hB2;
        ab_write.wr_data_en = 1'b1;
        @(posedge phi0);
        #300;
        check(apple_data_pin === 8'hB2,
              $sformatf("//e native read not driven (pin=%02X)",
                        apple_data_pin));
        @(negedge phi0);
        #12;
        check(apple_data_pin === 8'hFF,
              $sformatf("//e native read did not release at raw fall (pin=%02X)",
                        apple_data_pin));
        check(tini_data_dir_pin === 1'b0,
              "//e native read direction did not release at raw fall");
        ab_write.wr_data_en = 1'b0;

        // 7. A //e vTW-owned read retains the engine's explicit window: with
        // addr/R-W owned and wr_data_en high it may drive across raw PHI0.
        ab_write.wr_addr_rw_en = 1'b1;
        ab_write.wr_addr = 16'hC0EC;
        ab_write.wr_rw = 1'b1;
        ab_write.wr_data = 8'hD5;
        ab_write.wr_data_en = 1'b1;
        @(posedge phi0);
        #300;
        check(apple_data_pin === 8'hD5,
              $sformatf("//e owned read not driven (pin=%02X)",
                        apple_data_pin));
        @(negedge phi0);
        #12;
        check(apple_data_pin === 8'hD5,
              $sformatf("//e owned read window ended at raw fall (pin=%02X)",
                        apple_data_pin));
        check(tini_data_dir_pin === 1'b1,
              "//e owned read direction ended at raw fall");
        ab_write.wr_data_en = 1'b0;
        #12;
        check(apple_data_pin === 8'hFF,
              $sformatf("//e owned read did not follow engine release (pin=%02X)",
                        apple_data_pin));
        check(tini_data_dir_pin === 1'b0,
              "//e owned read direction did not follow engine release");

        if (errors == 0) $display("VTW DRIVEHOLD PASS");
        else begin
            $display("tb_vtw_drivehold: %0d FAILURES", errors);
            $fatal(1, "tb_vtw_drivehold failed");
        end
        $finish;
    end

    initial begin
        #200us;
        $fatal(1, "tb_vtw_drivehold: timeout");
    end
endmodule
