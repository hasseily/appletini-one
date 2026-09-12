`timescale 1ns / 1ps

// Compare architectural state, every write, and every Apple I/O read against
// the cycle-exact core at instruction boundaries. RAM-only dummy reads may
// differ. The generated vector and Klaus benches add independent references.
module tb_w65c02_turbo;
    logic clk = 1'b0;
    logic reset_n = 1'b0;
    logic debug_load = 1'b0;
    logic [1:0] enable = 2'b00;
    logic [1:0] ready = 2'b11;
    logic turbo = 1'b1;
    logic irq_n = 1'b1;
    logic nmi_n = 1'b1;
    logic nmi_on_fetch = 1'b0;
    logic [15:0] initial_pc;
    logic [7:0] initial_s, initial_a, initial_x, initial_y, initial_p;
    wire [15:0] addr [0:1];
    wire [7:0] data_out [0:1];
    wire [1:0] rwb, sync, done, waiting, stopped;
    wire [3:0] cycle_ticks [0:1];
    wire [15:0] pc [0:1];
    wire [7:0] s [0:1], a [0:1], x [0:1], y [0:1], p [0:1];
    logic [7:0] memory [0:1][0:65535];
    logic [24:0] events [0:1][0:31];
    integer event_ticks [0:1][0:31];
    integer event_count [0:1];
    integer cycles [0:1];
    integer guest_cycles [0:1];
    integer cases = 0;
    integer saved_cycles = 0;
    logic [31:0] rng = 32'hC0201333;

    for (genvar core = 0; core < 2; core++) begin : cores
        w65c02_core #(.DEBUG_STATE_LOAD(1'b1)) dut (
            .clk(clk), .reset_n(reset_n), .enable(enable[core]),
            .ready(ready[core]), .turbo(core == 1 ? turbo : 1'b0),
            .irq_n(irq_n), .nmi_n(nmi_n), .so_n(1'b1),
            .data_in(memory[core][addr[core]]), .addr(addr[core]),
            .data_out(data_out[core]), .rwb(rwb[core]), .sync(sync[core]),
            .vpb_n(), .mlb_n(), .waiting(waiting[core]), .stopped(stopped[core]),
            .instruction_done(done[core]), .cycle_ticks(cycle_ticks[core]),
            .debug_load(debug_load),
            .debug_pc_in(initial_pc), .debug_s_in(initial_s),
            .debug_a_in(initial_a), .debug_x_in(initial_x),
            .debug_y_in(initial_y), .debug_p_in(initial_p),
            .debug_pc(pc[core]), .debug_s(s[core]), .debug_a(a[core]),
            .debug_x(x[core]), .debug_y(y[core]), .debug_p(p[core])
        );
    end

    task automatic tick;
        #5 clk = 1'b1;
        #5 clk = 1'b0;
    endtask

    task automatic check(input logic condition, input string reason);
        if (condition !== 1'b1)
            $fatal(1, "TURBO FAIL case=%0d pc=%04x opcode=%02x: %s",
                   cases, initial_pc, memory[0][initial_pc], reason);
    endtask

    function automatic logic [31:0] random_word;
        rng = rng ^ (rng << 13);
        rng = rng ^ (rng >> 17);
        rng = rng ^ (rng << 5);
        return rng;
    endfunction

    task automatic put(input logic [15:0] address, input logic [7:0] value);
        memory[0][address] = value;
        memory[1][address] = value;
    endtask

    task automatic load;
        enable = 2'b00;
        ready = 2'b11;
        debug_load = 1'b1;
        tick();
        debug_load = 1'b0;
        enable = 2'b11;
        for (int core = 0; core < 2; core++) begin
            event_count[core] = 0;
            cycles[core] = 0;
            guest_cycles[core] = 0;
        end
    endtask

    task automatic run_instruction(
        input logic start_turbo,
        input integer switch_after_fetch,
        input logic later_turbo,
        input integer expected_fast_cycles,
        input logic stalls
    );
        integer elapsed;
        logic [24:0] frozen_bus [0:1];
        logic [1:0] frozen;
        turbo = start_turbo;
        load();
        if (nmi_on_fetch)
            nmi_n = 1'b0;
        elapsed = 0;
        while (enable != 2'b00 && elapsed < 100) begin
            #1;
            for (int core = 0; core < 2; core++) begin
                ready[core] = !stalls || (random_word() & 3) != 0;
                frozen[core] = enable[core] && !ready[core];
                frozen_bus[core] = {rwb[core], addr[core], data_out[core]};
                if (enable[core] && ready[core]) begin
                    cycles[core]++;
                    check(cycle_ticks[core] >= 1 && cycle_ticks[core] <= 3,
                          "invalid accepted-step guest-cycle count");
                    if (core == 0)
                        check(cycle_ticks[core] == 1, "classic step must count one cycle");
                    if (!rwb[core] || addr[core][15:12] == 4'hC) begin
                        check(event_count[core] < 32, "event buffer overflow");
                        events[core][event_count[core]] =
                            {rwb[core], addr[core], rwb[core]
                             ? memory[core][addr[core]] : data_out[core]};
                        event_ticks[core][event_count[core]] = guest_cycles[core] + 1;
                        event_count[core]++;
                    end
                    guest_cycles[core] += cycle_ticks[core];
                    if (!rwb[core])
                        memory[core][addr[core]] = data_out[core];
                end
            end
            tick();
            for (int core = 0; core < 2; core++) begin
                if (frozen[core])
                    check(frozen_bus[core] ===
                          {rwb[core], addr[core], data_out[core]},
                          "bus changed while RDY stalled");
                if (done[core] || waiting[core] || stopped[core])
                    enable[core] = 1'b0;
            end
            if (switch_after_fetch != 0 && cycles[1] >= 1)
                turbo = later_turbo;
            elapsed++;
        end
        check(enable == 0, "instruction timeout");
        check({pc[0], s[0], a[0], x[0], y[0], p[0]} ===
              {pc[1], s[1], a[1], x[1], y[1], p[1]},
              $sformatf("state differs: ref=%04x/%02x/%02x/%02x/%02x/%02x fast=%04x/%02x/%02x/%02x/%02x/%02x",
                        pc[0], s[0], a[0], x[0], y[0], p[0],
                        pc[1], s[1], a[1], x[1], y[1], p[1]));
        check(event_count[0] == event_count[1], "write/I/O event count differs");
        for (int event_index = 0; event_index < event_count[0]; event_index++) begin
            check(events[0][event_index] === events[1][event_index],
                  $sformatf("write/I/O event %0d differs", event_index));
            check(event_ticks[0][event_index] == event_ticks[1][event_index],
                  $sformatf("write/I/O guest time differs at event %0d: classic=%0d turbo=%0d",
                            event_index, event_ticks[0][event_index], event_ticks[1][event_index]));
        end
        check(guest_cycles[0] == guest_cycles[1],
              $sformatf("guest cycles differ: classic=%0d turbo=%0d", guest_cycles[0], guest_cycles[1]));
        check(cycles[1] <= cycles[0], "turbo instruction became slower");
        if (expected_fast_cycles >= 0)
            check(cycles[1] == expected_fast_cycles,
                  $sformatf("fast cycles=%0d expected=%0d",
                            cycles[1], expected_fast_cycles));
        saved_cycles += cycles[0] - cycles[1];
        cases++;
        nmi_n = 1'b1;
    endtask

    initial begin
        for (int address = 0; address < 65536; address++)
            put(16'(address), 8'(random_word()));
        tick();
        reset_n = 1'b1;
        initial_s = 8'h80;
        initial_a = 8'h81;
        initial_x = 8'h07;
        initial_y = 8'hFF;
        initial_p = 8'h24;
        initial_pc = 16'h2000;
        put(initial_pc, 8'hE8); // INX retires at the fetch edge.
        run_instruction(1'b1, 0, 1'b1, 1, 1'b0);
        run_instruction(1'b0, 0, 1'b0, 2, 1'b0);
        run_instruction(1'bx, 0, 1'bx, 2, 1'b0);
        run_instruction(1'bz, 0, 1'bz, 2, 1'b0);
        run_instruction(1'b0, 1, 1'b1, 2, 1'b0);

        // Implied and both branch dummy reads must survive in the I/O page.
        initial_pc = 16'hBFFF;
        put(initial_pc, 8'hEA);
        run_instruction(1'b1, 0, 1'b1, 2, 1'b0);
        initial_pc = 16'hBFFE;
        put(initial_pc, 8'hD0);
        put(initial_pc + 1, 8'hFE);
        run_instruction(1'b1, 0, 1'b1, 4, 1'b0);
        initial_pc = 16'hC030;
        put(initial_pc, 8'hD0);
        put(initial_pc + 1, 8'h01);
        run_instruction(1'b1, 0, 1'b1, 3, 1'b0);
        initial_pc = 16'h20FE;
        put(initial_pc, 8'hD0);
        put(initial_pc + 1, 8'hFE);
        run_instruction(1'b1, 0, 1'b1, 2, 1'b0);
        run_instruction(1'b1, 1, 1'b0, 4, 1'b0);
        run_instruction(1'b0, 1, 1'b1, 4, 1'b0);

        // Indexed zero-page arithmetic wraps at $FF; an immediate slow
        // request cancels it, while enabling midway cannot create a skip.
        initial_pc = 16'h2000;
        put(initial_pc, 8'hB5);
        put(initial_pc + 1, 8'hFC);
        run_instruction(1'b1, 0, 1'b1, 3, 1'b0);
        run_instruction(1'b1, 1, 1'b0, 4, 1'b0);
        run_instruction(1'b0, 1, 1'b1, 4, 1'b0);

        // IRQ/NMI win over a fetch that could otherwise retire an INX.
        put(initial_pc, 8'hE8);
        initial_p = 8'h20;
        irq_n = 1'b0;
        run_instruction(1'b1, 0, 1'b1, -1, 1'b0);
        irq_n = 1'b1;
        initial_p = 8'h24;
        nmi_on_fetch = 1'b1;
        run_instruction(1'b1, 0, 1'b1, -1, 1'b0);
        nmi_on_fetch = 1'b0;

        // Indexed dummy reads can name either the destination or the final
        // instruction byte. Cover both I/O exclusions, with and without carry.
        initial_pc = 16'h2000;
        initial_x = 8'h01;
        put(initial_pc, 8'h9D); // STA abs,X
        put(initial_pc + 1, 8'h20);
        put(initial_pc + 2, 8'h30);
        run_instruction(1'b1, 0, 1'b1, 4, 1'b1);
        put(initial_pc + 2, 8'hC0);
        run_instruction(1'b1, 0, 1'b1, 5, 1'b1);
        put(initial_pc + 1, 8'hFF);
        put(initial_pc + 2, 8'hBF); // Crossing into I/O still keeps the store.
        run_instruction(1'b1, 0, 1'b1, 4, 1'b1);
        initial_pc = 16'hC100;
        put(initial_pc, 8'h9D);
        put(initial_pc + 1, 8'hFF);
        put(initial_pc + 2, 8'h30);
        run_instruction(1'b1, 0, 1'b1, 5, 1'b1);

        initial_pc = 16'h2000;
        initial_y = 8'h01;
        put(initial_pc, 8'h91); // STA (zp),Y
        put(initial_pc + 1, 8'hFF);
        put(16'h00FF, 8'hFF);
        put(16'h0000, 8'h30);
        run_instruction(1'b1, 0, 1'b1, 5, 1'b1);
        initial_pc = 16'hC100;
        put(initial_pc, 8'h91);
        put(initial_pc + 1, 8'hFF);
        run_instruction(1'b1, 0, 1'b1, 6, 1'b1);

        initial_pc = 16'h2000;
        put(initial_pc, 8'hFE); // INC abs,X removes two safe dummy reads.
        put(initial_pc + 1, 8'h20);
        put(initial_pc + 2, 8'h30);
        run_instruction(1'b1, 0, 1'b1, 5, 1'b1);
        put(initial_pc, 8'hEE); // INC absolute I/O retains both device reads.
        put(initial_pc + 1, 8'h30);
        put(initial_pc + 2, 8'hC0);
        run_instruction(1'b1, 0, 1'b1, 6, 1'b1);

        // Stack traffic remains real and ordered. Test wrapping S, an I/O
        // return address, and JSR overwriting its own high operand on stack.
        initial_s = 8'hFF;
        put(initial_pc, 8'h48);
        run_instruction(1'b1, 0, 1'b1, 2, 1'b1);
        put(initial_pc, 8'h68);
        run_instruction(1'b1, 0, 1'b1, 2, 1'b1);
        put(initial_pc, 8'h60);
        put(16'h0100, 8'h34);
        put(16'h0101, 8'h30);
        run_instruction(1'b1, 0, 1'b1, 3, 1'b1);
        put(16'h0101, 8'hC0);
        run_instruction(1'b1, 0, 1'b1, 4, 1'b1);
        put(initial_pc, 8'h40);
        run_instruction(1'b1, 0, 1'b1, 4, 1'b1);
        initial_pc = 16'hBFFF;
        put(initial_pc, 8'h68); // Preserve the PC dummy read at $C000.
        run_instruction(1'b1, 0, 1'b1, 3, 1'b1);
        initial_pc = 16'h0180;
        initial_s = 8'h82;
        put(initial_pc, 8'h20);
        put(initial_pc + 1, 8'h34);
        put(initial_pc + 2, 8'h56);
        run_instruction(1'b1, 0, 1'b1, 5, 1'b1);

        // Full opcode coverage over fixed-seed random architectural states,
        // operands, page boundaries, I/O addresses and independent RDY stalls.
        for (int opcode = 0; opcode < 256; opcode++) begin
            for (int sample = 0; sample < 256; sample++) begin
                initial_pc = 16'(random_word());
                initial_s = 8'(random_word());
                initial_a = 8'(random_word());
                initial_x = 8'(random_word());
                initial_y = 8'(random_word());
                initial_p = 8'(random_word());
                put(initial_pc, 8'(opcode));
                put(initial_pc + 1, 8'(random_word()));
                put(initial_pc + 2, 8'(random_word()));
                run_instruction(1'b1, 0, 1'b1, -1, 1'b1);
            end
        end
        check(saved_cycles > 1000, "turbo shortcuts were not exercised");
        $display("W65C02 TURBO PASS cases=%0d saved_cycles=%0d", cases, saved_cycles);
        $finish;
    end
endmodule
