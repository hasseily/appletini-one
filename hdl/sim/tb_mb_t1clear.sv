`timescale 1ns / 1ps

// End-to-end reproduction of MB-Audit T6522_15's "must clear IFR.T1" subtests:
// wrapper + arbiter + mockingboard (slot 4, VIA-A at $C400) + via6522. We set
// up T1 one-shot so it underflows and sets IFR.T1, then exercise each register
// access the test cares about and read IFR back the way the 6502 would, so the
// sim tells us directly whether the flag clears (expected) or stays set
// (the reported 11:0F failure, Actual $40).
module tb_mb_t1clear;
    logic clk = 0;
    always #3.75 clk = ~clk;          // 133.333 MHz
    logic phi0 = 0;
    always #490 phi0 = ~phi0;         // ~1.02 MHz
    logic rstn = 0;

    // Apple bus pins with weak motherboard pulls.
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

    // TB drives address/RW every cycle; data only on writes (the card drives
    // read data for the registers it serves).
    logic        drv_ar = 0, drv_rw = 1'b1, drv_d = 0;
    logic [15:0] drv_addr = 16'hFFFF;
    logic [7:0]  drv_data = 8'hFF;
    assign apple_addr_pin = drv_ar ? drv_addr : 16'hzzzz;
    assign apple_rw_pin   = drv_ar ? drv_rw   : 1'bz;
    assign apple_data_pin = drv_d  ? drv_data : 8'hzz;

    logic tini_oe_pin, tini_addr_dir_pin, tini_data_dir_pin;
    logic [31:0] dbg_quality, dbg_tapmm, dbg_strobe, dbg_taplast;
    globals::AppleBus_read  ab_read;
    globals::AppleBus_write ab_write;
    globals::AppleBus_write mb_ab_write;

    // II+ mode (host_is_iiplus=1, data tap 52) is the reported-failure config;
    // //e mode (0, tap 59) passed. Select with +IIPLUS / +TAP=<n>.
    logic       host_iiplus;
    logic [5:0] tap = 6'd52;
    initial begin
        host_iiplus = 1'b1;
        if ($test$plusargs("IIE")) host_iiplus = 1'b0;
        void'($value$plusargs("TAP=%d", tap));
    end

    apple_bus_wrapper wrapper_i (
        .clk(clk), .rstn(rstn),
        .res_filtered_out(), .dbg_lost_cycle_count(),
        .dbg_bus_quality(dbg_quality), .dbg_tap_mismatch(dbg_tapmm),
        .dbg_strobe_anom(dbg_strobe), .dbg_tap_last(dbg_taplast),
        .dbg_clear(1'b0),
        .inh_allowed(1'b1), .gs_m2_qualify(1'b0), .m2sel_active_high(1'b0),
        .host_is_iiplus(host_iiplus), .iiplus_data_tap(tap),
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

    apple_bus_write_arbiter #(.NUM_CLIENTS(1)) arbiter_i (
        .inh_allowed(1'b1), .client_writes({mb_ab_write}), .ab_write(ab_write)
    );

    // Minimal soft-switch state: card $C4xx space selected (as mb-audit runs).
    globals::SoftSwitchState sss;
    always_comb begin
        sss = '0;
        sss.slot_access = 1'b1;
    end

    mockingboard mb_i (
        .clk(clk), .rstn(rstn),
        .ab_read(ab_read), .sss(sss),
        .slot_assign(3'd4),
        .pan(48'd0), .audio_control(32'd0), .audio_sample_tick(1'b0),
        .ab_write(mb_ab_write),
        .audio_l(), .audio_r(),
        .dbg_ssi_irq(), .dbg_ssi_backend_done(), .dbg_ssi_enable_ints()
    );

    // Capture the value a 6502 would read back from a served register: the
    // wrapper samples its own driven data at data_en for a read cycle.
    // VIA-A register offsets (slot 4 base $C400).
    localparam logic [15:0] BASE   = 16'hC400;
    localparam logic [3:0]  R_IFR_ = 4'hD;

    logic [7:0] readback = 8'hXX;
    logic       readback_valid = 0;
    always @(posedge clk) begin
        // Only latch the value returned for an IFR read, so later idle reads
        // don't clobber it before we print.
        if (rstn && ab_read.data_en && ab_read.rw &&
            ab_read.addr == (BASE | R_IFR_)) begin
            readback <= ab_read.data;
            readback_valid <= 1'b1;
        end
    end

    localparam logic [3:0]  R_T1CL = 4'h4;   // TIMER1L_COUNTER
    localparam logic [3:0]  R_T1CH = 4'h5;   // TIMER1H_COUNTER
    localparam logic [3:0]  R_T1LL = 4'h6;   // TIMER1L_LATCH
    localparam logic [3:0]  R_T1LH = 4'h7;   // TIMER1H_LATCH
    localparam logic [3:0]  R_ACR  = 4'hB;
    localparam logic [3:0]  R_IFR  = 4'hD;
    localparam logic [3:0]  R_IER  = 4'hE;

    // One 6502 bus cycle. rw=1 read (card/weak-pull drives data), rw=0 write.
    task automatic bus_cycle(input logic [15:0] a, input logic rw,
                             input logic [7:0] d);
        @(negedge phi0);
        drv_addr = a; drv_rw = rw; drv_ar = 1'b1;
        if (rw == 1'b0) begin drv_data = d; drv_d = 1'b1; end
        else            drv_d = 1'b0;
    endtask

    task automatic wr_reg(input logic [3:0] r, input logic [7:0] d);
        bus_cycle(BASE | r, 1'b0, d);
    endtask
    // Absolute-mode read (single cycle) of a register; returns via `readback`.
    task automatic rd_reg(input logic [3:0] r);
        readback_valid = 0;
        bus_cycle(BASE | r, 1'b1, 8'h00);
    endtask
    // STA (ZP),Y form: dummy READ then WRITE to the same register address.
    task automatic sta_zpy(input logic [3:0] r, input logic [7:0] d);
        bus_cycle(BASE | r, 1'b1, 8'h00);   // dummy read
        bus_cycle(BASE | r, 1'b0, d);       // write
    endtask
    // LDA (ZP),Y form: single READ (no false read for LDA (zp),y without page cross).
    // mb-audit subtest #5 uses LDA (MBBase),y -> a plain read of the register.
    task automatic idle(input int n);
        for (int k = 0; k < n; k++) bus_cycle(16'hE000, 1'b1, 8'hEA);
    endtask

    task automatic setup_ifr_t1;
        // Mirror @setupIFR_T1: clear IER.T1, clear IFR.T1, load T1=$0001 one-shot.
        wr_reg(R_ACR,  8'h00);   // ACR = one-shot
        wr_reg(R_IER,  8'h40);   // clear T1 enable
        wr_reg(R_IFR,  8'h40);   // clear T1 flag
        wr_reg(R_T1CL, 8'h01);
        wr_reg(R_T1CH, 8'h00);   // starts T1; underflows in ~2 ticks -> IFR.T1=1
        idle(6);                 // let it underflow and set the flag
    endtask

    task automatic show_ifr(input string tag);
        readback_valid = 0;
        bus_cycle(BASE | R_IFR, 1'b1, 8'h00);   // the IFR read cycle
        wait (readback_valid);                  // block until THIS read latches
        @(posedge clk);
        $display("  IFR after %-28s = %02X  (T1 flag=%s)", tag, readback,
                 (readback & 8'h40) ? "SET" : "clr");
    endtask

    initial begin
        repeat (8) @(posedge clk);
        rstn = 1;
        idle(8);                          // let PHI0 filter lock + card reset clear

        $display("=== T6522_15 must-clear reproduction (slot 4 VIA-A) ===");
        $display("    host_is_iiplus=%0d  data_tap=%0d", host_iiplus, tap);

        setup_ifr_t1;
        show_ifr("setup (expect SET)");

        // subtest #1: STA ABS T1C_L -- mustn't clear.
        wr_reg(R_T1CL, 8'h00);
        show_ifr("STA ABS  T1C_L (expect SET)");

        // subtest #2: STA (ZP),Y T1C_L -- MUST clear (false read).
        sta_zpy(R_T1CL, 8'h00);
        show_ifr("STA(ZP),Y T1C_L (expect clr)");

        // Re-arm and test the other must-clears.
        setup_ifr_t1;
        // subtest #4: STA (ZP),Y T1H_L -- MUST clear (write T1L_H).
        sta_zpy(R_T1LH, 8'h00);
        show_ifr("STA(ZP),Y T1H_L (expect clr)");

        setup_ifr_t1;
        // subtest #5: LDA T1C_L -- MUST clear (read T1C_L).
        rd_reg(R_T1CL);
        show_ifr("LDA       T1C_L (expect clr)");

        setup_ifr_t1;
        // subtest #6: STA (ZP),Y T1H_C -- MUST clear (write T1C_H).
        sta_zpy(R_T1CH, 8'h01);
        show_ifr("STA(ZP),Y T1H_C (expect clr)");

        // subtest #3 control: STA (ZP),Y T1L_L -- mustn't clear.
        setup_ifr_t1;
        sta_zpy(R_T1LL, 8'h00);
        show_ifr("STA(ZP),Y T1L_L (expect SET)");

        // Host-specific release regression. The experimental II/II+ path
        // retains the served byte through the end of PHI0. The //e uses the
        // physical PHI0 boundary and, critically, must not keep the external
        // data transceiver enabled after that falling edge.
        setup_ifr_t1;                       // IFR.T1 = $40
        @(negedge phi0);
        drv_addr = BASE | R_IFR; drv_rw = 1'b1; drv_ar = 1'b1;
        @(posedge phi0);
        #455;                               // ~35 ns before the fall
        $display("  served read: pin=%02X ~35ns before fall (expect 40)",
                 apple_data_pin);
        if (apple_data_pin !== 8'h40)
            $fatal(1, "Served data released before the late latch window");

        @(negedge phi0);
        #1;
        if (!host_iiplus) begin
            $display("  //e release: dir=%0d 1ns after physical fall (expect 0)",
                     tini_data_dir_pin);
            if (tini_data_dir_pin !== 1'b0)
                $fatal(1, "//e data transceiver still driving after physical PHI0 fall");
        end

        // The production board asserts virtual-card IRQ through the same
        // bidirectional translator lane used to observe external IRQ. The FPGA
        // may only pull it low; release must leave the motherboard pull-up in
        // control.
        setup_ifr_t1;                       // IFR.T1 = $40, IER.T1 clear
        wr_reg(R_IER, 8'hC0);               // enable the pending T1 interrupt
        idle(2);
        if (apple_irq_pin !== 1'b0)
            $fatal(1, "Virtual-card IRQ did not pull the Apple bus low");
        wr_reg(R_IER, 8'h40);               // disable T1 interrupt
        idle(2);
        if (apple_irq_pin !== 1'b1)
            $fatal(1, "Virtual-card IRQ did not release to the bus pull-up");
        $display("OPEN-COLLECTOR IRQ PASS: bus pulls low and releases high");

        // DIX uses VIA-A T1 as a PAL frame clock: ACR=$40 selects continuous
        // mode, IER=$E0 enables T1/T2, and T1=$4F36 should interrupt every
        // 20,280 Apple cycles. Exercise the exact long-period sequence twice;
        // the first T1C-L read models DIX's handler acknowledging IFR.T1.
        wr_reg(R_ACR,  8'h40);
        wr_reg(R_IER,  8'hE0);
        wr_reg(R_T1CL, 8'h36);
        wr_reg(R_T1CH, 8'h4F);
        idle(20279);
        if (apple_irq_pin !== 1'b1)
            $fatal(1, "DIX T1 IRQ asserted before the $4F36 period elapsed");
        idle(4);
        if (apple_irq_pin !== 1'b0)
            $fatal(1, "DIX continuous T1 did not assert the Apple IRQ bus");
        rd_reg(R_T1CL);
        idle(2);
        if (apple_irq_pin !== 1'b1)
            $fatal(1, "DIX T1C-L acknowledge did not release the IRQ output");
        idle(20283);
        if (apple_irq_pin !== 1'b0)
            $fatal(1, "DIX continuous T1 did not reassert after reload");
        $display("DIX TIMER PASS: $4F36 continuous T1 asserts, clears, and reloads");

        idle(4);
        // Forensics sanity on an ideal bus: everything must be zero except
        // data_en misses accumulated before the TB drove its first cycle
        // (real use clears the counters first; the bench mirrors that by
        // ignoring the startup misses).
        $display("forensics: ring=%0d short=%0d mm_rd=%0d mm_wr=%0d extra=%0d miss=%0d last=%08X",
                 dbg_quality[31:16], dbg_quality[15:0],
                 dbg_tapmm[31:16], dbg_tapmm[15:0],
                 dbg_strobe[31:16], dbg_strobe[15:0], dbg_taplast);
        if (dbg_quality != 32'd0 || dbg_tapmm != 32'd0 ||
            dbg_strobe[31:16] != 16'd0) begin
            $display("FORENSICS FAIL: nonzero hazard counter on clean bus");
        end else begin
            $display("FORENSICS PASS: clean bus reads clean");
        end
        $display("=== done ===");
        $finish;
    end
endmodule
