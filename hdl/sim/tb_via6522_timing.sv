`timescale 1ns / 1ps

// Focused 6522 timer bus-timing regression. This mirrors MB-Audit
// T6522_3 subtest 0 (displayed as 11:03:00): write $00 to T1C-H after
// loading $FFFF, execute the fixed 15-cycle path, and read T1C-L.
// A real VIA exposes the value from before the current cycle's timer tick.
module tb_via6522_timing;
    logic clk = 1'b0;
    logic reset = 1'b1;
    logic power_reset = 1'b1;
    logic we = 1'b0;
    logic strobe = 1'b0;
    logic slow_clock = 1'b0;
    logic timer_read_extra_clock = 1'b0;
    logic [3:0] addr = 4'h0;
    logic [7:0] data_in = 8'h00;
    wire [7:0] data_out;

    always #5 clk = ~clk;

    via6522 dut (
        .clk(clk),
        .reset(reset),
        .power_reset(power_reset),
        .we(we),
        .porta_in(8'hFF),
        .portb_in(8'hFF),
        .ifr_set_ext(7'h00),
        .ifr_clr_ext(7'h00),
        .ca1_in(1'b1),
        .ca2_in(1'b0),
        .cb1_in(1'b0),
        .cb2_in(1'b0),
        .strobe(strobe),
        .slow_clock(slow_clock),
        .timer_read_extra_clock(timer_read_extra_clock),
        .addr(addr),
        .data_in(data_in),
        .data_out(data_out),
        .irq(),
        .porta_out(),
        .portb_out(),
        .portb_bus(),
        .pcr_out(),
        .ddrb_out(),
        .ca2_out(),
        .cb1_out(),
        .cb2_out()
    );

    // One Apple cycle. The timer cadence pulse precedes serve_en/data_en
    // in apple_bus_wrapper, so every bus operation starts with this tick.
    task automatic timer_tick;
        @(negedge clk);
        slow_clock = 1'b1;
        @(negedge clk);
        slow_clock = 1'b0;
    endtask

    task automatic apple_write(input logic [3:0] reg_addr,
                               input logic [7:0] value);
        timer_tick();
        addr = reg_addr;
        data_in = value;
        we = 1'b1;
        strobe = 1'b1;
        @(negedge clk);
        strobe = 1'b0;
        we = 1'b0;
    endtask

    task automatic idle_cycle;
        timer_tick();
    endtask

    task automatic apple_read(input logic [3:0] reg_addr,
                              output logic [7:0] value);
        timer_tick();

        // serve_en changes the authoritative register address and captures
        // data_out here, before the later read strobe at data_en.
        addr = reg_addr;
        we = 1'b0;
        #1 value = data_out;

        strobe = 1'b1;
        @(negedge clk);
        strobe = 1'b0;
    endtask

    logic [7:0] read_value;
    logic [7:0] read_value_2;
    integer i;
    integer boundary_fails = 0;
    integer t15_fails = 0;

    // MB-Audit T6522_F (11:0F) and T6522_10/T6522_11 check the
    // one-cycle interval between counter underflow and the corresponding IFR
    // flag.  A read during the $FFFF cycle must still see the timer flag
    // clear; the next cycle must see it set.  The load value differs by one
    // so both cases use the same 17-cycle instruction path as T6522_F.
    task automatic check_ifr_boundary(input logic timer2,
                                      input logic [7:0] initial_low,
                                      input logic expect_set,
                                      input string name);
        logic [7:0] v;
        logic [3:0] lo_addr;
        logic [3:0] hi_addr;
        logic [7:0] flag;

        lo_addr = timer2 ? 4'h8 : 4'h4;
        hi_addr = timer2 ? 4'h9 : 4'h5;
        flag = timer2 ? 8'h20 : 8'h40;

        apple_write(4'hE, 8'h60);         // disable both timer IRQs
        apple_write(4'hD, 8'h60);         // clear both stale timer flags
        apple_write(lo_addr, initial_low);
        apple_write(hi_addr, 8'h00);      // start at $0010 or $000F

        // LDA #imm (2), LDY #IER (2), STA (zp),Y (6), DEY (2),
        // then the first four cycles of LDA (zp),Y.  apple_read below is
        // its fifth/final cycle, for 17 timer ticks after the load.
        repeat (9) idle_cycle();
        apple_write(4'hE, 8'h80 | flag);  // sixth cycle of STA (zp),Y
        repeat (6) idle_cycle();
        apple_read(4'hD, v);

        if ((v & flag) !== (expect_set ? flag : 8'h00)) begin
            boundary_fails = boundary_fails + 1;
            $display("  FAIL %s: IFR=%02X, expected timer flag %s",
                     name, v, expect_set ? "set" : "clear");
        end else
            $display("  ok   %s: IFR=%02X (%s)",
                     name, v, expect_set ? "set" : "clear");
    endtask

    // MB-Audit T6522_15 (11:15): T1 interrupt-flag clear behaviour.
    // Load T1=$0001 in one-shot, let it underflow so IFR.T1 sets, then check
    // which register accesses clear it. IFR.T1 must clear on: read T1C-L,
    // write T1C-H, write T1L-H; and must NOT clear on: write T1C-L / T1L-L.
    task automatic t15_setup_flag;
        apple_write(4'hE, 8'h60);   // IER: disable both timer interrupts
        apple_write(4'hD, 8'h60);   // IFR: clear both stale timer flags
        apple_write(4'h4, 8'h01);   // T1C-L = $01
        apple_write(4'h5, 8'h00);   // T1C-H = $00  -> T1=$0001, one-shot; flag sets on underflow
        repeat (5) idle_cycle();    // let the $0001 counter underflow
    endtask

    task automatic t15_check(input logic expect_set, input string name);
        logic [7:0] v;
        apple_read(4'hD, v);        // read IFR
        if ((v & 8'h40) !== (expect_set ? 8'h40 : 8'h00)) begin
            t15_fails = t15_fails + 1;
            $display("  FAIL %s: IFR.T1=%02X, expected %s",
                     name, v, expect_set ? "set($40)" : "clear($00)");
        end else
            $display("  ok   %s: IFR.T1=%02X (%s)",
                     name, v, expect_set ? "set" : "clear");
    endtask

    initial begin
        repeat (3) @(posedge clk);
        reset = 1'b0;
        power_reset = 1'b0;

        // MB-Audit primes T1C to $FFFF, then subtest 0 writes $00 to T1C-H.
        apple_write(4'h4, 8'hFF);
        apple_write(4'h5, 8'hFF);
        apple_write(4'h5, 8'h00);

        // JSR (6), LDY zp (3), DEY (2), then the first three cycles of
        // LDA abs,Y. Its fourth/final cycle is the timer-register read below.
        for (i = 0; i < 14; i = i + 1)
            idle_cycle();
        apple_read(4'h4, read_value);

        if (read_value !== 8'hF1)
            $fatal(1,
                   "MB-Audit 11:03:00: T1C-L got %02X, expected F1",
                   read_value);

        // MB-Audit T6522_4 applies the same check to Timer 2.
        apple_write(4'h8, 8'hFF);
        apple_write(4'h9, 8'hFF);
        apple_write(4'h9, 8'h00);
        for (i = 0; i < 14; i = i + 1)
            idle_cycle();
        apple_read(4'h8, read_value);
        if (read_value !== 8'hF1)
            $fatal(1,
                   "MB-Audit 11:04:00: T2C-L got %02X, expected F1",
                   read_value);

        // T6522_C's standard Mockingboard detection reads T1C-L twice,
        // five Apple cycles apart, and requires a delta of exactly five.
        apple_write(4'h4, 8'h80);
        apple_write(4'h5, 8'h00);
        apple_read(4'h4, read_value);
        for (i = 0; i < 4; i = i + 1)
            idle_cycle();
        apple_read(4'h4, read_value_2);
        if ((read_value - read_value_2) !== 8'h05)
            $fatal(1,
                   "MB-Audit 11:0C:00: T1C-L delta got %02X, expected 05",
                   read_value - read_value_2);

        // ===== MB-Audit T6522_F/T6522_10/T6522_11: IFR boundary =====
        $display("tb_via6522_timing: T6522_F (11:0F) IFR boundary checks:");
        check_ifr_boundary(1'b0, 8'h10, 1'b0,
                           "T1=$0010, final read at $FFFF");
        check_ifr_boundary(1'b0, 8'h0F, 1'b1,
                           "T1=$000F, final read after reload");
        check_ifr_boundary(1'b1, 8'h10, 1'b0,
                           "T2=$0010, final read at $FFFF");
        check_ifr_boundary(1'b1, 8'h0F, 1'b1,
                           "T2=$000F, final read at $FFFE");
        if (boundary_fails != 0)
            $fatal(1, "MB-Audit IFR boundary: %0d subtest(s) failed",
                   boundary_fails);

        // ===== MB-Audit T6522_15 (11:15): T1 flag clear behaviour =====
        $display("tb_via6522_timing: T6522_15 (11:15) T1 flag-clear checks:");
        apple_write(4'hB, 8'h00);           // ACR = one-shot

        t15_setup_flag();
        t15_check(1'b1, "setup (flag set)");            // must be set after underflow

        // subtest #1: write T1C-L via 'STA ABS' must NOT clear
        apple_write(4'h4, 8'h00);
        t15_check(1'b1, "#1 write T1C-L (no clear)");

        // subtest #2: 6502 STA (zp),Y false-read followed by its write.
        t15_setup_flag();
        apple_read(4'h4, read_value);
        apple_write(4'h4, 8'h00);
        t15_check(1'b0, "#2 false-read T1C-L (clear)");

        // subtest #3: write T1L-L must NOT clear
        t15_setup_flag();
        apple_write(4'h6, 8'h00);
        t15_check(1'b1, "#3 write T1L-L (no clear)");

        // subtest #4: write T1L-H must clear
        t15_setup_flag();
        apple_write(4'h7, 8'h00);
        t15_check(1'b0, "#4 write T1L-H (clear)");

        // subtest #5: read T1C-L must clear
        t15_setup_flag();
        apple_read(4'h4, read_value);
        t15_check(1'b0, "#5 read T1C-L (clear)");

        // subtest #6: write T1C-H must clear
        t15_setup_flag();
        apple_write(4'h5, 8'h01);
        t15_check(1'b0, "#6 write T1C-H (clear)");

        if (t15_fails != 0)
            $fatal(1, "MB-Audit 11:15 (T6522_15): %0d flag-clear subtest(s) failed",
                   t15_fails);

        $display("tb_via6522_timing: MB-Audit timer timing checks passed");
        $finish;
    end
endmodule
