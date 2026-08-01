`timescale 1ns / 1ps

// Automatic II+ vTW physical-/DMA refresh regression.
//
// Other masters hold /DMA continuously low. An active vTW session latched as
// II/II+ automatically releases the translated lane for about 30 ns late in
// each PHI0 and then pulls it low again before the next ownership boundary.
module tb_iiplus_dma_refresh;
    logic clk = 0;
    always #3.75 clk = ~clk;          // 133.333 MHz
    logic phi0 = 0;
    always #490 phi0 = ~phi0;         // ~1.02 MHz
    logic rstn = 0;
    logic iiplus_session_active = 0;

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
        .m2sel_active_high(1'b0), .host_is_iiplus(1'b1),
        .iiplus_dma_refresh_active(iiplus_session_active),
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
    int dma_falls = 0;
    int dma_rises = 0;
    int low_at_boundary = 0;
    bit monitor_refresh = 0;
    time last_dma_rise;

    task automatic check(input bit cond, input string msg);
        if (!cond) begin
            errors++;
            $display("FAIL: %s", msg);
        end
    endtask

    always @(posedge apple_dma_pin) begin
        if (monitor_refresh) begin
            dma_rises++;
            last_dma_rise = $time;
        end
    end

    always @(negedge apple_dma_pin) begin
        if (monitor_refresh) begin
            dma_falls++;
            check(($time - last_dma_rise) >= 29ns &&
                  ($time - last_dma_rise) <= 31ns,
                  $sformatf("II+ /DMA refresh notch was %0t, expected ~30 ns",
                            $time - last_dma_rise));
        end
    end

    // The refreshed /DMA must already be low at every next PHI1 boundary.
    always @(negedge phi0) begin
        if (monitor_refresh) begin
            #1;
            if (apple_dma_pin === 1'b0)
                low_at_boundary++;
        end
    end

    initial begin
        ab_write = '0;
        repeat (8) @(posedge clk);
        rstn = 1;

        // Let the PHI0 filter and timing pipe settle.
        repeat (4) @(negedge phi0);
        check(apple_dma_pin === 1'b1,
              "/DMA pin not released with no ownership request");

        // A non-II+ vTW session or another master retains continuous low.
        ab_write.assert_dma = 1'b1;
        #30;
        check(apple_dma_pin === 1'b0,
              "/DMA did not assert with no active II+ vTW session");
        monitor_refresh = 1;
        repeat (3) @(negedge phi0);
        monitor_refresh = 0;
        #2; // allow the boundary monitor's delayed sample to retire
        check(dma_rises == 0 && dma_falls == 0,
              "inactive II+ session altered continuous /DMA");

        // Mark the active vTW session as II/II+. Refresh is automatic: it must
        // create one clean reassertion per Apple cycle and still be low at
        // each boundary.
        dma_rises = 0;
        dma_falls = 0;
        low_at_boundary = 0;
        @(posedge phi0);
        #100;
        iiplus_session_active = 1'b1;
        monitor_refresh = 1;
        repeat (6) @(negedge phi0);
        #100;
        monitor_refresh = 0;

        check(dma_falls == 6,
              $sformatf("II+ produced %0d /DMA reassertions, expected 6",
                        dma_falls));
        check(dma_rises == 6,
              $sformatf("II+ produced %0d /DMA releases, expected 6",
                        dma_rises));
        check(low_at_boundary == 6,
              $sformatf("II+ /DMA low at %0d/6 ownership boundaries",
                        low_at_boundary));
        check(apple_dma_pin === 1'b0,
              "II+ /DMA was not held low after refresh");

        // Ending the II+ session must immediately restore the established
        // continuous-low path without disturbing another held request.
        iiplus_session_active = 1'b0;
        #30;
        check(apple_dma_pin === 1'b0,
              "ending II+ session released an active /DMA request");
        repeat (2) @(negedge phi0);
        check(apple_dma_pin === 1'b0,
              "/DMA did not remain continuously low after II+ session");

        ab_write.assert_dma = 1'b0;
        #30;
        check(apple_dma_pin === 1'b1,
              "/DMA did not release when ownership request cleared");

        if (errors == 0) $display("IIPLUS DMA REFRESH PASS");
        else begin
            $display("tb_iiplus_dma_refresh: %0d FAILURES", errors);
            $fatal(1, "tb_iiplus_dma_refresh failed");
        end
        $finish;
    end

    initial begin
        #200us;
        $fatal(1, "tb_iiplus_dma_refresh: timeout");
    end
endmodule
