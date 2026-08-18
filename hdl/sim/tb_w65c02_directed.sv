`timescale 1ns / 1ps

// Pin- and control-path checks that are not represented by the WDC
// single-step JSON corpus (notably reset, IRQ/NMI, RDY, WAI, STP, SO, MLB,
// and VPB).
module tb_w65c02_directed;
    logic clk = 1'b0;
    logic reset_n = 1'b0;
    logic enable = 1'b1;
    logic ready = 1'b1;
    logic irq_n = 1'b1;
    logic nmi_n = 1'b1;
    logic so_n = 1'b1;

    logic [7:0] data_in;
    logic [15:0] addr;
    logic [7:0] data_out;
    logic rwb;
    logic sync;
    logic vpb_n;
    logic mlb_n;
    logic waiting;
    logic stopped;
    logic instruction_done;

    logic debug_load = 1'b0;
    logic [15:0] debug_pc_in;
    logic [7:0] debug_s_in;
    logic [7:0] debug_a_in;
    logic [7:0] debug_x_in;
    logic [7:0] debug_y_in;
    logic [7:0] debug_p_in;
    logic [15:0] debug_pc;
    logic [7:0] debug_s;
    logic [7:0] debug_a;
    logic [7:0] debug_x;
    logic [7:0] debug_y;
    logic [7:0] debug_p;

    logic [7:0] memory [0:65535];
    integer index;
    integer checks;

    w65c02_core #(
        .DEBUG_STATE_LOAD(1'b1)
    ) dut (
        .clk(clk),
        .reset_n(reset_n),
        .enable(enable),
        .ready(ready),
        .irq_n(irq_n),
        .nmi_n(nmi_n),
        .so_n(so_n),
        .data_in(data_in),
        .addr(addr),
        .data_out(data_out),
        .rwb(rwb),
        .sync(sync),
        .vpb_n(vpb_n),
        .mlb_n(mlb_n),
        .waiting(waiting),
        .stopped(stopped),
        .instruction_done(instruction_done),
        .debug_load(debug_load),
        .debug_pc_in(debug_pc_in),
        .debug_s_in(debug_s_in),
        .debug_a_in(debug_a_in),
        .debug_x_in(debug_x_in),
        .debug_y_in(debug_y_in),
        .debug_p_in(debug_p_in),
        .debug_pc(debug_pc),
        .debug_s(debug_s),
        .debug_a(debug_a),
        .debug_x(debug_x),
        .debug_y(debug_y),
        .debug_p(debug_p)
    );

    assign data_in = memory[addr];

    always_ff @(posedge clk) begin
        if (reset_n && !debug_load && enable && ready && !rwb)
            memory[addr] <= data_out;
    end

    task automatic tick;
        begin
            #5 clk = 1'b1;
            #5 clk = 1'b0;
        end
    endtask

    task automatic fail(input string reason);
        begin
            $display("W65C02 DIRECTED FAIL: %s", reason);
            $display("  bus addr=%04x rwb=%0d din=%02x dout=%02x sync=%0d vpb_n=%0d mlb_n=%0d",
                     addr, rwb, data_in, data_out, sync, vpb_n, mlb_n);
            $display("  state PC=%04x S=%02x A=%02x X=%02x Y=%02x P=%02x wait=%0d stop=%0d",
                     debug_pc, debug_s, debug_a, debug_x, debug_y, debug_p,
                     waiting, stopped);
            $fatal(1, "W65C02 directed mismatch");
        end
    endtask

    task automatic check(input logic condition, input string reason);
        begin
            if (!condition)
                fail(reason);
            checks = checks + 1;
        end
    endtask

    task automatic check_read(
        input logic [15:0] expected_addr,
        input logic expected_sync,
        input logic expected_vpb_n,
        input logic expected_mlb_n
    );
        begin
            #1;
            check(rwb === 1'b1, "expected read cycle");
            check(addr === expected_addr,
                  $sformatf("read address %04x != %04x", addr, expected_addr));
            check(sync === expected_sync, "unexpected SYNC state");
            check(vpb_n === expected_vpb_n, "unexpected VPB state");
            check(mlb_n === expected_mlb_n, "unexpected MLB state");
        end
    endtask

    task automatic check_write(
        input logic [15:0] expected_addr,
        input logic [7:0] expected_data,
        input logic expected_mlb_n
    );
        begin
            #1;
            check(rwb === 1'b0, "expected write cycle");
            check(addr === expected_addr,
                  $sformatf("write address %04x != %04x", addr, expected_addr));
            check(data_out === expected_data,
                  $sformatf("write data %02x != %02x", data_out, expected_data));
            check(sync === 1'b0, "write asserted SYNC");
            check(vpb_n === 1'b1, "write asserted VPB");
            check(mlb_n === expected_mlb_n, "unexpected write MLB state");
        end
    endtask

    task automatic load_state(
        input logic [15:0] pc,
        input logic [7:0] s,
        input logic [7:0] a,
        input logic [7:0] x,
        input logic [7:0] y,
        input logic [7:0] p
    );
        begin
            enable = 1'b1;
            ready = 1'b1;
            irq_n = 1'b1;
            nmi_n = 1'b1;
            so_n = 1'b1;
            debug_pc_in = pc;
            debug_s_in = s;
            debug_a_in = a;
            debug_x_in = x;
            debug_y_in = y;
            debug_p_in = p;
            debug_load = 1'b1;
            tick();
            debug_load = 1'b0;
            #1;
        end
    endtask

    initial begin
        checks = 0;
        for (index = 0; index < 65536; index = index + 1)
            memory[index] = 8'h00;

        // Reset is seven bus cycles after RESB is released: two internal
        // reads, three stack reads, and the two vector reads.
        memory[16'hFFFC] = 8'h34;
        memory[16'hFFFD] = 8'h12;
        tick();
        reset_n = 1'b1;
        check_read(16'h0000, 1'b0, 1'b1, 1'b1); tick();
        check_read(16'h0000, 1'b0, 1'b1, 1'b1); tick();
        check_read(16'h01FF, 1'b0, 1'b1, 1'b1); tick();
        check_read(16'h01FE, 1'b0, 1'b1, 1'b1); tick();
        check_read(16'h01FD, 1'b0, 1'b1, 1'b1); tick();
        check_read(16'hFFFC, 1'b0, 1'b0, 1'b1); tick();
        check_read(16'hFFFD, 1'b0, 1'b0, 1'b1); tick();
        check(debug_pc == 16'h1234, "reset vector did not load PC");
        check(debug_s == 8'hFC, "reset did not perform three stack cycles");
        check(debug_p[2] && !debug_p[3], "reset flags I/D incorrect");
        check_read(16'h1234, 1'b1, 1'b1, 1'b1);

        // RDY holds both the opcode and operand cycles without changing any
        // architectural state.
        memory[16'h6000] = 8'hA9; // LDA #$55
        memory[16'h6001] = 8'h55;
        load_state(16'h6000, 8'hFF, 8'h11, 8'h22, 8'h33, 8'h24);
        ready = 1'b0;
        check_read(16'h6000, 1'b1, 1'b1, 1'b1); tick(); tick();
        check(debug_pc == 16'h6000 && debug_a == 8'h11,
              "RDY-low opcode fetch changed state");
        ready = 1'b1;
        tick();
        ready = 1'b0;
        check_read(16'h6001, 1'b0, 1'b1, 1'b1); tick(); tick();
        check(debug_pc == 16'h6001 && debug_a == 8'h11,
              "RDY-low operand read changed state");
        ready = 1'b1;
        tick();
        check(debug_pc == 16'h6002 && debug_a == 8'h55 && instruction_done,
              "LDA did not complete after RDY release");

        // The separate clock-enable input is also fully static.
        memory[16'h6300] = 8'hEA;
        load_state(16'h6300, 8'hFF, 8'h00, 8'h00, 8'h00, 8'h24);
        enable = 1'b0;
        check_read(16'h6300, 1'b1, 1'b1, 1'b1); tick(); tick();
        check(debug_pc == 16'h6300, "enable-low fetch changed state");
        enable = 1'b1;
        tick(); tick();
        check(debug_pc == 16'h6301 && instruction_done,
              "instruction did not resume after enable");

        // A stalled write must not reach memory until RDY is released.
        memory[16'h6100] = 8'h8D; // STA $1234
        memory[16'h6101] = 8'h34;
        memory[16'h6102] = 8'h12;
        memory[16'h1234] = 8'h00;
        load_state(16'h6100, 8'hFF, 8'hAA, 8'h00, 8'h00, 8'h24);
        tick(); tick(); tick();
        ready = 1'b0;
        check_write(16'h1234, 8'hAA, 1'b1); tick(); tick();
        check(memory[16'h1234] == 8'h00, "RDY-low write reached memory");
        ready = 1'b1;
        tick();
        check(memory[16'h1234] == 8'hAA && instruction_done,
              "stalled write did not complete");

        // MLB is asserted for the WDC-defined modify and write cycles only.
        memory[16'h6200] = 8'hE6; // INC $10
        memory[16'h6201] = 8'h10;
        memory[16'h0010] = 8'h7F;
        load_state(16'h6200, 8'hFF, 8'h00, 8'h00, 8'h00, 8'h24);
        tick(); tick();
        check_read(16'h0010, 1'b0, 1'b1, 1'b1); tick();
        check_read(16'h0010, 1'b0, 1'b1, 1'b0); tick();
        check_write(16'h0010, 8'h80, 1'b0); tick();
        check(memory[16'h0010] == 8'h80 && debug_p[7] && instruction_done,
              "INC/MLB sequence produced wrong result");

        // Apple-compatible 65C02s false-read the effective address before a
        // same-page STA abs,X/abs,Y write.  A page-crossing STA instead reads
        // the final instruction byte during the extra cycle.
        memory[16'h6500] = 8'h9D; // STA $C440,X
        memory[16'h6501] = 8'h40;
        memory[16'h6502] = 8'hC4;
        memory[16'hC444] = 8'h5A;
        load_state(16'h6500, 8'hFF, 8'hA5, 8'h04, 8'h00, 8'h24);
        check_read(16'h6500, 1'b1, 1'b1, 1'b1); tick();
        check_read(16'h6501, 1'b0, 1'b1, 1'b1); tick();
        check_read(16'h6502, 1'b0, 1'b1, 1'b1); tick();
        check_read(16'hC444, 1'b0, 1'b1, 1'b1); tick();
        check_write(16'hC444, 8'hA5, 1'b1); tick();
        check(memory[16'hC444] == 8'hA5 && instruction_done,
              "same-page STA abs,X false-read/write sequence incorrect");

        memory[16'h6510] = 8'h9D; // STA $C4FE,X
        memory[16'h6511] = 8'hFE;
        memory[16'h6512] = 8'hC4;
        memory[16'hC502] = 8'h5A;
        load_state(16'h6510, 8'hFF, 8'hB6, 8'h04, 8'h00, 8'h24);
        check_read(16'h6510, 1'b1, 1'b1, 1'b1); tick();
        check_read(16'h6511, 1'b0, 1'b1, 1'b1); tick();
        check_read(16'h6512, 1'b0, 1'b1, 1'b1); tick();
        check_read(16'h6512, 1'b0, 1'b1, 1'b1); tick();
        check_write(16'hC502, 8'hB6, 1'b1); tick();
        check(memory[16'hC502] == 8'hB6 && instruction_done,
              "page-crossing STA abs,X dummy-read/write sequence incorrect");

        memory[16'h6520] = 8'h99; // STA $C440,Y
        memory[16'h6521] = 8'h40;
        memory[16'h6522] = 8'hC4;
        memory[16'hC444] = 8'h5A;
        load_state(16'h6520, 8'hFF, 8'hC7, 8'h00, 8'h04, 8'h24);
        check_read(16'h6520, 1'b1, 1'b1, 1'b1); tick();
        check_read(16'h6521, 1'b0, 1'b1, 1'b1); tick();
        check_read(16'h6522, 1'b0, 1'b1, 1'b1); tick();
        check_read(16'hC444, 1'b0, 1'b1, 1'b1); tick();
        check_write(16'hC444, 8'hC7, 1'b1); tick();
        check(memory[16'hC444] == 8'hC7 && instruction_done,
              "same-page STA abs,Y false-read/write sequence incorrect");

        // Decimal ADC commits A/P only when its extra cycle completes.  RDY
        // must hold that cycle, its dummy bus read, and instruction_done.
        memory[16'h6600] = 8'h69; // ADC #$01: $99 + $01 = $00, C/Z set
        memory[16'h6601] = 8'h01;
        load_state(16'h6600, 8'hFF, 8'h99, 8'h00, 8'h00, 8'h28);
        check_read(16'h6600, 1'b1, 1'b1, 1'b1); tick();
        check_read(16'h6601, 1'b0, 1'b1, 1'b1); tick();
        check_read(16'h007F, 1'b0, 1'b1, 1'b1);
        check(debug_pc == 16'h6602 && debug_a == 8'h99 && debug_p == 8'h28,
              "decimal ADC changed state before its extra cycle");
        check(instruction_done === 1'b0,
              "decimal ADC completed before its extra cycle");
        ready = 1'b0;
        tick();
        check_read(16'h007F, 1'b0, 1'b1, 1'b1);
        check(debug_pc == 16'h6602 && debug_a == 8'h99 && debug_p == 8'h28,
              "RDY-low decimal ADC extra cycle changed state");
        check(instruction_done === 1'b0,
              "RDY-low decimal ADC asserted instruction_done");
        tick();
        check_read(16'h007F, 1'b0, 1'b1, 1'b1);
        check(debug_pc == 16'h6602 && debug_a == 8'h99 && debug_p == 8'h28,
              "second RDY-low decimal ADC cycle changed state");
        check(instruction_done === 1'b0,
              "second RDY-low decimal ADC cycle asserted instruction_done");
        ready = 1'b1;
        tick();
        check_read(16'h6602, 1'b1, 1'b1, 1'b1);
        check(debug_pc == 16'h6602 && debug_a == 8'h00 && debug_p == 8'h2B,
              "decimal ADC extra-cycle commit was incorrect");
        check(instruction_done === 1'b1,
              "decimal ADC completion did not assert instruction_done");

        // The same commit rule applies to SBC, including a clock-enable hold.
        memory[16'h6610] = 8'hE9; // SBC #$01: $00 - $01 = $99, C clear
        memory[16'h6611] = 8'h01;
        load_state(16'h6610, 8'hFF, 8'h00, 8'h00, 8'h00, 8'h29);
        check_read(16'h6610, 1'b1, 1'b1, 1'b1); tick();
        check_read(16'h6611, 1'b0, 1'b1, 1'b1); tick();
        check_read(16'h0000, 1'b0, 1'b1, 1'b1);
        check(debug_pc == 16'h6612 && debug_a == 8'h00 && debug_p == 8'h29,
              "decimal SBC changed state before its extra cycle");
        check(instruction_done === 1'b0,
              "decimal SBC completed before its extra cycle");
        enable = 1'b0;
        tick();
        check_read(16'h0000, 1'b0, 1'b1, 1'b1);
        check(debug_pc == 16'h6612 && debug_a == 8'h00 && debug_p == 8'h29,
              "disabled decimal SBC extra cycle changed state");
        check(instruction_done === 1'b0,
              "disabled decimal SBC asserted instruction_done");
        tick();
        check_read(16'h0000, 1'b0, 1'b1, 1'b1);
        check(debug_pc == 16'h6612 && debug_a == 8'h00 && debug_p == 8'h29,
              "second disabled decimal SBC cycle changed state");
        check(instruction_done === 1'b0,
              "second disabled decimal SBC cycle asserted instruction_done");
        enable = 1'b1;
        tick();
        check_read(16'h6612, 1'b1, 1'b1, 1'b1);
        check(debug_pc == 16'h6612 && debug_a == 8'h99 && debug_p == 8'hA8,
              "decimal SBC extra-cycle commit was incorrect");
        check(instruction_done === 1'b1,
              "decimal SBC completion did not assert instruction_done");

        // An SO edge during a stalled decimal extra cycle must survive the
        // pending arithmetic commit instead of being overwritten by its V.
        memory[16'h6620] = 8'h69; // ADC #$27: $15 + $27 = $42, V clear
        memory[16'h6621] = 8'h27;
        load_state(16'h6620, 8'hFF, 8'h15, 8'h00, 8'h00, 8'h28);
        check_read(16'h6620, 1'b1, 1'b1, 1'b1); tick();
        check_read(16'h6621, 1'b0, 1'b1, 1'b1); tick();
        ready = 1'b0;
        check_read(16'h007F, 1'b0, 1'b1, 1'b1); tick();
        so_n = 1'b0;
        check_read(16'h007F, 1'b0, 1'b1, 1'b1); tick();
        so_n = 1'b1;
        check_read(16'h007F, 1'b0, 1'b1, 1'b1); tick();
        check_read(16'h007F, 1'b0, 1'b1, 1'b1);
        check(debug_pc == 16'h6622 && debug_a == 8'h15,
              "stalled decimal ADC with SO changed PC or A");
        check(instruction_done === 1'b0,
              "stalled decimal ADC with SO asserted instruction_done");
        ready = 1'b1;
        tick();
        check_read(16'h6622, 1'b1, 1'b1, 1'b1);
        check(debug_a == 8'h42 && debug_p == 8'h68,
              "SO edge was lost at decimal ADC completion");
        check(instruction_done === 1'b1,
              "decimal ADC with SO did not complete exactly once released");

        // SO is edge-sensitive and is sampled even while the bus is stalled.
        load_state(16'h6400, 8'hFF, 8'h00, 8'h00, 8'h00, 8'h24);
        ready = 1'b0;
        so_n = 1'b0;
        tick();
        check(debug_p[6], "SO falling edge did not set V while RDY was low");
        ready = 1'b1;
        so_n = 1'b1;

        // A maskable IRQ is recognized at the fetch boundary, pushes B=0,
        // clears D, sets I, and uses the IRQ/BRK vector with VPB asserted.
        memory[16'h4000] = 8'hEA;
        memory[16'hFFFE] = 8'hCD;
        memory[16'hFFFF] = 8'hAB;
        load_state(16'h4000, 8'hF0, 8'h00, 8'h00, 8'h00, 8'h28);
        irq_n = 1'b0;
        check_read(16'h4000, 1'b1, 1'b1, 1'b1); tick();
        check_read(16'h4000, 1'b0, 1'b1, 1'b1); tick();
        check_write(16'h01F0, 8'h40, 1'b1); tick();
        check_write(16'h01EF, 8'h00, 1'b1); tick();
        check_write(16'h01EE, 8'h28, 1'b1); tick();
        check_read(16'hFFFE, 1'b0, 1'b0, 1'b1); tick();
        check_read(16'hFFFF, 1'b0, 1'b0, 1'b1); tick();
        check(debug_pc == 16'hABCD && debug_s == 8'hED,
              "IRQ vector/stack result incorrect");
        check(debug_p[2] && !debug_p[3] && instruction_done,
              "IRQ did not set I, clear D, or complete");
        irq_n = 1'b1;

        // An NMI edge on the exact fetch that observes IRQ must not be lost.
        // NMI has priority even though it was not pending on an earlier cycle.
        memory[16'h5100] = 8'hEA;
        memory[16'hFFFA] = 8'h9A;
        memory[16'hFFFB] = 8'hBC;
        load_state(16'h5100, 8'hD0, 8'h00, 8'h00, 8'h00, 8'h20);
        nmi_n = 1'b0;
        irq_n = 1'b0;
        check_read(16'h5100, 1'b1, 1'b1, 1'b1); tick();
        check_read(16'h5100, 1'b0, 1'b1, 1'b1); tick();
        check_write(16'h01D0, 8'h51, 1'b1); tick();
        check_write(16'h01CF, 8'h00, 1'b1); tick();
        check_write(16'h01CE, 8'h20, 1'b1); tick();
        check_read(16'hFFFA, 1'b0, 1'b0, 1'b1); tick();
        check_read(16'hFFFB, 1'b0, 1'b0, 1'b1); tick();
        check(debug_pc == 16'hBC9A && debug_s == 8'hCD,
              "same-fetch NMI edge was lost or IRQ won priority");
        check(debug_p[2] && instruction_done,
              "same-fetch NMI service did not complete");
        nmi_n = 1'b1;
        irq_n = 1'b1;

        // NMI is falling-edge sensitive, waits for the current instruction,
        // and wins over a simultaneous level-sensitive IRQ.
        memory[16'h5000] = 8'hEA;
        memory[16'h5001] = 8'hEA;
        memory[16'hFFFA] = 8'h78;
        memory[16'hFFFB] = 8'h56;
        load_state(16'h5000, 8'hE0, 8'h00, 8'h00, 8'h00, 8'h28);
        tick();
        nmi_n = 1'b0;
        irq_n = 1'b0;
        tick();
        check(debug_pc == 16'h5001 && instruction_done,
              "NMI interrupted before the current instruction completed");
        check_read(16'h5001, 1'b1, 1'b1, 1'b1); tick();
        check_read(16'h5001, 1'b0, 1'b1, 1'b1); tick();
        check_write(16'h01E0, 8'h50, 1'b1); tick();
        check_write(16'h01DF, 8'h01, 1'b1); tick();
        check_write(16'h01DE, 8'h28, 1'b1); tick();
        check_read(16'hFFFA, 1'b0, 1'b0, 1'b1); tick();
        check_read(16'hFFFB, 1'b0, 1'b0, 1'b1); tick();
        check(debug_pc == 16'h5678 && debug_s == 8'hDD,
              "NMI vector/stack result incorrect or IRQ won priority");
        check(debug_p[2] && !debug_p[3] && instruction_done,
              "NMI did not set I, clear D, or complete");
        nmi_n = 1'b1;
        irq_n = 1'b1;

        // WAI stops after its dummy read. A masked IRQ wakes without vectoring.
        memory[16'h2000] = 8'hCB;
        memory[16'h2001] = 8'hEA;
        load_state(16'h2000, 8'hFF, 8'h00, 8'h00, 8'h00, 8'h24);
        tick();
        check_read(16'h2001, 1'b0, 1'b1, 1'b1); tick();
        check(waiting && debug_pc == 16'h2001, "WAI did not enter wait state");
        tick(); tick();
        check(waiting && debug_pc == 16'h2001, "WAI advanced without interrupt");
        irq_n = 1'b0;
        tick();
        check(!waiting, "masked IRQ did not wake WAI");
        check_read(16'h2001, 1'b1, 1'b1, 1'b1);
        irq_n = 1'b1;

        // With I clear, WAI wake-up enters the normal IRQ sequence.
        memory[16'h2100] = 8'hCB;
        memory[16'hFFFE] = 8'h34;
        memory[16'hFFFF] = 8'h12;
        load_state(16'h2100, 8'hF8, 8'h00, 8'h00, 8'h00, 8'h20);
        tick(); tick();
        check(waiting && debug_pc == 16'h2101, "second WAI did not wait");
        irq_n = 1'b0;
        tick();
        check_read(16'h2101, 1'b0, 1'b1, 1'b1); tick();
        check_write(16'h01F8, 8'h21, 1'b1); tick();
        check_write(16'h01F7, 8'h01, 1'b1); tick();
        check_write(16'h01F6, 8'h20, 1'b1); tick();
        check_read(16'hFFFE, 1'b0, 1'b0, 1'b1); tick();
        check_read(16'hFFFF, 1'b0, 1'b0, 1'b1); tick();
        check(debug_pc == 16'h1234 && debug_s == 8'hF5 && instruction_done,
              "WAI IRQ service result incorrect");
        irq_n = 1'b1;

        // STP ignores interrupts and only reset can leave the stopped state.
        memory[16'h3000] = 8'hDB;
        load_state(16'h3000, 8'hFF, 8'h00, 8'h00, 8'h00, 8'h20);
        tick();
        check_read(16'h3001, 1'b0, 1'b1, 1'b1); tick();
        check(stopped && debug_pc == 16'h3001, "STP did not stop");
        irq_n = 1'b0;
        nmi_n = 1'b0;
        tick(); tick();
        check(stopped && debug_pc == 16'h3001, "interrupt escaped STP");
        reset_n = 1'b0;
        #1;
        check(!stopped, "reset did not leave STP");

        $display("W65C02 DIRECTED PASS checks=%0d", checks);
        $finish;
    end
endmodule
