`timescale 1ns / 1ps
module tb_vtw_video_coalescer;
    logic clk = 0;
    always #3.75 clk = ~clk;
    logic rstn = 0, clear = 0;
    logic write_valid = 0;
    logic [16:0] write_addr = 0;
    logic [7:0] write_data = 0;
    logic write_active = 1;
    logic flush_valid = 0, flush_bank = 0;
    logic write_ready, mirror_valid, mirror_ready = 0, drained;
    logic active_drained;
    logic [1:0] bank_drained;
    logic [16:0] mirror_addr;
    logic [7:0] mirror_data;
    logic [7:0] expected [0:131071];
    logic [7:0] physical [0:131071];
    bit touched [0:131071];
    int accepted = 0, emitted = 0, cycles = 0;
    bit automatic_drain = 0;
    logic [31:0] random_q = 32'h6532ACE1;
    vtw_video_coalescer dut (.*);

    logic ref_write_ready, ref_mirror_valid, ref_drained, ref_active_drained;
    logic [1:0] ref_bank_drained;
    logic [16:0] ref_mirror_addr;
    logic [7:0] ref_mirror_data;
    int equivalent_cycles = 0;
    vtw_video_coalescer_reference reference_i (
        .clk(clk), .rstn(rstn), .clear(clear),
        .write_valid(write_valid), .write_addr(write_addr),
        .write_data(write_data), .write_active(write_active),
        .flush_valid(flush_valid), .flush_bank(flush_bank),
        .mirror_ready(mirror_ready), .write_ready(ref_write_ready),
        .mirror_valid(ref_mirror_valid), .mirror_addr(ref_mirror_addr),
        .mirror_data(ref_mirror_data), .drained(ref_drained),
        .active_drained(ref_active_drained), .bank_drained(ref_bank_drained)
    );

    // Compare every cycle, including backpressure and reset, against the
    // original control logic. Addresses outside mirror_valid are unused.
    always @(posedge clk) begin
        #0.5;
        equivalent_cycles++;
        if ({write_ready, mirror_valid, drained, active_drained, bank_drained}
            !== {ref_write_ready, ref_mirror_valid, ref_drained,
                 ref_active_drained, ref_bank_drained})
            $fatal(1, "coalescer control changed at cycle %0d", equivalent_cycles);
        if (mirror_valid && {mirror_addr, mirror_data} !==
                            {ref_mirror_addr, ref_mirror_data})
            $fatal(1, "coalescer valid traffic changed at cycle %0d", equivalent_cycles);
    end

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

    task automatic send(input logic [16:0] addr, input logic [7:0] data);
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
        for (int i = 0; i < 131072; i++) begin
            if (touched[i] && physical[i] !== expected[i])
                $fatal(1, "lost latest byte %04x got=%02x expected=%02x",
                       i, physical[i], expected[i]);
        end
        automatic_drain = 0;
        mirror_ready = 0;
    endtask

    task automatic flush_one_bank(input bit bank);
        int guard;
        @(negedge clk);
        flush_valid = 1;
        flush_bank = bank;
        mirror_ready = 1;
        guard = 0;
        while (!bank_drained[bank] && guard < 200000) begin
            @(posedge clk); #1;
            guard++;
        end
        if (!bank_drained[bank]) $fatal(1, "deferred bank failed to drain");
        @(negedge clk);
        flush_valid = 0;
        mirror_ready = 0;
    endtask

    task automatic reset_during_flush(input bit hard_reset);
        write_active = 0;
        send(17'h000FF, 8'hA1);
        send(17'h100FF, 8'hB2);
        @(negedge clk);
        flush_valid = 1;
        flush_bank = 1;
        mirror_ready = 0;
        wait (mirror_valid);
        @(negedge clk);
        // Reset cancels the held snapshot and rejects a concurrent writer.
        rstn = !hard_reset;
        clear = !hard_reset;
        write_valid = 1;
        write_addr = 17'h100FF;
        write_data = 8'hC3;
        repeat (3) @(posedge clk);
        #1;
        if (write_ready || mirror_valid || drained || active_drained || |bank_drained)
            $fatal(1, "reset exposed a pending mirror or accepted a writer");
        @(negedge clk);
        rstn = 1;
        clear = 0;
        write_valid = 0;
        flush_valid = 0;
        for (int i = 0; i < 131072; i++)
            touched[i] = 0;
        wait (write_ready);
        #1;
        if (!drained || !active_drained || bank_drained != 2'b11 || mirror_valid)
            $fatal(1, "reset did not discard the old dirty pages");
    endtask

    initial begin
        repeat (4) @(posedge clk);
        @(negedge clk); rstn = 1;
        wait (write_ready);
        #1;
        if (!drained) $fatal(1, "fresh mirror is not drained");

        // Inactive display bytes retain their bank and latest value, but
        // they cause no physical traffic or active-display drain barrier.
        write_active = 0;
        send(17'h008DF, 8'h31);
        send(17'h108DF, 8'hA5);
        send(17'h008DF, 8'h62);
        repeat (2000) @(posedge clk);
        #1;
        if (!active_drained || drained || bank_drained != 2'b00 ||
            mirror_valid || emitted != 0)
            $fatal(1, "inactive dirty bytes entered the ordinary mirror");
        flush_one_bank(1);
        if (physical[17'h108DF] !== 8'hA5 ||
            physical[17'h008DF] === 8'h62 || bank_drained != 2'b10)
            $fatal(1, "AUX flush consumed or corrupted deferred MAIN data");
        flush_one_bank(0);
        if (physical[17'h008DF] !== 8'h62 || !drained)
            $fatal(1, "deferred MAIN flush lost its latest byte");
        write_active = 1;

        // Thousands of writes complete while the physical output cannot
        // accept even one byte. Repeated addresses consume no queue space.
        for (int i = 0; i < 12000; i++)
            send(16'h2000 + 16'(i % 1024), 8'(i ^ (i >> 8)));
        if (accepted != 12003 || emitted != 2)
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
        send(17'h1FFFF, 8'hA8);
        for (int i = 0; i < 5000; i++) begin
            random_q = random_q ^ (random_q << 13);
            random_q = random_q ^ (random_q >> 17);
            random_q = random_q ^ (random_q << 5);
            send({random_q[15:8], 4'h0, random_q[3:0]}, random_q[23:16]);
        end
        verify_drained();

        reset_during_flush(0);
        reset_during_flush(1);
        // Page ends must retain their bank; the next page loads separately.
        write_active = 0;
        send(17'h000FF, 8'h11);
        send(17'h00100, 8'h22);
        send(17'h0FFFF, 8'h33);
        send(17'h10000, 8'h44);
        send(17'h1FFFF, 8'h55);
        flush_one_bank(1);
        flush_one_bank(0);
        verify_drained();
        $display("VTW VIDEO COALESCER PASS: accepted=%0d mirrored=%0d equivalent_cycles=%0d",
                 accepted, emitted, equivalent_cycles);
        $finish;
    end
    initial begin
        #100000000;
        $fatal(1, "coalescer timeout");
    end
endmodule
