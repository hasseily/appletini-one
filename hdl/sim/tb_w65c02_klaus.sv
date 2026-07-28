`timescale 1ns / 1ps

// Program-level harness for Klaus Dormann's 6502/65C02 functional tests.
// scripts/test_w65c02_klaus.py supplies a 64 KiB memory image and a compact
// run-control block in build/w65c02_klaus.
module tb_w65c02_klaus;
    localparam logic [15:0] FEEDBACK_PORT = 16'hBFFC;

    logic clk = 1'b0;
    logic reset_n = 1'b0;
    logic enable = 1'b1;
    logic ready = 1'b1;
    logic irq_n;
    logic nmi_n;
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
    logic [7:0] control [0:15];
    logic [7:0] feedback_q = 8'h00;

    integer program_fd;
    integer config_fd;
    integer read_count;
    integer cycle_count;
    integer instruction_count;
    integer max_cycles;
    integer next_progress;
    integer same_pc_count;
    integer suite_id;
    integer termination_mode;
    integer feedback_enable;
    integer check_memory;
    logic [15:0] start_pc;
    logic [15:0] pass_pc;
    logic [15:0] expected_address;
    logic [15:0] last_sync_pc;
    logic [7:0] expected_value;

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

    assign data_in = (feedback_enable != 0 && addr == FEEDBACK_PORT)
                   ? feedback_q : memory[addr];
    assign irq_n = !((feedback_enable != 0) && feedback_q[0]);
    assign nmi_n = !((feedback_enable != 0) && feedback_q[1]);

    always_ff @(posedge clk) begin
        if (reset_n && !debug_load && enable && ready && !rwb) begin
            memory[addr] <= data_out;
            if (feedback_enable != 0 && addr == FEEDBACK_PORT)
                feedback_q <= data_out;
        end
    end

    task automatic tick;
        begin
            #5 clk = 1'b1;
            #5 clk = 1'b0;
        end
    endtask

    task automatic fail(input string reason);
        begin
            $display("KLAUS FAIL suite=%0d cycles=%0d instructions=%0d: %s",
                     suite_id, cycle_count, instruction_count, reason);
            $display("  bus addr=%04x rwb=%0d din=%02x dout=%02x sync=%0d",
                     addr, rwb, data_in, data_out, sync);
            $display("  state PC=%04x S=%02x A=%02x X=%02x Y=%02x P=%02x test_case=%02x",
                     debug_pc, debug_s, debug_a, debug_x, debug_y, debug_p,
                     memory[16'h0202]);
            if (suite_id == 3)
                $display("  decimal N1=%02x N2=%02x HA=%02x HP=%02x DA=%02x DP=%02x AR=%02x CF=%02x ERROR=%02x",
                         memory[16'h0000], memory[16'h0001], memory[16'h0002],
                         memory[16'h0003], memory[16'h0004], memory[16'h0005],
                         memory[16'h0006], memory[16'h000A], memory[16'h000B]);
            $finish;
        end
    endtask

    task automatic pass;
        begin
            if (check_memory != 0 && memory[expected_address] !== expected_value) begin
                fail($sformatf("memory[%04x]=%02x expected %02x",
                               expected_address, memory[expected_address],
                               expected_value));
            end else begin
                $display("KLAUS PASS suite=%0d cycles=%0d instructions=%0d PC=%04x",
                         suite_id, cycle_count, instruction_count, debug_pc);
                $finish;
            end
        end
    endtask

    initial begin
        program_fd = $fopen("build/w65c02_klaus/current_program.bin", "rb");
        if (program_fd == 0)
            $fatal(1, "cannot open current_program.bin");
        read_count = $fread(memory, program_fd, 0, 65536);
        $fclose(program_fd);
        if (read_count != 65536)
            $fatal(1, "program image is %0d bytes, expected 65536", read_count);

        config_fd = $fopen("build/w65c02_klaus/current_config.bin", "rb");
        if (config_fd == 0)
            $fatal(1, "cannot open current_config.bin");
        read_count = $fread(control, config_fd, 0, 16);
        $fclose(config_fd);
        if (read_count != 16)
            $fatal(1, "control block is %0d bytes, expected 16", read_count);

        start_pc = {control[1], control[0]};
        pass_pc = {control[3], control[2]};
        max_cycles = {control[7], control[6], control[5], control[4]};
        termination_mode = control[8];
        feedback_enable = control[9];
        expected_address = {control[11], control[10]};
        expected_value = control[12];
        suite_id = control[13];
        check_memory = control[14];

        tick();
        reset_n = 1'b1;
        debug_pc_in = start_pc;
        debug_s_in = 8'hFF;
        debug_a_in = 8'h00;
        debug_x_in = 8'h00;
        debug_y_in = 8'h00;
        debug_p_in = 8'h24;
        debug_load = 1'b1;
        tick();
        debug_load = 1'b0;

        instruction_count = 0;
        same_pc_count = 0;
        last_sync_pc = 16'hFFFF;
        next_progress = 10000000;

        for (cycle_count = 0; cycle_count < max_cycles;
             cycle_count = cycle_count + 1) begin
            #1;

            if (cycle_count == next_progress) begin
                $display("KLAUS PROGRESS suite=%0d cycles=%0d instructions=%0d PC=%04x",
                         suite_id, cycle_count, instruction_count, debug_pc);
                next_progress = next_progress + 10000000;
            end

            if (sync) begin
                instruction_count = instruction_count + 1;
                if (addr == last_sync_pc)
                    same_pc_count = same_pc_count + 1;
                else begin
                    last_sync_pc = addr;
                    same_pc_count = 1;
                end

                if (termination_mode == 0 && same_pc_count >= 3) begin
                    if (addr == pass_pc)
                        pass();
                    else
                        fail($sformatf("self-trap at PC=%04x", addr));
                end
            end

            if (stopped) begin
                if (termination_mode == 1)
                    pass();
                else
                    fail("unexpected STP");
            end

            if (waiting)
                fail("unexpected WAI");

            tick();
        end

        fail("cycle limit exceeded");
    end
endmodule
