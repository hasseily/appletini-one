`timescale 1ns / 1ps
// Execute a real 65C02 write train through vTW and the physical bus wrapper,
// then check the production VIA/AY/Phasor state. The card observes writes;
// its read/IRQ output is intentionally unused by this write-only program.
module tb_vtw_sound_writes #(
    parameter logic [1:0] CORE_SPEED = 2'd0,
    parameter bit INDEXED_STORES = 1'b0
);
    timeunit 1ns;
    timeprecision 1ps;

    tb_vtw_slowdown #(.RUN_TESTS(1'b0), .SPEED_MODE(CORE_SPEED)) base();
    globals::AppleBus_read sound_ab;
    globals::SoftSwitchState sound_sss;
    logic sound_watch = 1'b0;
    integer sound_write_count = 0;
    integer sound_read_count = 0;
    integer program_offset;
    time first_write, last_write;

    always_comb begin
        sound_sss = '0;
        sound_sss.slot_access = 1'b1;
        sound_ab = base.ab_read;
        if (!base.sound_card_active) begin
            sound_ab.addr_en = 1'b0;
            sound_ab.data_en = 1'b0;
            sound_ab.sss_en = 1'b0;
            sound_ab.serve_en = 1'b0;
        end
    end
    mockingboard sound_card_i (
        .clk(base.clk), .rstn(base.rstn), .apple_q3_raw(base.phi0),
        .ab_read(sound_ab), .sss(sound_sss), .slot_assign(3'd4),
        .pan(48'h808080808080), .ssi_pan(8'h88), .audio_control(32'd0),
        .audio_sample_tick(1'b0), .ab_write(), .audio_l(), .audio_r(),
        .dbg_ssi_irq(), .dbg_ssi_backend_done(), .dbg_ssi_enable_ints()
    );
    always @(posedge base.clk) begin
        if (sound_watch && base.dut.core_en && base.dut.cycle_rw_q &&
            (base.dut.cycle_addr_q[15:8] == 8'hC4 ||
             base.dut.cycle_addr_q[15:4] == 12'hC0C))
            sound_read_count++;
        if (sound_watch && sound_ab.data_en && !sound_ab.rw &&
            (sound_ab.addr[15:8] == 8'hC4 || sound_ab.addr[15:4] == 12'hC0C)) begin
            if (sound_write_count == 0) first_write = $time;
            last_write = $time;
            sound_write_count++;
            if (!INDEXED_STORES)
                base.check(base.dut.slow_cnt_q == 0,
                           "absolute virtual sound write train entered slowdown");
        end
    end

    task automatic emit(input logic [7:0] value);
        base.sh_write(18'h23000 + 18'(program_offset), value);
        program_offset++;
    endtask
    task automatic store(input logic [15:0] address, input logic [7:0] value);
        emit(8'hA9); emit(value);  // LDA #value
        emit(INDEXED_STORES ? 8'h99 : 8'h8D);
        emit(address[7:0]); emit(address[15:8]);
    endtask

    initial begin
        int terminal;
        base.core_run = 0; base.enable = 0; base.rstn = 0; base.res_drive_low = 1;
        base.sound_card_active = 1;
        base.sd_region_en = 10'b00_0000_1000;
        base.sd_duration = 16'd512;
        repeat (20) @(posedge base.clk);
        base.rstn = 1;
        base.load_program_abs(16'hC400); // reset vector and defined stack
        program_offset = 0;
        emit(8'hA0); emit(8'h00); // LDY #0; same address for either STA form
        // AY0: DDRs, reset/release, latch mixer register7, write $3E.
        store(16'hC402, 8'hFF); store(16'hC403, 8'hFF);
        store(16'hC400, 8'h00); store(16'hC400, 8'h04);
        store(16'hC401, 8'h07); store(16'hC400, 8'h07);
        store(16'hC400, 8'h04); store(16'hC401, 8'h3E);
        store(16'hC400, 8'h06); store(16'hC400, 8'h04);
        // AY1: same bus protocol, write channel-A volume register8=$0F.
        store(16'hC482, 8'hFF); store(16'hC483, 8'hFF);
        store(16'hC480, 8'h00); store(16'hC480, 8'h04);
        store(16'hC481, 8'h08); store(16'hC480, 8'h07);
        store(16'hC480, 8'h04); store(16'hC481, 8'h0F);
        store(16'hC480, 8'h06); store(16'hC480, 8'h04);
        store(16'hC0CD, 8'h00); // Phasor native mode5
        terminal = 16'hF000 + program_offset;
        emit(8'h4C); emit(terminal[7:0]); emit(terminal[15:8]);
        base.enable = 1; #10us;
        base.res_drive_low = 0; #4us;
        sound_watch = 1; base.core_run = 1;
        // Includes initial physical-bus acquisition. Indexed stores really
        // read Cxxx before writing it, and must retain that read slowdown.
        repeat (400) @(negedge base.phi0);
        sound_watch = 0;
        base.check(sound_write_count == 21,
                   $sformatf("real card lost/duplicated writes: %0d", sound_write_count));
        base.check(sound_card_i.psg0.ymreg[7] == 8'h3E,
                   "AY0 mixer register lost accelerated writes");
        base.check(sound_card_i.psg1.ymreg[8] == 8'h0F,
                   "AY1 volume register lost accelerated writes");
        base.check(sound_card_i.phasor_mode_q == 3'd5,
                   "Phasor mode switch lost accelerated write");
        base.check(sound_read_count == (INDEXED_STORES ? 21 : 0),
                   $sformatf("unexpected destination-read count: %0d", sound_read_count));
        base.check(INDEXED_STORES ? base.dut.slow_cnt_q != 0 : base.dut.slow_cnt_q == 0,
                   "sound slowdown did not match absolute/indexed read behavior");
        if (base.fails == 0)
            $display("VTW SOUND WRITES PASS speed=%0d indexed=%0b writes=%0d reads=%0d AY0R7=%02X AY1R8=%02X mode=%0d slow=%0d span_ns=%0d",
                     CORE_SPEED, INDEXED_STORES, sound_write_count, sound_read_count,
                     sound_card_i.psg0.ymreg[7],
                     sound_card_i.psg1.ymreg[8], sound_card_i.phasor_mode_q,
                     base.dut.slow_cnt_q, (last_write - first_write) / 1ns);
        else
            $display("VTW SOUND WRITES FAIL checks=%0d", base.fails);
        $finish;
    end
endmodule
