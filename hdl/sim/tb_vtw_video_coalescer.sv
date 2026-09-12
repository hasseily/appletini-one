`timescale 1ns / 1ps
module tb_vtw_video_coalescer;
    logic clk = 0;
    always #3.75 clk = ~clk;
    logic rstn = 0, clear = 0;
    logic write_valid = 0;
    logic [15:0] write_addr = 0;
    logic [7:0] write_data = 0;
    logic write_ready, mirror_valid, mirror_ready = 0, drained;
    logic [15:0] mirror_addr;
    logic [7:0] mirror_data;
    logic [7:0] expected [0:65535];
    logic [7:0] physical [0:65535];
    bit touched [0:65535];
    int accepted = 0, emitted = 0, cycles = 0;
    bit automatic_drain = 0;
    logic [31:0] random_q = 32'h6532ACE1;
    vtw_video_coalescer dut (.*);

    always @(posedge clk) begin
        if (rstn && !clear) begin
            cycles <= cycles + 1;
            if (write_valid && write_ready) begin
                expected[write_addr] = write_data;
                touched[write_addr] = 1;
                accepted++;
            end
            if (mirror_valid && mirror_ready) begin
                physical[mirror_addr] = mirror_data;
                emitted++;
            end
        end
    end
    always @(negedge clk) begin
        if (automatic_drain)
            mirror_ready = (cycles % 133 == 0);
    end

    task automatic send(input logic [15:0] addr, input logic [7:0] data);
        @(negedge clk);
        if (!write_ready) $fatal(1, "mirror bus unexpectedly throttled writer");
        write_valid = 1;
        write_addr = addr;
        write_data = data;
        @(posedge clk);
        #1;
        @(negedge clk);
        write_valid = 0;
    endtask

    task automatic verify_drained;
        int guard;
        guard = 0;
        automatic_drain = 1;
        while (!drained && guard < 3000000) begin
            @(posedge clk);
            #1;
            guard++;
        end
        if (!drained) $fatal(1, "mirror failed to drain");
        for (int i = 0; i < 65536; i++) begin
            if (touched[i] && physical[i] !== expected[i])
                $fatal(1, "lost latest byte %04x got=%02x expected=%02x",
                       i, physical[i], expected[i]);
        end
        automatic_drain = 0;
        mirror_ready = 0;
    endtask

    initial begin
        repeat (4) @(posedge clk);
        @(negedge clk); rstn = 1;
        wait (write_ready);
        #1;
        if (!drained) $fatal(1, "fresh mirror is not drained");

        // Thousands of writes complete while the physical output cannot
        // accept even one byte. Repeated addresses consume no queue space.
        for (int i = 0; i < 12000; i++)
            send(16'h2000 + 16'(i % 1024), 8'(i ^ (i >> 8)));
        if (accepted != 12000 || emitted != 0)
            $fatal(1, "writer depends on the physical 1 MHz output");
        verify_drained();

        // Overwrite the exact address on the read/dirty-clear edge.
        send(16'h4100, 8'h11);
        wait (dut.state_q == dut.FETCH_BYTE && dut.scan_addr_q == 16'h4100);
        @(negedge clk);
        write_valid = 1; write_addr = 16'h4100; write_data = 8'h22;
        @(posedge clk); #1;
        @(negedge clk); write_valid = 0;
        wait (mirror_valid);
        // Overwrite again while the old snapshot is held under backpressure.
        send(16'h4100, 8'h33);
        verify_drained();

        // Address extremes, different pages, and concurrent mirror drains.
        automatic_drain = 1;
        send(16'h0000, 8'h57);
        send(16'hFFFF, 8'hA8);
        for (int i = 0; i < 5000; i++) begin
            random_q = random_q ^ (random_q << 13);
            random_q = random_q ^ (random_q >> 17);
            random_q = random_q ^ (random_q << 5);
            send({random_q[15:8], 4'h0, random_q[3:0]}, random_q[23:16]);
        end
        verify_drained();
        $display("VTW VIDEO COALESCER PASS: accepted=%0d mirrored=%0d", accepted, emitted);
        $finish;
    end
    initial begin
        #100000000;
        $fatal(1, "coalescer timeout");
    end
endmodule
