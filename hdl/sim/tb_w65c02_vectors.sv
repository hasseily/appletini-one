`timescale 1ns / 1ps

// Binary-vector driver for SingleStepTests/65x02 wdc65c02/v1 data.
// scripts/test_w65c02_core.py converts the JSON corpus to fixed-size records
// and invokes this testbench through XSim.
module tb_w65c02_vectors;
    localparam int HEADER_BYTES = 16;
    localparam int RECORD_BYTES = 260;

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
    logic [7:0] header [0:HEADER_BYTES-1];
    logic [7:0] record [0:RECORD_BYTES-1];

    string vector_file;
    integer fd;
    integer read_count;
    integer test_count;
    integer opcode;
    integer test_index;
    integer cycle_index;
    integer cycle_count;
    integer init_count;
    integer final_count;
    integer pos;
    integer final_pos;
    integer cycles_pos;
    integer ram_index;
    integer tests_passed;
    integer next_progress;
    integer check_cycles;

    logic [15:0] expected_pc;
    logic [7:0] expected_s;
    logic [7:0] expected_a;
    logic [7:0] expected_x;
    logic [7:0] expected_y;
    logic [7:0] expected_p;
    logic [15:0] expected_addr;
    logic [7:0] expected_data;
    logic expected_write;

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

    function automatic logic [15:0] record_u16(input integer offset);
        record_u16 = {record[offset + 1], record[offset]};
    endfunction

    function automatic integer header_u16(input integer offset);
        header_u16 = {header[offset + 1], header[offset]};
    endfunction

    function automatic integer header_u32(input integer offset);
        header_u32 = {header[offset + 3], header[offset + 2],
                      header[offset + 1], header[offset]};
    endfunction

    task automatic clock_once;
        begin
            #5 clk = 1'b1;
            #5 clk = 1'b0;
        end
    endtask

    task automatic vector_fatal(input string reason);
        begin
            $display("W65C02 FAIL opcode=%02x test=%0d cycle=%0d: %s",
                     opcode, test_index, cycle_index, reason);
            $display("  bus addr=%04x rwb=%0d data_in=%02x data_out=%02x sync=%0d",
                     addr, rwb, data_in, data_out, sync);
            $display("  state PC=%04x S=%02x A=%02x X=%02x Y=%02x P=%02x",
                     debug_pc, debug_s, debug_a, debug_x, debug_y, debug_p);
            $fatal(1, "W65C02 vector mismatch");
        end
    endtask

    initial begin
        vector_file = "build/w65c02_vectors/current_vectors.bin";

        fd = $fopen(vector_file, "rb");
        if (fd == 0)
            $fatal(1, "cannot open vector file: %s", vector_file);

        read_count = $fread(header, fd, 0, HEADER_BYTES);
        if (read_count != HEADER_BYTES)
            $fatal(1, "short vector header: %0d bytes", read_count);
        if (header[0] != 8'h57 || header[1] != 8'h36 ||
            header[2] != 8'h35 || header[3] != 8'h56)
            $fatal(1, "bad vector magic");
        if (header_u16(4) != 1)
            $fatal(1, "unsupported vector version %0d", header_u16(4));
        if (header_u16(6) != RECORD_BYTES)
            $fatal(1, "record size %0d != %0d", header_u16(6), RECORD_BYTES);

        test_count = header_u32(8);
        opcode = header[12];
        check_cycles = header[13];
        tests_passed = 0;
        next_progress = 250000;
        cycle_index = -1;

        // Establish a known state once; every vector then uses the dedicated
        // architectural-state load path without running the reset sequence.
        clock_once();
        reset_n = 1'b1;
        clock_once();

        for (test_index = 0; test_index < test_count; test_index = test_index + 1) begin
            read_count = $fread(record, fd, 0, RECORD_BYTES);
            if (read_count != RECORD_BYTES)
                $fatal(1, "short record %0d: %0d bytes", test_index, read_count);

            opcode = record[256];

            debug_pc_in = record_u16(0);
            debug_s_in = record[2];
            debug_a_in = record[3];
            debug_x_in = record[4];
            debug_y_in = record[5];
            debug_p_in = record[6];

            init_count = record[7];
            pos = 8;
            for (ram_index = 0; ram_index < init_count; ram_index = ram_index + 1) begin
                memory[record_u16(pos)] = record[pos + 2];
                pos = pos + 3;
            end

            expected_pc = record_u16(pos);
            expected_s = record[pos + 2];
            expected_a = record[pos + 3];
            expected_x = record[pos + 4];
            expected_y = record[pos + 5];
            expected_p = record[pos + 6];
            pos = pos + 7;

            final_count = record[pos];
            pos = pos + 1;
            final_pos = pos;
            pos = pos + (final_count * 3);

            cycle_count = record[pos];
            pos = pos + 1;
            cycles_pos = pos;

            debug_load = 1'b1;
            clock_once();
            debug_load = 1'b0;

            for (cycle_index = 0; cycle_index < cycle_count; cycle_index = cycle_index + 1) begin
                pos = cycles_pos + (cycle_index * 4);
                expected_addr = record_u16(pos);
                expected_data = record[pos + 2];
                expected_write = record[pos + 3][0];

                #1;
                if (check_cycles != 0) begin
                    if (addr !== expected_addr)
                        vector_fatal($sformatf("address %04x != expected %04x",
                                             addr, expected_addr));
                    if (rwb === expected_write)
                        vector_fatal($sformatf("bus type %s != expected %s",
                                             rwb ? "read" : "write",
                                             expected_write ? "write" : "read"));
                    if (expected_write) begin
                        if (data_out !== expected_data)
                            vector_fatal($sformatf("write data %02x != expected %02x",
                                                 data_out, expected_data));
                    end else if (data_in !== expected_data) begin
                        vector_fatal($sformatf("read data %02x != expected %02x",
                                             data_in, expected_data));
                    end
                    if (sync !== (cycle_index == 0))
                        vector_fatal("SYNC does not identify only the opcode fetch");
                end

                if (cycle_index != (cycle_count - 1) && instruction_done)
                    vector_fatal("instruction completed before expected final cycle");

                clock_once();
            end

            if (!instruction_done)
                vector_fatal("instruction did not complete on expected final cycle");

            if (debug_pc !== expected_pc)
                vector_fatal($sformatf("PC %04x != expected %04x", debug_pc, expected_pc));
            if (debug_s !== expected_s)
                vector_fatal($sformatf("S %02x != expected %02x", debug_s, expected_s));
            if (debug_a !== expected_a)
                vector_fatal($sformatf("A %02x != expected %02x", debug_a, expected_a));
            if (debug_x !== expected_x)
                vector_fatal($sformatf("X %02x != expected %02x", debug_x, expected_x));
            if (debug_y !== expected_y)
                vector_fatal($sformatf("Y %02x != expected %02x", debug_y, expected_y));
            if (debug_p !== expected_p)
                vector_fatal($sformatf("P %02x != expected %02x", debug_p, expected_p));

            pos = final_pos;
            for (ram_index = 0; ram_index < final_count; ram_index = ram_index + 1) begin
                if (memory[record_u16(pos)] !== record[pos + 2])
                    vector_fatal($sformatf("RAM[%04x] %02x != expected %02x",
                                         record_u16(pos), memory[record_u16(pos)],
                                         record[pos + 2]));
                pos = pos + 3;
            end

            tests_passed = tests_passed + 1;
            if (tests_passed == next_progress) begin
                $display("W65C02 PROGRESS tests=%0d/%0d opcode=%02x",
                         tests_passed, test_count, opcode);
                next_progress = next_progress + 250000;
            end
        end

        $fclose(fd);
        $display("W65C02 PASS tests=%0d cycle_check=%0d",
                 tests_passed, check_cycles);
        $finish;
    end
endmodule
