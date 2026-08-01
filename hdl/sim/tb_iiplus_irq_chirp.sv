`timescale 1ns / 1ps

// II+ physical-IRQ re-arm regression.
//
// A persistent virtual-card IRQ remains level-sensitive internally, but the
// II+ physical pin is briefly released once per Apple cycle and then driven
// low again before the CPU sampling point. The //e remains continuously low.
module tb_iiplus_irq_chirp;
    logic clk = 0;
    always #3.75 clk = ~clk;          // 133.333 MHz
    logic phi0 = 0;
    always #490 phi0 = ~phi0;         // ~1.02 MHz
    logic rstn = 0;
    logic host_is_iiplus = 1;

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

    logic tini_oe_pin, tini_addr_dir_pin, tini_data_dir_pin;
    globals::AppleBus_read  ab_read;
    globals::AppleBus_write ab_write;

    apple_bus_wrapper wrapper_i (
        .clk(clk), .rstn(rstn),
        .res_filtered_out(), .dbg_lost_cycle_count(),
        .dbg_bus_quality(), .dbg_tap_mismatch(),
        .dbg_strobe_anom(), .dbg_tap_last(),
        .dbg_clear(1'b0),
        .inh_allowed(1'b1), .gs_m2_qualify(1'b0),
        .m2sel_active_high(1'b0), .host_is_iiplus(host_is_iiplus),
        .iiplus_dma_refresh_active(1'b0),
        .apple_data_pin(apple_data_pin), .apple_addr_pin(apple_addr_pin),
        .apple_rw_pin(apple_rw_pin), .apple_phi0_pin(phi0),
        .apple_m2sel_pin(1'b0), .apple_m2b0_pin(1'b0),
        .apple_inh_pin(apple_inh_pin), .apple_res_pin(apple_res_pin),
        .apple_irq_pin(apple_irq_pin), .apple_rdy_pin(apple_rdy_pin),
        .apple_dma_pin(apple_dma_pin), .apple_nmi_pin(apple_nmi_pin),
        .tini_oe_pin(tini_oe_pin), .tini_5v_pin(1'b0),
        .tini_addr_dir_pin(tini_addr_dir_pin),
        .tini_data_dir_pin(tini_data_dir_pin),
        .ab_read(ab_read), .ab_write(ab_write)
    );

    int errors = 0;
    int irq_falls = 0;
    int irq_rises = 0;
    int low_at_sample = 0;
    bit monitor_iiplus = 0;
    time last_irq_rise;

    task automatic check(input bit cond, input string msg);
        if (!cond) begin
            errors++;
            $display("FAIL: %s", msg);
        end
    endtask

    always @(negedge apple_irq_pin) begin
        if (monitor_iiplus) begin
            irq_falls++;
            check(($time - last_irq_rise) >= 29ns &&
                  ($time - last_irq_rise) <= 31ns,
                  $sformatf("II+ IRQ re-arm notch was %0t, expected ~30 ns",
                            $time - last_irq_rise));
        end
    end

    always @(posedge apple_irq_pin) begin
        if (monitor_iiplus) begin
            irq_rises++;
            last_irq_rise = $time;
        end
    end

    // Reassertion uses the original chirp's hardware-proven falling-edge
    // point. Confirm the pin is low just after every raw PHI0 fall.
    always @(negedge phi0) begin
        if (monitor_iiplus) begin
            #1;
            if (apple_irq_pin === 1'b0)
                low_at_sample++;
        end
    end

    initial begin
        ab_write = '0;
        repeat (8) @(posedge clk);
        rstn = 1;

        // Let the PHI0 filter and timing pipe settle.
        repeat (4) @(negedge phi0);
        check(apple_irq_pin === 1'b1,
              "IRQ pin not released with no request");

        // A II+ request must assert immediately and remain low apart from one
        // clean re-arm notch per Apple cycle. Assert before monitoring so the
        // initial level assertion is not counted as a periodic reassertion.
        @(posedge phi0);
        #100;
        ab_write.assert_irq = 1'b1;
        #30;
        check(apple_irq_pin === 1'b0,
              "II+ IRQ did not assert immediately");
        monitor_iiplus = 1;
        repeat (6) @(negedge phi0);
        #100;
        monitor_iiplus = 0;

        check(irq_falls == 6,
              $sformatf("II+ produced %0d periodic reassertions, expected 6",
                        irq_falls));
        check(irq_rises == 6,
              $sformatf("II+ produced %0d re-arm releases, expected 6",
                        irq_rises));
        check(low_at_sample == 6,
              $sformatf("II+ IRQ low at %0d/6 CPU samples",
                        low_at_sample));
        check(apple_irq_pin === 1'b0,
              "II+ IRQ was not held low after the sample");
        ab_write.assert_irq = 1'b0;
        #30;
        check(apple_irq_pin === 1'b1,
              "II+ IRQ did not release after request cleared");

        // A //e request keeps the established continuous level-low behavior.
        host_is_iiplus = 1'b0;
        ab_write.assert_irq = 1'b1;
        #30;
        check(apple_irq_pin === 1'b0,
              "//e IRQ did not assert continuously");
        repeat (3) @(negedge phi0);
        #200;
        check(apple_irq_pin === 1'b0,
              "//e IRQ was not held low across cycles");
        ab_write.assert_irq = 1'b0;
        #30;
        check(apple_irq_pin === 1'b1,
              "//e IRQ did not release after request cleared");

        if (errors == 0) $display("IIPLUS IRQ CHIRP PASS");
        else begin
            $display("tb_iiplus_irq_chirp: %0d FAILURES", errors);
            $fatal(1, "tb_iiplus_irq_chirp failed");
        end
        $finish;
    end

    initial begin
        #200us;
        $fatal(1, "tb_iiplus_irq_chirp: timeout");
    end
endmodule
