`timescale 1ns / 1ps

// Reproduce MB-Audit T6522_15 subtest #2 at the wrapper level: a 6502
// STA (ZP),Y to a VIA register is two back-to-back same-address bus cycles --
// a dummy READ (which must clear IFR.T1) then a WRITE. On phi+90ns sampling
// the read's clear reportedly doesn't land. This bench drives apple_bus_wrapper
// with that exact sequence and prints ab_read.{addr,rw,data_en,data} per cycle
// so we can see whether the wrapper delivers data_en with rw=READ for the
// dummy-read cycle.
module tb_mb_falseread;
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

    // TB drives address/RW (all cycle) and data (during PHI0-high).
    logic        drv_ar = 0;
    logic [15:0] drv_addr = 16'hFFFF;
    logic        drv_rw = 1'b1;
    logic        drv_d = 0;
    logic [7:0]  drv_data = 8'hFF;
    assign apple_addr_pin = drv_ar ? drv_addr : 16'hzzzz;
    assign apple_rw_pin   = drv_ar ? drv_rw   : 1'bz;
    assign apple_data_pin = drv_d  ? drv_data : 8'hzz;

    logic tini_oe_pin, tini_addr_dir_pin, tini_data_dir_pin;
    globals::AppleBus_read  ab_read;
    globals::AppleBus_write ab_write = '0;   // no card serving

    // host_is_iiplus selectable via plusarg +IIPLUS
    logic host_iiplus;
    logic [5:0] iiplus_tap = 6'd52;

    apple_bus_wrapper wrapper_i (
        .clk(clk), .rstn(rstn),
        .res_filtered_out(), .dbg_lost_cycle_count(), .dbg_clear(1'b0),
        .inh_allowed(1'b1), .gs_m2_qualify(1'b0), .m2sel_active_high(1'b0),
        .host_is_iiplus(host_iiplus), .iiplus_data_tap(iiplus_tap),
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

    // Drive one 6502 bus cycle. One call == one PHI0 period, so consecutive
    // calls are back-to-back cycles with no gap (the previous broken version
    // waited for two negedges per call and dropped every other PHI0-high).
    // Address/RW/data are presented at the falling edge (start of PHI1) and
    // held through PHI0-high; the wrapper samples addr at its PHI0-high tap and
    // data at the late data tap, so a continuous hold reproduces a real 6502
    // read or write faithfully at the sample points.
    task automatic bus_cycle(input logic [15:0] a, input logic rw,
                             input logic [7:0] d);
        @(negedge phi0);
        drv_addr = a; drv_rw = rw; drv_data = d;
        drv_ar = 1'b1; drv_d = 1'b1;
    endtask

    // Per-cycle observation: print when data_en / serve_en / sss_en pulse.
    always @(posedge clk) begin
        if (rstn && ab_read.data_en)
            $display("  [%7t] data_en:  addr=%04X rw=%b data=%02X",
                     $time, ab_read.addr, ab_read.rw, ab_read.data);
        if (rstn && ab_read.serve_en)
            $display("  [%7t] serve_en: addr=%04X rw=%b",
                     $time, ab_read.addr, ab_read.rw);
    end

    initial begin
        host_iiplus = 1'b0;
        if ($test$plusargs("IIPLUS")) host_iiplus = 1'b1;
        $display("tb_mb_falseread: host_is_iiplus=%0d tap=%0d", host_iiplus, iiplus_tap);

        // Reset + let the PHI0 majority filter lock.
        repeat (8) @(posedge clk);
        rstn = 1;
        repeat (6) bus_cycle(16'hE000, 1'b1, 8'hEA);   // idle ROM reads to lock

        $display("--- STA (ZP),Y $C404 = two cycles: dummy READ then WRITE ---");
        // MB-Audit's actual sequence around the false-read (addresses from the
        // captured trace): fetch operand, deref pointer, then the two $C404
        // cycles. We only need the R $C404 / W $C404 pair for the wrapper.
        bus_cycle(16'h3542, 1'b1, 8'h91);   // opcode STA (zp),Y
        bus_cycle(16'h3543, 1'b1, 8'hFE);   // operand (zp)
        bus_cycle(16'h00FE, 1'b1, 8'h00);   // ptr lo
        bus_cycle(16'h00FF, 1'b1, 8'hC4);   // ptr hi -> $C400, Y=4 -> $C404
        $display("  >>> dummy READ $C404 (this must produce data_en rw=1):");
        bus_cycle(16'hC404, 1'b1, 8'hFF);   // dummy READ of T1C-L
        $display("  >>> WRITE $C404:");
        bus_cycle(16'hC404, 1'b0, 8'h00);   // WRITE T1C-L
        bus_cycle(16'h3544, 1'b1, 8'hA9);   // next opcode

        repeat (4) bus_cycle(16'hE000, 1'b1, 8'hEA);
        $display("tb_mb_falseread: done");
        $finish;
    end
endmodule
