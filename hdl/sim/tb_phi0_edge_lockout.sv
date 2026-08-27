`timescale 1ns / 1ps
// Focused bench: the PHI0 edge lockout in apple_bus_wrapper.
//
// A fabric bus master (vTW) drives address/park at fall+8, INH at fall+32
// and write data at rise+18..40. On hardware that activity rings the PHI0
// input hard enough to beat the five-sample majority filter: a PAL //e
// under vTW logged 2576 "short edges" and ~7.5k split PHI0 periods per
// session, each phantom fall an extra sss_en that skewed the 50/60 Hz
// line-period verdict and flapped the HDMI mode.
//
// The bench feeds the wrapper a clean 1.02 MHz PHI0 with injected ring
// bursts after every real edge (30 ns = four fabric samples, enough to
// flip the majority output) and checks:
//   1. exactly one sss_en, one data_en and one addr_en per real period;
//   2. the accepted edge is not delayed: fall -> sss_en spacing is the
//      same with and without ringing;
//   3. the ringing really reached the filter (majority output toggled
//      more than twice per period), so the test is not vacuous;
//   4. busdbg: short edges = 0, extra/missing data_en = 0, lost cycles = 0
//      while ring events count up;
//   5. the DMA write phaser starts once per period (a phantom rise inside
//      the lockout must not start the data drive early).

module tb_phi0_edge_lockout;

    timeunit 1ns;
    timeprecision 1ps;

    logic clk = 1'b0;
    always #3.75 clk = ~clk;   // 133.333 MHz

    logic rstn = 1'b0;

    tri [7:0]  apple_data_pin;
    tri [15:0] apple_addr_pin;
    tri        apple_rw_pin;
    logic      phi0_real = 1'b0;
    logic      glitch = 1'b0;
    logic      ring_en = 1'b0;
    wire       apple_phi0_pin = phi0_real ^ glitch;
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
    assign (weak0, weak1) apple_data_pin = 8'hFF;
    assign (weak0, weak1) apple_addr_pin = 16'hFFFF;
    assign (weak0, weak1) apple_rw_pin   = 1'b1;

    // Real PHI0: 490 ns half-periods, the shortest legal shape.
    always #490 phi0_real = ~phi0_real;

    // Ring bursts. After a fall: two pulses at +60 ns (30 ns) and +130 ns
    // (25 ns) -- where our address drive lands. After a rise: one pulse at
    // +140 ns (30 ns) -- where the DMA write data drive starts.
    always @(negedge phi0_real) begin
        if (ring_en) begin
            #60 glitch = 1'b1; #30 glitch = 1'b0;
            #40 glitch = 1'b1; #25 glitch = 1'b0;
        end
    end
    always @(posedge phi0_real) begin
        if (ring_en) begin
            #140 glitch = 1'b1; #30 glitch = 1'b0;
        end
    end

    logic tini_oe_pin;
    logic tini_addr_dir_pin;
    logic tini_data_dir_pin;
    logic [31:0] dbg_lost_cycle_count;
    logic [31:0] dbg_bus_quality;
    logic [31:0] dbg_strobe_anom;
    globals::AppleBus_read ab_read;
    globals::AppleBus_write ab_write = '0;

    apple_bus_wrapper dut (
        .clk(clk),
        .rstn(rstn),
        .physical_bus_isolate(1'b0),
        .res_filtered_out(),
        .dbg_lost_cycle_count(dbg_lost_cycle_count),
        .dbg_bus_quality(dbg_bus_quality),
        .dbg_tap_mismatch(),
        .dbg_strobe_anom(dbg_strobe_anom),
        .dbg_tap_last(),
        .dbg_ghost_write(),
        .dbg_clear(1'b0),
        .inh_allowed(1'b1),
        .gs_m2_qualify(1'b0),
        .m2sel_active_high(1'b0),
        .host_is_iiplus(1'b0),
        .iiplus_dma_refresh_active(1'b0),
        .apple_data_pin(apple_data_pin),
        .apple_addr_pin(apple_addr_pin),
        .apple_rw_pin(apple_rw_pin),
        .apple_phi0_pin(apple_phi0_pin),
        .apple_m2sel_pin(1'b0),
        .apple_m2b0_pin(1'b0),
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

    // Strobe and edge counters over a measurement window.
    int unsigned sss_cnt, data_cnt, addr_cnt;
    int unsigned maj_toggle_cnt, phase_start_cnt;
    logic        maj_prev;
    logic [5:0]  phase_prev;
    logic        counting = 1'b0;

    wire maj_now = (dut.phi0_ones >= 3'd3);

    always @(posedge clk) begin
        if (counting) begin
            if (ab_read.sss_en)  sss_cnt  <= sss_cnt + 1;
            if (ab_read.data_en) data_cnt <= data_cnt + 1;
            if (ab_read.addr_en) addr_cnt <= addr_cnt + 1;
            if (maj_now != maj_prev) maj_toggle_cnt <= maj_toggle_cnt + 1;
            if (dut.dma_write_phase_q == 6'd1 && phase_prev == 6'd0)
                phase_start_cnt <= phase_start_cnt + 1;
        end
        maj_prev   <= maj_now;
        phase_prev <= dut.dma_write_phase_q;
    end

    // Fall -> sss_en spacing in fabric clocks.
    task automatic measure_fall_to_sss(output int unsigned clocks);
        int unsigned n;
        @(negedge phi0_real);
        n = 0;
        do begin
            @(posedge clk);
            n++;
        end while (!ab_read.sss_en);
        clocks = n;
    endtask

    task automatic window(input int unsigned periods);
        sss_cnt = 0; data_cnt = 0; addr_cnt = 0;
        maj_toggle_cnt = 0; phase_start_cnt = 0;
        // Open and close the window on a real rise. Every strobe of a
        // period then lands inside it: fall strobes 250 ns after the fall
        // (addr_en/sss_en), the rise's data_en ~5 ns after the next fall,
        // the DMA phase start ~30 ns after the rise. Nothing from the
        // closing rise can land before the window shuts.
        @(posedge phi0_real);
        counting = 1'b1;
        repeat (periods) @(posedge phi0_real);
        counting = 1'b0;
    endtask

    int unsigned clean_spacing, ring_spacing;
    int unsigned clean_ring_events;
    logic [31:0] clean_strobe_anom, clean_lost_cycles;

    initial begin
        repeat (10) @(posedge clk);
        rstn = 1'b1;
        repeat (20) @(negedge phi0_real);   // filter and pipes settle

        // Stand in for a vTW write cycle so the DMA write phaser runs.
        ab_write.wr_addr        = 16'h0400;
        ab_write.wr_rw          = 1'b0;
        ab_write.wr_addr_rw_en  = 1'b1;
        ab_write.wr_dma_data_en = 1'b1;
        ab_write.wr_data        = 8'hA5;
        ab_write.assert_dma     = 1'b1;
        repeat (4) @(negedge phi0_real);
        check(dut.dma_write_request === 1'b1,
              "bench must present a live DMA write request");

        // ---- Clean reference -------------------------------------------
        window(200);
        check(sss_cnt == 200,  $sformatf("clean: sss_en %0d != 200", sss_cnt));
        check(data_cnt == 200, $sformatf("clean: data_en %0d != 200", data_cnt));
        check(addr_cnt == 200, $sformatf("clean: addr_en %0d != 200", addr_cnt));
        check(maj_toggle_cnt == 400,
              $sformatf("clean: majority toggles %0d != 400", maj_toggle_cnt));
        check(phase_start_cnt == 200,
              $sformatf("clean: DMA phase starts %0d != 200", phase_start_cnt));
        measure_fall_to_sss(clean_spacing);
        clean_ring_events = dbg_bus_quality[31:16];
        // The bench's first PHI0 period after reset has no data_en yet
        // (tap 59 lands after the next fall), so compare deltas.
        clean_strobe_anom = dbg_strobe_anom;
        clean_lost_cycles = dbg_lost_cycle_count;

        // ---- Ringing on every edge -------------------------------------
        ring_en = 1'b1;
        repeat (4) @(negedge phi0_real);
        window(200);
        // The bursts beat the majority filter: without the lockout each
        // one would be a phantom edge pair. Prove the stimulus is real.
        check(maj_toggle_cnt > 400,
              $sformatf({"ring: majority output toggled only %0d times; ",
                         "bursts did not reach the filter"}, maj_toggle_cnt));
        check(sss_cnt == 200,  $sformatf("ring: sss_en %0d != 200", sss_cnt));
        check(data_cnt == 200, $sformatf("ring: data_en %0d != 200", data_cnt));
        check(addr_cnt == 200, $sformatf("ring: addr_en %0d != 200", addr_cnt));
        check(phase_start_cnt == 200,
              $sformatf({"ring: DMA phase starts %0d != 200 (phantom rise ",
                         "started the write data drive)"}, phase_start_cnt));
        measure_fall_to_sss(ring_spacing);
        check(ring_spacing == clean_spacing,
              $sformatf({"ring: fall->sss_en spacing %0d != clean %0d ",
                         "(accepted edge moved)"}, ring_spacing, clean_spacing));
        $display("fall->sss_en spacing: %0d clocks (clean and ringing)",
                 clean_spacing);

        check(dbg_bus_quality[31:16] > clean_ring_events,
              "ring: busdbg ring events must count the bursts");
        check(dbg_bus_quality[15:0] == 16'd0,
              $sformatf("ring: busdbg short edges %0d != 0",
                        dbg_bus_quality[15:0]));
        check(dbg_strobe_anom == clean_strobe_anom,
              $sformatf("ring: busdbg extra/missing data_en %08x != clean %08x",
                        dbg_strobe_anom, clean_strobe_anom));
        check(dbg_lost_cycle_count == clean_lost_cycles,
              $sformatf("ring: lost cycles %0d != clean %0d",
                        dbg_lost_cycle_count, clean_lost_cycles));

        // ---- Back to clean: nothing sticks -----------------------------
        ring_en = 1'b0;
        repeat (4) @(negedge phi0_real);
        window(100);
        check(sss_cnt == 100 && data_cnt == 100 && addr_cnt == 100,
              "post-ring: strobes must return to one per period");
        check(dbg_bus_quality[15:0] == 16'd0 &&
              dbg_strobe_anom == clean_strobe_anom,
              "post-ring: forensics must stay clean");

        $display("PHI0 EDGE LOCKOUT PASS");
        $finish;
    end

    initial begin
        #2_000_000;
        $fatal(1, "PHI0 EDGE LOCKOUT TIMEOUT");
    end

endmodule
